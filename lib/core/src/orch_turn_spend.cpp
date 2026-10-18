#include "nimbus/orch/turn_spend.h"

#include "nimbus/orch/model_price_defaults.h"

#include <cstdio>
#include <cstring>

namespace nimbus {
namespace orch {

namespace {

uint32_t addSat32(uint32_t a, uint32_t b) { return a > UINT32_MAX - b ? UINT32_MAX : a + b; }
uint16_t addSat16(uint16_t a, uint16_t b) {
  return (uint32_t)a + b > 0xFFFFu ? (uint16_t)0xFFFFu : (uint16_t)(a + b);
}

void addInto(SpendSlot& s, const TokenUsage& u) {
  s.in = addSat32(s.in, u.promptTokens);
  s.out = addSat32(s.out, u.completionTokens);
  s.cacheR = addSat32(s.cacheR, u.cacheReadTokens);
  s.cacheW = addSat32(s.cacheW, u.cacheWriteTokens);
  s.billed = addSat16(s.billed, u.responses);
}

void copyBounded(char* dst, size_t cap, const char* src) {
  std::snprintf(dst, cap, "%s", src ? src : "");
}

}  // namespace

void TurnSpend::clear() {
  for (SpendSlot& s : slot) s = SpendSlot();
  n = 0;
}

void TurnSpend::add(const char* host, const char* configuredModel, const TokenUsage& use) {
  if (!host || !*host) return;
  const bool billed = use.responses || use.promptTokens || use.completionTokens ||
                      use.cacheReadTokens || use.cacheWriteTokens;
  if (!billed) return;
  char key[kModelKeyCap];
  usageModelKey(host, configuredModel ? configuredModel : "", use.servedModel.c_str(), key);
  // A request over the model's size tier bills at that tier's row (the whole of this
  // host's spend goes there: priced high, never low).
  char longKey[kModelKeyCap];
  if (builtinLongTierKey(host, key, use.maxRequestIn, longKey)) std::memcpy(key, longKey, sizeof(key));
  char h[kProvKeyCap];
  copyBounded(h, sizeof(h), host);
  SpendSlot* sameHost = nullptr;
  for (size_t i = 0; i < n; ++i) {
    if (std::strcmp(slot[i].host, h) != 0) continue;
    if (std::strcmp(slot[i].model, key) == 0) { addInto(slot[i], use); return; }
    if (!sameHost) sameHost = &slot[i];
  }
  if (n == kSlots && sameHost) {   // full: this host's own slot takes it
    addInto(*sameHost, use);
    return;
  }
  if (n == kSlots) freeOneSlot();   // a new host: fold another host's second model
  if (n < kSlots) {
    SpendSlot& s = slot[n++];
    copyBounded(s.host, sizeof(s.host), h);
    copyBounded(s.model, sizeof(s.model), key);
    addInto(s, use);
    return;
  }
  // Four hosts in one turn (the fabric and the ladder both stop at three): the last
  // slot is the guard for a caller that broke that rule - tokens are never dropped.
  addInto(slot[kSlots - 1], use);
}

void TurnSpend::freeOneSlot() {
  for (size_t i = 0; i < n; ++i)
    for (size_t j = i + 1; j < n; ++j) {
      if (std::strcmp(slot[i].host, slot[j].host) != 0) continue;
      // Slot j's tokens join slot i (that model's precision is lost, never a token).
      SpendSlot& a = slot[i];
      const SpendSlot& b = slot[j];
      a.in = addSat32(a.in, b.in);
      a.out = addSat32(a.out, b.out);
      a.cacheR = addSat32(a.cacheR, b.cacheR);
      a.cacheW = addSat32(a.cacheW, b.cacheW);
      a.billed = addSat16(a.billed, b.billed);
      for (size_t k = j + 1; k < n; ++k) slot[k - 1] = slot[k];
      slot[--n] = SpendSlot();
      return;
    }
}

TokenUsage TurnSpend::total() const {
  TokenUsage t;
  for (size_t i = 0; i < n; ++i) {
    t.promptTokens = addSat32(t.promptTokens, slot[i].in);
    t.completionTokens = addSat32(t.completionTokens, slot[i].out);
    t.cacheReadTokens = addSat32(t.cacheReadTokens, slot[i].cacheR);
    t.cacheWriteTokens = addSat32(t.cacheWriteTokens, slot[i].cacheW);
    t.responses = addSat16(t.responses, slot[i].billed);
  }
  if (const SpendSlot* last = lastBilled()) t.servedModel = last->model;
  return t;
}

const SpendSlot* TurnSpend::lastBilled() const {
  for (size_t i = n; i-- > 0;)
    if (!slot[i].empty()) return &slot[i];
  return nullptr;
}

SpendSlot TurnSpend::hostTotal(const char* host) const {
  SpendSlot s;
  if (!host) return s;
  copyBounded(s.host, sizeof(s.host), host);
  for (size_t i = 0; i < n; ++i) {
    if (std::strcmp(slot[i].host, s.host) != 0) continue;
    s.in = addSat32(s.in, slot[i].in);
    s.out = addSat32(s.out, slot[i].out);
    s.cacheR = addSat32(s.cacheR, slot[i].cacheR);
    s.cacheW = addSat32(s.cacheW, slot[i].cacheW);
    s.billed = addSat16(s.billed, slot[i].billed);
  }
  return s;
}

size_t spendLine(const TurnSpend& s, char* out, size_t cap) {
  if (!out || !cap) return 0;
  out[0] = 0;
  if (s.empty()) {
    copyBounded(out, cap, "(none)");
    return std::strlen(out);
  }
  size_t off = 0;
  for (size_t i = 0; i < s.n && off + 1 < cap; ++i) {
    const SpendSlot& x = s.slot[i];
    const int w = std::snprintf(out + off, cap - off, "%s%s/%s in=%u out=%u cR=%u cW=%u n=%u",
                                i ? " | " : "", x.host, x.model[0] ? x.model : "-",
                                (unsigned)x.in, (unsigned)x.out, (unsigned)x.cacheR,
                                (unsigned)x.cacheW, (unsigned)x.billed);
    if (w < 0) break;
    off += (size_t)w;
  }
  if (off >= cap) off = cap - 1;
  out[off] = 0;
  return off;
}

}  // namespace orch
}  // namespace nimbus
