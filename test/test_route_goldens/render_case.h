#pragma once
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "golden_io.h"
#include "nimbus/harness/http.h"
#include "nimbus/harness/plain/route.h"
#include "nimbus/harness/plain/wire.h"
#include "nimbus/orch/tx/arena.h"
#include "transcripts.h"

// render_case.h - one Case rendered through the wire as it is (wireFor) into the text a request
// golden holds:
//   # route golden request <signature>/<name>
//   POST https://<host>:<port><path>
//   header <name>: <value>          (each header, in the order the wire wrote them)
//   splices <n>                     (photo bytes spliced at send time)
//   body <bytes>
//   <the materialized body, photos base64-encoded in place>
// A render the wire refuses is pinned as "render refused <err> at <where>" instead.
namespace route_goldens {

inline constexpr const char* kFixtureKey = "fixture-key-not-a-secret";

// The photos every fixture may name: a JPEG start and fixed filler, so base64 bytes never move.
inline std::map<std::string, std::string> photoStore() {
  std::map<std::string, std::string> m;
  const char* refs[] = {"pa1b2c3d4", "pb0000001", "pb0000002", "pb0000003", "pb0000004"};
  for (int i = 0; i < 5; ++i) m[refs[i]] = std::string("\xFF\xD8\xFF\xE0", 4) + std::string(48 + 8 * i, (char)('A' + i));
  return m;
}

// Everything a render reads, owned in one place so RenderInput's pointers stay valid.
struct World {
  plain::TurnCtx turn;
  plain::HeadEndpoint head;
  plain::SystemBlocks system;
  std::vector<plain::ToolSpec> tools;
  std::map<std::string, std::string> photos = photoStore();
  tx::TxArena arena;
  plain::RenderInput in;
  RowFacts row;
  plain::Route route;              // a chat row's route and pre-flight (lane E2-R6)
  plain::PreflightState own;

  tx::Status load(const Case& c) {
    tx::Status st = arena.init(1 << 22, [](size_t n) { return std::malloc(n); }, [](void* p) { std::free(p); });
    for (size_t i = 0; st.ok() && i < c.tx.items.size(); ++i) st = arena.append(c.tx.items[i]);
    if (!st.ok()) return st;
    row = rowOf(c.signature, c.facts.routed);
    setTurn(c);
    setHead(c.facts);
    setSystem(c.facts);
    if (!c.facts.noTools) tools = fixedTools();
    setInput(c);
    return tx::Status::success();
  }

  void setTurn(const Case& c) {
    turn.chatKey = "2002";
    turn.chatId = "2002";
    turn.turn = c.facts.openTurn ? c.facts.openTurn : c.tx.turn;
    turn.host = row.turnHost;
    turn.model = row.model;
    turn.dialect = row.dialect;
    turn.dayStartMin = c.facts.dayStartMin;
  }
  void setHead(const Facts& f) {
    head.host = row.headHost;
    head.effort = f.effort;
    head.routed = row.routed;
    head.key = [] { return std::string(kFixtureKey); };
  }
  void setSystem(const Facts& f) {
    if (f.system == Sys::Summarizer) {   // plain_compact.cpp SummarizerSystem: summary as core, ask as digests
      system.core = kSummaryCore;
      system.digests = kSummaryAsk;
      return;
    }
    system.core = kCore;
    system.notes = kNotes;
    system.digests = kDigests;
  }
  void setInput(const Case& c) {
    const Facts& f = c.facts;
    in.turn = &turn;
    in.head = &head;
    in.system = &system;
    in.tools = &tools;
    in.items = &arena;
    in.stubPointer = f.pointer;
    in.toolChoiceNone = f.toolChoiceNone;
    in.maxTokens = f.maxTokens;
    in.answerSchema = f.schema ? kSchema : nullptr;
    in.ladder = &f.ladder;
    in.longCache = f.longCache;
    in.bound.view = f.viewBound;
    const std::map<std::string, std::string>* bulk = &c.tx.bulk;
    in.bulk = [bulk](const std::string& handle, std::string& out) {
      auto it = bulk->find(handle);
      if (it == bulk->end()) return tx::Status::fail(tx::Err::NotFound, "route goldens bulk");
      out = it->second;
      return tx::Status::success();
    };
    const std::map<std::string, std::string>* ph = &photos;
    const bool fileRef = f.photos == Photos::FileRef;
    in.photo = [ph, fileRef](const std::string& ref, plain::PhotoBytes& p) {
      auto it = ph->find(ref);
      if (it == ph->end()) return tx::Status::fail(tx::Err::NotFound, "route goldens photo");
      p.bytes = (const uint8_t*)it->second.data();
      p.len = it->second.size();
      p.mime = "image/jpeg";
      p.fileId = fileRef ? "file_" + ref : std::string();
      return tx::Status::success();
    };
  }
};

inline std::string refusedLine(const tx::Status& st) {
  return std::string("render refused ") + tx::errSlug(st.err) + " at " + st.where + "\n";
}

// The golden text of one rendered request.
inline std::string requestText(const agent::HttpRequest& r) {
  std::string o = std::string(r.method) + (r.tls ? " https://" : " http://") + r.host + ":" + std::to_string(r.port) +
                  r.path + "\n";
  for (const auto& h : r.headers) o += "header " + h.first + ": " + h.second + "\n";
  const std::string body = agent::materializeBody(r);
  o += "splices " + std::to_string(r.splices.size()) + "\nbody " + std::to_string(body.size()) + "\n";
  return o + body + "\n";
}

inline std::string goldenPath(const Case& c) { return std::string(kRoot) + c.signature + "/" + c.name + ".req"; }

// Render `c` once through the wire as it is today.
inline std::string renderCase(const Case& c) {
  std::string head = "# route golden request " + c.signature + "/" + c.name + "\n";
  World w;
  tx::Status st = w.load(c);
  if (!st.ok()) return head + "fixture refused by the arena: " + tx::errSlug(st.err) + " at " + st.where + "\n";
  const plain::WireCodec* wire = nullptr;
  if (!(st = plain::wireFor(w.row.dialect, wire)).ok() || !wire) return head + refusedLine(st);
  if (w.row.route) {   // as the exchange renders it: the route, then the pre-flight (its dropped bits pinned)
    if (!(st = plain::resolveRoute(w.head, w.turn, w.route)).ok()) return head + refusedLine(st);
    head += "preflight dropped " + std::to_string(plain::preflight(w.route, 0, w.own, w.in)) + "\n";
  }
  agent::HttpRequest req;
  if (!(st = wire->render(w.in, req)).ok()) return head + refusedLine(st);
  return head + requestText(req);
}

// One request of `c` as renderCase renders it, for the shape checker (lane E2-R7). The request's
// photo splices point into `w`, which outlives it.
inline tx::Status renderRequest(const Case& c, World& w, agent::HttpRequest& req) {
  tx::Status st = w.load(c);
  const plain::WireCodec* wire = nullptr;
  if (st.ok()) st = plain::wireFor(w.row.dialect, wire);
  if (st.ok() && w.row.route && (st = plain::resolveRoute(w.head, w.turn, w.route)).ok()) plain::preflight(w.route, 0, w.own, w.in);
  return st.ok() ? wire->render(w.in, req) : st;
}

}  // namespace route_goldens
