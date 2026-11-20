// Guards scaled to the head, the spend of one request, identical-call suppression
// (plain/guards.h). Lane RB-E1b; the soft guards, the loop and the table at full lane RB-X3.
#include "nimbus/harness/plain/guards.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "nimbus/harness/plain/exchange.h"
#include "nimbus/harness/plain/texts.h"
#include "nimbus/orch/tool_groups.h"
#include "nimbus/orch/tx/budget.h"
#include "nimbus/orch/tx/item.h"

namespace agent {
namespace plain {

namespace {

uint32_t clamp32(uint64_t v) { return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v; }

// Tokens x (micro-USD per 1M tokens), floored, without an intermediate overflow.
uint64_t costOf(uint64_t tokens, uint64_t microPerM) {
  return (tokens / 1000000u) * microPerM + (tokens % 1000000u) * microPerM / 1000000u;
}

}  // namespace

Status guardLimitsFor(int storedRounds, const HeadCost& cost, GuardLimits& out) {
  return guardLimitsFor(storedRounds, cost, false, out);
}

namespace {

// The size and spend caps for a turn of `rounds` requests: a progressing turn of that
// many requests at the head's size fits, so only a runaway (requests far above the due mark,
// every round a cache miss) reaches them.
void capsFor(const HeadCost& cost, uint16_t rounds, GuardLimits& out) {
  const uint64_t request = cost.requestTokensAtTrigger ? cost.requestTokensAtTrigger : kDefaultRequestTokens;
  out.maxPromptTokens = clamp32(std::max<uint64_t>(4ull * cost.targetTokens, (uint64_t)rounds * request));
  out.maxSpendMicroUsd = clamp32(std::max<uint64_t>(
      kMinSpendCapMicroUsd, (uint64_t)rounds * cost.warmRequestMicroUsd + cost.coldRequestMicroUsd));
}

uint32_t fnv(uint32_t h, const std::string& s) {
  for (unsigned char ch : s) h = (h ^ ch) * 16777619u;
  return h;
}
uint32_t keyHash(const std::string& a, const std::string& b) {
  return fnv(fnv(2166136261u, a) ^ 0x9e3779b9u, b) | 1u;   // never 0
}

}  // namespace

Status guardLimitsFor(int storedRounds, const HeadCost& cost, bool attended, GuardLimits& out) {
  out = GuardLimits{};
  // A stored orchLoopRnds of 8 or less reads as 16 (the old engine's default was 8); 9 to 32 is
  // honoured; above 32 is the ceiling.
  out.rounds = storedRounds <= 8 ? kDefaultRounds : (uint16_t)std::min<int>(storedRounds, kMaxRounds);
  out.softWallMs = kSoftWallMs;
  out.attended = attended;
  // Unattended: today's limits (ruling 84 item 9). Attended: the soft figures are checked again
  // and the hard ones bound the turn (ruling 84 item 2).
  out.hardRounds = attended ? std::max<uint16_t>(kHardRounds, out.rounds) : out.rounds;
  out.timeLimitMs = attended ? kAttendedHardWallMs - kLastRoundReserveMs : kSoftWallMs;
  out.lastRoundWallMs = attended ? kAttendedHardWallMs : kHardWallMs;
  capsFor(cost, attended ? out.hardRounds : kDefaultRounds, out);   // unattended: today's 16 x
  return Status::success();
}

HeadCost guardCostFor(const nimbus::orch::tx::HeadBudget& budget, const HeadRates& rates) {
  HeadCost c;
  c.targetTokens = budget.tokens.aim;
  c.requestTokensAtTrigger = budget.tokens.due;
  c.inMicroPerM = rates.in;
  c.outMicroPerM = rates.out;
  c.cacheReadMicroPerM = rates.cacheRead;
  c.cacheWriteMicroPerM = rates.cacheWrite;
  const uint32_t p = budget.tokens.due;
  c.warmRequestMicroUsd = requestMicroUsd(c, RequestTokens{p / 10, kGuardOutputTokens, p - p / 10, 0});
  c.coldRequestMicroUsd = requestMicroUsd(c, RequestTokens{p, kGuardOutputTokens, 0, 0});
  return c;
}

// A call progresses unless suppression answered it or it failed exactly as its previous attempt.
void GuardProgress::noteCall(const std::string& tool, const std::string& args, bool suppressed, bool isError,
                             const std::string& result) {
  if (suppressed) return;
  const uint32_t key = keyHash(tool, args);
  const uint32_t outcome = isError ? keyHash(result, "") : 0;
  for (std::pair<uint32_t, uint32_t>& a : attempts) {
    if (a.first != key) continue;
    const bool same = outcome && a.second == outcome;
    a.second = outcome;
    if (same) return;
    roundProgressed = progressed = true;
    return;
  }
  attempts.emplace_back(key, outcome);
  roundProgressed = progressed = true;
}

void GuardProgress::noteText(const std::string& text) {
  if (text.empty()) return;
  const uint32_t h = keyHash(text, "");
  if (std::find(texts.begin(), texts.end(), h) != texts.end()) return;
  texts.push_back(h);
  progressed = true;
}

void GuardProgress::endRound() {
  idle = roundProgressed ? 0 : (uint8_t)std::min<int>(idle + 1, 255);
  roundProgressed = false;
}

SoftCheck checkSoftGuards(const GuardLimits& limits, const GuardState& state, GuardProgress& progress,
                          GuardTrip& which) {
  which = GuardTrip::None;
  if (!limits.attended || !limits.rounds || !limits.softWallMs) return SoftCheck::None;
  const uint16_t atRounds = progress.nextRounds ? progress.nextRounds : limits.rounds;
  const uint32_t atMs = progress.nextMs ? progress.nextMs : limits.softWallMs;
  const bool byRounds = state.rounds >= atRounds;
  const bool byTime = (uint64_t)state.elapsedMs + kMinRequestMs >= atMs;
  if (!byRounds && !byTime) return SoftCheck::None;
  which = byRounds ? GuardTrip::Rounds : GuardTrip::Time;
  if (!progress.progressed) return SoftCheck::Stop;
  progress.progressed = false;   // the next check asks for progress since this one
  if (byRounds) progress.nextRounds = (uint16_t)std::min<uint32_t>((uint32_t)atRounds + limits.rounds, UINT16_MAX);
  if (byTime) progress.nextMs = (uint32_t)std::min<uint64_t>((uint64_t)atMs + limits.softWallMs, UINT32_MAX);
  return SoftCheck::GoOn;
}

Status checkGuards(const GuardLimits& limits, const GuardState& state, GuardTrip& out) {
  out = GuardTrip::None;
  if (!limits.rounds || !limits.softWallMs || !limits.hardRounds || !limits.timeLimitMs)
    return Status::fail(Err::InvalidArgument, "plain::checkGuards");
  if (state.idleRounds >= kLoopRounds) out = GuardTrip::Loop;
  else if (state.rounds >= limits.hardRounds) out = GuardTrip::Rounds;
  else if ((uint64_t)state.elapsedMs + kMinRequestMs >= limits.timeLimitMs) out = GuardTrip::Time;
  else if (limits.maxSpendMicroUsd && state.spendMicroUsd >= limits.maxSpendMicroUsd) out = GuardTrip::Spend;
  else if (limits.maxPromptTokens && state.promptTokens >= limits.maxPromptTokens) out = GuardTrip::Size;
  return Status::success();
}

uint32_t requestMicroUsd(const HeadCost& cost, const RequestTokens& t) {
  const uint64_t cacheR = cost.cacheReadMicroPerM ? cost.cacheReadMicroPerM : cost.inMicroPerM / 10;
  const uint64_t cacheW = cost.cacheWriteMicroPerM ? cost.cacheWriteMicroPerM : cost.inMicroPerM * 5 / 4;
  const uint64_t sum = costOf(t.prompt, cost.inMicroPerM) + costOf(t.output, cost.outMicroPerM) +
                       costOf(t.cacheRead, cacheR) + costOf(t.cacheWrite, cacheW);
  return clamp32(sum);
}

Status isPureTool(const std::string& name, bool& out) {
  out = !name.empty() && nimbus::orch::toolClassFor(name).effect == nimbus::orch::EffectClass::Pure;
  return name.empty() ? Status::fail(Err::InvalidArgument, "plain::isPureTool") : Status::success();
}

namespace {

// Identical-call suppression: what one chat's turn has read so far. A pure call whose target
// and arguments match an earlier call of the same turn, while that call's result is still shown
// whole and no effect has been tried since, is answered with a pointer to it instead of running.
struct Suppression {
  struct Seen {
    std::string key;    // the tool it reached, then its arguments as parsed and written again
    uint64_t q = 0;     // its stored result
    bool bulk = false;  // over tier 1: ladder rung 2 may stub it once it is older than the newest two rounds
  };
  // One turn's attempt: a retry of the turn (same number, next attempt, a fresh open) starts clean.
  std::string chat;
  uint32_t turn = 0;
  uint8_t attempt = 0;
  uint32_t startedMs = 0;
  std::vector<Seen> seen;
  const ToolCall* pending = nullptr;   // the pure call that was let run, until its result lands
  std::string pendingKey;

  static std::string keyOf(const ToolCall& c, const ArduinoJson::JsonDocument& doc, bool parsed) {
    std::string args;
    if (!parsed) args = c.argsJson;
    else serializeJson(doc, args);
    return c.name + '\n' + args;
  }

  // Arguments that name a span measured back from now (since_hours, before_days, recent, ...): the same
  // call later in the turn may answer differently, so it is never answered from an earlier one.
  static bool relativeToNow(const ArduinoJson::JsonDocument& doc) {
    static const char* const kWords[] = {"since", "before", "after", "ago", "recent", "latest", "now",
                                         "minutes", "hours", "days", "today"};
    if (!doc.is<ArduinoJson::JsonObjectConst>()) return false;
    for (ArduinoJson::JsonPairConst kv : doc.as<ArduinoJson::JsonObjectConst>()) {
      const std::string k = kv.key().c_str();
      for (const char* w : kWords)
        if (k.find(w) != std::string::npos) return true;
    }
    return false;
  }

  void sync(const TurnCtx& t) {
    if (t.turn == turn && t.chatKey == chat && t.attempt == attempt && t.startedMs == startedMs) return;
    chat = t.chatKey;
    turn = t.turn;
    attempt = t.attempt;
    startedMs = t.startedMs;
    seen.clear();
    pending = nullptr;
  }

  DispatchDecision before(const DispatchCtx& ctx) {
    DispatchDecision d;
    pending = nullptr;
    if (!ctx.turn || !ctx.call) return d;
    sync(*ctx.turn);
    if (!ctx.pure) {   // a write, a poll, the clock, the web: the world may have moved, nothing earlier answers
      seen.clear();
      return d;
    }
    const ToolCall& target = ctx.target ? *ctx.target : *ctx.call;
    ArduinoJson::JsonDocument doc;
    const bool parsed = !deserializeJson(doc, target.argsJson, ArduinoJson::DeserializationOption::NestingLimit(64));
    if (parsed && relativeToNow(doc)) return d;
    std::string key = keyOf(target, doc, parsed);
    for (const Seen& s : seen)
      if (s.key == key && (!s.bulk || s.q > ctx.stubThrough)) {   // whole after this round's ladder step
        d.verdict = DispatchVerdict::Answer;
        d.text = texts::tool_results::kSameAsEarlier;
        return d;
      }
    pending = ctx.call;
    pendingKey = std::move(key);
    return d;
  }

  void after(const ResultCtx& ctx) {
    if (!pending || ctx.call != pending || !ctx.output) return;
    pending = nullptr;
    if (ctx.output->isError || ctx.output->outside) return;   // only a whole internal result answers a repeat
    seen.push_back(Seen{std::move(pendingKey), ctx.q, ctx.output->text.size() > nimbus::orch::tx::kTier1Receipt});
  }
};

}  // namespace

Status installSuppression(PlainHooks& hooks) {
  std::shared_ptr<Suppression> s = std::make_shared<Suppression>();
  Status st = hooks.beforeDispatch(HookSlot::Suppression, [s](const DispatchCtx& c) { return s->before(c); });
  if (!st.ok()) return st;
  return hooks.afterResult(HookSlot::Suppression, [s](const ResultCtx& c) { s->after(c); });
}

}  // namespace plain
}  // namespace agent
