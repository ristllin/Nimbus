// test_fold_floor - lane RB-BGb (ruling 83 item 9 (a)) on the engine the Virtual Nimbus links:
// after a request sent at the 85,000 B send floor (a fragmented DMA pool, the device link) the
// automatic fold waits for a request above the floor, and the fold that follows keeps the head's
// full keep (59,040 B, about 23,600 tokens on claude-sonnet-5-5). On v5.1.2 it kept v5.1.1's
// 4,096 B (about 1,600 tokens). A Summarize now is never deferred. Offline: the host answers
// from a script, no keys, no provider.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "../../test/support/fake_config.h"
#include "../../test/support/fake_platform.h"
#include "nimbus/harness/engine.h"
#include "nimbus/orch/budget.h"
#include "nimbus/orch/body_budget.h"
#include "nimbus/orch/compact.h"
#include "nimbus/orch/request_budget.h"
#include "test_util.h"

namespace orch = nimbus::orch;

namespace {

const char kChat[] = "1001";

std::string idOf(uint32_t sfx) {
  char b[16];
  std::snprintf(b, sizeof(b), "m%08x", (unsigned)sfx);
  return b;
}

// Rows of one chat in id order; a before read newest first, an after read oldest first.
struct Store {
  std::vector<orch::EpisodicMessage> rows;
  uint32_t seq = 0;
  std::string add(const std::string& chat, const std::string& role, const std::string& text, const std::string& tags) {
    orch::EpisodicMessage m;
    m.id = idOf(++seq);
    m.sessionId = chat;
    m.role = role;
    m.kind = orch::MsgKind::Message;
    m.text = text;
    m.tags = tags;
    m.tsHours = 498552 + seq / 40;
    rows.push_back(m);
    return m.id;
  }
  std::vector<orch::EpisodicMessage> read(const std::string& chat, const agent::HistoryQuery& q) const {
    std::vector<orch::EpisodicMessage> out;
    if (!q.after.empty()) {
      if (q.after.find(':') != std::string::npos) {   // a byte cursor on a complete index
        if (q.info) q.info->cursorStale = true;
        return out;
      }
      const uint32_t from = orch::epiIdSuffix(q.after);
      for (const auto& m : rows)
        if (orch::epiIdSuffix(m.id) > from && m.sessionId == chat && out.size() < q.limit) out.push_back(m);
      return out;
    }
    const uint32_t before = q.before.empty() ? UINT32_MAX : orch::epiIdSuffix(q.before);
    for (auto it = rows.rbegin(); it != rows.rend() && out.size() < q.limit; ++it)
      if (orch::epiIdSuffix(it->id) < before && it->sessionId == chat) out.push_back(*it);
    return out;
  }
};

struct BlobIO : orch::FoldStoreIO {
  std::string blob;
  std::string load() override { return blob; }
  void save(const std::string& b) override { blob = b; }
};

struct Rig {
  harness_test::FakePlatform plat;
  harness_test::FakeConfig cfg;
  Store store;
  BlobIO blob;
  orch::FoldStore folds;
  std::unique_ptr<agent::TurnEngine> eng;
  bool floorPool = false;
  uint32_t token = 0;

  Rig() {
    cfg.orchHost = "anthropic";
    cfg.orchModels["anthropic"] = "claude-sonnet-5-5";
    cfg.keyed.insert("anthropic");
    cfg.priority = "anthropic";
    plat.epoch = 1794787200;
    folds.begin(&blob);
    agent::TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    d.deliver = [](const std::string&, const std::string&) {};
    d.composeInputs = [](const std::string&) {
      agent::ComposeInputs in;
      in.devName = "Nimbus";
      for (size_t i = 0; in.runningMemory.size() < 24000; i++)
        in.runningMemory += "- note " + std::to_string(i) + ": the owner tracks meals, walks and the garden.\n";
      return in;
    };
    d.apply.deliver = d.deliver;
    d.apply.principalFor = [](const std::string& c) { return orch::principalForRole(c, orch::Role::Admin); };
    d.apply.captureAssistant = [this](const std::string& c, const std::string& t) {
      store.add(c, "assistant", t, "out:reply");
      folds.noteMessage(c, t.size());
    };
    d.hosts.add("anthropic", [this](std::string&, const std::string& ins, const std::string& in, std::string& out,
                                    std::string& err, const agent::HeadTools*, orch::TokenUsage* u) {
      orch::noteRoundBudget(floorPool);   // as the wire does for every round (wire_budget.cpp)
      if (floorPool && ins.size() + in.size() > orch::kLegacyWireBodyBytes) {
        err = orch::bodyBudgetError(orch::kLegacyWireBodyBytes, ins.size() + in.size());
        return false;
      }
      if (u) u->responses = 1;
      std::string reply = "T" + std::to_string(++token) + ". ";
      while (reply.size() < 2400) reply += "Noted, and here is a longer answer about the day. ";
      out = "{\"reply\":\"" + reply + "\",\"memory\":\"\",\"ask\":\"\"}";
      return true;
    });
    d.histV2 = [] { return true; };
    d.captureUserRow = [this](const std::string& c, const std::string& t, const std::string& tags) {
      const std::string id = store.add(c, "user", t, tags);
      folds.noteOwnerMessage(c, t);
      return id;
    };
    d.historyRows = [this](const std::string& c, const agent::HistoryQuery& q) { return store.read(c, q); };
    d.historyMarks = [this](const std::string& c) {
      const orch::ChatFold f = folds.get(c);
      return agent::HistoryMarks{f.thru, f.softStart, 0, f.lastFoldEpoch};
    };
    d.noteHistoryShift = [this](const std::string& c, const std::string& soft) { folds.setSoftStart(c, soft); };
    d.noteHistoryBytes = [this](const std::string& c, size_t bytes) { folds.noteHistoryBytes(c, bytes); };
    eng.reset(new agent::TurnEngine(std::move(d)));
  }
  void turn() {
    plat.ms += 1000;
    plat.epoch += 60;
    eng->handleMessage("T" + std::to_string(++token) + ". what about lunch, about " + std::string(150, 'x'), "Roy",
                       kChat);
  }
  agent::FoldSlice automatic() { return eng->historyFoldSlice(kChat, 24 * 1024, false); }
};

}  // namespace

int main() {
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY", "CUMULO_API_KEY", "Z_AI_TOKEN"})
    unsetenv(k);
  ndtest::Ctx t;
  orch::installBudgetLink(orch::BudgetLink::Device);   // the send floor exists on the device link only
  const size_t keep = orch::headContextBudget("anthropic", "claude-sonnet-5-5", false, {}).historyKeepBytes;
  t.eqi((long)keep, 59040, "Sonnet's full keep (about 23,600 tokens)");
  Rig r;
  for (int i = 0; i < 30; i++) r.turn();
  agent::FoldSlice s = r.automatic();
  t.ok(!s.digest.empty() && s.keptBytes + 8192 >= keep, "healthy: the automatic fold keeps the full keep (counter)");

  r.floorPool = true;
  for (int n = 1; n <= 6; n++) {   // any number of floor sends
    r.turn();
    s = r.automatic();
    const bool shortened = !s.digest.empty() && s.keptBytes + 8192 < keep;
    t.ok(!shortened, "floor send " + std::to_string(n) + ": no automatic fold with a shortened tail (kept " +
                         std::to_string(s.keptBytes) + " B)");
    t.ok(s.digest.empty() && s.state == agent::SliceState::Wait,
         "floor send " + std::to_string(n) + ": the automatic fold waits for a request above the floor");
  }
  const agent::FoldSlice manual = r.eng->historyFoldSlice(kChat, 24 * 1024, true);
  t.ok(!manual.digest.empty(), "a Summarize now at the floor is never deferred (counter)");

  r.floorPool = false;
  r.turn();
  s = r.automatic();
  t.ok(!s.digest.empty(), "a request above the floor ends the deferral");
  t.ok(s.keptBytes + 8192 >= keep, "the next fold keeps the full keep (kept " + std::to_string(s.keptBytes) + " B)");
  std::printf("\n%s: %d checks, %d failures\n", "test_fold_floor", t.checks, t.failures);
  return t.failures ? 1 : 0;
}
