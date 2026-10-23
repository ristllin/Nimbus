#pragma once
#include <cstddef>
#include <cstdint>

#include "nimbus/orch/model_key.h"
#include "nimbus/orch/model_ledger.h"   // kProvKeyCap, TokenDelta
#include "nimbus/orch/token_usage.h"

// turn_spend - who billed a turn, filed at the moment each provider answered.
//
// A turn used to collect its spend in ONE TokenUsage that every provider step added
// into, and the engine filed the sum under whichever host was current when the turn
// ended. A turn that switched provider booked every earlier host's rounds (and their
// cache writes) under the last host and its model; a failed turn booked nothing; a
// model changed during the turn relabelled it (usage RCA, 18 Oct).
//
// TurnSpend is a fixed table of per-(host, model) slots. Each host's billed usage is
// added under the model key computed from the model configured at that moment and
// that host's own served echo (usageModelKey), so nothing current at record time can
// relabel it. Counts keep each provider's NATIVE meaning (Anthropic excludes cache
// traffic from input_tokens, OpenAI includes it); ledgerDelta (usage_ledger.h) is the
// one place that turns a slot into ledger tokens. Fixed size, no heap, portable
// (NO Arduino), host-tested.

namespace nimbus {
namespace orch {

struct SpendSlot {
  char     host[kProvKeyCap] = {0};
  char     model[kModelKeyCap] = {0};   // usageModelKey at bill time ("" = provider level)
  uint32_t in = 0, out = 0, cacheR = 0, cacheW = 0;   // provider-native counts
  uint32_t cacheW1h = 0;   // the 1-hour-TTL part of cacheW (Anthropic; 0 elsewhere)
  uint16_t billed = 0;                  // billed responses (HTTP 200s with usage)
  bool empty() const { return !billed && !in && !out && !cacheR && !cacheW; }
};

struct TurnSpend {
  // The fabric speaks at most three hosts and the legacy ladder tries the head plus
  // two alternates: three hosts, so a fourth slot holds a second model of one host.
  static constexpr size_t kSlots = 4;
  SpendSlot slot[kSlots];
  uint8_t   n = 0;

  void clear();
  // File one host's billed usage under (host, usageModelKey(host, configuredModel,
  // use.servedModel)). Nothing billed (no response, no tokens) files nothing, so no
  // slot exists for a host that billed nothing. When the table is full, a new model of
  // a host with a slot merges into that slot, and a new host gets the slot of another
  // host's second model (folded into that host's first): model precision is lost,
  // never tokens, and spend never merges across hosts (up to kSlots hosts).
  void add(const char* host, const char* configuredModel, const TokenUsage& use);
  // The display / back-compat sum of every slot (what a turn's TokenUsage was):
  // servedModel is the model key of the last slot that billed.
  TokenUsage total() const;
  // The last slot that billed (the model that produced the final answer, or on a
  // failed turn the last one that billed), or nullptr when nothing billed.
  const SpendSlot* lastBilled() const;
  // Slots of `host`'s spend summed (nullptr-safe: all zero when none).
  SpendSlot hostTotal(const char* host) const;
  size_t billedSlots() const { return n; }
  bool   empty() const { return n == 0; }

 private:
  void freeOneSlot();
};

// One log-ready line for every slot, at most `cap` bytes into `out`:
//   "anthropic/claude-haiku-5-5 in=816 out=250 cR=9743 cW=0 n=1 | mistral/... "
// "(none)" when nothing billed. Returns strlen(out).
size_t spendLine(const TurnSpend& s, char* out, size_t cap);

}  // namespace orch
}  // namespace nimbus
