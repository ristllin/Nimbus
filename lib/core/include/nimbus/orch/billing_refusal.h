#pragma once
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

// billing_refusal - ONE table for "the provider refused because the account is out
// of credit or quota", shared by every paid seam (the chat head's Billing step
// error, embeddings, the caption step, spawns, speech, the download scan) through
// the one provider park, so a new provider token is added once for all of them.
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
//     credits_or_subscription_required, 4xx funding_cap_reached, and its monthly
//     cap as 429 ceiling_reached.
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
    "ceiling_reached",   // the Cumulo router's monthly cap (HTTP 429)
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
// meets the refusal (chat turns, embeddings, the caption step, spawns, speech, the
// download scan): the first seam to see it sends the notice, the rest stay quiet
// until the episode ends (a request at that provider succeeds again, or its key
// changes). The latch lives in the one provider park (provider_park.h), which keeps
// it on flash, so a restart no longer re-sends the notice.
// True exactly once per episode: the caller that gets true sends the notice.
bool billingNoticeDue(const char* provider);
// The episode is over: the next refusal at this provider notices again.
void billingEpisodeEnd(const char* provider);

// What a turn refused for billing gets (a refusal at the provider, or a turn the park
// answered with no request because every host it could use is parked for billing):
// the notice the first time the episode reaches the owner, then one short answer to
// the owner's next message, then nothing for the rest of the episode. A turn the
// owner did not write (a routine, a wakeup) may carry the notice but never takes the
// short answer: that is kept for the owner's own next message. Takes the latches
// (shared with every seam, kept across a restart); the caller sends what it says.
enum class BillingTurnAnswer : uint8_t { Notice, Short, Nothing };
BillingTurnAnswer billingTurnAnswer(const char* provider, bool ownerWrote);

// The short answer: no cause and no next step again (the notice said both), only
// that the message went unanswered and when the provider is asked again.
// `provider` is the display name, `recheckInMs` the wait (0 = not known).
inline std::string billingShortAnswer(const std::string& provider, uint32_t recheckInMs) {
  if (recheckInMs == 0)
    return "Your message was not answered. Replies resume when " + provider + " accepts requests again.";
  const uint32_t min = (recheckInMs + 59999u) / 60000u;
  const std::string when = min <= 1 ? std::string("in a minute") : "in " + std::to_string(min) + " minutes";
  return "Your message was not answered. " + provider + " is checked again " + when +
         ", and replies resume when it accepts requests.";
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
