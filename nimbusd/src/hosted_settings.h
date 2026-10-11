#pragma once
#include <ArduinoJson.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "nimbus/orch/caps.h"
#include "nimbus/orch/provider_slots.h"
#include "posix_fs.h"

// hosted_settings - the owner's settings on a hosted instance (CUM-543), and the
// rule that every field the web app posts is either APPLIED or REFUSED.
//
// Before this, POST /api/orch and POST /api/config answered {"ok":true} for
// every field and stored nothing but keys and models, so the page said "Saved"
// over a dropped write (primary provider, fallback orders, directive, time zone,
// Clear Conversation). The settings a hosted assistant honors now persist on the
// instance volume (<data>/mem/settings.json, the device's NVS role) and take effect
// on the next turn; every other field answers 400 "Not available on a hosted
// Nimbus: <field>", and a device knob posted at its hosted value (the page's Save
// Changes always posts the whole form) is a no-op, never a refusal.
namespace nimbusd {

// The settings a hosted instance applies. Empty strings mean "the default": no
// pinned head, the shipped directive; devTz "" is the operator's TZ, else UTC.
struct HostedSettings {
  std::string orchHost;    // device orchHost ("" = first keyed provider in provPrio)
  std::string provPrio;    // device provPrio (head failover order)
  std::string subPrio;     // device subPrio ("" = same as provPrio)
  std::string devTz;       // POSIX TZ or zoneinfo name ("" = UTC)
  std::string directive;   // device sysPrompt ("" = the shipped default)
  std::string devName;     // the instance's display name
  bool toolLoop = true;    // device orchLoop
};

// Thread-safe, durable store: the HTTP thread reads for GET /api/state and
// /api/orch and applies /api/config writes, the engine thread applies /api/orch
// writes and reads per turn, so every accessor copies.
class SettingsStore {
 public:
  void init(const std::string& path, const HostedSettings& defaults) {
    std::lock_guard<std::mutex> lk(mu_);
    path_ = path;
    s_ = defaults;
    std::string blob;
    JsonDocument d;
    if (!fsutil::readFile(path_, blob) || deserializeJson(d, blob) || !d.is<JsonObject>()) return;
    readStr(d, "orchHost", s_.orchHost);
    readStr(d, "provPrio", s_.provPrio);
    readStr(d, "subPrio", s_.subPrio);
    readStr(d, "devTz", s_.devTz);
    readStr(d, "directive", s_.directive);
    readStr(d, "devName", s_.devName);
    if (d["toolLoop"].is<bool>()) s_.toolLoop = d["toolLoop"].as<bool>();
  }
  HostedSettings get() const {
    std::lock_guard<std::mutex> lk(mu_);
    return s_;
  }
  // Apply a mutation only if it persists (atomic tmp->rename): a failed write
  // leaves the live settings untouched, so a refusal never hides a half-applied
  // change and a success is always on disk. `fn` only assigns fields.
  bool update(const std::function<void(HostedSettings&)>& fn) {
    std::lock_guard<std::mutex> lk(mu_);
    HostedSettings next = s_;
    fn(next);
    JsonDocument d;
    d["orchHost"] = next.orchHost;
    d["provPrio"] = next.provPrio;
    d["subPrio"] = next.subPrio;
    d["devTz"] = next.devTz;
    d["directive"] = next.directive;
    d["devName"] = next.devName;
    d["toolLoop"] = next.toolLoop;
    std::string out;
    serializeJson(d, out);
    if (!fsutil::writeFileAtomic(path_, out)) return false;
    s_ = std::move(next);
    return true;
  }

 private:
  static void readStr(const JsonDocument& d, const char* k, std::string& out) {
    if (d[k].is<const char*>()) out = d[k].as<const char*>();
  }
  mutable std::mutex mu_;
  std::string path_;
  HostedSettings s_;
};

namespace hosted {

// The refusal every unsupported field gets (TASK contract, exact copy).
inline std::string notAvailable(const std::string& field) {
  return "Not available on a hosted Nimbus: " + field;
}

inline std::string trim(std::string s) {
  while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
  size_t b = 0;
  while (b < s.size() && isspace((unsigned char)s[b])) b++;
  return s.substr(b);
}

// A head pin: "" (automatic) or a provider this instance can run.
inline bool validHost(const std::string& h) {
  return h.empty() || nimbus::orch::isProviderSlug(h.c_str());
}

// A provider order: only known slugs, each once, in the order given ("" when
// nothing valid survives - the caller refuses it rather than store an empty order).
inline std::string sanitizePriority(const std::string& csv) {
  std::string out;
  std::vector<std::string> seen;
  size_t start = 0;
  while (start <= csv.size()) {
    size_t comma = csv.find(',', start);
    if (comma == std::string::npos) comma = csv.size();
    const std::string t = trim(csv.substr(start, comma - start));
    const bool dup = std::find(seen.begin(), seen.end(), t) != seen.end();
    if (!t.empty() && !dup && nimbus::orch::isProviderSlug(t.c_str())) {
      seen.push_back(t);
      out += (out.empty() ? "" : ",") + t;
    }
    start = comma + 1;
  }
  return out;
}

// POSIX TZ grammar (std offset [dst [offset] [,rule,rule]]), checked loosely but
// completely enough that a typo is refused instead of silently running as UTC.
inline bool posixName(const std::string& s, size_t& i) {
  if (i < s.size() && s[i] == '<') {
    const size_t close = s.find('>', i);
    if (close == std::string::npos || close - i < 4) return false;
    i = close + 1;
    return true;
  }
  const size_t b = i;
  while (i < s.size() && isalpha((unsigned char)s[i])) i++;
  return i - b >= 3;
}
inline bool posixOffset(const std::string& s, size_t& i) {
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
  const size_t b = i;
  while (i < s.size() && (isdigit((unsigned char)s[i]) || s[i] == ':')) i++;
  return i > b && isdigit((unsigned char)s[b]);
}
inline bool posixRule(const std::string& r) {
  if (r.empty()) return false;
  for (char ch : r)
    if (!isdigit((unsigned char)ch) && ch != 'M' && ch != 'J' && ch != '.' && ch != '/' && ch != ':' &&
        ch != '+' && ch != '-')
      return false;
  return true;
}
inline bool validPosixTz(const std::string& s) {
  size_t i = 0;
  if (!posixName(s, i) || !posixOffset(s, i)) return false;
  if (i == s.size()) return true;
  if (s[i] != ',') {
    if (!posixName(s, i)) return false;
    if (i < s.size() && s[i] != ',' && !posixOffset(s, i)) return false;
  }
  if (i == s.size()) return true;
  if (s[i] != ',') return false;
  const size_t c2 = s.find(',', i + 1);
  return c2 != std::string::npos && posixRule(s.substr(i + 1, c2 - i - 1)) && posixRule(s.substr(c2 + 1));
}
// A zoneinfo name (Europe/London, UTC) that this host's tz database carries.
inline bool knownZone(const std::string& s, const std::string& zoneDir = "/usr/share/zoneinfo") {
  if (s.empty() || s.find("..") != std::string::npos || s[0] == '/') return false;
  for (char ch : s)
    if (!isalnum((unsigned char)ch) && ch != '/' && ch != '_' && ch != '-' && ch != '+') return false;
  struct stat st;
  return ::stat((zoneDir + "/" + s).c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
inline bool validTz(const std::string& s) {
  if (s.empty()) return true;   // blank = UTC
  if (s.size() > 48) return false;
  return knownZone(s) || validPosixTz(s);
}

// The process time zone is global state (TZ + tzset). Every local-time read the
// daemon makes and every change go through this one lock, so a change never races
// a strftime here. Other threads still call getenv() for OTHER variables (Config,
// libcurl): POSIX calls setenv/getenv racy, so TZ is put in the environment before
// any thread starts (NimbusdRig's constructor) and a later setenv only replaces
// that existing entry - glibc swaps one pointer and never frees the old string, so
// a concurrent getenv reads either value, never freed memory. A change is a rare
// owner action (Settings > Time zone).
inline std::mutex& tzMutex() {
  static std::mutex m;
  return m;
}
inline void applyTz(const std::string& tz) {
  std::lock_guard<std::mutex> lk(tzMutex());
  setenv("TZ", tz.empty() ? "UTC0" : tz.c_str(), 1);
  tzset();
}
inline std::string localTime(time_t t, const char* fmt) {
  std::lock_guard<std::mutex> lk(tzMutex());
  struct tm lt{};
  localtime_r(&t, &lt);
  char buf[64];
  strftime(buf, sizeof buf, fmt, &lt);
  return buf;
}

// ---- the /api/orch field policy -------------------------------------------
// Device knobs the hosted page shows: GET /api/orch reports these hosted values,
// so the page's Save Changes (which posts the whole form) round-trips them as a
// no-op; any other value is refused with the field name. A blank number or text
// knob is "unchanged" (the form before the first load fills it), and capProbe
// reads "passive" - what a hosted instance does: it marks a key verified when a
// turn on it answers. Values match the page's own defaults where possible.
struct FixedKnob {
  const char* field;
  const char* value;
  bool boolean;
};
inline const std::vector<FixedKnob>& fixedKnobs() {
  static const std::vector<FixedKnob> k = {
      {"midFail", "0", true},       {"codeSbx", "0", true},     {"orchTrace", "0", true},
      {"tlsVerify", "1", true},     {"loopRounds", "12", false}, {"loopDeadline", "600", false},
      {"compactKB", "0", false},    {"tlsSlots", "1", false},   {"capProbe", "1", false},
      {"capProbeH", "24", false},   {"custBase", "", false},    {"custModel", "", false},
      {"custConv", "openai", false},
  };
  return k;
}
// Secret fields whose EMPTY value means "keep what is set" (the page posts them
// blank on every Save Changes); a non-empty value would set something a hosted
// instance does not store from the page.
inline bool keepIfEmptyField(const std::string& f) {
  return f == "custKey" || f == "tgToken" || f == "tavKey" || f == "clr_custKey";
}
inline std::string normBool(const std::string& v) {
  return (v == "1" || v == "true" || v == "on") ? "1" : "0";
}

enum class Verdict { Apply, NoOp, Refuse };

// The fields POST /api/orch applies on a hosted instance: the canonical key
// fields, the per-provider head and sub-agent model picks, routing, the directive,
// the tool loop, the Telegram allowlist and the conversation/memory clears.
inline bool appliedOrchField(const std::string& f) {
  static const char* kApplied[] = {"orchHost", "provPrio", "subPrio", "sysPrompt", "clearConv",
                                   "clearMem", "convReset", "orchLoop", "tgAllow"};
  for (const char* a : kApplied)
    if (f == a) return true;
  for (const auto& slot : nimbus::orch::kProviderSlots) {
    const std::string slug = slot.slug;
    for (const std::string& n : {std::string(slot.keyField), "orchM_" + slug, "subM_" + slug})
      if (f == n || f == "clr_" + n) return true;
  }
  return false;
}

// What POST /api/orch does with one posted field: the applied set applies; the
// knob and keep-if-empty tables decide the no-ops; everything else is refused.
inline Verdict orchFieldVerdict(const std::string& field, const std::string& value) {
  if (field == "t" || field == "token") return Verdict::NoOp;   // auth, not a setting
  if (appliedOrchField(field)) return Verdict::Apply;
  for (const auto& k : fixedKnobs()) {
    if (field != k.field) continue;
    if (!k.boolean && value.empty()) return Verdict::NoOp;   // blank = unchanged
    const std::string v = k.boolean ? normBool(value) : value;
    return v == k.value ? Verdict::NoOp : Verdict::Refuse;
  }
  if (keepIfEmptyField(field)) return value.empty() ? Verdict::NoOp : Verdict::Refuse;
  return Verdict::Refuse;
}

// What POST /api/config does with one posted field: the name and the time zone
// apply, "Sync now" is a no-op (the host clock is always synced), the rest are
// device-only (mode, battery, display, onboarding...).
inline Verdict configFieldVerdict(const std::string& field) {
  if (field == "devName" || field == "devTz") return Verdict::Apply;
  if (field == "clockSync" || field == "t" || field == "token") return Verdict::NoOp;
  return Verdict::Refuse;
}

// A display name: 1-32 printable characters.
inline bool validName(const std::string& n) {
  if (n.empty() || n.size() > 32) return false;
  for (unsigned char ch : n)
    if (ch < 0x20 || ch == 0x7f) return false;
  return true;
}

}  // namespace hosted
}  // namespace nimbusd
