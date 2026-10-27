#include "nimbus/orch/spend_guard.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "nimbus/orch/connectors_wire.h"   // urlRoutableToProviderHead - the one private-host rule
#include "nimbus/orch/seam_meter.h"        // kEstimateCharsPerToken, kImageEstimateChars

namespace nimbus {
namespace orch {

namespace {

// The host the custom adapter dials from a base URL, read the way it reads it (custom.cpp
// parseBase: "http(s)://host[:port][/path]", "host", "host:port"), or "" when the base is
// anything this simple shape does not cover: userinfo, a query or fragment marker, an IPv6
// literal, a second ':' or a non-numeric port. "" is never local (lane QW4 review: a base
// the classifier and the dialer read differently, "http://a.com:80@192.168.1.5", must
// price as the public host the adapter dials).
std::string dialedHost(const std::string& base) {
  if (base.find_first_of("@?#[] \t\r\n\\") != std::string::npos) return std::string();
  std::string rest = base;
  const size_t sch = rest.find("://");
  if (sch != std::string::npos) {
    const std::string scheme = rest.substr(0, sch);
    if (scheme != "http" && scheme != "https") return std::string();
    rest = rest.substr(sch + 3);
  }
  rest = rest.substr(0, rest.find('/'));
  const size_t colon = rest.find(':');
  if (colon == std::string::npos) return rest;
  const std::string port = rest.substr(colon + 1);
  if (port.empty() || port.find_first_not_of("0123456789") != std::string::npos) return std::string();
  return rest.substr(0, colon);
}

}  // namespace

bool customEndpointIsLocal(const std::string& baseUrl) {
  const std::string host = dialedHost(baseUrl);
  // The one private-host rule (loopback, private and link-local addresses in any IPv4
  // spelling, localhost, .local): a host a provider's cloud could not dial.
  return !host.empty() && !urlRoutableToProviderHead("http://" + host);
}

bool customEndpointIsFree(const std::string& baseUrl, bool hasKey) {
  return !hasKey && customEndpointIsLocal(baseUrl);
}

std::string frozenReplyText(const CeilingFacts& f) {
  const std::string d = resetDateText(f.monthKey);
  return "Not answered: paid AI calls are paused because this month's estimated spend reached "
         "the monthly spend ceiling of " + formatUsd(f.limitMicro) +
         ". To raise it, open the device's web page, go to Assistant > Usage, and set the "
         "monthly ceiling. " +
         (d.empty() ? std::string("Otherwise paid calls resume once the device's clock is set and the month changes.")
                    : "Otherwise paid calls resume on " + d + ".");
}

std::string frozenReplyTextOther() {
  return "I can't answer right now: paid AI calls are paused on this device.";
}

bool FrozenReplyThrottle::allow(const std::string& chat, bool owner, uint32_t episode, uint32_t nowMs) {
  uint32_t h = 2166136261u;
  for (unsigned char ch : chat) h = (h ^ ch) * 16777619u;
  Slot* slot = nullptr;
  Slot* oldest = &slots_[0];
  for (Slot& s : slots_) {
    if (s.used && s.chat == h) slot = &s;
    if (!s.used || (oldest->used && (uint32_t)(nowMs - s.atMs) > (uint32_t)(nowMs - oldest->atMs))) oldest = &s;
  }
  if (slot) {
    const bool newFreeze = slot->episode != episode;
    if (!newFreeze && (!owner || (uint32_t)(nowMs - slot->atMs) < kFrozenReplyEveryMs)) return false;
  } else {
    slot = oldest;
  }
  *slot = Slot{h, episode, nowMs, true};
  return true;
}

namespace {
// Days since 1970-01-01 of a civil date (Howard Hinnant's days_from_civil).
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}
}  // namespace

bool parseHttpDate(const char* s, int64_t& epoch) {
  static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char wd[4] = {}, mon[4] = {};
  int d = 0, y = 0, hh = 0, mm = 0, ss = 0;
  if (!s || std::sscanf(s, "%3s, %d %3s %d %d:%d:%d", wd, &d, mon, &y, &hh, &mm, &ss) != 7) return false;
  unsigned m = 0;
  for (unsigned i = 0; i < 12; ++i)
    if (std::strcmp(mon, kMonths[i]) == 0) m = i + 1;
  if (!m || d < 1 || d > 31 || hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60) return false;
  epoch = daysFromCivil(y, m, (unsigned)d) * 86400 + hh * 3600 + mm * 60 + ss;
  return epoch >= (int64_t)kCeilingClockValid;
}

namespace {
std::mutex& serverClockMx() {
  static std::mutex mx;
  return mx;
}
}  // namespace

// Re-anchored on every sample (so the uptime counted on top stays short, never near its
// 49-day wrap): at the sample, or at the time already known when that is later (never
// back). A sample past kServerDateMax, or a jump of more than kServerDateMaxJumpS, is
// refused.
void ServerClock::note(int64_t epoch, uint32_t nowMs) {
  if (epoch < (int64_t)kCeilingClockValid || epoch > kServerDateMax) return;
  std::lock_guard<std::mutex> lk(serverClockMx());
  if (date_ > 0) {
    const int64_t known = date_ + (int64_t)((uint32_t)(nowMs - atMs_) / 1000);
    if (epoch > known + kServerDateMaxJumpS) return;
    epoch = epoch > known ? epoch : known;
  }
  date_ = epoch;
  atMs_ = nowMs;
}

int64_t ServerClock::epoch(int64_t systemNow, uint32_t nowMs) const {
  if (systemNow >= (int64_t)kCeilingClockValid) return systemNow;
  std::lock_guard<std::mutex> lk(serverClockMx());
  if (date_ <= 0) return systemNow;
  return date_ + (int64_t)((uint32_t)(nowMs - atMs_) / 1000);
}

ServerClock& serverClock() {
  static ServerClock c;
  return c;
}

void noteServerDate(int64_t epoch, uint32_t nowMs) { serverClock().note(epoch, nowMs); }

bool trustedDateHost(const std::string& host) {
  static const char* const kHosts[] = {"openai.com", "anthropic.com", "mistral.ai", "z.ai",
                                       "bigmodel.cn", "cumulo-nimbus.ai", "telegram.org"};
  for (const char* d : kHosts) {
    const std::string dom(d);
    if (host == dom) return true;
    if (host.size() > dom.size() && host.compare(host.size() - dom.size(), dom.size(), dom) == 0 &&
        host[host.size() - dom.size() - 1] == '.')
      return true;
  }
  return false;
}

void noteHttpDateHeader(const char* value, uint32_t nowMs) {
  while (value && *value == ' ') ++value;
  int64_t e = 0;
  if (parseHttpDate(value, e)) noteServerDate(e, nowMs);
}

bool frozenReplyDue(bool telegramChat, bool frozen, bool answered) {
  return telegramChat && frozen && !answered;
}

bool reconcileProvisional(SpendCeiling& c, uint32_t month, bool seedOwed,
                          uint64_t ledgerMonthToDateMicro, uint64_t provisionalMicro) {
  if (month == 0) return false;
  const bool newMonth = c.monthKey != 0 && month > c.monthKey;   // a count from an earlier month
  bool changed = c.roll(month);
  if (newMonth && provisionalMicro) changed = c.add(provisionalMicro, month) || changed;
  if (seedOwed) changed = c.add(ledgerMonthToDateMicro, month) || changed;
  return changed;
}

std::string ClockDebt::serialize() const {
  if (!seed && !provisional) return std::string();
  return std::to_string((unsigned)seed) + "|" + std::to_string((unsigned long long)provisional);
}

bool ClockDebt::deserialize(const std::string& text) {
  *this = ClockDebt();
  const size_t bar = text.find('|');
  const std::string s = text.substr(0, bar), p = bar == std::string::npos ? "" : text.substr(bar + 1);
  const bool sOk = s.size() == 1 && s[0] >= '0' && s[0] <= '2';
  const bool pOk = p.find_first_not_of("0123456789") == std::string::npos && p.size() <= 19;
  if (!sOk || !pOk) {
    seed = 1;   // unreadable: owe the seed (fail-closed)
    return false;
  }
  seed = (uint8_t)(s[0] - '0');
  provisional = p.empty() ? 0 : std::strtoull(p.c_str(), nullptr, 10);
  return true;
}

void ClockDebt::noteCommit(uint64_t micro, bool clock) {
  if (!clock) provisional = addSatMicro(provisional, micro);
}

ClockDebt::Settled ClockDebt::settle(SpendCeiling& c, uint32_t month, uint64_t ledgerMonthToDateMicro) {
  Settled out;
  if (month == 0) return out;
  out.changed = reconcileProvisional(c, month, seed != 0, ledgerMonthToDateMicro, provisional);
  provisional = 0;
  if (seed) {
    out.seeded = true;
    out.migration = seed == 2;
    out.changed = true;
  } else if (floor) {
    out.changed = floorFromLedger(c, month, ledgerMonthToDateMicro) || out.changed;
  }
  seed = 0;
  floor = false;
  return out;
}

bool floorFromLedger(SpendCeiling& c, uint32_t month, uint64_t ledgerMonthToDateMicro) {
  if (month == 0 || c.monthKey != month || ledgerMonthToDateMicro <= c.spentMicro) return false;
  return c.add(ledgerMonthToDateMicro - c.spentMicro, month);
}

bool verifyProbeAllowed(bool ownerAsked, bool frozen, bool parkOpen) {
  return ownerAsked || (!frozen && parkOpen);
}

std::string httpBodyOf(const std::string& raw) {
  const size_t at = raw.find("\r\n\r\n");
  return at == std::string::npos ? std::string() : raw.substr(at + 4);
}

std::string helpersStoppedText(const std::vector<std::string>& labels, bool ceiling,
                               const std::vector<std::string>& providers) {
  if (labels.empty()) return std::string();
  std::string list;
  for (size_t i = 0; i < labels.size(); ++i)
    list += (i == 0 ? "" : (i + 1 == labels.size() ? " and " : ", ")) + labels[i];
  std::string why = "this month's spend ceiling is reached";
  if (!ceiling) {
    std::string names;
    for (size_t i = 0; i < providers.size(); ++i)
      names += (i == 0 ? "" : (i + 1 == providers.size() ? " and " : ", ")) + providers[i];
    why = (providers.size() == 1 ? names + "'s monthly limit is" : "the monthly limits of " + names + " are") +
          " reached";
  }
  const bool one = labels.size() == 1;
  return "Paid AI calls are paused (" + why + "), so the device is stopping " +
         (one ? std::string("a running helper: ") : std::to_string(labels.size()) + " running helpers: ") +
         list + ". What " + (one ? "it uses" : "they use") + " until " + (one ? "it stops" : "they stop") +
         " is counted, and " + (one ? "its result is" : "their results are") +
         " not delivered. Raising the limit does not restart " + (one ? "it" : "them") +
         "; ask again if you still need the work.";
}

bool sentWithoutAnswer(int status, const std::string& transportError) {
  if (transportError.empty() || (status != 0 && (status < 200 || status >= 300))) return false;
  // The request went out and no status line came back (the deadline, a peer close), or
  // an answer began and was cut (headers, or a body that did not parse whole or was too
  // large to hold). A refused request (4xx, 5xx) bills nothing and is not counted.
  static const char* const kSent[] = {"no response", "response headers cut short", "response parse failed",
                                      "IncompleteInput", "NoMemory", "too large"};
  for (const char* k : kSent)
    if (transportError.find(k) != std::string::npos) return true;
  return false;
}

TokenUsage unansweredEstimate(const UnansweredRequest& r) {
  TokenUsage u = sentRequestEstimate(r.bodyChars, r.images);
  if (r.startsRun && u.promptTokens < kSessionInputEstimateTokens) u.promptTokens = kSessionInputEstimateTokens;
  uint64_t out = (uint64_t)r.heldMs * kEstimateOutputTokensPerSec / 1000;
  if (r.outputCap && out > r.outputCap) out = r.outputCap;
  u.completionTokens = out > UINT32_MAX ? UINT32_MAX : (uint32_t)out;
  return u;
}

uint32_t bodyOutputCap(const std::string& body) {
  for (const char* key : {"\"max_output_tokens\":", "\"max_completion_tokens\":", "\"max_tokens\":"}) {
    const size_t at = body.find(key);
    if (at == std::string::npos) continue;
    size_t i = at + std::strlen(key);
    while (i < body.size() && body[i] == ' ') ++i;
    uint64_t v = 0;
    for (; i < body.size() && body[i] >= '0' && body[i] <= '9' && v <= UINT32_MAX; ++i) v = v * 10 + (uint64_t)(body[i] - '0');
    if (v) return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
  }
  return 0;
}

bool bodyStartsRun(const std::string& path, const std::string& body) {
  if (path.find("/v1/responses") != std::string::npos && body.find("\"background\":true") != std::string::npos)
    return true;
  return path.size() >= 17 && path.compare(path.size() - 17, 17, "/v1/conversations") == 0;
}

TokenUsage sentRequestEstimate(size_t bodyChars, size_t images) {
  TokenUsage u;
  const uint64_t chars = (uint64_t)bodyChars + (uint64_t)images * kImageEstimateChars;
  const uint64_t tokens = (chars + kEstimateCharsPerToken - 1) / kEstimateCharsPerToken;
  u.promptTokens = tokens > UINT32_MAX ? UINT32_MAX : (uint32_t)tokens;
  u.completionTokens = 0;
  u.responses = 1;
  return u;
}

}  // namespace orch
}  // namespace nimbus
