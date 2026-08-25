#include "nimbus/ota/ota_logic.h"

#include <cstdio>
#include <cstring>

#include <ArduinoJson.h>

namespace nimbus {
namespace ota {

// --- version ----------------------------------------------------------------

bool parseVersion(const char* s, int& maj, int& min, int& pat) {
  if (!s || !*s) return false;
  if (*s == 'v' || *s == 'V') s++;
  int parts[3] = {0, 0, 0};
  for (int i = 0; i < 3; i++) {
    if (*s < '0' || *s > '9') return false;
    long v = 0;
    while (*s >= '0' && *s <= '9') {
      v = v * 10 + (*s - '0');
      if (v > 100000) return false;
      s++;
    }
    parts[i] = (int)v;
    if (i < 2) {
      if (*s != '.') return false;
      s++;
    }
  }
  // trailing "-rc1" / "+build" is fine; any other trailing junk is not.
  if (*s != '\0' && *s != '-' && *s != '+') return false;
  maj = parts[0]; min = parts[1]; pat = parts[2];
  return true;
}

int compareVersions(const char* a, const char* b) {
  int am, ai, ap, bm, bi, bp;
  bool okA = parseVersion(a, am, ai, ap);
  bool okB = parseVersion(b, bm, bi, bp);
  if (!okA && !okB) return 0;
  if (!okA) return -1;  // garbage compares older
  if (!okB) return 1;
  if (am != bm) return am < bm ? -1 : 1;
  if (ai != bi) return ai < bi ? -1 : 1;
  if (ap != bp) return ap < bp ? -1 : 1;
  return 0;
}

// --- decoding helpers -------------------------------------------------------

static int hexNib(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool decodeHex(const char* src, uint8_t* dst, size_t dstLen) {
  if (!src) return false;
  for (size_t i = 0; i < dstLen; i++) {
    int hi = hexNib(src[2 * i]);
    if (hi < 0) return false;
    int lo = hexNib(src[2 * i + 1]);
    if (lo < 0) return false;
    dst[i] = (uint8_t)((hi << 4) | lo);
  }
  return src[2 * dstLen] == '\0';
}

static int b64Val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

size_t decodeB64(const char* src, uint8_t* dst, size_t dstCap) {
  if (!src) return 0;
  size_t n = strlen(src);
  if (n == 0 || (n % 4) != 0) return 0;
  size_t out = 0;
  for (size_t i = 0; i < n; i += 4) {
    int v[4];
    int pads = 0;
    for (int k = 0; k < 4; k++) {
      char c = src[i + k];
      if (c == '=') {
        // '=' only in the last two positions of the final quad.
        if (i + 4 != n || k < 2) return 0;
        v[k] = 0;
        pads++;
      } else {
        if (pads) return 0;  // data after padding
        v[k] = b64Val(c);
        if (v[k] < 0) return 0;
      }
    }
    uint32_t trip = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) |
                    ((uint32_t)v[2] << 6) | (uint32_t)v[3];
    int bytes = 3 - pads;
    for (int k = 0; k < bytes; k++) {
      if (out >= dstCap) return 0;
      dst[out++] = (uint8_t)(trip >> (16 - 8 * k));
    }
  }
  return out;
}

// --- manifest ---------------------------------------------------------------

static void copyStr(char* dst, size_t cap, const char* src) {
  if (!src) { dst[0] = '\0'; return; }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

static bool fail(const char** errOut, const char* why) {
  if (errOut) *errOut = why;
  return false;
}

bool parseManifest(const char* json, size_t len, const char* variant,
                   ManifestInfo& out, const char** errOut) {
  if (errOut) *errOut = nullptr;
  if (!json || !len || !variant || !*variant) return fail(errOut, "schema");

  // Filter: only the fields we consume (the variants map is small - keep all
  // of it filtered to the three per-variant fields + sig).
  JsonDocument filter;
  filter["schema"] = true;
  filter["version"] = true;
  filter["build"] = true;
  filter["notes"] = true;
  filter["minVersion"] = true;
  JsonObject fv = filter["variants"][variant].to<JsonObject>();
  fv["url"] = true;
  fv["size"] = true;
  fv["sha256"] = true;
  fv["sig"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, json, len, DeserializationOption::Filter(filter));
  if (err) return fail(errOut, "schema");
  if ((doc["schema"] | 0) != 2) return fail(errOut, "schema");

  const char* ver = doc["version"] | (const char*)nullptr;
  int mj, mi, pa;
  if (!ver || !parseVersion(ver, mj, mi, pa)) return fail(errOut, "version");
  copyStr(out.version, sizeof(out.version), ver);
  copyStr(out.build, sizeof(out.build), doc["build"] | "");
  copyStr(out.notes, sizeof(out.notes), doc["notes"] | "");
  const char* minV = doc["minVersion"] | "";
  if (*minV && !parseVersion(minV, mj, mi, pa)) return fail(errOut, "version");
  copyStr(out.minVersion, sizeof(out.minVersion), minV);

  JsonObject v = doc["variants"][variant];
  if (v.isNull()) return fail(errOut, "variant");

  const char* url = v["url"] | (const char*)nullptr;
  if (!url || strncmp(url, "https://", 8) != 0 ||
      strlen(url) >= sizeof(out.v.url))
    return fail(errOut, "url");
  copyStr(out.v.url, sizeof(out.v.url), url);

  uint32_t size = v["size"] | 0u;
  if (size < kMinFwBytes || size > kMaxFwBytes) return fail(errOut, "size");
  out.v.size = size;

  const char* sha = v["sha256"] | (const char*)nullptr;
  if (!sha || strlen(sha) != 64 || !decodeHex(sha, out.v.sha256, 32))
    return fail(errOut, "sha256");
  copyStr(out.v.shaHex, sizeof(out.v.shaHex), sha);
  // Canonicalize to lowercase - the signed message uses the hex string.
  for (char* p = out.v.shaHex; *p; p++)
    if (*p >= 'A' && *p <= 'F') *p += 'a' - 'A';

  const char* sig = v["sig"] | (const char*)nullptr;
  if (!sig) return fail(errOut, "sig");
  size_t sl = decodeB64(sig, out.v.sig, sizeof(out.v.sig));
  if (sl < 8 || sl > kMaxSigLen) return fail(errOut, "sig");
  out.v.sigLen = (uint8_t)sl;
  return true;
}

// --- device type ------------------------------------------------------------

bool typeAllowedForBoard(const char* type, bool isFreenove) {
  if (!type || !*type) return false;
  if (isFreenove)
    return strcmp(type, kTypeFreenove28) == 0 ||
           strcmp(type, kTypeFreenove35) == 0 ||
           strcmp(type, kTypeFreenove40) == 0;
  return strcmp(type, kTypeNimbusTft) == 0;
}

size_t deriveDeviceType(char* buf, size_t cap, bool isFreenove,
                        bool screenIsTft) {
  const char* t = isFreenove ? kTypeFreenove28
                             : (screenIsTft ? kTypeNimbusTft : "");
  size_t n = strlen(t);
  if (cap < n + 1) { if (cap) buf[0] = '\0'; return 0; }
  memcpy(buf, t, n + 1);
  return n;
}

// --- signed message ---------------------------------------------------------

size_t buildSigMessage(char* buf, size_t cap, const char* version,
                       const char* variant, const char* shaHex) {
  static const char kPrefix[] = "nimbus-ota-v2";
  size_t need = sizeof(kPrefix) - 1 + 1 + strlen(version) + 1 +
                strlen(variant) + 1 + strlen(shaHex) + 1;
  if (cap < need + 1) return 0;
  size_t n = 0;
  memcpy(buf + n, kPrefix, sizeof(kPrefix) - 1);
  n += sizeof(kPrefix) - 1;
  buf[n++] = '\n';
  size_t l = strlen(version); memcpy(buf + n, version, l); n += l;
  buf[n++] = '\n';
  l = strlen(variant); memcpy(buf + n, variant, l); n += l;
  buf[n++] = '\n';
  l = strlen(shaHex); memcpy(buf + n, shaHex, l); n += l;
  buf[n++] = '\n';
  buf[n] = '\0';
  return n;
}

// --- state machine ----------------------------------------------------------

const char* stateStr(State s) {
  switch (s) {
    case State::Idle: return "idle";
    case State::Checking: return "checking";
    case State::UpToDate: return "up-to-date";
    case State::Available: return "available";
    case State::Downloading: return "downloading";
    case State::Verifying: return "verifying";
    case State::ReadyToReboot: return "rebooting";
    case State::Error: return "error";
    case State::Unsupported: return "unsupported";
  }
  return "?";
}

bool canCheck(State s) {
  switch (s) {
    case State::Idle:
    case State::UpToDate:
    case State::Available:
    case State::Error:
      return true;
    default:
      return false;  // Checking, Downloading, Verifying, ReadyToReboot, Unsupported
  }
}

bool canInstall(State s) { return s == State::Available; }

// --- eligibility ------------------------------------------------------------

Eligibility eligibility(const char* currentVersion, const ManifestInfo& m) {
  int cmp = compareVersions(m.version, currentVersion);
  if (cmp < 0) return Eligibility::Older;
  if (cmp == 0) return Eligibility::Same;
  if (m.minVersion[0] && compareVersions(currentVersion, m.minVersion) < 0)
    return Eligibility::BlockedMinVersion;
  return Eligibility::Newer;
}

bool autoEligible(Eligibility e) { return e == Eligibility::Newer; }

// --- boot-health / rollback -------------------------------------------------

bool shouldRollback(bool pending, uint8_t bootCount) {
  return pending && bootCount > kOtaMaxBootAttempts;
}

bool bootHealthy(uint32_t uptimeS, bool wifiEverUp) {
  return uptimeS >= (wifiEverUp ? 120u : 600u);
}

// --- auto-install idle window -----------------------------------------------

bool autoInstallAllowed(const IdleSnapshot& s) {
  if (s.turnInFlight || s.voiceActive || s.audioPlaying) return false;
  if (!s.onExternalPower && s.battPct < kAutoBattFloorPct) return false;
  // Health floor applies on battery too (gate parity with the manual path): a
  // worn pack can sag under the write load and brick a slot mid-flash.
  if (!s.onExternalPower && s.healthPct < kManualHealthFloorPct) return false;
  return s.internalFreeB >= kAutoHeapFloorB;
}

// --- battery / health install gate ------------------------------------------

InstallGate installGate(const InstallGateInput& in) {
  if (!in.battMonEnabled) return InstallGate::Allowed;   // no pack to protect
  if (in.onExternalPower) return InstallGate::Allowed;   // wall power finishes it
  if (in.battPct < kManualBattFloorPct) return InstallGate::NeedPower;
  if (in.healthPct < kManualHealthFloorPct) return InstallGate::NeedRecalibrate;
  return InstallGate::Allowed;
}

const char* installGateStr(InstallGate g) {
  switch (g) {
    case InstallGate::Allowed: return "allowed";
    case InstallGate::NeedPower: return "need-power";
    case InstallGate::NeedRecalibrate: return "need-recalibrate";
  }
  return "?";
}

// --- definitive check result ------------------------------------------------

CheckResult checkResult(State settled, bool reachedServer) {
  switch (settled) {
    case State::UpToDate: return CheckResult::UpToDate;
    case State::Available: return CheckResult::NewVersion;
    case State::Error: return reachedServer ? CheckResult::Failed : CheckResult::Unreachable;
    default: return CheckResult::Pending;  // Idle/Checking/install states
  }
}

const char* checkResultStr(CheckResult r) {
  switch (r) {
    case CheckResult::Pending: return "pending";
    case CheckResult::UpToDate: return "up-to-date";
    case CheckResult::NewVersion: return "new-version";
    case CheckResult::Unreachable: return "unreachable";
    case CheckResult::Failed: return "failed";
  }
  return "?";
}

State stateFromStr(const char* s) {
  if (!s) return State::Idle;
  if (!std::strcmp(s, "checking")) return State::Checking;
  if (!std::strcmp(s, "up-to-date")) return State::UpToDate;
  if (!std::strcmp(s, "available")) return State::Available;
  if (!std::strcmp(s, "downloading")) return State::Downloading;
  if (!std::strcmp(s, "verifying")) return State::Verifying;
  if (!std::strcmp(s, "rebooting")) return State::ReadyToReboot;
  if (!std::strcmp(s, "error")) return State::Error;
  if (!std::strcmp(s, "unsupported")) return State::Unsupported;
  return State::Idle;
}

// An in-progress affordance shows while a check or an install is running.
static bool updateBusy(State s) {
  return s == State::Checking || s == State::Downloading ||
         s == State::Verifying || s == State::ReadyToReboot;
}

// Format the owner-facing status line for `s` into buf (always UpdateView::line,
// so its fixed capacity is a constant). Split out of updateView, and holding the
// param count at the gate's max, to keep each function within the complexity gate.
static constexpr size_t kUpdateLineCap = sizeof(UpdateView::line);
static void updateLine(char* buf, State s, const char* lat, const char* err,
                       const char* fw, int pct) {
  switch (s) {
    case State::Idle:          buf[0] = '\0'; return;   // keep the default help
    case State::Checking:      std::snprintf(buf, kUpdateLineCap, "Checking for updates..."); return;
    case State::UpToDate:      std::snprintf(buf, kUpdateLineCap, "Up to date - %s", fw); return;
    case State::Available:     std::snprintf(buf, kUpdateLineCap, "Update available: %s", lat); return;
    case State::Downloading:
      if (pct >= 0) std::snprintf(buf, kUpdateLineCap, "Installing... %d%%", pct);
      else          std::snprintf(buf, kUpdateLineCap, "Installing...");
      return;
    case State::Verifying:     std::snprintf(buf, kUpdateLineCap, "Verifying update..."); return;
    case State::ReadyToReboot: std::snprintf(buf, kUpdateLineCap, "Restarting to finish..."); return;
    case State::Error:
      if (err && err[0]) std::snprintf(buf, kUpdateLineCap, "Update check failed (%s)", err);
      else               std::snprintf(buf, kUpdateLineCap, "Update check failed. Try again.");
      return;
    case State::Unsupported:   std::snprintf(buf, kUpdateLineCap, "Updates aren't available on this build."); return;
  }
}

const char* checkRefusalCopy(const char* why) {
  if (!why) return "Couldn't start the check. Try again.";
  if (!std::strcmp(why, "no-wifi"))     return "Can't check: no Wi-Fi. Connect and try again.";
  if (!std::strcmp(why, "low-heap"))    return "Can't check now: low memory. Try again.";
  if (!std::strcmp(why, "unsupported")) return "This build doesn't receive updates.";
  if (!std::strcmp(why, "busy") || !std::strcmp(why, "in-progress"))
    return "An update is already running.";
  return "Couldn't start the check. Try again.";
}

bool lastResultStale(const char* lastResult, const char* runningVersion) {
  if (!lastResult || !lastResult[0]) return false;   // nothing to clear
  const char* tok = lastResult;                       // last space-separated field
  for (const char* p = lastResult; *p; ++p)
    if (*p == ' ') tok = p + 1;
  int a, b, c;
  if (!parseVersion(tok, a, b, c)) return false;      // not a version -> leave it
  return compareVersions(tok, runningVersion) != 0;   // names a different image
}

UpdateView updateView(State s, int pct, const char* latest, const char* err,
                      const char* fwVersion) {
  UpdateView v;
  v.busy = updateBusy(s);
  v.showInstall = (s == State::Available);
  v.pct = (s == State::Downloading) ? pct : -1;
  const char* lat = (latest && latest[0]) ? latest : "a new version";
  const char* fw  = (fwVersion && fwVersion[0]) ? fwVersion : "this version";
  updateLine(v.line, s, lat, err, fw, pct);
  return v;
}

}  // namespace ota
}  // namespace nimbus
