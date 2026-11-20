#pragma once
// inturn_replay_case - lane RB-BGb part 2 (ruling 93 item 3): one owner turn that reads a 40 KB
// file in parts (five rounds of files_read, 7,800 B each, then the answer) through the real
// TurnEngine and the real Anthropic adapter onto a fake transport, with a history about half
// the head's history max. Shared by the native suite (test_inturn_replay) and nimbusd
// (tests/test_inturn_replay.cpp), so both build the same requests. Offline, no keys.
#include <cstdio>
#include <cstdlib>
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
#include "nimbus/orch/request_budget.h"
#include "nimbus/orch/router_route.h"

namespace inturn_replay {

namespace orch = nimbus::orch;

constexpr int kParts = 5;
constexpr size_t kPartBytes = 7800;
constexpr uint64_t kNow = 1795113600ull + 15 * 3600;

// Part i of the file: lines that each carry a token "P<i>L<n>." the checks look for.
inline std::string part(int i) {
  std::string s;
  for (int n = 0; s.size() < kPartBytes; n++) s += "P" + std::to_string(i) + "L" + std::to_string(n) + ". row of data\n";
  s.resize(kPartBytes);
  return s;
}
// A token from the back half of part i: a fold line's preview of the result never holds it.
inline std::string firstToken(int i) { return "P" + std::to_string(i) + "L300."; }

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
    for (int i = 0; i < kParts; i++) {
      const std::string args = "{\"path\":\"notes/big.txt\",\"offset\":" + std::to_string(i * kPartBytes) + "}";
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
      const int i = at == std::string::npos ? 0 : (int)(std::strtoul(req.c_str() + at + 9, nullptr, 10) / kPartBytes);
      return "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"" + esc(part(i)) +
             "\"}]}}";
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
  for (int i = 0; i < kParts; i++)
    if (body.find(firstToken(i)) != std::string::npos) in.push_back(i);
  return in;
}

}  // namespace inturn_replay
