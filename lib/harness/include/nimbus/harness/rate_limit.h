#pragma once
#include <cstdint>
#include <string>

#include "nimbus/harness/http.h"   // HeaderList

// rate_limit - WHY a provider answered HTTP 429, and what the owner should hear.
//
// "Wait a minute" was the only 429 reply, and it is wrong more often than right on
// a small plan (CUM-460): a key whose plan allows 0 requests per minute on an
// endpoint never clears by waiting, and a spent DAILY quota (Mistral Studio
// connectors: 50/day, reset at midnight UTC, measured live) does not clear for
// hours. The provider says which window refused, in its headers (Mistral's
// x-ratelimit-*) or in its error text (OpenAI / Anthropic), so the reply names it.
//
// The adapter that sees the headers classifies them and carries the result inside
// its error string as a tag ("[rl:day-utc]"); the engine, which only sees that
// string, reads the tag back (or the provider text the adapter quoted) and picks
// the reply. Pure functions, host-tested (test/test_rate_limit).
namespace agent {

enum class RateLimit : uint8_t {
  Unknown = 0,   // a 429 that does not say which window: no promise about when
  PerMinute,     // a per-minute request/token window: clears within a minute
  DailyUtc,      // a daily quota that resets at midnight UTC (Mistral connectors, measured)
  Daily,         // a daily quota, reset time not stated by the provider
  NotAllowed,    // the plan allows 0 of this request: waiting never helps
  Quota,         // the account's quota / credit / monthly allowance is spent
  kCount         // sentinel: every value below it has a slug and a reply
};

// Mistral quota headers ("x-ratelimit" prefix, names matched case-insensitively)
// -> the window that refused. Rules, in order: any window whose LIMIT is 0 ->
// NotAllowed; an exhausted (remaining 0) month window -> Quota; the exhausted
// Studio connector day window (custom-day, the one whose midnight-UTC reset was
// measured) -> DailyUtc; any other exhausted day window -> Daily; otherwise any
// minute window present -> PerMinute (a token window can refuse with tokens still
// remaining, when the request needs more than is left); else Unknown.
RateLimit rateLimitFromHeaders(const HeaderList& headers);

// Provider error TEXT -> window (OpenAI "requests per day (RPD)" /
// "insufficient_quota" / "Request too large" (can never pass: NotAllowed),
// Anthropic "... per minute", ...). Unknown when the text names no window.
RateLimit rateLimitFromText(const std::string& text);

// Machine slug for a kind ("minute", "day-utc", ...), and the tag an adapter
// appends to its error string ("" for Unknown - nothing to carry).
const char* rateLimitSlug(RateLimit k);
std::string rateLimitTag(RateLimit k);

// Engine side: the kind behind a failed turn's error string - the adapter's tag
// when present, else whatever the quoted provider text says.
RateLimit rateLimitFromError(const std::string& err);

// The owner-facing reply for a turn that failed on a 429 (device copy rules:
// printable ASCII, calm, what happened then the one next step).
const char* rateLimitReply(RateLimit k);

}  // namespace agent
