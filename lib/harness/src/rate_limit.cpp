#include "nimbus/harness/rate_limit.h"

#include <cctype>
#include <cstring>

namespace agent {

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool has(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}

// A header value that reads as the count 0 ("0", " 0", "00"); anything else,
// including a non-number, is not an exhausted window.
bool headerIsZero(const std::string& v) {
  const size_t b = v.find_first_not_of(' ');
  if (b == std::string::npos) return false;
  size_t e = v.find(' ', b);
  if (e == std::string::npos) e = v.size();
  return v.find_first_not_of('0', b) >= e;
}

// One quota header, split: "x-ratelimit-remaining-custom-day" -> field
// "remaining", window "day". The by-size family has no dimension segment
// ("x-ratelimitbysize-limit-minute"). Anything else reads as not a quota header.
struct QuotaHeader {
  std::string field;    // "limit" | "remaining"
  std::string dim;      // "req" | "tokens" | "custom" (Studio connectors) | "" (by-size)
  std::string window;   // "minute" | "day" | "month" | other
};
bool splitQuotaHeader(const std::string& name, QuotaHeader& out) {
  static const char* kPrefixes[] = {"x-ratelimitbysize-", "x-ratelimit-"};
  std::string rest;
  for (const char* p : kPrefixes)
    if (name.rfind(p, 0) == 0) { rest = name.substr(std::strlen(p)); break; }
  const size_t dash = rest.find('-');
  if (dash == std::string::npos) return false;
  out.field = rest.substr(0, dash);
  if (out.field != "limit" && out.field != "remaining") return false;
  const size_t last = rest.rfind('-');
  out.window = rest.substr(last + 1);
  out.dim = last > dash ? rest.substr(dash + 1, last - dash - 1) : std::string();
  return !out.window.empty();
}

}  // namespace

RateLimit rateLimitFromHeaders(const HeaderList& headers) {
  bool zeroLimit = false, monthSpent = false, connectorDaySpent = false, daySpent = false;
  bool minuteSeen = false;
  for (const auto& h : headers) {
    QuotaHeader q;
    if (!splitQuotaHeader(lower(h.first), q)) continue;
    if (q.window == "minute") minuteSeen = true;
    if (!headerIsZero(h.second)) continue;
    if (q.field == "limit") zeroLimit = true;
    else if (q.window == "month") monthSpent = true;
    else if (q.window == "day" && q.dim == "custom") connectorDaySpent = true;
    else if (q.window == "day") daySpent = true;
  }
  if (zeroLimit)         return RateLimit::NotAllowed;
  if (monthSpent)        return RateLimit::Quota;
  if (connectorDaySpent) return RateLimit::DailyUtc;
  if (daySpent)          return RateLimit::Daily;
  if (minuteSeen)        return RateLimit::PerMinute;
  return RateLimit::Unknown;
}

RateLimit rateLimitFromText(const std::string& text) {
  const std::string t = lower(text);
  // OpenAI answers 429 when ONE request is bigger than the per-minute window
  // ("Request too large ... The input or output tokens must be reduced"): that
  // request can never pass, however long the owner waits.
  if (has(t, "request too large")) return RateLimit::NotAllowed;
  if (has(t, "insufficient_quota") || has(t, "exceeded your current quota") ||
      has(t, "credit balance") || has(t, "monthly"))
    return RateLimit::Quota;
  if (has(t, "per day") || has(t, "(rpd)") || has(t, "(tpd)") || has(t, "daily"))
    return RateLimit::Daily;
  if (has(t, "per min") || has(t, "per-minute") || has(t, "(rpm)") || has(t, "(tpm)"))
    return RateLimit::PerMinute;
  return RateLimit::Unknown;
}

const char* rateLimitSlug(RateLimit k) {
  switch (k) {
    case RateLimit::PerMinute:  return "minute";
    case RateLimit::DailyUtc:   return "day-utc";
    case RateLimit::Daily:      return "day";
    case RateLimit::NotAllowed: return "plan";
    case RateLimit::Quota:      return "quota";
    case RateLimit::Unknown:
    case RateLimit::kCount:     break;
  }
  return "unknown";
}

std::string rateLimitTag(RateLimit k) {
  if (k == RateLimit::Unknown || k == RateLimit::kCount) return std::string();
  return std::string("[rl:") + rateLimitSlug(k) + "]";
}

RateLimit rateLimitFromError(const std::string& err) {
  const size_t at = err.find("[rl:");
  if (at != std::string::npos) {
    const size_t end = err.find(']', at);
    const std::string slug = err.substr(at + 4, end == std::string::npos ? 0 : end - at - 4);
    for (int i = 1; i < int(RateLimit::kCount); i++)
      if (slug == rateLimitSlug(RateLimit(i))) return RateLimit(i);
  }
  return rateLimitFromText(err);
}

const char* rateLimitReply(RateLimit k) {
  switch (k) {
    case RateLimit::PerMinute:
      return "The provider's per-minute limit for this key was reached, so that didn't "
             "finish. Nothing is still running; wait a minute and ask again.";
    case RateLimit::DailyUtc:
      return "This key's daily limit at the provider is used up, so that didn't finish. "
             "Nothing is still running; it resets at midnight UTC.";
    case RateLimit::Daily:
      return "This key's daily limit at the provider is used up, so that didn't finish. "
             "Nothing is still running; ask again later.";
    case RateLimit::NotAllowed:
      return "This key's plan at the provider doesn't allow this request, so waiting "
             "won't help. Nothing is still running; check the plan with the provider.";
    case RateLimit::Quota:
      return "The provider says this key's quota is used up, so that didn't finish. "
             "Nothing is still running; check the plan and billing with the provider.";
    case RateLimit::Unknown:
    case RateLimit::kCount:
      break;
  }
  return "The provider is limiting requests from this key, so that didn't finish. "
         "Nothing is still running; ask again later.";
}

}  // namespace agent
