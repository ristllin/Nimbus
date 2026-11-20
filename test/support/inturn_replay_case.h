#pragma once
// inturn_replay_case - lane RB-BGb part 2 (ruling 93 item 3): one owner turn that reads a 40 KB
// file in parts (RB-TR1's files.read parts of at most 8,000 B, each naming the next offset, then
// the answer) through the real
// TurnEngine and the real Anthropic adapter onto a fake transport, with a history about half
// the head's history max. Shared by the native suite (test_inturn_replay) and nimbusd
// (tests/test_inturn_replay.cpp), so both build the same requests. Offline, no keys.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "fake_config.h"
#include "fake_platform.h"
#include "fake_provider_deps.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/budget.h"
#include "nimbus/orch/episodic.h"
#include "nimbus/orch/file_aids_e1.h"
#include "nimbus/orch/request_budget.h"
#include "nimbus/orch/router_route.h"

namespace inturn_replay {

namespace orch = nimbus::orch;

constexpr size_t kPartBytes = 8000;   // about one part: the result cap RB-TR1's readPart fills
constexpr size_t kPromptPad = 30000;
constexpr uint64_t kNow = 1795113600ull + 15 * 3600;

// The file: about 40 KB of numbered lines.
inline const std::string& fileText() {
  static const std::string f = [] {
    std::string s;
    for (int n = 0; s.size() < 40000; n++) s += "L" + std::to_string(n) + ". a row of data in the long file\n";
    return s;
  }();
  return f;
}

// The file as RB-TR1's files.read gives it (readPart, each result at most kPartBytes): the offset
// of each part, the part as the tool returns it, and a line token from the back of each part (a
// fold line's preview of the result never holds it).
struct MemFile : nimbus::orch::aids::AidSource {
  bool readAt(uint64_t off, size_t len, char* out, size_t& got) override {
    const std::string& f = fileText();
    got = off < f.size() ? std::min(len, (size_t)(f.size() - off)) : 0;
    if (got) std::memcpy(out, f.data() + off, got);
    return true;
  }
};
struct Plan {
  std::vector<uint64_t> offsets;
  std::vector<std::string> results, tokens;
};
inline const Plan& plan() {
  static const Plan p = [] {
    Plan q;
    MemFile src;
    uint64_t at = 0;
    for (;;) {
      std::string out;
      nimbus::orch::aids::e1::readPart(src, fileText().size(), at, kPartBytes, out);
      q.offsets.push_back(at);
      q.results.push_back(out);
      const size_t back = out.rfind("\nL", out.size() * 3 / 4);
      q.tokens.push_back(out.substr(back + 1, out.find('.', back) - back));
      const size_t next = out.find("next part: \"offset\":");
      if (next == std::string::npos || next > out.find('\n')) break;
      at = std::strtoull(out.c_str() + next + 20, nullptr, 10);
    }
    return q;
  }();
  return p;
}
inline int partCount() { return (int)plan().offsets.size(); }

inline std::string esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if (c == '\n') {
      o += "\\n";
      continue;
    }
    o += c;
  }
  return o;
}

struct Head {
  const char* host;    // "anthropic" or "cumulo"
  const char* model;   // the head model (a cumulo selector for a routed head)
};

struct Result {
  std::vector<std::string> bodies;   // every request the head sent, in order
  std::string reply;
  int dispatched = 0;
};

struct Rig {
  harness_test::FakePlatform plat;
  harness_test::FakeConfig cfg;
  harness_test::FakeProviderDeps wire;
  agent::providers::ProviderDeps pd;
  std::vector<orch::EpisodicMessage> rows;
  std::unique_ptr<agent::TurnEngine> eng;
  Head head;
  int seq = 0;
  int dispatched = 0;
  std::string reply;
  std::vector<std::string> kept;   // the results ring: what results.get would read back

  std::string addRow(uint32_t at, const char* role, const std::string& text, const std::string& tags) {
    orch::EpisodicMessage m;
    char id[16];
    std::snprintf(id, sizeof(id), "m%08x", (unsigned)(++seq));
    m.id = id;
    m.sessionId = "1001";
    m.tsHours = at / 3600u;
    m.role = role;
    m.kind = orch::MsgKind::Message;
    m.text = text;
    m.tags = tags + ",at:" + std::to_string(at);
    rows.push_back(m);
    return m.id;
  }

  // History of yesterday, `bytes` of dialogue in exchanges of about 1 KB.
  void history(size_t bytes) {
    const uint32_t start = (uint32_t)(kNow - kNow % 86400u - 86400u + 6u * 3600u);
    size_t have = 0;
    for (int i = 0; have < bytes; i++) {
      const std::string u = "H" + std::to_string(i) + ". " + std::string(200, 'q');
      const std::string a = "A" + std::to_string(i) + ". " + std::string(800, 'a');
      const std::string id = addRow(start + 120u * (uint32_t)i, "user", u, "from:Owner");
      addRow(start + 120u * (uint32_t)i + 30u, "assistant", a, "fx:none,out:reply,turn:" + id);
      have += u.size() + a.size();
    }
  }

  void script() {
    using harness_test::Exchange;
    for (int i = 0; i < partCount(); i++) {
      const std::string args = "{\"path\":\"notes/big.txt\",\"offset\":" + std::to_string(plan().offsets[i]) + "}";
      Exchange e;
      e.status = 200;
      e.body = "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_" + std::to_string(i) +
               "\",\"name\":\"files_read\",\"input\":" + args +
               "}],\"stop_reason\":\"tool_use\",\"usage\":{\"input_tokens\":10,\"output_tokens\":5}}";
      wire.http.script.push_back(e);
    }
    Exchange fin;
    fin.status = 200;
    fin.body =
        "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_f\",\"name\":\"orch_turn\",\"input\":{\"reply\":\"the file is "
        "read\"}}],\"stop_reason\":\"tool_use\",\"usage\":{\"input_tokens\":10,\"output_tokens\":5}}";
    for (int i = 0; i < 6; i++) wire.http.script.push_back(fin);
  }

  Rig(const Head& h, size_t historyBytes) : head(h) {
    agent::providers::clearHostParks();
    plat.epoch = kNow;
    cfg.toolLoop = true;
    cfg.keyed = {h.host};
    cfg.keys[h.host] = "fake";
    cfg.orchHost = h.host;
    cfg.priority = h.host;
    cfg.orchModels[h.host] = h.model;
    cfg.resultCap = 0;   // the derived per-result budget (the device default)
    cfg.totalCap = 0;
    pd = wire.contract();
    pd.key = [](const char*) { return std::string("fake"); };
    pd.orchModel = [this](const char* host) { return cfg.orchModels[host]; };
    history(historyBytes);
    script();
    agent::TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    d.deliver = [this](const std::string&, const std::string& t) { reply += t; };
    d.apply.deliver = d.deliver;
    d.apply.principalFor = [](const std::string& c) { return orch::principalForRole(c, orch::Role::Admin); };
    d.composeInputs = [](const std::string&) {
      agent::ComposeInputs in;
      in.devName = "Nimbus";
      // A device's prompt: working notes to about kPromptPad (a device's is 30 to 56 KB).
      for (size_t i = 0; in.runningMemory.size() < kPromptPad; i++)
        in.runningMemory += "- note " + std::to_string(i) + ": the owner keeps lists of things to do.\n";
      in.speakerRole = "admin";
      in.speakerLabel = "Owner";
      return in;
    };
    d.toolSpecs = [](const orch::Principal&) {
      return std::vector<orch::ToolRegistry::Spec>{
          {"files.read", "read a file in parts",
           "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\"}}}"}};
    };
    d.mcpDispatch = [this](const std::string& req, const orch::Principal&) {
      dispatched++;
      const size_t at = req.find("\"offset\":");
      const uint64_t offset = at == std::string::npos ? 0 : std::strtoull(req.c_str() + at + 9, nullptr, 10);
      MemFile src;
      std::string out;
      nimbus::orch::aids::e1::readPart(src, fileText().size(), offset, kPartBytes, out);
      return "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"" + esc(out) +
             "\"}]}}";
    };
    d.spillResult = [this](const std::string&, const std::string& text, const std::string&) {
      kept.push_back(text);
      return "r" + std::to_string(kept.size());
    };
    d.histV2 = [] { return true; };
    d.historyRows = [this](const std::string& c, const agent::HistoryQuery& q) {
      std::vector<orch::EpisodicMessage> out;
      const uint32_t before = q.before.empty() ? UINT32_MAX : orch::epiIdSuffix(q.before);
      const uint32_t after = q.after.empty() ? 0 : orch::epiIdSuffix(q.after);
      if (!q.after.empty()) {
        for (const auto& m : rows)
          if (m.sessionId == c && orch::epiIdSuffix(m.id) > after && out.size() < q.limit) out.push_back(m);
        return out;
      }
      for (auto it = rows.rbegin(); it != rows.rend() && out.size() < q.limit; ++it)
        if (it->sessionId == c && orch::epiIdSuffix(it->id) < before) out.push_back(*it);
      return out;
    };
    d.captureUserRow = [this](const std::string&, const std::string& t, const std::string& tags) {
      return addRow((uint32_t)kNow, "user", t, tags);
    };
    const std::string host = h.host;
    d.hosts.add(host, [this, host](std::string& conv, const std::string& ins, const std::string& inp, std::string& out,
                                   std::string& err, const agent::HeadTools* tools, orch::TokenUsage* use) {
      if (host != "cumulo") return agent::providers::orchTurnAnthropic(pd, conv, ins, inp, out, err, tools, use);
      const orch::RouterRoute rr = orch::resolveRouterRoute(cfg.orchModels["cumulo"]);
      agent::providers::ProviderDeps r = pd;
      r.routerBase = [] { return std::string("https://app.cumulo-nimbus.ai"); };
      r.routerKey = [] { return std::string("cumulo_sk_test"); };
      const std::string model = rr.model;
      r.orchModel = [model](const char*) { return model; };
      return agent::providers::orchTurnAnthropic(r, conv, ins, inp, out, err, tools, use);
    });
    eng.reset(new agent::TurnEngine(std::move(d)));
  }
  ~Rig() { agent::providers::clearHostParks(); }

  Result run() {
    eng->handleMessage("read notes/big.txt and tell me what it holds", "Owner", "1001");
    Result r;
    for (const auto& s : wire.http.seen) r.bodies.push_back(s.body);
    r.reply = reply;
    r.dispatched = dispatched;
    return r;
  }
};

// The parts (0..kParts-1) whose first token the request carries.
inline std::vector<int> partsIn(const std::string& body) {
  std::vector<int> in;
  for (int i = 0; i < partCount(); i++)
    if (body.find(plan().tokens[i]) != std::string::npos) in.push_back(i);
  return in;
}

}  // namespace inturn_replay
