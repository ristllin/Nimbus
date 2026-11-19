#include "nimbus/orch/head_qual.h"

#include <cstring>
#include <set>

namespace nimbus {
namespace orch {
namespace headqual {

namespace {

constexpr State Q = State::Qualified;
constexpr State A = State::Assumed;
constexpr State H = State::Held;

// One row per line, in this exact shape: tools/make_manifest.py --hold-line parses
// these HQ_ROW lines to render a manifest's holdHeads, and tools/test_make_manifest.py
// pins its rendering to the same golden as test/test_head_qual. Keep each row on one
// line. Within a host, specific families come before the host's "*" row. A family is
// the exact id plus its dated forms ("<id>-*"), never a bare prefix that would also
// match a different model ("claude-sonnet-5-5*" would take claude-sonnet-5-50).
#define HQ_ROW(host, family, wire, e1, e2, truth) Row{host, family, Wire::wire, e1, e2, Truth::truth}

// Initial rows (PRD section 9; ruling 9 for the truth modes). Anthropic Sonnet and
// Haiku are qualified on e1 by v5.2.0's own parity run (RB-R1 flips a row back if it
// fails); OpenAI, Z.ai and routed heads are assumed until their reduced parity run
// (RB-R1 for e1, RB-W2 to RB-W4 for e2); Mistral and custom are held. Nothing is
// qualified on e2 before its parity run.
const Row kRows[] = {
    HQ_ROW("anthropic", "claude-sonnet-5-5", AnthropicMessages, Q, A, Enforce),
    HQ_ROW("anthropic", "claude-sonnet-5-5-*", AnthropicMessages, Q, A, Enforce),
    HQ_ROW("anthropic", "claude-haiku-5-5", AnthropicMessages, Q, A, Enforce),
    HQ_ROW("anthropic", "claude-haiku-5-5-*", AnthropicMessages, Q, A, Enforce),
    HQ_ROW("anthropic", "claude-sonnet-5", AnthropicMessages, A, A, Enforce),
    HQ_ROW("anthropic", "*", AnthropicMessages, H, H, Enforce),
    HQ_ROW("openai", "gpt-5.6-luna*", OpenAiResponses, A, A, ReceiptOnly),
    HQ_ROW("openai", "gpt-5.6*", OpenAiResponses, A, A, ReceiptOnly),
    HQ_ROW("openai", "*", OpenAiResponses, H, H, ReceiptOnly),
    HQ_ROW("zai", "glm-5.3*", ChatCompat, A, A, ReceiptOnly),
    HQ_ROW("zai", "*", ChatCompat, H, H, ReceiptOnly),
    HQ_ROW("cumulo", "*", Router, A, A, ReceiptOnly),
    HQ_ROW("mistral", "*", MistralChat, H, H, ReceiptOnly),
    HQ_ROW("custom", "*", ChatCompat, H, H, ReceiptOnly),
};

#undef HQ_ROW

// The row every unknown head gets: held everywhere, receipt-only.
const Row kUnknown{"*", "*", Wire::Any, H, H, Truth::ReceiptOnly};

State stateOn(const Row& r, Line l) { return l == Line::E1 ? r.e1 : r.e2; }

bool hostIs(const Row& r, const char* host) { return host && std::strcmp(r.host, host) == 0; }

}  // namespace

const char* lineSlug(Line l) { return l == Line::E1 ? "e1" : "e2"; }

bool lineFromSlug(const char* s, Line& out) {
  if (!s) return false;
  if (!std::strcmp(s, "e1")) { out = Line::E1; return true; }
  if (!std::strcmp(s, "e2")) { out = Line::E2; return true; }
  return false;
}

const char* stateSlug(State s) {
  switch (s) {
    case State::Qualified: return "qualified";
    case State::Assumed:   return "assumed";
    case State::Held:      return "held";
  }
  return "held";
}

const char* wireSlug(Wire w) {
  switch (w) {
    case Wire::Any:               return "any";
    case Wire::AnthropicMessages: return "anthropic-messages";
    case Wire::OpenAiResponses:   return "openai-responses";
    case Wire::MistralChat:       return "mistral-chat";
    case Wire::ChatCompat:        return "chat-compat";
    case Wire::Router:            return "router";
  }
  return "any";
}

Wire wireForHost(const char* host) {
  if (!host) return Wire::Any;
  for (const Row& r : kRows)
    if (hostIs(r, host)) return r.wire;
  return Wire::Any;
}

const char* truthSlug(Truth t) {
  switch (t) {
    case Truth::ReceiptOnly: return "receipt-only";
    case Truth::Enforce:     return "enforce";
    case Truth::Shadow:      return "shadow";
  }
  return "receipt-only";
}

const Row* rows(size_t& count) {
  count = sizeof(kRows) / sizeof(kRows[0]);
  return kRows;
}

static const Row& matchRow(const char* host, const char* model, Wire wire) {
  for (const Row& r : kRows) {
    if (!hostIs(r, host)) continue;
    if (wire != Wire::Any && r.wire != wire) continue;
    if (patternMatch(r.family, model)) return r;
  }
  return kUnknown;
}

Verdict lookup(const char* host, const char* model, Line line, Wire wire) {
  const Row& r = matchRow(host, model, wire);
  // The state decides which engine answers and the truth mode, never the window (ruling 83 item 6):
  // e1 keeps every head's v5.1.2 budget, e2 gives every head its full budget (tx::budgetFor).
  return Verdict{&r, stateOn(r, line), r.truth};
}

std::string renderHoldHeads(Line line) {
  // "*/*" first holds every head; the rows then follow in REVERSE match order, so
  // with later entries winning the table's first match decides. A hold entry is left
  // out while nothing on its host has been excepted yet: everything there is still
  // held by "*/*", so it could change no verdict.
  std::string out = "[\"*/*\"";
  std::set<std::string> excepted;
  const size_t n = sizeof(kRows) / sizeof(kRows[0]);
  for (size_t i = n; i-- > 0;) {
    const Row& r = kRows[i];
    const bool qualified = !holds(stateOn(r, line));
    if (!qualified && !excepted.count(r.host)) continue;
    if (qualified) excepted.insert(r.host);
    out += ",\"";
    if (qualified) out += '!';
    out += r.host;
    out += '/';
    out += r.family;
    out += '"';
  }
  out += ']';
  return out;
}

}  // namespace headqual
}  // namespace orch
}  // namespace nimbus
