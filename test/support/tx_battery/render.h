#pragma once
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../scripted_provider/request_view.h"
#include "items.h"
#include "nimbus/harness/http.h"
#include "nimbus/harness/plain/wire.h"
#include "nimbus/orch/tx/arena.h"
#include "props.h"
#include "rows.h"

// tx_battery/render.h - one request rendered from a stored transcript through a dialect's
// real WireCodec (lane RB-B0b), read back into a RequestView. The wire-level properties
// (prefix, pairing, coverage, overhead) run on this: no engine, no network.
namespace txb {

namespace plain = agent::plain;

inline sp::Dialect viewDialect(plain::Dialect d, const std::string& path) {
  sp::Dialect out;
  if (sp::dialectOfPath(path, out)) return out;
  switch (d) {
    case plain::Dialect::AnthropicMessages: return sp::Dialect::Anthropic;
    case plain::Dialect::OpenAIResponses: return sp::Dialect::Responses;
    default: return sp::Dialect::Chat;
  }
}

// The fixed inputs around the transcript: system blocks, three tools, a head.
struct RenderFixture {
  plain::Dialect dialect = plain::Dialect::AnthropicMessages;
  plain::TurnCtx turn;
  plain::HeadEndpoint head;
  plain::SystemBlocks system;
  std::vector<plain::ToolSpec> tools;
  std::map<std::string, std::string> photoBytes;   // ref -> jpeg bytes

  explicit RenderFixture(plain::Dialect d = plain::Dialect::AnthropicMessages) : dialect(d) {
    turn.chatKey = "1001";
    turn.chatId = "1001";
    turn.dialect = d;
    turn.host = "api.anthropic.com";
    turn.model = "claude-sonnet-5-5";
    head.host = turn.host;
    head.key = [] { return std::string("sk-fake-battery"); };
    system.core = "You are Nimbus, a pocket assistant. [battery core]";
    system.notes = "Pinned: the boat is the Nereid II.";
    system.digests = "Mon 16 Nov: talked about the boat engine.";
    tools = {{"read_file", "Read a file.", R"({"type":"object","properties":{"path":{"type":"string"}}})"},
             {"ledger_write", "Change the ledger.", R"({"type":"object","properties":{"day":{"type":"string"}}})"},
             {"web_search", "Search the web.", R"({"type":"object","properties":{"q":{"type":"string"}}})"}};
    photoBytes["pab12cd34"] = std::string("\xFF\xD8\xFF\xE0", 4) + std::string(996, 'j');
    // Lane E2-R6: a chat dialect renders as a head of that dialect, through its route and pre-flight:
    // Z.ai for chat (its ids pass through, so the checkers find the stored ids), a custom endpoint for text mode.
    if (d == plain::Dialect::ChatCompletions || d == plain::Dialect::Flat) {
      const bool chat = d == plain::Dialect::ChatCompletions;
      turn.host = head.host = chat ? "api.z.ai" : "custom.example.test";
      turn.model = chat ? "glm-5.3" : "battery-model";
      routeAndPreflight = true;
    }
  }
  // The same inputs for one provider table row (lane E2-R3a): its dialect, endpoint, model and route.
  explicit RenderFixture(const Row& r) : RenderFixture(r.dialect) {
    turn.host = r.host;
    turn.model = turnModel(r);
    head.host = r.host;
    head.routed = r.routed;
    routeAndPreflight = r.dialect == plain::Dialect::ChatCompletions || r.dialect == plain::Dialect::Flat;
  }
  // Lane E2-R6: a chat row renders as the exchange sends it, through its route and pre-flight.
  bool routeAndPreflight = false;
};

struct Rendered {
  agent::HttpRequest req;
  std::string body;
  sp::RequestView view;
};

// Load `items` into a fresh arena (RB-T1) and render one request (the dialect's lane).
inline tx::Status renderItems(const RenderFixture& fx, const Convo& c, uint64_t pointer, Rendered& out,
                              uint32_t openTurn = 0, bool toolChoiceNone = false) {
  tx::TxArena arena;
  tx::Status st = arena.init(tx::kActiveChatArenaBytes, [](size_t n) { return std::malloc(n); }, [](void* p) { std::free(p); });
  if (!st.ok()) return st;
  for (const tx::Item& it : c.items)
    if (!(st = arena.append(it)).ok()) return st;
  const plain::WireCodec* wire = nullptr;
  if (!(st = plain::renderWireFor(fx.dialect, wire)).ok()) return st;   // render only: no parse needed
  if (!wire) return tx::Status::fail(tx::Err::NotFound, "txb::renderItems: renderWireFor gave no codec");
  plain::TurnCtx turn = fx.turn;
  turn.turn = openTurn ? openTurn : c.turn;
  plain::RenderInput in;
  in.turn = &turn;
  in.head = &fx.head;
  in.system = &fx.system;
  in.tools = &fx.tools;
  in.items = &arena;
  in.stubPointer = pointer;
  in.maxTokens = 4096;
  in.toolChoiceNone = toolChoiceNone;   // the guard's last round (lane E2-R3a, property 2)
  const auto* bulk = &c.bulk;
  in.bulk = [bulk](const std::string& handle, std::string& o) {
    auto it = bulk->find(handle);
    if (it == bulk->end()) return tx::Status::fail(tx::Err::NotFound, "txb bulk");
    o = it->second;
    return tx::Status::success();
  };
  const auto* photos = &fx.photoBytes;
  in.photo = [photos](const std::string& ref, plain::PhotoBytes& o) {
    auto it = photos->find(ref);
    if (it == photos->end()) return tx::Status::fail(tx::Err::NotFound, "txb photo");
    o.bytes = (const uint8_t*)it->second.data();
    o.len = it->second.size();
    o.mime = "image/jpeg";
    o.fileId = "file_" + ref;
    return tx::Status::success();
  };
  plain::Route route;
  plain::PreflightState own;
  if (fx.routeAndPreflight) {
    if (!(st = plain::resolveRoute(fx.head, turn, route)).ok()) return st;
    (void)plain::preflight(route, 0, own, in);
  }
  if (!(st = wire->render(in, out.req)).ok()) return st;
  out.body = agent::materializeBody(out.req);
  sp::parseRequest(viewDialect(fx.dialect, out.req.path), out.body, out.view);
  return tx::Status::success();
}

// The stored text a request shows (u a c r h; k of the open turn) as JSON string bytes,
// and the blocks it shows them in, for the overhead property.
inline void renderedText(const Convo& c, const CoverInput& in, OverheadInput& out) {
  const std::set<uint64_t> sup = detail::supersededSet(c.items);
  for (const tx::Item& it : c.items) {
    if (it.q <= in.thru) continue;
    switch (it.kind) {
      case tx::ItemKind::User:
        ++out.ownerMessages;
        out.stampBytes += jsonStringBytes(stampOf(it, in.dayStartMin)) - 2;   // inside the text's own quotes
        out.itemBytes += jsonStringBytes(it.x);
        for (const tx::Part& p : it.parts) out.photoLabels += p.kind == tx::PartKind::Image;
        break;
      case tx::ItemKind::Assistant:
      case tx::ItemKind::DeviceNote:
        ++out.textBlocks;
        out.itemBytes += jsonStringBytes(it.x);
        break;
      case tx::ItemKind::RoundMeta:
        if (it.turn != in.openTurn) break;
        ++out.textBlocks;
        out.itemBytes += jsonStringBytes(it.x);
        break;
      case tx::ItemKind::Call:
        ++out.calls;
        out.itemBytes += jsonStringBytes(it.id) + jsonStringBytes(it.n) + jsonStringBytes(it.args);
        break;
      case tx::ItemKind::Result:
        ++out.results;
        out.itemBytes += jsonStringBytes(it.id) +
                         jsonStringBytes(expectedShow(it, in, sup) == Shown::Stub ? it.st : detail::resultWhole(it, in));
        break;
      default:
        break;
    }
  }
}

}  // namespace txb
