#include "dream_scenarios.h"

#include <chrono>
#include <cstdio>

#include "lab_wire.h"   // isProviderHost - count real provider calls
#include "nimbus/harness/dream.h"
#include "nimbus/harness/fault_transport.h"

namespace lab {

namespace dream = agent::dream;

namespace {

uint32_t nowHours() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::hours>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

dream::NightClock nowClock() {
  dream::NightClock c;
  c.epoch = (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
  c.localDay = (uint32_t)(c.epoch / 86400u);
  c.minuteOfDay = dream::kDefaultMinuteOfDay;   // 03:30: inside the night window
  c.synced = true;
  return c;
}

std::string lowerCopy(std::string s) {
  for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
  return s;
}
bool contains(const std::string& hay, const char* needle) {
  return lowerCopy(hay).find(lowerCopy(needle)) != std::string::npos;
}

int providerCalls(LabRig& rig) {
  int n = 0;
  for (const auto& call : rig.http().calls)
    if (isProviderHost(call.host) && call.path.find("/embeddings") == std::string::npos) n++;
  return n;
}

// A synthetic day (no owner data in the repo): 12 owner rows carrying three durable
// facts, greetings and noise, and long assistant rows.
void seedDay(LabRig& rig) {
  const uint32_t h = nowHours();
  const char* owner[] = {
      "morning!",
      "I've switched from cappuccino to flat white, by the way.",
      "what's the weather like?",
      "thanks",
      "Remember that my sister Anna's birthday is on 3 March.",
      "can you remind me what we said about the router?",
      "ok never mind",
      "I work from home on Fridays now, so plan around that.",
      "lol",
      "good night",
      "one more thing: nothing",
      "bye"};
  for (int i = 0; i < 12; ++i) {
    orch::EpisodicMessage u;
    u.sessionId = "lab";
    u.kind = orch::MsgKind::Message;
    u.role = "user";
    u.text = owner[i];
    u.tsHours = h - 10 + (uint32_t)(i / 2);
    u.id = "u" + std::to_string(i);
    rig.episodic().addMessage(u);
    orch::EpisodicMessage a = u;
    a.role = "assistant";
    a.id = "a" + std::to_string(i);
    a.text = std::string("Here is a long and friendly answer about that, with some detail that "
                         "does not need remembering. ") + std::string(200, '.');
    rig.episodic().addMessage(a);
  }
}

struct LabNight {
  dream::DreamState state;
  bool have = false;
  dream::NightDeps deps(LabRig& rig, const std::vector<std::string>& sessions) {
    dream::NightDeps d;
    d.loadState = [this](dream::DreamState& s) {
      s = state;
      return have;
    };
    d.saveState = [this](const dream::DreamState& s) {
      state = s;
      have = true;
      return true;
    };
    d.maintain = [&rig] {
      return dream::maintainVectors(rig.vectors(), 0.95f, nowHours(), 1500, [] { return true; });
    };
    d.yesterday = [&rig, sessions] { return dream::readYesterday(rig.episodic(), nowHours(), sessions); };
    d.scratchBlock = [&rig] {
      std::string out;
      rig.scratchpad().appendPromptBlock(out);
      return out;
    };
    d.scratchHash = [&rig] { return orch::fnv64(rig.scratchpad().serialize()); };
    d.stats = [&rig] {
      dream::MemStats m;
      m.vectors = rig.vectors().size();
      m.episodicMsgs = (int)rig.episodic().messageCount();
      return m;
    };
    d.embeddingsUp = [&rig] {
      return rig.options().embeddings && rig.hostAvailable(rig.options().embedHost);
    };
    d.memOps = [&rig] { return rig.memOps(); };
    d.injectTurn = [&rig](const std::string& inputs) {
      return rig.scheduled("lab", inputs, "Dream", /*quietOk=*/true, dream::kLoopId);
    };
    return d;
  }
};

void report(LabRig& rig, const dream::NightResult& r, const Note& note) {
  note("row: " + dream::renderDreamRow(r.report));
  if (!rig.turns().empty())
    for (const auto& d : rig.turns().back().deliveries) note("delivered: " + d.substr(0, 200));
  note("outcome " + std::string(dream::outcomeSlug(r.report.outcome)) +
       (r.report.cause.empty() ? "" : " (" + r.report.cause + ")") + ", tokens " +
       std::to_string(r.report.tokens));
}

bool onlyMemoryTools(const TurnRecord& t) {
  for (const auto& c : t.toolCalls)
    if (c.rfind("memory_", 0) != 0 && c.rfind("memory.", 0) != 0) return false;
  return true;
}

}  // namespace

void dreamDistill(LabRig& rig, const Check& check, const Note& note) {
  seedDay(rig);
  rig.callTool("memory.write", R"({"content":"Roy drinks a cappuccino every morning."})");
  rig.callTool("memory.write", R"({"content":"Roy prefers trains to driving."})");
  rig.callTool("memory.write", R"({"content":"Roy would rather take the train than drive."})");
  LabNight n;
  const dream::NightResult r = dream::runNight(n.deps(rig, {"lab"}), nowClock(), false);
  report(rig, r, note);
  const TurnRecord& t = rig.turns().back();
  for (const auto& c : t.toolCalls) note("tool: " + c.substr(0, 140));
  check(r.report.outcome == dream::DreamOutcome::Ran, "the night ran and every memory write landed");
  check(r.report.ops.added + r.report.ops.updated >= 2, "at least two durable facts were stored");
  check(t.deliveries.empty(), "nothing was delivered to the owner");
  check(r.report.tokens > 0, "the reflection reports its own spend");
  check(onlyMemoryTools(t), "the dream called memory tools only");
  const std::string sister = rig.callTool("memory.search", R"({"query":"sister birthday","n_results":3})");
  check(contains(sister, "march") || contains(sister, "anna"), "memory.search finds the sister's birthday");
  const std::string coffee = rig.callTool("memory.search", R"({"query":"what coffee does Roy drink","n_results":5})");
  check(contains(coffee, "flat white"), "the coffee fact now says flat white");
  bool stale = false;
  for (const auto& e : rig.vectors().getAll())
    if (contains(e.content, "cappuccino every morning")) stale = true;
  // L-1 (the primary head) asserts the supersede; the parity legs (L-5) report it:
  // without the stored facts in its prompt (merge candidates are wave-2 work) a
  // model may store the new fact beside the old one.
  const bool primary = rig.options().priority.rfind("anthropic", 0) == 0;
  if (primary)
    check(!stale, "no live memory still says cappuccino every morning (updated, not duplicated)");
  else
    note(std::string("supersede (reported on parity legs): ") +
         (stale ? "the stale cappuccino memory is still live" : "the stale memory was replaced"));
  // The paraphrase pair: merged, or both kept (logged so a provider that never
  // merges shows up across runs).
  int trains = 0;
  for (const auto& e : rig.vectors().getAll())
    if (contains(e.content, "train")) trains++;
  note("train paraphrases still live: " + std::to_string(trains));
  check(dream::renderDreamRow(r.report).rfind("Nightly dream ok: ", 0) == 0, "the row says ok with counts");
}

void dreamQuiet(LabRig& rig, const Check& check, const Note& note) {
  LabNight n;
  n.have = true;
  n.state.scrHash = orch::fnv64(rig.scratchpad().serialize());   // unchanged since the last dream
  const int before = providerCalls(rig);
  const dream::NightResult r = dream::runNight(n.deps(rig, {"lab"}), nowClock(), false);
  report(rig, r, note);
  check(r.report.outcome == dream::DreamOutcome::Skipped, "a quiet night is skipped");
  check(providerCalls(rig) == before, "zero chat calls on a quiet night");
}

void dreamBadKey(LabRig& base, const Check& check, const Note& note) {
  LabRig::Options o = base.options();
  o.priority = "anthropic";   // one host, so the failure is attributable
  agent::FaultRule f;
  std::string why;
  agent::parseFaultRule(
      "anthropic:*:401:body={\"type\":\"error\",\"error\":{\"type\":\"authentication_error\","
      "\"message\":\"invalid x-api-key\"}}",
      f, why);
  o.faults.push_back(f);
  LabRig rig(base.env(), o);
  seedDay(rig);
  LabNight n;
  const dream::NightResult r = dream::runNight(n.deps(rig, {"lab"}), nowClock(), false);
  report(rig, r, note);
  check(r.report.outcome == dream::DreamOutcome::Failed, "a refused key fails the night");
  check(r.report.cause == "key", "the cause is the key");
  check(r.fire.retryAfterSec == 0, "a key failure does not retry tonight");
  check(rig.turns().back().deliveries.empty(), "nothing was delivered at night");
  check(r.report.tokens == 0, "no spend is reported");
  check(providerCalls(rig) == 0, "no real provider call was made (faulted)");
}

void dreamNoEmbed(LabRig& base, const Check& check, const Note& note) {
  LabRig::Options o = base.options();
  o.embeddings = false;
  LabRig rig(base.env(), o);
  seedDay(rig);
  LabNight n;
  const dream::NightResult r = dream::runNight(n.deps(rig, {"lab"}), nowClock(), false);
  report(rig, r, note);
  const TurnRecord& t = rig.turns().back();
  check(r.report.outcome == dream::DreamOutcome::Degraded, "the night is degraded, not ok");
  check(contains(t.userText, "memory writes are unavailable tonight"), "the turn was told memory is down");
  bool searched = false;
  for (const auto& c : t.toolCalls)
    if (c.rfind("memory_search", 0) == 0 || c.rfind("memory.search", 0) == 0) searched = true;
  check(!searched, "the turn made no memory.search call");
  check(t.deliveries.empty(), "nothing was delivered");
}

}  // namespace lab
