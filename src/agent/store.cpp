#include "nimbus/orch/budget.h"    // deriveBudget - the effective tool-result cap
#include "nimbus/orch/compact.h"   // modelCtxTokens
#include "store.h"
#include "nimbus/device_identity.h"   // makeSetupPass - the setup-AP passphrase generator
#include "nimbus/power/power_policy.h"

#include <esp_random.h>
#include <esp_heap_caps.h>   // PSRAM buffer for the usage-history JSON payload
#include <solide/memory.h>
#include <solide/board.h>   // batt.cells: per-cell sleep/wake threshold scaling (CUM-202)
#include <LittleFS.h>   // daily usage history (/data/usage_hist.txt - too big for NVS)

#include <ctime>
#include <mutex>

#include "agent_config.h"
#include "nimbus/orch/usage_ledger.h"

// Device config accessors backed by solide::memory (NVS). Defaults mirror
// Nuage-Solide storage.cpp: priorities default to "openai,anthropic,mistral"
// (Mistral host ported 2026-07), per-provider models default to that
// provider's flagship. A stored NVS value always wins over these defaults.

namespace agent {
namespace store {

// ---- credentials ----
String openaiKey()    { return solide::memory::getString(AKEY_OPENAI_KEY, ""); }
bool   hasOpenaiKey() { return openaiKey().length() > 0; }
String anthropicKey()    { return solide::memory::getString(AKEY_ANTHROPIC_KEY, ""); }
bool   hasAnthropicKey() { return anthropicKey().length() > 0; }
String mistralKey()   { return solide::memory::getString(AKEY_MISTRAL_KEY, ""); }
bool   hasMistralKey(){ return mistralKey().length() > 0; }
String tavilyKey()    { return solide::memory::getString(AKEY_TAVILY_KEY, ""); }
String deviceTz()     { return solide::memory::getString(AKEY_DEVICE_TZ, ""); }
void   setDeviceTz(const String& tz) { solide::memory::setString(AKEY_DEVICE_TZ, tz); }
String dreamScratchHash() { return solide::memory::getString(AKEY_DREAM_SCRATCH, ""); }
void   setDreamScratchHash(const String& hex) { solide::memory::setString(AKEY_DREAM_SCRATCH, hex); }
bool   hasTavilyKey() { return tavilyKey().length() > 0; }

// Per-device web/MCP auth token. Generated (96-bit hardware-random) on FIRST use and
// persisted, so it is stable across reboots + unique per device - never a shipped
// constant. Shown to the owner via the Config QR; required on state-changing web POSTs
// + /mcp. Read-only surface: there is no setter (the device owns it).
String webAuthToken() {
  String t = solide::memory::getString(AKEY_WEB_TOKEN, "");
  if (t.length() == 0) {
    char buf[25];
    for (int i = 0; i < 24; i += 8) snprintf(buf + i, 9, "%08x", (unsigned)esp_random());
    buf[24] = 0;
    t = buf;
    solide::memory::setString(AKEY_WEB_TOKEN, t);
  }
  return t;
}

// Rotate the token: mint a fresh 96-bit hardware-random value + persist it. Every
// browser holding the old token is instantly logged out (must re-scan the Config QR).
// Owner-triggered from the web UI (already an authenticated action) - the device still
// owns the value; there is no way to SET a chosen token, only to regenerate.
String regenWebAuthToken() {
  char buf[25];
  for (int i = 0; i < 24; i += 8) snprintf(buf + i, 9, "%08x", (unsigned)esp_random());
  buf[24] = 0;
  solide::memory::setString(AKEY_WEB_TOKEN, buf);
  return String(buf);
}

// Per-device setup-AP passphrase. Generated (hardware-random, 10 chars from an
// unambiguous lowercase+digit alphabet - nimbus::identity::makeSetupPass) on FIRST
// use and persisted, so every device gets a UNIQUE setup-network password instead
// of the old fleet-wide "nimbus1234". The owner learns it from the device's setup
// screen (printed + inside the Wi-Fi join QR) - it is never advertised over the
// network. No setter: it regenerates only when a factory reset wipes NVS wholesale
// (nvs_flash_erase in main.cpp). Cached in a static so one boot always serves ONE
// value even if the NVS write fails - the AP and the screen must never disagree.
String setupApPass() {
  static String cached;
  if (cached.length()) return cached;
  String p = solide::memory::getString(AKEY_AP_PASS, "");
  if (p.length() < 8) {  // unset, or too short for WPA2 (softAP would silently fall back OPEN)
    p = String(nimbus::identity::makeSetupPass(esp_random).c_str());
    solide::memory::setString(AKEY_AP_PASS, p);
  }
  cached = p;
  return cached;
}

// --- Cloud relay (cumulo-nimbus) pairing state -----------------------------
bool cloudOptIn() { return solide::memory::getInt(AKEY_CLOUD_OPTIN, 0) != 0; }
void setCloudOptIn(bool on) { solide::memory::setInt(AKEY_CLOUD_OPTIN, on ? 1 : 0); }
String cloudDeviceId() { return solide::memory::getString(AKEY_CLOUD_DEVID, ""); }
String cloudCred() { return solide::memory::getString(AKEY_CLOUD_CRED, ""); }
String cloudHost() { return solide::memory::getString(AKEY_CLOUD_HOST, "app.cumulo-nimbus.ai"); }
String cloudName() { return solide::memory::getString(AKEY_CLOUD_NAME, ""); }
bool cloudPaired() { return cloudDeviceId().length() > 0 && cloudCred().length() > 0; }
void setCloudPairing(const String& deviceId, const String& cred, const String& host,
                     const String& name) {
  solide::memory::setString(AKEY_CLOUD_DEVID, deviceId);
  solide::memory::setString(AKEY_CLOUD_CRED, cred);
  if (host.length()) solide::memory::setString(AKEY_CLOUD_HOST, host);
  solide::memory::setString(AKEY_CLOUD_NAME, name);
}
void clearCloudPairing() {
  solide::memory::setString(AKEY_CLOUD_DEVID, "");
  solide::memory::setString(AKEY_CLOUD_CRED, "");
  solide::memory::setString(AKEY_CLOUD_NAME, "");
}

String customBase()  { return solide::memory::getString(AKEY_CUSTOM_BASE, ""); }
String customKey()   { return solide::memory::getString(AKEY_CUSTOM_KEY, ""); }
String customConv()  { return solide::memory::getString(AKEY_CUSTOM_CONV, "openai"); }
String customModel() { return solide::memory::getString(AKEY_CUSTOM_MODEL, ""); }
bool   hasCustom()   { return customBase().length() > 0; }

String zaiKey()      { return solide::memory::getString(AKEY_ZAI_KEY, ""); }
bool   hasZaiKey()   { return zaiKey().length() > 0; }
String zaiBase()     { return solide::memory::getString(AKEY_ZAI_BASE, ""); }
void   setZaiBase(const String& host) { solide::memory::setString(AKEY_ZAI_BASE, host); }
String cumuloKey()   { return solide::memory::getString(AKEY_CUMULO_KEY, ""); }
bool   hasCumuloKey(){ return cumuloKey().length() > 0; }
String cumuloBase()  { return solide::memory::getString(AKEY_CUMULO_BASE, ""); }
String   fallbackRulesJson() { return solide::memory::getString(AKEY_FALLBACK_RULES, ""); }
uint32_t fallbackSyncTs()    { return (uint32_t)solide::memory::getInt(AKEY_FALLBACK_SYNC, 0); }

// ---- routing + models ----
String orchHost() { return solide::memory::getString(AKEY_ORCH_HOST, ""); }

// The host a turn ACTUALLY runs on: explicit orchHost, else the head of the
// priority list. ⚠ Use this (never raw orchHost()) wherever the model's context
// window is resolved - orchHost is "" by default, and modelCtxTokens("") falls
// back to the conservative 100K default, which silently HALVED the derived
// brief/fold-slice budgets and made the web UI report the wrong effective caps
// (prism 2026-08-05). The engine resolves the same way inline.
bool providerHasKey(const String& name) {
  String n = name; n.trim();
  if (n == "openai")    return hasOpenaiKey();
  if (n == "anthropic") return hasAnthropicKey();
  if (n == "mistral")   return hasMistralKey();
  return false;
}
String resolvedOrchHost() {
  String h = orchHost();
  if (h.length()) return h;
  // Walk the priority list and pick the FIRST provider that actually has a key,
  // so the active provider is one the owner configured. A bare default list heads
  // with "openai" even when only Mistral is keyed - the reported bug. Fall back to
  // the raw head only if nothing is keyed yet (fresh device mid-onboarding).
  const String pr = providerPriority();
  String head;
  int start = 0;
  while (start <= (int)pr.length()) {
    int c = pr.indexOf(',', start);
    String tok = (c < 0) ? pr.substring(start) : pr.substring(start, c);
    tok.trim();
    if (tok.length()) {
      if (head.length() == 0) head = tok;
      if (providerHasKey(tok)) return tok;
    }
    if (c < 0) break;
    start = c + 1;
  }
  return head;
}
String orchConvId() { return solide::memory::getString(AKEY_ORCH_CONVID, ""); }
void   setOrchConvId(const String& v) { solide::memory::setString(AKEY_ORCH_CONVID, v); }

String providerPriority() { return solide::memory::getString(AKEY_PROV_PRIORITY, "openai,anthropic,mistral"); }
String orchPriority()     { return providerPriority(); }
String subPriority()      { return solide::memory::getString(AKEY_SUB_PRIORITY, "openai,anthropic,mistral"); }

static String defaultModelFor(const String& provider) {
  if (provider == "openai")    return OPENAI_MODEL;
  if (provider == "anthropic") return ANT_MODEL;
  if (provider == "mistral")   return MISTRAL_MODEL;
  if (provider == "custom")    return customModel();
  return "";
}

String orchModel(const String& provider) {
  String key = String(AKEY_ORCH_MODEL_PFX) + provider;
  String v = solide::memory::getString(key.c_str(), "");
  return v.length() ? v : defaultModelFor(provider);
}
String subModel(const String& provider) {
  String key = String(AKEY_SUB_MODEL_PFX) + provider;
  String v = solide::memory::getString(key.c_str(), "");
  return v.length() ? v : defaultModelFor(provider);
}
String agentFabricCfg() {
  return solide::memory::getString(AKEY_AGENT_FABRIC, "code:openai,research:openai,ops:anthropic");
}

// ---- directive + TTS + benign config ----
String sysPrompt() { return solide::memory::getString(AKEY_SYS_PROMPT, ""); }
bool   ttsEnabled() { return solide::memory::getBool(AKEY_TTS_ENABLED, TTS_ENABLED_DEFAULT); }
// P6: the tool loop is THE turn path now - default ON (single-shot survives only
// as the heap-pressure fallback in runTurn). Caps are user-tunable + clamped.
bool   orchToolLoop() { return solide::memory::getBool(AKEY_ORCH_TOOLLOOP, true); }
bool   codeSandbox() { return solide::memory::getBool(AKEY_CODE_SANDBOX, false); }
bool   midTurnFailover() { return solide::memory::getBool(AKEY_MID_FAILOVER, true); }
bool   orchPromptV2() { return solide::memory::getBool(AKEY_ORCH_PROMPTV2, false); }
bool   orchTrace() { return solide::memory::getBool(AKEY_ORCH_TRACE, true); }
void   setOrchTrace(bool v) { solide::memory::setBool(AKEY_ORCH_TRACE, v); }
static int clampI(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
int  orchLoopRounds()    { return clampI(solide::memory::getInt(AKEY_LOOP_ROUNDS, ORCH_LOOP_MAX_ROUNDS), 1, 32); }
int  orchLoopDeadlineS() { return clampI(solide::memory::getInt(AKEY_LOOP_DEADLINE, ORCH_LOOP_DEADLINE_MS / 1000), 30, 3600); }
// Ceilings raised 2026-08-03 so the token budget is dialable for a PSRAM stress test
// (the accumulating context is PSRAM-backed). The internal-SRAM re-gate + wall-clock
// deadline stay the real limits regardless of these caps.
// 0 = key ABSENT = auto: the engine derives the cap from the head model's
// context window (deriveBudget - 8192/65536 at the 200K anchor, i.e. identical
// to the old macro defaults on today's fleet). A SET key is the owner's
// override and wins verbatim under the same clamps.
int  orchLoopResultCap() { int v = solide::memory::getInt(AKEY_LOOP_RESCAP, 0); return v > 0 ? clampI(v, 512, 65536) : 0; }
int  orchLoopTotalCap()  { int v = solide::memory::getInt(AKEY_LOOP_TOTCAP, 0); return v > 0 ? clampI(v, 2048, 1048576) : 0; }
void setOrchLoopRounds(int v)    { solide::memory::setInt(AKEY_LOOP_ROUNDS, clampI(v, 1, 32)); }
void setOrchLoopDeadlineS(int v) { solide::memory::setInt(AKEY_LOOP_DEADLINE, clampI(v, 30, 3600)); }
// Setter ceilings MUST match the getter clamps above (65536 / 1048576) - else a
// dialed-up value is silently capped here. Raised 2026-08-04 to allow a true ~200K-token
// (~800 KB) context stress: the accumulation + request body are PSRAM-backed (128 B extmem
// threshold), so the ceiling is the provider window, not internal SRAM. Defaults unchanged.
// ⚠ 0 is the AUTO sentinel the getters report - the setters must round-trip it,
// or posting the value the API just returned silently pins the 512/2048 minimum
// (prism 2026-08-05). Mirrors MemConfig::setMaxContextBytes.
void setOrchLoopResultCap(int v) { solide::memory::setInt(AKEY_LOOP_RESCAP, v <= 0 ? 0 : clampI(v, 512, 65536)); }
void setOrchLoopTotalCap(int v)  { solide::memory::setInt(AKEY_LOOP_TOTCAP, v <= 0 ? 0 : clampI(v, 2048, 1048576)); }

// Local Loops governor overrides (CUM-73). Stored raw (0 = no override); the
// tighten-only fold against the caps.h defaults happens in clampLoopCaps at the
// loops subsystem. Getters guard against a negative NVS value (treated as unset)
// and an absurd upper bound so a stray write can never overflow the caps math.
int  loopCapMaxCount()        { int v = solide::memory::getInt(AKEY_LOOP_MAXCNT, 0); return v > 0 ? clampI(v, 1, 1000) : 0; }
int  loopCapMinIntervalS()    { int v = solide::memory::getInt(AKEY_LOOP_MINIVL, 0); return v > 0 ? clampI(v, 1, 86400) : 0; }
int  loopCapFiresPerDay()     { int v = solide::memory::getInt(AKEY_LOOP_FIRES, 0);  return v > 0 ? clampI(v, 1, 100000) : 0; }
int  loopCapTokensPerDay()    { int v = solide::memory::getInt(AKEY_LOOP_TOKENS, 0); return v > 0 ? clampI(v, 1, 100000000) : 0; }
int  loopCapDevTokensPerDay() { int v = solide::memory::getInt(AKEY_LOOP_DEVTOK, 0); return v > 0 ? clampI(v, 1, 100000000) : 0; }
int  loopCapDevFiresWindow()  { int v = solide::memory::getInt(AKEY_LOOP_DEVFIR, 0); return v > 0 ? clampI(v, 1, 100000) : 0; }
void setLoopCapMaxCount(int v)        { solide::memory::setInt(AKEY_LOOP_MAXCNT, v <= 0 ? 0 : clampI(v, 1, 1000)); }
void setLoopCapMinIntervalS(int v)    { solide::memory::setInt(AKEY_LOOP_MINIVL, v <= 0 ? 0 : clampI(v, 1, 86400)); }
void setLoopCapFiresPerDay(int v)     { solide::memory::setInt(AKEY_LOOP_FIRES,  v <= 0 ? 0 : clampI(v, 1, 100000)); }
void setLoopCapTokensPerDay(int v)    { solide::memory::setInt(AKEY_LOOP_TOKENS, v <= 0 ? 0 : clampI(v, 1, 100000000)); }
void setLoopCapDevTokensPerDay(int v) { solide::memory::setInt(AKEY_LOOP_DEVTOK, v <= 0 ? 0 : clampI(v, 1, 100000000)); }
void setLoopCapDevFiresWindow(int v)  { solide::memory::setInt(AKEY_LOOP_DEVFIR, v <= 0 ? 0 : clampI(v, 1, 100000)); }

// The per-tool-result clamp a turn will actually apply (owner override, else the
// value derived from the resolved head model's window). The results.get view
// must fit inside THIS together with its header.
int effectiveToolResultCap() {
  nimbus::orch::BudgetOverrides ov;
  ov.toolResultCap = orchLoopResultCap();
  const String h = resolvedOrchHost();
  return (int)nimbus::orch::deriveBudget(
             nimbus::orch::modelCtxTokens(h.c_str(), orchModel(h).c_str()), ov)
      .toolResultBytes;
}
bool   allowHwTests() { return solide::memory::getBool(AKEY_ALLOW_HW_TESTS, true); }  // default ON
bool   onboarded()    { return solide::memory::getBool(AKEY_ONBOARDED, false); }      // default false => wizard
void   setOnboarded(bool v) { solide::memory::setBool(AKEY_ONBOARDED, v); }
// Default flipped 2 -> 1 (2026-08-05, MEASURED): a 2nd concurrent work-TLS
// (e.g. a web/MCP embed) running beside a heavy fan-out turn collapsed the head
// turn's largest contiguous internal block (intLargest 27K -> 8K) and the send
// failed - with 1 slot the same turn completes (Board 1, the 6-way synthesis
// that failed at tlsSlots=2 succeeds at 1). The cost is throughput: an STT
// upload now serializes behind a running turn. Reliability > that. 2 remains
// selectable for a board with headroom.
int    tlsSlots() { int v = solide::memory::getInt(AKEY_TLS_SLOTS, 1); return v < 1 ? 1 : (v > 2 ? 2 : v); }
void   setTlsSlots(int v) { solide::memory::setInt(AKEY_TLS_SLOTS, v < 1 ? 1 : (v > 2 ? 2 : v)); }
bool   tlsVerify() { return solide::memory::getBool(AKEY_TLS_VERIFY, true); }  // default ON: validate provider certs
void   setTlsVerify(bool v) { solide::memory::setBool(AKEY_TLS_VERIFY, v); }
int    capProbe() { int v = solide::memory::getInt(AKEY_CAP_PROBE, 1); return v < 0 ? 0 : (v > 2 ? 2 : v); }
int    fetchPolicy() { int v = solide::memory::getInt(AKEY_FETCH_POL, 1); return v < 0 ? 1 : (v > 3 ? 1 : v); }
void   setFetchPolicy(int v) { solide::memory::setInt(AKEY_FETCH_POL, v < 0 ? 1 : (v > 3 ? 1 : v)); }
bool   modInbound()  { return solide::memory::getInt(AKEY_MOD_INBOUND, 0) != 0; }
bool   modOutbound() { return solide::memory::getInt(AKEY_MOD_OUTBOUND, 0) != 0; }
bool   modInjection(){ return solide::memory::getInt(AKEY_MOD_INJECTION, 0) != 0; }
void   setModInbound(bool on)  { solide::memory::setInt(AKEY_MOD_INBOUND, on ? 1 : 0); }
void   setModOutbound(bool on) { solide::memory::setInt(AKEY_MOD_OUTBOUND, on ? 1 : 0); }
void   setModInjection(bool on){ solide::memory::setInt(AKEY_MOD_INJECTION, on ? 1 : 0); }
void   setCapProbe(int v) { solide::memory::setInt(AKEY_CAP_PROBE, v < 0 ? 0 : (v > 2 ? 2 : v)); }
int    capProbeHours() { int v = solide::memory::getInt(AKEY_CAP_PROBE_H, 24); return v < 1 ? 1 : (v > 168 ? 168 : v); }
void   setCapProbeHours(int v) { solide::memory::setInt(AKEY_CAP_PROBE_H, v < 1 ? 1 : (v > 168 ? 168 : v)); }
// ---- OTA firmware update (rollback guard state + knobs; src/sys/ota_update) ----
int    otaPending()   { return solide::memory::getInt(AKEY_OTA_PENDING, 0); }
void   setOtaPending(int v) { solide::memory::setInt(AKEY_OTA_PENDING, v ? 1 : 0); }
int    otaBootCount() { return solide::memory::getInt(AKEY_OTA_BOOTS, 0); }
void   setOtaBootCount(int v) { solide::memory::setInt(AKEY_OTA_BOOTS, v < 0 ? 0 : v); }
String otaPrevSlot()  { return solide::memory::has(AKEY_OTA_PREV) ? solide::memory::getString(AKEY_OTA_PREV, "") : String(); }
void   setOtaPrevSlot(const String& v) { solide::memory::setString(AKEY_OTA_PREV, v); }
String otaLastResult(){ return solide::memory::has(AKEY_OTA_LASTRES) ? solide::memory::getString(AKEY_OTA_LASTRES, "") : String(); }
void   setOtaLastResult(const String& v) { solide::memory::setString(AKEY_OTA_LASTRES, v); }
bool   otaAutoUpdate(){ return solide::memory::getBool(AKEY_OTA_AUTO, false); }  // default OFF
void   setOtaAutoUpdate(bool v) { solide::memory::setBool(AKEY_OTA_AUTO, v); }
String otaNotifiedVersion() { return solide::memory::has(AKEY_OTA_NOTIF) ? solide::memory::getString(AKEY_OTA_NOTIF, "") : String(); }
String otaPendingNotes() { return solide::memory::has(AKEY_OTA_NOTES) ? solide::memory::getString(AKEY_OTA_NOTES, "") : String(); }
void   setOtaPendingNotes(const String& v) { solide::memory::setString(AKEY_OTA_NOTES, v); }
void   setOtaNotifiedVersion(const String& v) { solide::memory::setString(AKEY_OTA_NOTIF, v); }
String batteryModelState() { return solide::memory::getString(AKEY_BATT_MODEL, ""); }
void   setBatteryModelState(const String& v) { solide::memory::setString(AKEY_BATT_MODEL, v); }
uint32_t lowBattPingEpoch() { return (uint32_t)solide::memory::getInt(AKEY_LOWBATT_PING, 0); }
void     setLowBattPingEpoch(uint32_t e) { solide::memory::setInt(AKEY_LOWBATT_PING, (long)e); }
// Model-settable `priority` targets the SUB-SESSION list (AKEY_SUB_PRIORITY), never
// the orchestrator-host list (AKEY_PROV_PRIORITY / providerPriority). Keeping the
// host list off any model-writable path preserves the "model can't redirect its own
// brain" invariant that also blocks the orchHost/fabric protected config keys.
void   setSubPriority(const String& v) { solide::memory::setString(AKEY_SUB_PRIORITY, v); }
int    ledBright() { return solide::memory::getInt("ledBright", 128); }
void   setLedBright(int v) { solide::memory::setInt("ledBright", v); }

// ---- Telegram ----
String  telegramToken()     { return solide::memory::getString(AKEY_TG_TOKEN, ""); }
String  telegramAllowlist() { return solide::memory::getString(AKEY_TG_ALLOWLIST, ""); }
String  telegramOwners()    { return solide::memory::getString(AKEY_TG_OWNERS, ""); }
void    setTelegramOwners(const String& v) { solide::memory::setString(AKEY_TG_OWNERS, v); }
String  telegramNames()     { return solide::memory::getString(AKEY_TG_NAMES, ""); }
String  tgBotName()         { return solide::memory::getString(AKEY_TG_BOTNAME, ""); }
void    setTgBotName(const String& v) { solide::memory::setString(AKEY_TG_BOTNAME, v); }
void    setTelegramNames(const String& v) { solide::memory::setString(AKEY_TG_NAMES, v); }
void    replaceTelegramName(const String& id, const String& name) {
  // Rebuild the "id:name,..." sidecar dropping every EXACT-id entry, then append the
  // new mapping. The old writer blind-appended on each approve, so a re-add with a
  // new name never renamed (the reader matched the FIRST entry) and the blob grew
  // unbounded (owner rename ask, 2026-07-16). Caller sanitizes `name`.
  String nm = telegramNames(), out;
  int s = 0;
  while (s < (int)nm.length()) {
    int e = nm.indexOf(',', s); if (e < 0) e = nm.length();
    String entry = nm.substring(s, e); entry.trim();
    s = e + 1;
    if (!entry.length()) continue;
    int colon = entry.indexOf(':');
    String eid = colon > 0 ? entry.substring(0, colon) : entry;
    if (eid == id) continue;              // drop every stale mapping for this id
    out += (out.length() ? "," : "") + entry;
  }
  if (name.length()) out += (out.length() ? "," : "") + id + ":" + name;
  setTelegramNames(out);
}
bool    telegramPublic()    { return solide::memory::getBool(AKEY_TG_PUBLIC, false); }
void    setTelegramPublic(bool v) { solide::memory::setBool(AKEY_TG_PUBLIC, v); }
int32_t telegramOffset()    { return solide::memory::getInt(AKEY_TG_OFFSET, 0); }
void    setTelegramOffset(int32_t v) { solide::memory::setInt(AKEY_TG_OFFSET, v); }

// ---- Anthropic managed-agents caches ----
String antEnvId()      { return solide::memory::getString(AKEY_ANT_ENVID, ""); }
void   setAntEnvId(const String& v)    { solide::memory::setString(AKEY_ANT_ENVID, v); }
String antAgentMap()   { return solide::memory::getString(AKEY_ANT_AGENTMAP, ""); }
void   setAntAgentMap(const String& v) { solide::memory::setString(AKEY_ANT_AGENTMAP, v); }
String antOrchAgent()  { return solide::memory::getString(AKEY_ANT_ORCHAGENT, ""); }
void   setAntOrchAgent(const String& v){ solide::memory::setString(AKEY_ANT_ORCHAGENT, v); }

// ---- HUMAN-ONLY setters (see the credential-gate note in store.h) -----------
void setOpenaiKey(const String& v)    { solide::memory::setString(AKEY_OPENAI_KEY, v); }
void setAnthropicKey(const String& v) { solide::memory::setString(AKEY_ANTHROPIC_KEY, v); }
void setMistralKey(const String& v)   { solide::memory::setString(AKEY_MISTRAL_KEY, v); }
void setTavilyKey(const String& v)    { solide::memory::setString(AKEY_TAVILY_KEY, v); }
void setCustomBase(const String& v)   { solide::memory::setString(AKEY_CUSTOM_BASE, v); }
void setCustomKey(const String& v)    { solide::memory::setString(AKEY_CUSTOM_KEY, v); }
void setCustomConv(const String& v)   { solide::memory::setString(AKEY_CUSTOM_CONV, v); }
void setCustomModel(const String& v)  { solide::memory::setString(AKEY_CUSTOM_MODEL, v); }
void setZaiKey(const String& v)       { solide::memory::setString(AKEY_ZAI_KEY, v); }
void setCumuloKey(const String& v)    { solide::memory::setString(AKEY_CUMULO_KEY, v); }
void setCumuloBase(const String& v)   { solide::memory::setString(AKEY_CUMULO_BASE, v); }
void setFallbackRulesJson(const String& json) { solide::memory::setString(AKEY_FALLBACK_RULES, json); }
void setFallbackSyncTs(uint32_t ts)   { solide::memory::setInt(AKEY_FALLBACK_SYNC, (int)ts); }
void setOrchHost(const String& v)     { solide::memory::setString(AKEY_ORCH_HOST, v); }
void setProviderPriority(const String& v) { solide::memory::setString(AKEY_PROV_PRIORITY, v); }
void setSysPrompt(const String& v)    { solide::memory::setString(AKEY_SYS_PROMPT, v); }
void setOrchModel(const String& provider, const String& v) {
  String key = String(AKEY_ORCH_MODEL_PFX) + provider;
  solide::memory::setString(key.c_str(), v);
}
void setSubModel(const String& provider, const String& v) {
  String key = String(AKEY_SUB_MODEL_PFX) + provider;
  solide::memory::setString(key.c_str(), v);
}
// Live model choices harvested from the provider's /v1/models by the verify pass
// (owner 2026-07-16: the static lists were stale - no Opus 4.8 / Fable / new-gen
// OpenAI). "" = never harvested; the static defaults stay the fallback.
String modelChoices(const String& provider) {
  String key = String("mch_") + provider;
  return solide::memory::getString(key.c_str(), "");
}
void setModelChoices(const String& provider, const String& csv) {
  String key = String("mch_") + provider;
  solide::memory::setString(key.c_str(), csv);
}
String modelCatalogJson(const String& provider) {
  String key = String("mcat_") + provider;
  return solide::memory::getString(key.c_str(), "");
}
void setModelCatalogJson(const String& provider, const String& json) {
  String key = String("mcat_") + provider;
  solide::memory::setString(key.c_str(), json);
}
void setTelegramToken(const String& v)     { solide::memory::setString(AKEY_TG_TOKEN, v); }
void setTelegramAllowlist(const String& v) { solide::memory::setString(AKEY_TG_ALLOWLIST, v); }
void setTtsEnabled(bool v)                 { solide::memory::setBool(AKEY_TTS_ENABLED, v); }
void setOrchToolLoop(bool v)               { solide::memory::setBool(AKEY_ORCH_TOOLLOOP, v); }
void setCodeSandbox(bool v)                { solide::memory::setBool(AKEY_CODE_SANDBOX, v); }
void setMidTurnFailover(bool v)            { solide::memory::setBool(AKEY_MID_FAILOVER, v); }
void setAllowHwTests(bool v)               { solide::memory::setBool(AKEY_ALLOW_HW_TESTS, v); }

// ---- provider key-verification cache ("R:TS" per provider, see agent_config) -
static String verifyKey(const String& provider) {
  return String(AKEY_VERIFY_PFX) + provider;
}
int8_t verifyResult(const String& provider) {
  String v = solide::memory::getString(verifyKey(provider).c_str(), "");
  if (!v.length()) return -1;                 // never verified (ts()==0 confirms)
  return (int8_t)v.substring(0, v.indexOf(':')).toInt();
}
uint32_t verifyTs(const String& provider) {
  String v = solide::memory::getString(verifyKey(provider).c_str(), "");
  int sep = v.indexOf(':');
  if (sep < 0) return 0;
  return (uint32_t)v.substring(sep + 1).toInt();
}
void setVerify(const String& provider, int8_t r, uint32_t ts) {
  String v = String((int)r) + ":" + String(ts);
  solide::memory::setString(verifyKey(provider).c_str(), v);
}

// ---- voice provider config (STT/TTS; freely changeable, unlike embed) --------
String sttProvider() { return solide::memory::getString(AKEY_STT_PROVIDER, STT_DEFAULT_PROVIDER); }
String ttsProvider() { return solide::memory::getString(AKEY_TTS_PROVIDER, TTS_DEFAULT_PROVIDER); }
String ttsVoice()    { return solide::memory::getString(AKEY_TTS_VOICE, ""); }
String theme()       { return solide::memory::getString(AKEY_THEME, "teal"); }
// Default "eink": a fresh or NVS-erased device must come up on the shipped,
// proven panel, never on a driver its hardware may not have.
String screenModel() {
  const String v = solide::memory::getString(AKEY_SCREEN_MODEL, "eink");
  return v == "tft" ? v : String("eink");
}
bool   screenIsTft() { return screenModel() == "tft"; }
// The display/input drivers bind ONCE at boot, so the stored screenModel can
// diverge from what is actually driving the panel after the wizard changes it
// (the change needs a restart to take effect). main.cpp records the bound
// driver here at boot; RAM-only, never persisted. -1 = not yet recorded.
static int s_bootScreenIsTft = -1;
void setBootScreenIsTft(bool v) { s_bootScreenIsTft = v ? 1 : 0; }
int  bootScreenIsTft() { return s_bootScreenIsTft; }
String touchCal()    { return solide::memory::getString(AKEY_TOUCH_CAL, ""); }
void   setTouchCal(const String& v) {
  // Validated by the caller (nimbus::touch::parseCal). Stored verbatim so a
  // round-trip is byte-exact and an empty string means "use the defaults".
  solide::memory::setString(AKEY_TOUCH_CAL, v);
}
uint8_t sfxLevelNotif() { return (uint8_t)solide::memory::getInt(AKEY_SFX_LVL_NOTIF, 0); }
uint8_t sfxLevelOrch()  { return (uint8_t)solide::memory::getInt(AKEY_SFX_LVL_ORCH, 2); }
String  sfxTheme()      { return solide::memory::getString(AKEY_SFX_THEME, "pulse"); }
uint8_t sfxVolume()     { int v = solide::memory::getInt(AKEY_SFX_VOL, 50); return v < 0 ? 0 : (v > 100 ? 100 : (uint8_t)v); }
uint16_t compactAtKB() {
  int v = solide::memory::getInt(AKEY_COMPACT_KB, 48);
  if (v <= 0) return 0;                       // 0 = auto-fold off (manual /compact stays)
  return v < 8 ? 8 : (v > 512 ? 512 : (uint16_t)v);
}
void setCompactAtKB(uint16_t v) { solide::memory::setInt(AKEY_COMPACT_KB, v > 512 ? 512 : v); }
// Idle minutes before the screen rests. An explicit owner setting always wins.
//
// The backlight is the largest continuous draw on the device, so an hour of full
// brightness at an empty desk is the single most wasteful thing a battery board
// can do. A tap wakes it instantly, so a short default rest costs the owner
// nothing. The sentinel matters: getInt(key, 5) cannot tell "unset" from "the
// owner chose 5", so the fallback has to be resolved from a value that cannot be set.
uint16_t saverMin() {
  int v = solide::memory::getInt(AKEY_SAVER_MIN, -1);
  if (v < 0) v = 5;   // default 5 min: the backlight is the device's largest idle draw
  return v < 0 ? 0 : (v > 1440 ? 1440 : (uint16_t)v);
}
// ⚠ ceiling 6800, NOT 8000: a truly-full pack reads ~7900 mV RAW (S3 ADC top-band
// compression), so any threshold >= ~7900 is ALWAYS satisfied -> instant sleep on a
// full pack, USB invisible (no VBUS pin) = a soft-bricked bench board (review).
uint32_t battRtop()     { long v = solide::memory::getInt(AKEY_BATT_RTOP, 220000); return v < 1000 ? 1000 : (v > 10000000 ? 10000000 : (uint32_t)v); }
uint32_t battRbot()     { long v = solide::memory::getInt(AKEY_BATT_RBOT, 100000); return v < 1000 ? 1000 : (v > 10000000 ? 10000000 : (uint32_t)v); }
uint16_t battDividerX100() { uint32_t rt = battRtop(), rb = battRbot(); uint32_t d = (uint64_t(rt + rb) * 100) / (rb ? rb : 1); return d < 100 ? 100 : (d > 2000 ? 2000 : (uint16_t)d); }
uint16_t battCapMah()   { int v = solide::memory::getInt(AKEY_BATT_CAPMAH, 3500); return v < 100 ? 100 : (v > 20000 ? 20000 : (uint16_t)v); }
String   battChem()     { return solide::memory::getString(AKEY_BATT_CHEM, "liion"); }   // "liion" | "lifepo4"
uint8_t  battCellsOvr() { int v = solide::memory::getInt(AKEY_BATT_CELLS, 0); return (v == 1 || v == 2) ? (uint8_t)v : 0; }  // 0 = board default
String   battCurve()    { return solide::memory::getString(AKEY_BATT_CURVE, ""); }        // "" = chemistry default curve
// Effective series-cell count for the sleep/wake VOLTAGE thresholds: an owner
// override wins, else the board map (both boards set it; guard to 1). The sleep +
// wake mV are PACK voltages, so their defaults AND clamp ceilings must scale with
// cells - a 2S 6000 mV floor is above the whole 1S range and would insta-sleep a
// full 1S board the moment it left USB (CUM-202).
static uint8_t battCellsEff() { uint8_t o = battCellsOvr(); if (o) return o; uint8_t c = solide::board().batt.cells; return c ? c : 1; }
uint16_t sleepMv()      { const uint8_t n = battCellsEff(); const uint16_t ceil = nimbus::power::sleepMvCeilFor(n);
                          int v = solide::memory::getInt(AKEY_SLEEP_MV, nimbus::power::sleepMvDefaultFor(n)); return v < 0 ? 0 : (v > ceil ? ceil : (uint16_t)v); }
uint16_t wakeMv()       { const uint8_t n = battCellsEff(); const uint16_t ceil = nimbus::power::wakeMvCeilFor(n);
                          int v = solide::memory::getInt(AKEY_WAKE_MV, nimbus::power::wakeMvDefaultFor(n)); return v < 0 ? 0 : (v > ceil ? ceil : (uint16_t)v); }
bool     sleepOvr()     { return solide::memory::getInt(AKEY_SLEEP_OVR, 0) != 0; }
bool     brightOvr()    { return solide::memory::getInt(AKEY_BRIGHT_OVR, 0) != 0; }
// Owner default OFF: a ring breathing red all night is the brightest thing the
// device does, held exactly when there is least power to spend on it.
// Which end of the LANDSCAPE panel is up. Cannot be detected in software - it is
// purely how the module was mounted - so it is a setting rather than a build flag.
bool     tftFlip()      { return solide::memory::getInt(AKEY_TFT_FLIP, 0) != 0; }
bool     lowBattRing()  { return solide::memory::getInt(AKEY_LOWBATT_RING, 0) != 0; }
// Owner default ON (note the 1): the T1 battery-mode switch is SHIPPED behaviour,
// so defaulting this off would silently remove a power saving from every device.
bool     lowBattSaver() { return solide::memory::getInt(AKEY_LOWBATT_SAVER, 1) != 0; }
// Battery monitoring on/off. def is the board-derived default (battery-native
// boards ON; all-in-one desk boards OFF/opt-in) - passed in by main so the store
// layer stays board-agnostic.
bool     battMon(bool def) { return solide::memory::getInt(AKEY_BATT_MON, def ? 1 : 0) != 0; }
void setSfxLevelNotif(uint8_t v) { solide::memory::setInt(AKEY_SFX_LVL_NOTIF, v > 3 ? 3 : v); }
void setSfxLevelOrch(uint8_t v)  { solide::memory::setInt(AKEY_SFX_LVL_ORCH, v > 3 ? 3 : v); }
void setSfxTheme(const String& v) {
  // Clamp at the store layer: g_theme feeds an SD path (`/sfx/<theme>/...`).
  // The web UI already allowlists, but a provision/NVS/test path must not be
  // able to poison it. Legacy themes (terran/protoss/zerg) stay storable -
  // absent pools fall through to general in the resolver.
  const bool known = v == "pulse" || v == "terran" || v == "protoss" || v == "zerg";
  solide::memory::setString(AKEY_SFX_THEME, known ? v.c_str() : "pulse");
}
void setSfxVolume(uint8_t v) { solide::memory::setInt(AKEY_SFX_VOL, v > 100 ? 100 : v); }
void setSaverMin(uint16_t v) { solide::memory::setInt(AKEY_SAVER_MIN, v > 1440 ? 1440 : v); }
void setBattRtop(uint32_t o) { solide::memory::setInt(AKEY_BATT_RTOP, int(o < 1000 ? 1000 : (o > 10000000 ? 10000000 : o))); }
void setBattRbot(uint32_t o) { solide::memory::setInt(AKEY_BATT_RBOT, int(o < 1000 ? 1000 : (o > 10000000 ? 10000000 : o))); }
void setBattCapMah(uint16_t m) { solide::memory::setInt(AKEY_BATT_CAPMAH, m < 100 ? 100 : (m > 20000 ? 20000 : m)); }
void setBattChem(const String& slug) { solide::memory::setString(AKEY_BATT_CHEM, slug == "lifepo4" ? "lifepo4" : "liion"); }
void setBattCells(uint8_t cells) { solide::memory::setInt(AKEY_BATT_CELLS, (cells == 1 || cells == 2) ? cells : 0); }
void setBattCurve(const String& csv) { solide::memory::setString(AKEY_BATT_CURVE, csv); }
void setSleepMv(uint16_t v)  { const uint16_t ceil = nimbus::power::sleepMvCeilFor(battCellsEff()); solide::memory::setInt(AKEY_SLEEP_MV, v > ceil ? ceil : v); }
void setWakeMv(uint16_t v)   { const uint16_t ceil = nimbus::power::wakeMvCeilFor(battCellsEff());  solide::memory::setInt(AKEY_WAKE_MV,  v > ceil ? ceil : v); }
void setSleepOvr(bool on)    { solide::memory::setInt(AKEY_SLEEP_OVR, on ? 1 : 0); }
void setBrightOvr(bool on)   { solide::memory::setInt(AKEY_BRIGHT_OVR, on ? 1 : 0); }
void setTftFlip(bool on)      { solide::memory::setInt(AKEY_TFT_FLIP, on ? 1 : 0); }
void setLowBattRing(bool on)  { solide::memory::setInt(AKEY_LOWBATT_RING, on ? 1 : 0); }
void setLowBattSaver(bool on) { solide::memory::setInt(AKEY_LOWBATT_SAVER, on ? 1 : 0); }
void setBattMon(bool on) { solide::memory::setInt(AKEY_BATT_MON, on ? 1 : 0); }
String connectorsJson() { return solide::memory::getString(AKEY_CONNECTORS, ""); }
void   setConnectorsJson(const String& v) { solide::memory::setString(AKEY_CONNECTORS, v); }
void   setTheme(const String& v)       { solide::memory::setString(AKEY_THEME, v); }
void   setScreenModel(const String& v) {
  // Clamp at the store layer (same reasoning as setSfxTheme): this value picks
  // a display AND an input driver at boot, so an unrecognised slug must fall
  // back to the shipped panel rather than leave the device with no UI at all.
  solide::memory::setString(AKEY_SCREEN_MODEL, v == "tft" ? "tft" : "eink");
}
void   setSttProvider(const String& v) { solide::memory::setString(AKEY_STT_PROVIDER, v); }
void   setTtsProvider(const String& v) { solide::memory::setString(AKEY_TTS_PROVIDER, v); }
void   setTtsVoice(const String& v)    { solide::memory::setString(AKEY_TTS_VOICE, v); }

// ---- embedding config (set-once; the caller enforces the reset-before-change) -
String embedProvider() { return solide::memory::getString(AKEY_EMBED_PROVIDER, EMBED_DEFAULT_PROVIDER); }
String embedModel()    { return solide::memory::getString(AKEY_EMBED_MODEL, EMBED_DEFAULT_MODEL); }
int    embedDims()     { return solide::memory::getInt(AKEY_EMBED_DIMS, EMBED_DEFAULT_DIMS); }
bool   embedLocked()   { return solide::memory::getBool(AKEY_EMBED_LOCKED, false); }
void   setEmbedConfig(const String& provider, const String& model, int dims) {
  solide::memory::setString(AKEY_EMBED_PROVIDER, provider);
  solide::memory::setString(AKEY_EMBED_MODEL, model);
  solide::memory::setInt(AKEY_EMBED_DIMS, dims);
}
void   setEmbedLocked(bool v) { solide::memory::setBool(AKEY_EMBED_LOCKED, v); }

// ---- per-provider usage ledger + budgets -------------------------------------
// The ledger is loaded from NVS once, cached in RAM, and re-serialized to NVS after
// each mutation (turns are seconds apart at minimum, so per-turn NVS writes are cheap
// and bounded). The mutex serializes the turn task's record vs the web task's read.
namespace {
std::mutex             g_ledgerMx;
nimbus::orch::UsageLedger g_ledger;
bool                   g_ledgerLoaded = false;

nimbus::orch::UsageLedger& ledgerLocked() {  // caller holds g_ledgerMx
  if (!g_ledgerLoaded) {
    String blob = solide::memory::getString(AKEY_USAGE_LEDGER, "");
    g_ledger.deserialize(std::string(blob.c_str()));
    g_ledgerLoaded = true;
  }
  return g_ledger;
}

void persistLedgerLocked() {  // caller holds g_ledgerMx
  std::string s = g_ledger.serialize();
  solide::memory::setString(AKEY_USAGE_LEDGER, s.c_str());
}

// Current rolling-month key for a provider's reset day, from the SNTP wall clock.
// Before the clock syncs (year < 2021) everything accumulates in period 0 and rolls
// to the real period once time is set - no spurious mid-life reset, no fake spend.
uint32_t currentPeriodKey(uint8_t resetDay) {
  time_t now = time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);
  int year = tmv.tm_year + 1900;
  if (year < 2021) return 0;
  return nimbus::orch::usagePeriodKey(year, tmv.tm_mon + 1, tmv.tm_mday, resetDay);
}

// UTC days-since-epoch for the daily history buckets; 0 while the clock is unsynced
// (the history core refuses dayKey 0, so pre-SNTP spend is simply not graphed -
// it still lands in the ledger's period counters).
uint32_t currentDayKey() {
  time_t now = time(nullptr);
  if (now < 1609459200) return 0;   // 2021-01-01 - same sanity bar as the period key
  return (uint32_t)(now / 86400);
}

// ---- daily usage history (the graphs' time series) --------------------------
// Cached in RAM under the same mutex as the ledger; persisted to LittleFS
// (/data/usage_hist.txt - up to ~30 KB at the 720-bucket cap, far past the ~4 KB
// NVS value ceiling). Loaded lazily; rewritten after each mutation (turns are
// minutes apart at best - bounded wear, same policy as /data/loops.json).
constexpr const char* kUsageHistPath = "/data/usage_hist.txt";
nimbus::orch::UsageHistory g_hist;
bool                       g_histLoaded = false;

nimbus::orch::UsageHistory& histLocked() {  // caller holds g_ledgerMx
  if (!g_histLoaded) {
    File f = LittleFS.open(kUsageHistPath, "r");
    if (f) {
      String blob = f.readString();
      f.close();
      g_hist.deserialize(std::string(blob.c_str()));
    }
    g_histLoaded = true;
  }
  return g_hist;
}

void persistHistLocked() {  // caller holds g_ledgerMx
  LittleFS.mkdir("/data");
  File f = LittleFS.open(kUsageHistPath, "w");
  if (!f) return;
  std::string s = g_hist.serialize();
  f.write((const uint8_t*)s.data(), s.size());
  f.close();
}
}  // namespace

void recordProviderTokens(const String& provider, uint32_t tokensIn, uint32_t tokensOut) {
  recordProviderTokens(provider, tokensIn, tokensOut, "");   // untagged
}

void recordProviderTokens(const String& provider, uint32_t tokensIn, uint32_t tokensOut,
                          const char* tag) {
  recordProviderTokens(provider, tokensIn, tokensOut, 0, 0, tag);
}

void recordProviderTokens(const String& provider, uint32_t tokensIn, uint32_t tokensOut,
                          uint32_t cacheRead, uint32_t cacheWrite, const char* tag) {
  if (provider.length() == 0 ||
      (tokensIn == 0 && tokensOut == 0 && cacheRead == 0 && cacheWrite == 0))
    return;
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  auto& L = ledgerLocked();
  const std::string prov(provider.c_str());
  const auto* e = L.find(prov);
  uint8_t rd = e ? e->resetDay : 1;
  L.recordTokens(prov, tokensIn, tokensOut, cacheRead, cacheWrite, currentPeriodKey(rd),
                 std::string(tag ? tag : ""));
  persistLedgerLocked();
  // Daily bucket for the graphs (skipped pre-SNTP: dayKey 0 is refused by the core).
  const uint32_t day = currentDayKey();
  if (day) {
    auto& H = histLocked();
    H.record(prov, day, tokensIn, tokensOut, 0);
    H.prune(day, 60);
    persistHistLocked();
  }
}

void recordProviderCall(const String& provider) {
  if (provider.length() == 0) return;
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  auto& L = ledgerLocked();
  const std::string prov(provider.c_str());
  const auto* e = L.find(prov);
  uint8_t rd = e ? e->resetDay : 1;
  L.recordCall(prov, currentPeriodKey(rd));
  persistLedgerLocked();
  const uint32_t day = currentDayKey();
  if (day) {
    auto& H = histLocked();
    H.record(prov, day, 0, 0, 1);
    H.prune(day, 60);
    persistHistLocked();
  }
}

void setProviderRates(const String& provider, uint32_t centsPerMIn,
                      uint32_t centsPerMOut, uint32_t centsPerKCalls) {
  if (provider.length() == 0) return;
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  ledgerLocked().setRates(std::string(provider.c_str()), centsPerMIn, centsPerMOut,
                          centsPerKCalls);
  persistLedgerLocked();
}

char* usageHistoryJsonPs(size_t& outLen) {
  outLen = 0;
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  auto& H = histLocked();
  // Build into ONE PSRAM allocation (review HIGH: an internal-heap String of up to
  // ~50 KB at the bucket cap - plus the response copy - was exactly the near-OOM
  // class v2.5.1 fixed; big dynamic payloads go to PSRAM, streamed chunked).
  // 128 B/entry upper-bounds the widest bucket line; +48 for the envelope.
  const size_t cap = 48 + H.entries().size() * 128;
  char* buf = (char*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  if (!buf) buf = (char*)heap_caps_malloc(cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!buf) return nullptr;
  size_t off = (size_t)snprintf(buf, cap, "{\"today\":%lu,\"days\":[",
                                (unsigned long)currentDayKey());
  bool first = true;
  for (const auto& e : H.entries()) {
    int n = snprintf(buf + off, cap - off,
                     "%s{\"d\":%lu,\"prov\":\"%s\",\"in\":%llu,\"out\":%llu,\"calls\":%lu}",
                     first ? "" : ",", (unsigned long)e.dayKey, e.prov.c_str(),
                     (unsigned long long)e.tokIn, (unsigned long long)e.tokOut,
                     (unsigned long)e.calls);
    if (n < 0 || off + (size_t)n >= cap - 3) break;   // bound respected by sizing; belt anyway
    off += (size_t)n;
    first = false;
  }
  off += (size_t)snprintf(buf + off, cap - off, "]}");
  outLen = off;
  return buf;
}

bool providerOverBudget(const String& provider) {
  if (provider.length() == 0) return false;
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  // Dated form (prism: the forever-gated wedge) - a stale periodKey reads as a
  // FRESH period, so "wait for the reset day" actually comes true even though
  // a gated provider never records. Unsynced clock (pre-SNTP) falls back to
  // the strict form: never un-gate on a clock we don't trust.
  time_t now = time(nullptr);
  if (now < 1600000000) return ledgerLocked().overBudget(std::string(provider.c_str()));
  struct tm tmv;
  localtime_r(&now, &tmv);
  return ledgerLocked().overBudget(std::string(provider.c_str()),
                                   tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
}

void setProviderBudget(const String& provider, uint64_t tokenLimit,
                       uint32_t callLimit, uint8_t resetDay, uint64_t centsLimit) {
  if (provider.length() == 0) return;
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  ledgerLocked().setLimits(std::string(provider.c_str()), tokenLimit, callLimit, resetDay,
                           centsLimit);
  persistLedgerLocked();
}

String providerBudgetJson() {
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  auto& L = ledgerLocked();
  String out = "[";
  bool first = true;
  for (const auto& e : L.entries()) {
    if (!first) out += ",";
    first = false;
    out += "{\"prov\":\"";     out += e.name.c_str();
    out += "\",\"in\":";       out += String((double)e.tokensIn, 0);
    out += ",\"out\":";         out += String((double)e.tokensOut, 0);
    out += ",\"calls\":";       out += String(e.calls);
    out += ",\"estCents\":";    out += String((double)e.estCents(), 0);
    out += ",\"centsLimit\":";  out += String((double)e.centsLimit, 0);
    out += ",\"tokenLimit\":";  out += String((double)e.tokenLimit, 0);
    out += ",\"callLimit\":";   out += String(e.callLimit);
    out += ",\"resetDay\":";    out += String(e.resetDay);
    out += ",\"over\":";        out += (L.overBudget(e.name) ? "true" : "false");
    out += "}";
  }
  out += "]";
  return out;
}

String providerUsageJson() {
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  auto& L = ledgerLocked();
  String out = "[";
  bool first = true;
  for (const auto& e : L.entries()) {
    if (!first) out += ",";
    first = false;
    out += "{\"prov\":\"";
    out += e.name.c_str();
    out += "\",\"tokens\":";     out += String((double)e.tokens, 0);
    out += ",\"calls\":";        out += String(e.calls);
    out += ",\"tokenLimit\":";   out += String((double)e.tokenLimit, 0);
    out += ",\"callLimit\":";    out += String(e.callLimit);
    out += ",\"resetDay\":";     out += String(e.resetDay);
    out += ",\"over\":";         out += (L.overBudget(e.name) ? "true" : "false");
    out += ",\"tokIn\":";        out += String((double)e.tokensIn, 0);
    out += ",\"tokOut\":";       out += String((double)e.tokensOut, 0);
    out += ",\"totIn\":";        out += String((double)e.totalIn, 0);
    out += ",\"totOut\":";       out += String((double)e.totalOut, 0);
    out += ",\"totCalls\":";     out += String((double)e.totalCalls, 0);
    out += ",\"rateIn\":";       out += String(e.centsPerMIn);
    out += ",\"rateOut\":";      out += String(e.centsPerMOut);
    out += ",\"rateCall\":";     out += String(e.centsPerKCalls);
    // W16: the $ ceiling + the SERVER-computed period spend estimate (cents) -
    // one formula (ProviderBudget::estCents) shared with the budget gate and
    // device.status, so no surface can disagree with another.
    out += ",\"centsLimit\":";   out += String((double)e.centsLimit, 0);
    out += ",\"estCents\":";     out += String((double)e.estCents(), 0);
    // Spend attribution (additive): all-time per-tag split for this provider -
    // turn / synthesis / loop:<id> / spawn:<backend>. Bounded (kMaxTags total).
    out += ",\"tags\":[";
    bool tFirst = true;
    for (const auto& t : L.tagEntries()) {
      if (t.prov != e.name) continue;
      if (!tFirst) out += ",";
      tFirst = false;
      out += "{\"tag\":\"";
      out += t.tag.c_str();
      out += "\",\"in\":";  out += String((double)t.tokIn, 0);
      out += ",\"out\":";   out += String((double)t.tokOut, 0);
      out += "}";
    }
    out += "]}";
  }
  out += "]";
  return out;
}

}  // namespace store
}  // namespace agent
