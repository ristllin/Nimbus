// Guards scaled to the head, the spend of one request, identical-call suppression
// (plain/guards.h). Lane RB-E1b.
#include "nimbus/harness/plain/guards.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "nimbus/harness/plain/exchange.h"
#include "nimbus/harness/plain/texts.h"
#include "nimbus/orch/tool_groups.h"

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
  out = GuardLimits{};
  // A stored orchLoopRnds of 8 or less reads as 16 (the old engine's default was 8); 9 to 32
  // is honoured; above 32 is the ceiling.
  out.rounds = storedRounds <= 8 ? kDefaultRounds : (uint16_t)std::min<int>(storedRounds, kMaxRounds);
  out.softWallMs = kSoftWallMs;
  const uint64_t request = cost.requestTokensAtTrigger ? cost.requestTokensAtTrigger : kDefaultRequestTokens;
  out.maxPromptTokens = clamp32(std::max<uint64_t>(4ull * cost.targetTokens, 16ull * request));
  out.maxSpendMicroUsd =
      clamp32(std::max<uint64_t>(kMinSpendCapMicroUsd, 16ull * cost.warmRequestMicroUsd + cost.coldRequestMicroUsd));
  return Status::success();
}

Status checkGuards(const GuardLimits& limits, const GuardState& state, GuardTrip& out) {
  out = GuardTrip::None;
  if (!limits.rounds || !limits.softWallMs) return Status::fail(Err::InvalidArgument, "plain::checkGuards");
  if (state.rounds >= limits.rounds) out = GuardTrip::Rounds;
  else if ((uint64_t)state.elapsedMs + kMinRequestMs >= limits.softWallMs) out = GuardTrip::Time;
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
    std::string key;   // the tool it reached, then its arguments as parsed and written again
    uint64_t q = 0;    // its stored result
  };
  std::string chat;
  uint32_t turn = 0;
  std::vector<Seen> seen;
  const ToolCall* pending = nullptr;   // the pure call that was let run, until its result lands
  std::string pendingKey;

  static std::string keyOf(const ToolCall& c) {
    ArduinoJson::JsonDocument doc;
    std::string args;
    if (deserializeJson(doc, c.argsJson, ArduinoJson::DeserializationOption::NestingLimit(64))) args = c.argsJson;
    else serializeJson(doc, args);
    return c.name + '\n' + args;
  }

  void sync(const TurnCtx& t) {
    if (t.turn == turn && t.chatKey == chat) return;
    chat = t.chatKey;
    turn = t.turn;
    seen.clear();
    pending = nullptr;
  }

  DispatchDecision before(const DispatchCtx& ctx) {
    DispatchDecision d;
    pending = nullptr;
    if (!ctx.turn || !ctx.call) return d;
    sync(*ctx.turn);
    if (ctx.effect) {   // a write may change what an earlier read showed: nothing is answered after it
      seen.clear();
      return d;
    }
    if (!ctx.pure) return d;   // only declared-pure tools (never the web, the clock or the device)
    const ToolCall& target = ctx.target ? *ctx.target : *ctx.call;
    std::string key = keyOf(target);
    for (const Seen& s : seen)
      if (s.key == key && s.q > ctx.stubThrough) {   // shown whole in this request
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
    seen.push_back(Seen{std::move(pendingKey), ctx.q});
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
