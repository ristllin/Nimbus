#pragma once
#include <cctype>
#include <cstring>
#include <string>

// billing_refusal - ONE table for "the provider refused because the account is out
// of credit or quota", shared by the chat head (the fabric's Billing step error)
// and the embedding seam, so a new provider token is added once for both.
//
// A billing refusal is permanent until the owner adds credit or changes the key:
// retrying it on the same host costs a request every time and can never succeed.
// Lumi on 13-14 Oct: every turn sent an OpenAI 429 "no credits", a retry, an
// Anthropic 400 "credit balance is too low", a retry, then answered on Mistral.
//
// Recorded shapes (status, then the text or code that names billing):
//   - Anthropic: 400 invalid_request_error "Your credit balance is too low to
//     access the Anthropic API. Please go to Plans & Billing ...";
//   - OpenAI: 429 code "insufficient_quota" ("You exceeded your current quota,
//     please check your plan and billing details.") and the newer message
//     "You have no credits remaining. Add credits to continue ...";
//   - OpenAI embeddings: 429 code "credit_balance_exhausted";
//   - Mistral: 402 Payment Required; the Cumulo router: 402 with
//     insufficient_credits, insufficient_credits_for_call, budget_exceeded,
//     credits_or_subscription_required, and 4xx funding_cap_reached.
// Every 402 is billing whatever its body says.
namespace nimbus {
namespace orch {

// The text and code markers, matched case-insensitively anywhere in the provider's
// error message or code. Exposed so a test can walk every token.
inline constexpr const char* kBillingTokens[] = {
    "credit balance is too low",
    "insufficient_quota",
    "exceeded your current quota",
    "no credits remaining",
    "credit_balance_exhausted",
    "funding_cap_reached",
    "insufficient_credits",
    "budget_exceeded",
    "credits_or_subscription_required",
    "billing_hard_limit_reached",
};

namespace detail {
inline bool containsNoCase(const std::string& hay, const char* needle) {
  const size_t n = std::strlen(needle);
  if (n == 0 || hay.size() < n) return false;
  for (size_t i = 0; i + n <= hay.size(); i++) {
    size_t k = 0;
    while (k < n && std::tolower((unsigned char)hay[i + k]) == std::tolower((unsigned char)needle[k]))
      ++k;
    if (k == n) return true;
  }
  return false;
}
}  // namespace detail

// True when (status, text) is a billing refusal. `text` is anything the provider
// said: its error message, error code or type, or the device's whole error line
// ("resp HTTP 429: You have no credits remaining..."). status 0 = unknown.
inline bool isBillingRefusal(int status, const std::string& text) {
  if (status == 402) return true;
  if (status != 0 && status != 400 && status != 403 && status != 429) return false;
  for (const char* t : kBillingTokens)
    if (detail::containsNoCase(text, t)) return true;
  return false;
}

// One owner notice per billing episode per provider, shared by every seam that
// meets the refusal (chat turns, embeddings): the first seam to see it sends the
// notice, the rest stay quiet until the episode ends (a request at that provider
// succeeds again, or its key or model changes). RAM only - a restart re-arms it,
// which costs at most one more notice. Single-task by design (AGENTS.md), so a
// plain table needs no lock.
namespace detail {
struct BillingEpisodes {
  static constexpr size_t kMax = 6;
  char provider[kMax][16] = {};
  bool noticed[kMax] = {};
  int find(const char* p) const {
    for (size_t i = 0; i < kMax; i++)
      if (provider[i][0] && std::strncmp(provider[i], p, sizeof(provider[i]) - 1) == 0) return (int)i;
    return -1;
  }
};
inline BillingEpisodes& billingEpisodes() {
  static BillingEpisodes e;
  return e;
}
}  // namespace detail

// True exactly once per episode: the caller that gets true sends the notice.
inline bool billingNoticeDue(const char* provider) {
  detail::BillingEpisodes& e = detail::billingEpisodes();
  int i = e.find(provider);
  if (i < 0) {
    for (size_t k = 0; k < detail::BillingEpisodes::kMax && i < 0; k++)
      if (!e.provider[k][0]) i = (int)k;
    if (i < 0) i = 0;   // table full: reuse the first slot (bounded, never grows)
    std::strncpy(e.provider[i], provider, sizeof(e.provider[i]) - 1);
    e.provider[i][sizeof(e.provider[i]) - 1] = 0;
    e.noticed[i] = false;
  }
  if (e.noticed[i]) return false;
  e.noticed[i] = true;
  return true;
}

// The episode is over: the next refusal at this provider notices again.
inline void billingEpisodeEnd(const char* provider) {
  detail::BillingEpisodes& e = detail::billingEpisodes();
  const int i = e.find(provider);
  if (i >= 0) {
    e.provider[i][0] = 0;
    e.noticed[i] = false;
  }
}

// The owner-facing words for a billing refusal, plain and with the one next step.
// `next` is the provider the turn moves to ("" = none).
inline std::string billingNotice(const std::string& provider, int status, const std::string& next) {
  std::string s = provider + " says the account is out of credit";
  if (status > 0) s += " (HTTP " + std::to_string(status) + ")";
  s += ". Add credit on its billing page";
  if (!next.empty()) s += "; until then, " + next + " answers";
  s += ".";
  return s;
}

}  // namespace orch
}  // namespace nimbus
