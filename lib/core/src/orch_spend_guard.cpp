#include "nimbus/orch/spend_guard.h"

#include <cstdlib>

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
         (d.empty() ? std::string("Otherwise paid calls resume when the next month starts.")
                    : "Otherwise paid calls resume on " + d + ".");
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
  return "Paid AI calls are paused (" + why + "), so the device stopped " +
         (one ? std::string("a running helper: ") : std::to_string(labels.size()) + " running helpers: ") +
         list + ". What " + (one ? "it" : "they") + " used until now is counted. Raising the limit does not "
         "restart " + (one ? "it" : "them") + "; ask again if you still need the work.";
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
