#include "nimbus/orch/budget.h"    // deriveBudget - the effective tool-result cap
#include "nimbus/orch/caps.h"      // kMemDirectiveMax - owner-directive byte cap
#include "nimbus/orch/compact.h"   // modelCtxTokens
#include "store.h"
#include "nimbus/device_identity.h"   // makeSetupPass - the setup-AP passphrase generator
#include "nimbus/power/power_policy.h"
#include "nimbus/power/board_power.h"   // clampBattCellsOverride - guard 1S/2S override (CUM-371)

#include <esp_random.h>
#include <esp_heap_caps.h>   // PSRAM buffer for the usage-history JSON payload
#include <solide/memory.h>
#include <solide/board.h>   // batt.cells: per-cell sleep/wake threshold scaling (CUM-202)
#include <LittleFS.h>   // daily usage history (/data/usage_hist.txt - too big for NVS)
#include <nvs.h>        // nvs_entry_find - the reserve's eviction pass lists key names

#include <ctime>
#include <mutex>

#include "agent_config.h"
#include "fs_atomic.h"                     // writeFileAtomic - the usage files (tmp + rename)
#include "provider_gate.h"                // the daily spend notice (L19-2)
#include "nimbus/orch/connectors_wire.h"   // resolveHeadHost - the one head-host rule
#include "nimbus/orch/model_catalog.h"     // the model store line format (lane F)
#include "nimbus/orch/model_choices.h"     // effectiveModelChoices - the priceable models
#include "nimbus/orch/model_defaults.h"    // role defaults (lane DEF): head, sub, background
#include "spend_ceiling_store.h"           // the monthly spend ceiling counts every record
#include "version.h"                       // NIMBUS_FW_VERSION - the models file header
#include "nimbus/orch/provider_slots.h"
#include "nimbus/orch/usage_ledger.h"
#include "nimbus/orch/usage_models_json.h" // GET /api/usage/models body
#include "nimbus/store/provider_cache.h"   // CUM-238: bounded provider-config reads
#include "nimbus/store/persist.h"          // CUM-479: write verdicts + catalog placement
#include "nimbus/store/nvs_reserve.h"      // the reserve: key classes + eviction plan
#include "nimbus/store/moved_store.h"      // the usage ledger's move out of NVS
#include "loops_subsystem.h"               // live routine ids: deleted routines' ledger rows go
#include "../sys/ota_update.h"             // imageValidAtBoot - when the legacy ledger copy may go
#include <ArduinoJson.h>
#include "../sys/agent_log.h"              // alogc + errlog::cat - loud NVS-full seed hint (CUM-408)
#include "../sys/config_nvs.h"             // notePersist - the persist ledger (CUM-479)

// Device config accessors backed by solide::memory (NVS). Defaults mirror
// Nuage-Solide storage.cpp: priorities default to "openai,anthropic,mistral"
// (Mistral host ported 2026-07), per-provider models default to that
// provider's flagship. A stored NVS value always wins over these defaults.

namespace agent {
namespace store {

// ---- write helpers (CUM-479) --------------------------------------------------
// EVERY NVS write in this file goes through one of these, never a bare
// solide::memory set/erase call: each reports its verdict to the persist ledger
// (nimbus::sys::notePersist), which logs the failing key once and lets a save route's
// SaveScope answer HTTP 507 storage_full instead of {"ok":true} over a dropped write.
// The verdict logic itself is portable (nimbus::store::writeString & co., host-tested
// against a full-partition fake in test_nvs_full); test_nvs_full also scans this file
// for a bare write outside this block, so a new setter cannot silently skip the ledger
// again (the void-setter class of CUM-479 / CUM-389 / CUM-408).
namespace {
using nimbus::sys::notePersist;
using nimbus::store::NvsValKind;

// The solide::memory seam the portable primitives run on. Raw: every helper below
// asks the reserve (nimbus::sys::nvsAdmit) for the WHOLE write first, so the
// erase-then-write step inside writeShrinkSafe is never refused halfway.
nimbus::sys::NvsKvRaw g_nvs;

// May a write of `bytes` to `key` grow NVS here? Erases and shrinking rewrites always
// may; a refusal reads as a failed write (507 through a SaveScope, logged by key).
bool admit(const char* key, NvsValKind kind, size_t bytes) {
  return nimbus::sys::nvsAdmit(nimbus::sys::kNvsNsSolide, key, kind, bytes);
}

bool putStr(const char* key, const String& v) {
  return notePersist(admit(key, NvsValKind::Str, v.length()) &&
                         nimbus::store::writeString(g_nvs, key, v),
                     key);
}
bool clearKey(const char* key) { return notePersist(nimbus::store::eraseLanded(g_nvs, key), key); }
bool putOrClear(const char* key, const String& v) {
  if (!v.length()) return clearKey(key);
  return putStr(key, v);
}
bool putStrShrinkSafe(const char* key, const String& v) {
  if (!v.length()) return clearKey(key);
  return notePersist(admit(key, NvsValKind::Str, v.length()) &&
                         nimbus::store::writeShrinkSafe(g_nvs, key, v),
                     key);
}
bool putInt(const char* key, int32_t v) {
  return notePersist(admit(key, NvsValKind::Int, 0) && solide::memory::setInt(key, v), key);
}
bool putBool(const char* key, bool v) {
  return notePersist(admit(key, NvsValKind::Int, 0) && solide::memory::setBool(key, v), key);
}
// A value equal to its getter's default, for a key that is still ABSENT, needs no
// write: it already reads back as that value. The Save buttons post every field at
// its current value, so without this a full device would answer 507 over defaults
// nobody changed - and a device whose owner just freed room would spend it on them.
bool putIntDef(const char* key, int32_t v, int32_t def) {
  return (v == def && !solide::memory::has(key)) || putInt(key, v);
}
bool putBoolDef(const char* key, bool v, bool def) {
  return (v == def && !solide::memory::has(key)) || putBool(key, v);
}

// A string read that never logs. Preferences::getString() prints an [E] NOT_FOUND
// line for an absent key, and a clear is now an ERASE (absent reads as the default),
// so every string getter reads through has() - the quiet idiom pcRead introduced for
// CUM-299. Values are unchanged; only the log noise goes.
String getStrQuiet(const char* key, const char* def) {
  return nimbus::sys::nvsGetStr(key, def);
}
}  // namespace

// ---- provider-config read cache (CUM-238) -----------------------------------
// The credentials + routing keys below are polled on hot paths (the screen
// render resolves the active host every frame; the engine re-checks provider
// readiness each turn). Reading NVS per call spammed hundreds of getString()
// NOT_FOUND hits/second on a fresh device. Snapshot them once and serve reads
// from RAM; every provider-config SETTER marks the snapshot dirty so the next
// read refreshes. A factory reset erases NVS and reboots, so the cache is always
// rebuilt fresh on boot - there is no live-erase path to leave it stale.
// The fill itself reads each key through has() (see pcRead) so an unset key on a
// fresh/wiped device does not even emit the one-per-fill getString NOT_FOUND [E]
// line - the cache bounds the read COUNT, has() keeps that bounded read QUIET
// (CUM-299).
namespace {
using PSnap = ::nimbus::store::ProviderSnapshot;
std::mutex g_pcMx;
::nimbus::store::ProviderCache g_pc;

// Read a provider-config key WITHOUT the Preferences [E] log that a bare
// getString() emits on every read of a MISSING key. Arduino Preferences::getString
// calls log_e("nvs_get_str len fail: <key> NOT_FOUND") whenever the key is absent
// (Preferences.cpp:506); has() routes through isKey()/getType(), which probes the
// value types silently and returns false for an absent key, so we return the
// default without ever entering getString's error path. On a fresh or wiped device
// every provider key is unset, so the plain reads spat a burst of NOT_FOUND [E]
// lines on each cache fill - the console flood (orchHost + siblings) during exactly
// the first-run diagnosis that matters most (CUM-299). The cache already bounds
// this to one fill per config write; this makes that one fill quiet too. Since
// CUM-479 every string getter in this file reads the same way (getStrQuiet).
String pcRead(const char* key, const char* def) {
  return getStrQuiet(key, def);
}

// The one place that actually reads NVS for these keys. Defaults here mirror the
// per-getter defaults exactly (custom conv "openai", the priority lists).
void pcFill(::nimbus::store::ProviderSnapshot& s) {
  s.openaiKey        = pcRead(AKEY_OPENAI_KEY, "").c_str();
  s.anthropicKey     = pcRead(AKEY_ANTHROPIC_KEY, "").c_str();
  s.mistralKey       = pcRead(AKEY_MISTRAL_KEY, "").c_str();
  s.tavilyKey        = pcRead(AKEY_TAVILY_KEY, "").c_str();
  s.zaiKey           = pcRead(AKEY_ZAI_KEY, "").c_str();
  s.zaiBase          = pcRead(AKEY_ZAI_BASE, "").c_str();
  s.cumuloKey        = pcRead(AKEY_CUMULO_KEY, "").c_str();
  s.cumuloBase       = pcRead(AKEY_CUMULO_BASE, "").c_str();
  s.customBase       = pcRead(AKEY_CUSTOM_BASE, "").c_str();
  s.customKey        = pcRead(AKEY_CUSTOM_KEY, "").c_str();
  s.customConv       = pcRead(AKEY_CUSTOM_CONV, "openai").c_str();
  s.customModel      = pcRead(AKEY_CUSTOM_MODEL, "").c_str();
  s.orchHost         = pcRead(AKEY_ORCH_HOST, "").c_str();
  s.providerPriority = pcRead(AKEY_PROV_PRIORITY, "openai,anthropic,mistral").c_str();
  s.subPriority      = pcRead(AKEY_SUB_PRIORITY, "openai,anthropic,mistral").c_str();
  s.fallbackRules    = pcRead(AKEY_FALLBACK_RULES, "").c_str();
}

// Copy one cached field out under the store lock.
String pcGet(std::string PSnap::* field) {
  std::lock_guard<std::mutex> lk(g_pcMx);
  return String((g_pc.snapshot(pcFill).*field).c_str());
}
void pcInvalidate() {
  std::lock_guard<std::mutex> lk(g_pcMx);
  g_pc.markDirty();
}
// A provider-config write: invalidate the snapshot whatever the verdict (a failed
// write leaves the old value, which the refresh then reads back), pass the verdict on.
bool pcWrite(bool ok) {
  pcInvalidate();
  return ok;
}
}  // namespace

// ---- credentials ----
String openaiKey()    { return pcGet(&PSnap::openaiKey); }
bool   hasOpenaiKey() { return openaiKey().length() > 0; }
String anthropicKey()    { return pcGet(&PSnap::anthropicKey); }
bool   hasAnthropicKey() { return anthropicKey().length() > 0; }
String mistralKey()   { return pcGet(&PSnap::mistralKey); }
bool   hasMistralKey(){ return mistralKey().length() > 0; }
String tavilyKey()    { return pcGet(&PSnap::tavilyKey); }
String deviceTz()     { return getStrQuiet(AKEY_DEVICE_TZ, ""); }
bool   setDeviceTz(const String& tz) { return putOrClear(AKEY_DEVICE_TZ, tz); }
String dreamScratchHash() { return getStrQuiet(AKEY_DREAM_SCRATCH, ""); }
bool   hasTavilyKey() { return tavilyKey().length() > 0; }

// Per-device web/MCP auth token. Generated (96-bit hardware-random) on FIRST use and
// persisted, so it is stable across reboots + unique per device - never a shipped
// constant. Required on state-changing web POSTs + /mcp. Read-only surface: there is
// no setter (the device owns it).
//
// This value is NEVER shown to the owner and never travels in a URL or a QR: CUM-45
// took it out of both, because a URL is committed to browser history before any
// script runs and browsers sync history across machines. The Config QR and the panel
// "Show code" screen carry a SINGLE-USE, TTL-bounded sign-in code instead
// (signin_codes.h); a caller trades that code for this token exactly once, at
// POST /api/signin/exchange (webui.cpp), which is the only route by which it ever
// leaves the device. The comment here used to say "Shown to the owner via the Config
// QR" - the pre-CUM-45 behaviour. Wake-run #17 corrected four other sites that made
// the same stale claim (webui.h, webui.cpp, agent_config.h, test_console.cpp) but not
// this one, the token's own definition and so the most authoritative of them; six
// consecutive wake-runs read claims of this shape and told the owner to read a
// durable token off a QR that has not carried one since CUM-45.
String webAuthToken() {
  String t = getStrQuiet(AKEY_WEB_TOKEN, "");
  if (t.length() == 0) {
    char buf[25];
    for (int i = 0; i < 24; i += 8) snprintf(buf + i, 9, "%08x", (unsigned)esp_random());
    buf[24] = 0;
    t = buf;
    putStr(AKEY_WEB_TOKEN, t);
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
  // A rotation that does not land leaves the OLD token in force: hand the old one
  // back, so the calling browser never swaps to a token the device will refuse
  // (POST /api/token/regen also answers 507 through its SaveScope).
  if (!putStr(AKEY_WEB_TOKEN, buf)) return webAuthToken();
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
  String p = getStrQuiet(AKEY_AP_PASS, "");
  if (p.length() < 8) {  // unset, or too short for WPA2 (softAP would silently fall back OPEN)
    p = String(nimbus::identity::makeSetupPass(esp_random).c_str());
    putStr(AKEY_AP_PASS, p);
  }
  cached = p;
  return cached;
}

// --- Cloud relay (cumulo-nimbus) pairing state -----------------------------
bool cloudOptIn() { return solide::memory::getInt(AKEY_CLOUD_OPTIN, 0) != 0; }
bool setCloudOptIn(bool on) { return putInt(AKEY_CLOUD_OPTIN, on ? 1 : 0); }
String cloudDeviceId() { return getStrQuiet(AKEY_CLOUD_DEVID, ""); }
String cloudCred() { return getStrQuiet(AKEY_CLOUD_CRED, ""); }
String cloudHost() { return getStrQuiet(AKEY_CLOUD_HOST, "app.cumulo-nimbus.ai"); }
String cloudName() { return getStrQuiet(AKEY_CLOUD_NAME, ""); }
bool cloudPaired() { return cloudDeviceId().length() > 0 && cloudCred().length() > 0; }
bool setCloudPairing(const String& deviceId, const String& cred, const String& host,
                     const String& name) {
  auto writeAll = [&]() -> bool {
    putStr(AKEY_CLOUD_DEVID, deviceId);
    putStr(AKEY_CLOUD_CRED, cred);
    if (host.length()) putStr(AKEY_CLOUD_HOST, host);
    putOrClear(AKEY_CLOUD_NAME, name);
    // The pairing is usable iff BOTH the id and the credential read back (cloudPaired()).
    // Verify against the store rather than trusting the individual verdicts.
    return cloudDeviceId() == deviceId && cloudCred() == cred;
  };
  if (writeAll()) return true;
  // NVS-starved. The credential is critical and load-bearing. Since CUM-479 the
  // rebuildable mcat_ catalog caches live in flash files and the boot heal evicts
  // any legacy copy, so this eviction normally frees nothing - it stays as the
  // backstop for a device that was downgraded and re-upgraded mid-session.
  evictLegacyCatalogCaches();
  return writeAll();
}
bool clearCloudPairing() {
  const bool a = clearKey(AKEY_CLOUD_DEVID);
  const bool b = clearKey(AKEY_CLOUD_CRED);
  const bool c = clearKey(AKEY_CLOUD_NAME);
  return a && b && c;
}

String customBase()  { return pcGet(&PSnap::customBase); }
String customKey()   { return pcGet(&PSnap::customKey); }
String customConv()  { return pcGet(&PSnap::customConv); }
String customModel() { return pcGet(&PSnap::customModel); }
bool   hasCustom()   { return customBase().length() > 0; }

String zaiKey()      { return pcGet(&PSnap::zaiKey); }
bool   hasZaiKey()   { return zaiKey().length() > 0; }
String zaiBase()     { return pcGet(&PSnap::zaiBase); }
bool   setZaiBase(const String& host) { return pcWrite(putOrClear(AKEY_ZAI_BASE, host)); }
String cumuloKey()   { return pcGet(&PSnap::cumuloKey); }
bool   hasCumuloKey(){ return cumuloKey().length() > 0; }
String cumuloBase()  { return pcGet(&PSnap::cumuloBase); }
String   fallbackRulesJson() { return pcGet(&PSnap::fallbackRules); }
uint32_t fallbackSyncTs()    { return (uint32_t)solide::memory::getInt(AKEY_FALLBACK_SYNC, 0); }

// ---- routing + models ----
String orchHost() { return pcGet(&PSnap::orchHost); }

// The host a turn ACTUALLY runs on, by the ONE rule the engine dispatches with
// (nimbus::orch::resolveHeadHost): the pinned orchHost while it has a key, else the
// first KEYED provider in priority, else the keyed router (Cumulo, then Z.ai), else
// any other keyed provider; only with no key anywhere the pin or the raw head (a
// fresh device mid-onboarding, where every turn is refused locally anyway).
// HOST-FALLBACK (22 Oct): a pinned host whose key was cleared used to win here and
// in the engine, so the turn dialed it keyless (HTTP 401).
// ⚠ Use this (never raw orchHost()) wherever the model's context window is resolved -
// orchHost is "" by default, and modelCtxTokens("") falls back to the conservative
// 100K default, which silently HALVED the derived brief/fold-slice budgets and made
// the web UI report the wrong effective caps (prism 2026-08-05).
bool providerHasKey(const String& name) {
  String n = name; n.trim();
  if (n == "openai")    return hasOpenaiKey();
  if (n == "anthropic") return hasAnthropicKey();
  if (n == "mistral")   return hasMistralKey();
  if (n == "cumulo")    return hasCumuloKey();
  if (n == "zai")       return hasZaiKey();
  if (n == "custom")    return hasCustom();
  return false;
}
String resolvedOrchHost() {
  const std::string rf = hasCumuloKey() ? "cumulo" : hasZaiKey() ? "zai" : "";
  return String(nimbus::orch::resolveHeadHost(
                    std::string(orchHost().c_str()), std::string(providerPriority().c_str()),
                    [](const std::string& h) { return providerHasKey(String(h.c_str())); }, rf)
                    .c_str());
}
String orchConvId() { return getStrQuiet(AKEY_ORCH_CONVID, ""); }
bool   setOrchConvId(const String& v) { return putOrClear(AKEY_ORCH_CONVID, v); }

String providerPriority() { return pcGet(&PSnap::providerPriority); }
String orchPriority()     { return providerPriority(); }
String subPriority()      { return pcGet(&PSnap::subPriority); }

// The model a role runs on when the owner picked none (lane DEF, owner requirement 21
// Oct): the ONE role-default table (nimbus/orch/model_defaults.h), picked by price
// class from this provider's catalog. A custom endpoint runs its one configured model.
// CUM-288 holds for every slot with a row: a router head (Cumulo, Z.ai) resolves to
// its default model, never "", so the budget derives from the real window.
static String defaultModelFor(const String& provider, nimbus::orch::DefaultRole role) {
  if (provider == "custom") return customModel();
  const char* p = provider.c_str();
  if (!nimbus::orch::roleDefaultRow(p, role)) return "";
  const String harvested = modelChoices(provider);   // "" = never harvested
  const std::string list =
      nimbus::orch::effectiveModelChoices(p, std::string(harvested.c_str()));
  return nimbus::orch::roleDefaultModel(p, role, list, harvested.length() > 0).c_str();
}

String orchModel(const String& provider) {
  // A background turn (a routine, a wakeup, the dream) runs on the cheapest tier
  // whatever the head is (the 22 Oct silent-routine cost: a routine on the priciest
  // head spent 3K thinking tokens and sent nothing). Thread-local: a web request on
  // another task meanwhile still reads the owner's configured head.
  if (nimbus::orch::currentTurnRole() == nimbus::orch::TurnRole::Background) {
    const String bg = defaultModelFor(provider, nimbus::orch::DefaultRole::Background);
    if (bg.length()) return bg;
  }
  String key = String(AKEY_ORCH_MODEL_PFX) + provider;
  String v = getStrQuiet(key.c_str(), "");
  return v.length() ? v : defaultModelFor(provider, nimbus::orch::DefaultRole::Head);
}
String subModel(const String& provider) {
  String key = String(AKEY_SUB_MODEL_PFX) + provider;
  String v = getStrQuiet(key.c_str(), "");
  return v.length() ? v : defaultModelFor(provider, nimbus::orch::DefaultRole::Sub);
}
String captionModel(const String& provider) {
  return defaultModelFor(provider, nimbus::orch::DefaultRole::Caption);
}
String roleDefaultModel(const String& provider, nimbus::orch::DefaultRole role) {
  return defaultModelFor(provider, role);
}
String agentFabricCfg() {
  return getStrQuiet(AKEY_AGENT_FABRIC, "code:openai,research:openai,ops:anthropic");
}

// ---- directive + TTS + benign config ----
String sysPrompt() { return getStrQuiet(AKEY_SYS_PROMPT, ""); }
bool   ttsEnabled() { return solide::memory::getBool(AKEY_TTS_ENABLED, TTS_ENABLED_DEFAULT); }
// P6: the tool loop is THE turn path now - default ON (single-shot survives only
// as the heap-pressure fallback in runTurn). Caps are user-tunable + clamped.
bool   orchToolLoop() { return solide::memory::getBool(AKEY_ORCH_TOOLLOOP, true); }
bool   codeSandbox() { return solide::memory::getBool(AKEY_CODE_SANDBOX, false); }
bool   midTurnFailover() { return solide::memory::getBool(AKEY_MID_FAILOVER, true); }
bool   orchPromptV2() { return solide::memory::getBool(AKEY_ORCH_PROMPTV2, false); }
bool   orchTrace() { return solide::memory::getBool(AKEY_ORCH_TRACE, true); }
bool   setOrchTrace(bool v) { return putBoolDef(AKEY_ORCH_TRACE, v, true); }
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
bool setOrchLoopRounds(int v)    { return putIntDef(AKEY_LOOP_ROUNDS, clampI(v, 1, 32), ORCH_LOOP_MAX_ROUNDS); }
bool setOrchLoopDeadlineS(int v) { return putIntDef(AKEY_LOOP_DEADLINE, clampI(v, 30, 3600), ORCH_LOOP_DEADLINE_MS / 1000); }
// Setter ceilings MUST match the getter clamps above (65536 / 1048576) - else a
// dialed-up value is silently capped here. Raised 2026-08-04 to allow a true ~200K-token
// (~800 KB) context stress: the accumulation + request body are PSRAM-backed (128 B extmem
// threshold), so the ceiling is the provider window, not internal SRAM. Defaults unchanged.
// ⚠ 0 is the AUTO sentinel the getters report - the setters must round-trip it,
// or posting the value the API just returned silently pins the 512/2048 minimum
// (prism 2026-08-05). Mirrors MemConfig::setMaxContextBytes.
bool setOrchLoopResultCap(int v) { return putIntDef(AKEY_LOOP_RESCAP, v <= 0 ? 0 : clampI(v, 512, 65536), 0); }
bool setOrchLoopTotalCap(int v)  { return putIntDef(AKEY_LOOP_TOTCAP, v <= 0 ? 0 : clampI(v, 2048, 1048576), 0); }

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
bool setLoopCapMaxCount(int v)        { return putIntDef(AKEY_LOOP_MAXCNT, v <= 0 ? 0 : clampI(v, 1, 1000), 0); }
bool setLoopCapMinIntervalS(int v)    { return putIntDef(AKEY_LOOP_MINIVL, v <= 0 ? 0 : clampI(v, 1, 86400), 0); }
bool setLoopCapFiresPerDay(int v)     { return putIntDef(AKEY_LOOP_FIRES,  v <= 0 ? 0 : clampI(v, 1, 100000), 0); }
bool setLoopCapTokensPerDay(int v)    { return putIntDef(AKEY_LOOP_TOKENS, v <= 0 ? 0 : clampI(v, 1, 100000000), 0); }
bool setLoopCapDevTokensPerDay(int v) { return putIntDef(AKEY_LOOP_DEVTOK, v <= 0 ? 0 : clampI(v, 1, 100000000), 0); }
bool setLoopCapDevFiresWindow(int v)  { return putIntDef(AKEY_LOOP_DEVFIR, v <= 0 ? 0 : clampI(v, 1, 100000), 0); }

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
bool   setOnboarded(bool v) { return putBool(AKEY_ONBOARDED, v); }
// Default flipped 2 -> 1 (2026-08-05, MEASURED): a 2nd concurrent work-TLS
// (e.g. a web/MCP embed) running beside a heavy fan-out turn collapsed the head
// turn's largest contiguous internal block (intLargest 27K -> 8K) and the send
// failed - with 1 slot the same turn completes (Board 1, the 6-way synthesis
// that failed at tlsSlots=2 succeeds at 1). The cost is throughput: an STT
// upload now serializes behind a running turn. Reliability > that. 2 remains
// selectable for a board with headroom.
int    tlsSlots() { int v = solide::memory::getInt(AKEY_TLS_SLOTS, 1); return v < 1 ? 1 : (v > 2 ? 2 : v); }
bool   setTlsSlots(int v) { return putIntDef(AKEY_TLS_SLOTS, v < 1 ? 1 : (v > 2 ? 2 : v), 1); }
bool   tlsVerify() { return solide::memory::getBool(AKEY_TLS_VERIFY, true); }  // default ON: validate provider certs
bool   setTlsVerify(bool v) { return putBoolDef(AKEY_TLS_VERIFY, v, true); }
int    capProbe() { int v = solide::memory::getInt(AKEY_CAP_PROBE, 1); return v < 0 ? 0 : (v > 2 ? 2 : v); }
int    fetchPolicy() { int v = solide::memory::getInt(AKEY_FETCH_POL, 1); return v < 0 ? 1 : (v > 3 ? 1 : v); }
bool   setFetchPolicy(int v) { return putIntDef(AKEY_FETCH_POL, v < 0 ? 1 : (v > 3 ? 1 : v), 1); }
bool   modInbound()  { return solide::memory::getInt(AKEY_MOD_INBOUND, 0) != 0; }
bool   modOutbound() { return solide::memory::getInt(AKEY_MOD_OUTBOUND, 0) != 0; }
bool   modInjection(){ return solide::memory::getInt(AKEY_MOD_INJECTION, 0) != 0; }
bool   setModInbound(bool on)  { return putIntDef(AKEY_MOD_INBOUND, on ? 1 : 0, 0); }
bool   setModOutbound(bool on) { return putIntDef(AKEY_MOD_OUTBOUND, on ? 1 : 0, 0); }
bool   setModInjection(bool on){ return putIntDef(AKEY_MOD_INJECTION, on ? 1 : 0, 0); }
bool   setCapProbe(int v) { return putIntDef(AKEY_CAP_PROBE, v < 0 ? 0 : (v > 2 ? 2 : v), 1); }
int    capProbeHours() { int v = solide::memory::getInt(AKEY_CAP_PROBE_H, 24); return v < 1 ? 1 : (v > 168 ? 168 : v); }
bool   setCapProbeHours(int v) { return putIntDef(AKEY_CAP_PROBE_H, v < 1 ? 1 : (v > 168 ? 168 : v), 24); }
// ---- OTA firmware update (rollback guard state + knobs; src/sys/ota_update) ----
int    otaPending()   { return solide::memory::getInt(AKEY_OTA_PENDING, 0); }
bool   setOtaPending(int v) { return putInt(AKEY_OTA_PENDING, v ? 1 : 0); }
int    otaBootCount() { return solide::memory::getInt(AKEY_OTA_BOOTS, 0); }
bool   setOtaBootCount(int v) { return putInt(AKEY_OTA_BOOTS, v < 0 ? 0 : v); }
String otaPrevSlot()  { return getStrQuiet(AKEY_OTA_PREV, ""); }
bool   setOtaPrevSlot(const String& v) { return putOrClear(AKEY_OTA_PREV, v); }
String otaLastResult(){ return getStrQuiet(AKEY_OTA_LASTRES, ""); }
bool   setOtaLastResult(const String& v) { return putOrClear(AKEY_OTA_LASTRES, v); }
bool   otaAutoUpdate(){ return solide::memory::getBool(AKEY_OTA_AUTO, false); }  // default OFF
bool   setOtaAutoUpdate(bool v) { return putBool(AKEY_OTA_AUTO, v); }
bool   usbUpdateConfirm(){ return solide::memory::getBool(AKEY_OTA_USBCFM, false); }  // default OFF
bool   setUsbUpdateConfirm(bool v) { return putBool(AKEY_OTA_USBCFM, v); }
String otaNotifiedVersion() { return getStrQuiet(AKEY_OTA_NOTIF, ""); }
String otaPendingNotes() { return getStrQuiet(AKEY_OTA_NOTES, ""); }
bool   setOtaPendingNotes(const String& v) { return putOrClear(AKEY_OTA_NOTES, v); }
bool   setOtaNotifiedVersion(const String& v) { return putOrClear(AKEY_OTA_NOTIF, v); }
String batteryModelState() { return getStrQuiet(AKEY_BATT_MODEL, ""); }
bool   setBatteryModelState(const String& v) { return putOrClear(AKEY_BATT_MODEL, v); }
uint32_t lowBattPingEpoch() { return (uint32_t)solide::memory::getInt(AKEY_LOWBATT_PING, 0); }
bool     setLowBattPingEpoch(uint32_t e) { return putInt(AKEY_LOWBATT_PING, (int32_t)e); }
// Model-settable `priority` targets the SUB-SESSION list (AKEY_SUB_PRIORITY), never
// the orchestrator-host list (AKEY_PROV_PRIORITY / providerPriority). Keeping the
// host list off any model-writable path preserves the "model can't redirect its own
// brain" invariant that also blocks the orchHost/fabric protected config keys.
bool   setSubPriority(const String& v) { return pcWrite(putStr(AKEY_SUB_PRIORITY, v)); }
int    ledBright() { return solide::memory::getInt("ledBright", 128); }
bool   setLedBright(int v) { return putInt("ledBright", v); }

// ---- Telegram ----
String  telegramToken()     { return getStrQuiet(AKEY_TG_TOKEN, ""); }
String  telegramAllowlist() { return getStrQuiet(AKEY_TG_ALLOWLIST, ""); }
String  telegramOwners()    { return getStrQuiet(AKEY_TG_OWNERS, ""); }
// Shrink-safe like the allowlist: removing an owner must land on a full NVS too (a
// dropped removal would keep a removed chat an owner).
bool    setTelegramOwners(const String& v) { return putStrShrinkSafe(AKEY_TG_OWNERS, v); }
String  telegramNames()     { return getStrQuiet(AKEY_TG_NAMES, ""); }
String  tgBotName()         { return getStrQuiet(AKEY_TG_BOTNAME, ""); }
bool    setTgBotName(const String& v) { return putOrClear(AKEY_TG_BOTNAME, v); }
bool    setTelegramNames(const String& v) { return putOrClear(AKEY_TG_NAMES, v); }
bool    replaceTelegramName(const String& id, const String& name) {
  // Rebuild the "id:name,..." sidecar dropping every EXACT-id entry, then append the
  // new mapping. The old writer blind-appended on each approve, so a re-add with a
  // new name never renamed (the reader matched the FIRST entry) and the blob grew
  // unbounded (owner rename ask, 2026-07-16). Caller sanitizes `name`. Entries of
  // chats no longer on the allowlist are dropped too, so the sidecar stays bounded by
  // the allowlist (a removed chat's name is display data nobody can see).
  String allow = String(",") + telegramAllowlist() + ",";
  allow.replace(" ", "");
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
    eid.trim();
    if (allow.indexOf(String(",") + eid + ",") < 0) continue;   // no longer allow-listed
    out += (out.length() ? "," : "") + entry;
  }
  if (name.length()) out += (out.length() ? "," : "") + id + ":" + name;
  return setTelegramNames(out);
}
bool    telegramPublic()    { return solide::memory::getBool(AKEY_TG_PUBLIC, false); }
// Off is the default (an absent key reads false), so turning open access OFF is an
// erase: it lands even on a full NVS, where writing `false` could fail and leave the
// bot accepting anyone (the security direction must never be the one that drops).
bool    setTelegramPublic(bool v) { return v ? putBool(AKEY_TG_PUBLIC, true) : clearKey(AKEY_TG_PUBLIC); }
int32_t telegramOffset()    { return solide::memory::getInt(AKEY_TG_OFFSET, 0); }
// 0 is the default (an absent key reads 0), so a reset to 0 is an erase that lands on
// a full NVS too - a token clear resets the offset as part of the storage-full remedy.
bool    setTelegramOffset(int32_t v) { return v == 0 ? clearKey(AKEY_TG_OFFSET) : putInt(AKEY_TG_OFFSET, v); }

// ---- Anthropic managed-agents caches ----
String antEnvId()      { return getStrQuiet(AKEY_ANT_ENVID, ""); }
bool   setAntEnvId(const String& v)    { return putOrClear(AKEY_ANT_ENVID, v); }
String antAgentMap()   { return getStrQuiet(AKEY_ANT_AGENTMAP, ""); }
bool   setAntAgentMap(const String& v) { return putOrClear(AKEY_ANT_AGENTMAP, v); }
String antOrchAgent()  { return getStrQuiet(AKEY_ANT_ORCHAGENT, ""); }
bool   setAntOrchAgent(const String& v){ return putOrClear(AKEY_ANT_ORCHAGENT, v); }

// ---- HUMAN-ONLY setters (see the credential-gate note in store.h) -----------
bool setOpenaiKey(const String& v) { return pcWrite(putOrClear(AKEY_OPENAI_KEY, v)); }
bool setAnthropicKey(const String& v) { return pcWrite(putOrClear(AKEY_ANTHROPIC_KEY, v)); }
bool setMistralKey(const String& v) { return pcWrite(putOrClear(AKEY_MISTRAL_KEY, v)); }
bool setTavilyKey(const String& v) { return pcWrite(putOrClear(AKEY_TAVILY_KEY, v)); }
bool setCustomBase(const String& v) { return pcWrite(putOrClear(AKEY_CUSTOM_BASE, v)); }
bool setCustomKey(const String& v) { return pcWrite(putOrClear(AKEY_CUSTOM_KEY, v)); }
bool setCustomConv(const String& v)   {
  // "openai" is the default an absent key reads as: no write needed (see putIntDef).
  if (v == "openai" && !solide::memory::has(AKEY_CUSTOM_CONV)) return true;
  return pcWrite(putStr(AKEY_CUSTOM_CONV, v));
}
bool setCustomModel(const String& v) { return pcWrite(putOrClear(AKEY_CUSTOM_MODEL, v)); }
bool setZaiKey(const String& v) { return pcWrite(putOrClear(AKEY_ZAI_KEY, v)); }
bool setCumuloKey(const String& v) { return pcWrite(putOrClear(AKEY_CUMULO_KEY, v)); }
bool setCumuloBase(const String& v) { return pcWrite(putOrClear(AKEY_CUMULO_BASE, v)); }
bool setFallbackRulesJson(const String& json) { return pcWrite(putOrClear(AKEY_FALLBACK_RULES, json)); }
bool setFallbackSyncTs(uint32_t ts)   { return putInt(AKEY_FALLBACK_SYNC, (int32_t)ts); }
bool setOrchHost(const String& v) { return pcWrite(putOrClear(AKEY_ORCH_HOST, v)); }
bool setProviderPriority(const String& v) { return pcWrite(putStr(AKEY_PROV_PRIORITY, v)); }
bool setSysPrompt(const String& v)    {
  // Clamp to the owner-directive cap at the store layer (defense-in-depth: the
  // web route rejects oversize with an honest error first, and OrchMemory
  // re-caps UTF-8-safely on read - this just guarantees no path persists past
  // the cap). A byte substring is safe here because it is a backstop behind the
  // route guard, never the primary enforcement. "" (revert to the default
  // directive) is an erase: an absent key reads "" exactly like a stored one.
  const int cap = nimbus::orch::kMemDirectiveMax;
  return putOrClear(AKEY_SYS_PROMPT, v.length() > (unsigned)cap ? v.substring(0, cap) : v);
}
// "" = the provider default (the getters fall back on an empty OR absent value).
bool setOrchModel(const String& provider, const String& v) {
  String key = String(AKEY_ORCH_MODEL_PFX) + provider;
  return putOrClear(key.c_str(), v);
}
bool setSubModel(const String& provider, const String& v) {
  String key = String(AKEY_SUB_MODEL_PFX) + provider;
  return putOrClear(key.c_str(), v);
}
// ---- the model store (lane F): /data/models_<slug>.txt, never NVS ----------------
// One LittleFS file per provider holds every model its last fetch kept (line format,
// nimbus/orch/model_catalog.h) plus, in its header, the fetch time and the short list.
// The text is cached in RAM (one allocation per provider, PSRAM: over the 128 B
// extmem threshold), never as parsed rows: each read scans it in place, so no
// per-model string lives on the internal heap. Older builds kept the short list in
// NVS (mch_<slug>) and a JSON catalog in /data/mcat_<slug>.json; the JSON file is
// never written or deleted here, so a rollback still reads it (critique C2).
namespace {
std::mutex g_modelMx;
struct ModelStoreSlot {
  bool        loaded = false;
  bool        have = false;            // the text has a store header
  std::string text;
  nimbus::orch::ModelStoreMeta meta;   // its header, parsed once per load or write
};
ModelStoreSlot g_modelStore[nimbus::orch::kProviderSlotCount + 1];   // + custom

int modelSlot(const String& provider) {
  int i = 0, found = -1;
  nimbus::store::forEachCatalogProvider([&](const char* slug) {
    if (found < 0 && provider == slug) found = i;
    ++i;
  });
  return found;
}

// The slot, its file read once per boot (callers hold g_modelMx).
const ModelStoreSlot& modelStoreLocked(int slot, const String& provider) {
  ModelStoreSlot& s = g_modelStore[slot];
  if (s.loaded || !agent::flashMounted()) return s;
  s.loaded = true;
  const std::string path = nimbus::orch::modelStorePath(provider.c_str());
  if (path.empty() || !LittleFS.exists(path.c_str())) return s;
  File f = LittleFS.open(path.c_str(), "r");
  if (!f) return s;
  const size_t n = f.size();
  if (n > 0 && n <= nimbus::orch::kModelStoreMaxBytes + 1024) {
    s.text.resize(n);
    s.text.resize(f.read((uint8_t*)&s.text[0], n));
  }
  f.close();
  s.have = nimbus::orch::parseModelStoreHeader(s.text, s.meta);
  return s;
}
}  // namespace

std::string modelStoreText(const String& provider) {
  const int slot = modelSlot(provider);
  if (slot < 0) return std::string();
  std::lock_guard<std::mutex> lk(g_modelMx);
  return modelStoreLocked(slot, provider).text;
}

// The flash write runs outside g_modelMx (readers on the web and turn tasks never wait
// on it); a write mutex keeps two writers (the verify task, the boot merge) in order.
// The header is parsed before the lock is taken, so the swap is all the lock covers.
bool setModelStoreText(const String& provider, const std::string& text) {
  const int slot = modelSlot(provider);
  const std::string path = nimbus::orch::modelStorePath(provider.c_str());
  if (slot < 0 || path.empty()) return false;
  static std::mutex s_writeMx;
  std::lock_guard<std::mutex> wl(s_writeMx);
  LittleFS.mkdir("/data");
  if (!agent::writeFileAtomic(path.c_str(), text, "models")) return false;
  nimbus::orch::ModelStoreMeta meta;
  const bool have = nimbus::orch::parseModelStoreHeader(text, meta);
  std::string copy = text;
  std::lock_guard<std::mutex> lk(g_modelMx);
  ModelStoreSlot& s = g_modelStore[slot];
  s.text.swap(copy);
  s.meta = std::move(meta);
  s.have = have;
  s.loaded = true;
  return true;
}

// Read on hot paths (every role-default lookup, the prompt line): the cached header,
// never a copy of the file text.
String modelChoices(const String& provider) {
  const int slot = modelSlot(provider);
  if (slot < 0) return String();
  std::lock_guard<std::mutex> lk(g_modelMx);
  const ModelStoreSlot& s = modelStoreLocked(slot, provider);
  return s.have ? String(s.meta.choices.c_str()) : String();
}

String modelListNote(const String& provider) {
  const int slot = modelSlot(provider);
  if (slot < 0) return String();
  std::lock_guard<std::mutex> lk(g_modelMx);
  const ModelStoreSlot& s = modelStoreLocked(slot, provider);
  return String(nimbus::orch::modelListNote(s.have, s.meta).c_str());
}

// The check's sources other than the store text. The role defaults are read first:
// they take the store lock themselves (the short list).
static nimbus::orch::SelectableSources baseSources(const String& provider) {
  nimbus::orch::SelectableSources src;
  src.coldStart = nimbus::orch::staticModelChoices(provider.c_str());
  src.defaultModel = defaultModelFor(provider, nimbus::orch::DefaultRole::Head).c_str();
  src.subDefaultModel = defaultModelFor(provider, nimbus::orch::DefaultRole::Sub).c_str();
  src.now = nimbus::orch::calendarNowOrZero(time(nullptr));
  return src;
}

nimbus::orch::SelectableSources modelSources(const String& provider, std::string& text) {
  nimbus::orch::SelectableSources src = baseSources(provider);
  text = modelStoreText(provider);
  src.storeText = &text;
  return src;
}

bool modelSelectable(const String& provider, const String& id, nimbus::orch::PickRole role) {
  nimbus::orch::SelectableSources src = baseSources(provider);
  const int slot = modelSlot(provider);
  if (slot < 0) return nimbus::orch::modelSelectable(provider.c_str(), id.c_str(), role, src);
  std::lock_guard<std::mutex> lk(g_modelMx);   // scanned in place: no copy of the text
  src.storeText = &modelStoreLocked(slot, provider).text;
  return nimbus::orch::modelSelectable(provider.c_str(), id.c_str(), role, src);
}

int migrateModelStores() {
  if (!agent::flashMounted()) return 0;   // Notifier mode: nothing to merge into
  const bool imageValid = otaupd::imageValidAtBoot();
  int erased = 0, merged = 0;
  nimbus::store::forEachCatalogProvider([&](const char* slug) {
    const std::string key = std::string("mch_") + slug;
    const String csv = getStrQuiet(key.c_str(), "");
    const std::string jpath = nimbus::store::catalogCachePath(slug);
    const bool haveJson = !jpath.empty() && LittleFS.exists(jpath.c_str());
    const std::string existing = modelStoreText(slug);
    if (csv.length() || (existing.empty() && haveJson)) {
      std::string json;
      if (existing.empty() && haveJson) {   // read only: a rollback keeps reading it
        File f = LittleFS.open(jpath.c_str(), "r");
        if (f) {
          json = f.readString().c_str();
          f.close();
        }
      }
      const std::string text =
          nimbus::orch::mergeLegacyLists(slug, existing, json, csv.c_str(), NIMBUS_FW_VERSION);
      if (!text.empty() && setModelStoreText(slug, text)) ++merged;
    }
    if (nimbus::orch::legacyListErasable(imageValid, !modelStoreText(slug).empty(), csv.length() > 0) &&
        clearKey(key.c_str()))
      ++erased;
  });
  if (merged || erased) {
    nimbus::store::NvsUsage u;
    nimbus::sys::nvsUsage(u);
    agent::alogcf(nimbus::errlog::cat::kNvs,
                  "models: merged %d older list(s) into the model store, erased %d NVS list(s) (%u B free)",
                  merged, erased, (unsigned)u.freeBytes);
  }
  return erased;
}

// ---- legacy provider capability catalogs (CUM-479) --------------------------------
// Before lane F the catalogs were /data/mcat_<provider>.json (and before CUM-479 the
// NVS keys mcat_<provider>). This build never writes them; the boot heal below still
// carries a legacy NVS copy to its file (so a rollback finds it) and frees the key.
// Written to a temp file and renamed over the old one.
static bool writeCatalogFile(const std::string& path, const String& json) {
  LittleFS.mkdir("/data");
  const std::string tmp = path + ".tmp";
  File f = LittleFS.open(tmp.c_str(), "w");
  if (!f) return false;
  const size_t n = f.write((const uint8_t*)json.c_str(), json.length());
  f.close();
  if (n == json.length()) {
    if (LittleFS.rename(tmp.c_str(), path.c_str())) return true;   // lfs_rename replaces
    // A VFS that refuses to rename over an existing file: replace non-atomically.
    LittleFS.remove(path.c_str());
    if (LittleFS.rename(tmp.c_str(), path.c_str())) return true;
  }
  LittleFS.remove(tmp.c_str());
  return false;
}
int evictLegacyCatalogCaches() { return nimbus::store::evictLegacyCatalogs(g_nvs); }
int migrateCatalogCachesOffNvs() {
  // Carry each legacy NVS catalog to its file first (so GET /api/models keeps its
  // models until the next verify re-harvests), then evict every legacy key. The
  // eviction runs even when a copy fails: the cache is rebuildable, the room is not.
  // LittleFS is mounted only in Orchestrator mode (memory::begin via beginWeb); in
  // Notifier mode skip the copy rather than log a VFS error per provider.
  const bool fsUp = LittleFS.totalBytes() > 0;
  int moved = 0;
  nimbus::store::forEachCatalogProvider([&](const char* slug) {
    const std::string key = nimbus::store::legacyCatalogKey(slug);
    if (!fsUp || key.empty() || !solide::memory::has(key.c_str())) return;
    const std::string path = nimbus::store::catalogCachePath(slug);
    if (LittleFS.exists(path.c_str())) return;
    const String blob = getStrQuiet(key.c_str(), "");
    if (blob.length() && writeCatalogFile(path, blob)) ++moved;
  });
  const int evicted = evictLegacyCatalogCaches();
  if (evicted) {
    nimbus::store::NvsUsage u;
    nimbus::sys::nvsUsage(u);
    agent::alogcf(nimbus::errlog::cat::kNvs,
                  "nvs: moved %d catalog cache(s) to flash, freed %d NVS key(s) (%u B free)",
                  moved, evicted, (unsigned)u.freeBytes);
  }
  return evicted;
}
// ---- the NVS reserve: the main-loop eviction pass ---------------------------------
// Runs on the main loop (otaupd::tick), never inside a web route: a write refused by
// the reserve only raises a request (nvs_guard), and this pass erases rebuildable
// caches in the fixed order of nimbus::store::nvsEvictionPlan until the reserve is
// back. When it cannot be, the device is STARVED: owner-config writes are accepted
// again and only rebuildable and telemetry writes stay refused, said once per boot.
// The usage ledger's main-loop upkeep: retry a ledger the file did not take (once a
// minute while dirty, nimbus::store::DirtyRetry), and every 6 h drop the attribution rows of deleted routines
// (the live ids come from the loops subsystem, read with no ledger lock held).
static void usageMaintain(uint32_t nowMs) {
  static uint32_t s_nextPruneMs = 10u * 60u * 1000u;   // first pass at 10 min uptime
  retryUsageLedger();   // a no-op unless dirty and a minute has passed since the last try
  if ((int32_t)(nowMs - s_nextPruneMs) < 0) return;
  s_nextPruneMs = nowMs + 6u * 3600u * 1000u;
  // Not loaded (or Notifier mode): keep every row. A loaded subsystem always holds the
  // reserved nightly routine, so "every routine deleted" still reads count() >= 1 and
  // still prunes.
  if (loops::count() <= 0) return;
  JsonDocument d;
  if (deserializeJson(d, loops::loopsJson()) || !d.is<JsonArray>()) return;
  std::vector<std::string> ids;
  for (JsonObjectConst o : d.as<JsonArrayConst>()) {
    const char* id = o["id"] | "";
    if (*id) ids.emplace_back(id);
  }
  if (!ids.empty()) noteLiveLoopIds(ids);
}

void nvsMaintain(uint32_t nowMs) {
  usageMaintain(nowMs);
  static uint32_t s_nextMs = 0;
  static bool     s_starvedLogged = false;
  const bool requested = nimbus::sys::takeNvsEvictRequest();
  if (!requested && (int32_t)(nowMs - s_nextMs) < 0) return;
  s_nextMs = nowMs + 30000;   // a full device is re-checked twice a minute

  constexpr size_t kReserve = nimbus::store::kNvsReserveEntries;
  size_t avail = nimbus::sys::nvsAvailableEntries();
  if (avail >= kReserve) {
    nimbus::sys::setNvsStarved(false);
    return;
  }
  // The evictable keys stored right now (names only; NVS keys are <= 15 chars).
  constexpr size_t kMaxCand = 32;
  char names[kMaxCand][16];
  const char* keys[kMaxCand];
  size_t n = 0;
  nvs_iterator_t it = nullptr;
  esp_err_t e = nvs_entry_find(NVS_DEFAULT_PART_NAME, nimbus::sys::kNvsNsSolide,
                               NVS_TYPE_ANY, &it);
  while (e == ESP_OK && n < kMaxCand) {
    nvs_entry_info_t info = {};
    nvs_entry_info(it, &info);
    const nimbus::store::NvsKeyClass* row =
        nimbus::store::findNvsKeyClass(nimbus::sys::kNvsNsSolide, info.key);
    if (row && row->evictOrder > 0) {
      strncpy(names[n], info.key, sizeof(names[n]) - 1);
      names[n][sizeof(names[n]) - 1] = 0;
      keys[n] = names[n];
      ++n;
    }
    e = nvs_entry_next(&it);
  }
  nvs_release_iterator(it);   // erase only after the iterator is gone

  size_t order[kMaxCand];
  const size_t m = nimbus::store::nvsEvictionPlan(keys, n, fallbackSyncTs() != 0, order, kMaxCand);
  uint32_t evicted = 0;
  for (size_t i = 0; i < m && avail < kReserve; ++i) {
    const char* k = keys[order[i]];
    if (!clearKey(k)) continue;
    if (strcmp(k, AKEY_FALLBACK_RULES) == 0) pcInvalidate();   // a cached provider key
    ++evicted;
    avail = nimbus::sys::nvsAvailableEntries();
  }
  if (evicted) {
    nimbus::sys::noteNvsEvicted(evicted);
    agent::alogcf(nimbus::errlog::cat::kNvs,
                  "nvs: freed %u rebuildable key(s) for the reserve (%u of %u entries free)",
                  (unsigned)evicted, (unsigned)avail, (unsigned)kReserve);
  }
  const bool starved = avail < kReserve;
  nimbus::sys::setNvsStarved(starved);
  if (starved && !s_starvedLogged) {
    s_starvedLogged = true;
    agent::alogcf(nimbus::errlog::cat::kNvs,
                  "nvs: below the reserve after eviction (%u of %u entries free) - "
                  "background saves are paused, owner settings still save while "
                  "%u entries stay free",
                  (unsigned)avail, (unsigned)kReserve,
                  (unsigned)nimbus::store::kNvsStarvedFloorEntries);
  }
}

bool setTelegramToken(const String& v)     { return putOrClear(AKEY_TG_TOKEN, v); }
// Shrink-safe: removing a chat must land even on a full NVS (a dropped removal would
// leave that chat allowed). The worst case of the erase-then-write path is an EMPTY
// allowlist - nobody allowed, fail-closed.
bool setTelegramAllowlist(const String& v) { return putStrShrinkSafe(AKEY_TG_ALLOWLIST, v); }
bool setTtsEnabled(bool v)                 { return putBoolDef(AKEY_TTS_ENABLED, v, TTS_ENABLED_DEFAULT); }
bool setOrchToolLoop(bool v)               { return putBoolDef(AKEY_ORCH_TOOLLOOP, v, true); }
bool setCodeSandbox(bool v)                { return putBoolDef(AKEY_CODE_SANDBOX, v, false); }
bool setMidTurnFailover(bool v)            { return putBoolDef(AKEY_MID_FAILOVER, v, true); }
bool setAllowHwTests(bool v)               { return putBool(AKEY_ALLOW_HW_TESTS, v); }

// ---- provider key-verification cache ("R:TS" per provider, see agent_config) -
static String verifyKey(const String& provider) {
  return String(AKEY_VERIFY_PFX) + provider;
}
int8_t verifyResult(const String& provider) {
  String v = getStrQuiet(verifyKey(provider).c_str(), "");
  if (!v.length()) return -1;                 // never verified (ts()==0 confirms)
  return (int8_t)v.substring(0, v.indexOf(':')).toInt();
}
uint32_t verifyTs(const String& provider) {
  String v = getStrQuiet(verifyKey(provider).c_str(), "");
  int sep = v.indexOf(':');
  if (sep < 0) return 0;
  return (uint32_t)v.substring(sep + 1).toInt();
}
bool setVerify(const String& provider, int8_t r, uint32_t ts) {
  // (-1, 0) is "never verified", which is exactly what an ABSENT key reads as - so
  // the reset a key write/clear does is an erase. That keeps a key clear (the
  // storage-full remedy) landing on a 100% full partition, where writing "-1:0"
  // would itself need a free entry.
  if (r == -1 && ts == 0) return clearKey(verifyKey(provider).c_str());
  String v = String((int)r) + ":" + String(ts);
  return putStr(verifyKey(provider).c_str(), v);
}

// ---- voice provider config (STT/TTS; freely changeable, unlike embed) --------
String sttProvider() { return getStrQuiet(AKEY_STT_PROVIDER, STT_DEFAULT_PROVIDER); }
String ttsProvider() { return getStrQuiet(AKEY_TTS_PROVIDER, TTS_DEFAULT_PROVIDER); }
String ttsVoice()    { return getStrQuiet(AKEY_TTS_VOICE, ""); }
String theme()       { return getStrQuiet(AKEY_THEME, "teal"); }
// Default "tft": the colour panel is the only supported display (e-ink removed in
// v4.4), so a fresh or NVS-erased device comes up on it with no misconfiguration
// notice. Only an EXPLICIT stored non-"tft" value (a real e-ink migration) reads
// back as such - see nimbus::display::showsUnsupportedNotice, which the boot seam
// uses on the RAW value to tell an absent key from an explicit "eink" (CUM-189).
String screenModel() {
  const String v = getStrQuiet(AKEY_SCREEN_MODEL, "tft");
  return v == "tft" ? v : String("eink");
}
// The RAW stored value ("" when the key is absent). Lets the boot seam distinguish
// a fresh/erased device (silent colour panel) from an explicit legacy value (the
// honest migration notice); screenModel() above collapses that distinction away.
String screenModelStored() { return getStrQuiet(AKEY_SCREEN_MODEL, ""); }
bool   screenIsTft() { return screenModel() == "tft"; }
// The display/input drivers bind ONCE at boot, so the stored screenModel can
// diverge from what is actually driving the panel after the wizard changes it
// (the change needs a restart to take effect). main.cpp records the bound
// driver here at boot; RAM-only, never persisted. -1 = not yet recorded.
static int s_bootScreenIsTft = -1;
void setBootScreenIsTft(bool v) { s_bootScreenIsTft = v ? 1 : 0; }
int  bootScreenIsTft() { return s_bootScreenIsTft; }
String touchCal()    { return getStrQuiet(AKEY_TOUCH_CAL, ""); }
bool   setTouchCal(const String& v) {
  // Validated by the caller (nimbus::touch::parseCal). Stored verbatim so a
  // round-trip is byte-exact; "" ("use the defaults") is an erase - an absent key
  // reads "" too.
  return putOrClear(AKEY_TOUCH_CAL, v);
}
uint8_t sfxLevelNotif() { return (uint8_t)solide::memory::getInt(AKEY_SFX_LVL_NOTIF, 0); }
uint8_t sfxLevelOrch()  { return (uint8_t)solide::memory::getInt(AKEY_SFX_LVL_ORCH, 2); }
String  sfxTheme()      { return getStrQuiet(AKEY_SFX_THEME, "pulse"); }
uint8_t sfxVolume()     { int v = solide::memory::getInt(AKEY_SFX_VOL, 50); return v < 0 ? 0 : (v > 100 ? 100 : (uint8_t)v); }
uint16_t compactAtKB() {
  int v = solide::memory::getInt(AKEY_COMPACT_KB, 48);
  if (v <= 0) return 0;                       // 0 = auto-fold off (manual /compact stays)
  return v < 8 ? 8 : (v > 512 ? 512 : (uint16_t)v);
}
bool setCompactAtKB(uint16_t v) { return putIntDef(AKEY_COMPACT_KB, v > 512 ? 512 : v, 48); }
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
String   battChem()     { return getStrQuiet(AKEY_BATT_CHEM, "liion"); }   // "liion" | "lifepo4"
uint8_t  battCellsOvr() { int v = solide::memory::getInt(AKEY_BATT_CELLS, 0); return (v == 1 || v == 2) ? (uint8_t)v : 0; }  // 0 = board default
String   battCurve()    { return getStrQuiet(AKEY_BATT_CURVE, ""); }        // "" = chemistry default curve
// Effective series-cell count for the sleep/wake VOLTAGE thresholds: an owner
// override wins, else the board map (both boards set it; guard to 1). The sleep +
// wake mV are PACK voltages, so their defaults AND clamp ceilings must scale with
// cells - a 2S 6000 mV floor is above the whole 1S range and would insta-sleep a
// full 1S board the moment it left USB (CUM-202).
uint8_t battCellsEff() { uint8_t o = battCellsOvr(); if (o) return o; uint8_t c = solide::board().batt.cells; return c ? c : 1; }
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
// The set*() paths are the OWNER's explicit choice (web UI, device menu, or the AI
// config action), so they latch the "was set" flag: from now on this value wins over
// any battery-mode default (CUM-395).
// Both writes always run (the flag latches even if the value write fails, as before);
// the verdict is true only when both landed.
bool setSfxLevelNotif(uint8_t v) { const bool a = putInt(AKEY_SFX_LVL_NOTIF, v > 3 ? 3 : v); const bool b = putInt(AKEY_SFX_LVL_N_SET, 1); return a && b; }
bool setSfxLevelOrch(uint8_t v)  { const bool a = putInt(AKEY_SFX_LVL_ORCH, v > 3 ? 3 : v);  const bool b = putInt(AKEY_SFX_LVL_O_SET, 1); return a && b; }
bool hasSaverMin()      { return solide::memory::getInt(AKEY_SAVER_MIN_SET, 0) != 0; }
bool hasSfxLevelNotif() { return solide::memory::getInt(AKEY_SFX_LVL_N_SET, 0) != 0; }
bool hasSfxLevelOrch()  { return solide::memory::getInt(AKEY_SFX_LVL_O_SET, 0) != 0; }
// One-time migration: a device updated to this firmware may already hold an owner's
// screen-rest / sound-level value from BEFORE battery modes seeded them (those values
// carry no "was set" flag). The first time a mode seeds defaults, adopt any already-
// stored value as an explicit owner choice so the seed never overwrites it. After this
// runs once, only applyProfileDefaults writes unflagged values, so re-seeding an
// untouched key on later switches stays correct (CUM-395).
// CUM-408: RAM latch so a full-NVS device fails loud ONCE per boot and then stops
// retrying the migration (and re-logging) on every battery-mode switch. It clears on a
// reboot, which retries once more in case the reboot freed NVS headroom.
static bool s_profSeedNvsFull = false;
bool profileSeedDeferredNvsFull() { return s_profSeedNvsFull; }

static void adoptPreFeatureValuesAsOwnerSet() {
  if (solide::memory::getInt(AKEY_PROF_SEED_INIT, 0) != 0) return;   // steady state: done
  // CUM-408: on a full NVS the marker write below silently drops (putInt -> 0), so
  // without this latch the whole migration - and its loud log line - re-runs on every
  // battery-mode switch and never completes. Once we have detected a full NVS this boot,
  // stop retrying until a reboot.
  if (s_profSeedNvsFull) return;
  // Adopt a persisted value as an explicit owner choice ONLY when it differs from the
  // shipped hard default (these mirror the getters above: saverMin 5, sfxLevelNotif 0,
  // sfxLevelOrch 2). The OLD device menu wrote the sfx levels UNCONDITIONALLY on any
  // menu change, so a great many updated devices hold the plain default with no real
  // owner intent - treat those as unset so battery modes can still seed them.
  const int sv = solide::memory::getInt(AKEY_SAVER_MIN, -1);
  const int sn = solide::memory::getInt(AKEY_SFX_LVL_NOTIF, -1);
  const int so = solide::memory::getInt(AKEY_SFX_LVL_ORCH, -1);
  if (nimbus::adoptAsOwnerSet(sv >= 0, sv, 5)) putInt(AKEY_SAVER_MIN_SET, 1);
  if (nimbus::adoptAsOwnerSet(sn >= 0, sn, 0)) putInt(AKEY_SFX_LVL_N_SET, 1);
  if (nimbus::adoptAsOwnerSet(so >= 0, so, 2)) putInt(AKEY_SFX_LVL_O_SET, 1);
  // Verify-after-write: the marker must actually PERSIST for the one-time migration to be
  // done. A full NVS drops the write (putInt returns 0 -> setInt false); read it back to
  // be sure, then decide via the portable state machine (host-tested over {fresh, seeded,
  // full}).
  const bool wrote = putInt(AKEY_PROF_SEED_INIT, 1);
  const bool persisted = wrote && solide::memory::getInt(AKEY_PROF_SEED_INIT, 0) == 1;
  if (nimbus::planSeedAttempt(/*markerPresent=*/false, persisted) ==
      nimbus::SeedAttempt::DeferredNvsFull) {
    s_profSeedNvsFull = true;   // latch: no more retries or logging this boot
    agent::alogc(nimbus::errlog::cat::kNvs,
                 "NVS full: profile-default seed migration deferred; free NVS space to apply it");
  }
}
// Seed a battery mode's screen-rest + sound-level defaults, but only into keys the
// owner has not explicitly set (their value always wins). Writing the seed WITHOUT
// the "was set" flag means a later profile switch re-seeds the same untouched key,
// while an owner-set key is skipped forever after (CUM-395).
void applyProfileDefaults(nimbus::ProfileId id) {
  adoptPreFeatureValuesAsOwnerSet();
  if (!hasSaverMin())
    putInt(AKEY_SAVER_MIN, nimbus::profileSaverMinutes(id));
  if (!hasSfxLevelNotif())
    putInt(AKEY_SFX_LVL_NOTIF, nimbus::profileSfxLevel(id, /*notifier=*/true));
  if (!hasSfxLevelOrch())
    putInt(AKEY_SFX_LVL_ORCH, nimbus::profileSfxLevel(id, /*notifier=*/false));
}
bool setSfxTheme(const String& v) {
  // Clamp at the store layer: g_theme feeds an SD path (`/sfx/<theme>/...`).
  // The web UI already allowlists, but a provision/NVS/test path must not be
  // able to poison it. Legacy themes (terran/protoss/zerg) stay storable -
  // absent pools fall through to general in the resolver.
  const bool known = v == "pulse" || v == "terran" || v == "protoss" || v == "zerg";
  return putStr(AKEY_SFX_THEME, known ? v : String("pulse"));
}
bool setSfxVolume(uint8_t v) { return putInt(AKEY_SFX_VOL, v > 100 ? 100 : v); }
// Owner's explicit screen-rest choice: latch the "was set" flag so a battery-mode
// switch never overwrites it (CUM-395 - same rule as the two sound levels above).
bool setSaverMin(uint16_t v) { const bool a = putInt(AKEY_SAVER_MIN, v > 1440 ? 1440 : v); const bool b = putInt(AKEY_SAVER_MIN_SET, 1); return a && b; }
bool setBattRtop(uint32_t o) { return putInt(AKEY_BATT_RTOP, int32_t(o < 1000 ? 1000 : (o > 10000000 ? 10000000 : o))); }
bool setBattRbot(uint32_t o) { return putInt(AKEY_BATT_RBOT, int32_t(o < 1000 ? 1000 : (o > 10000000 ? 10000000 : o))); }
bool setBattCapMah(uint16_t m) { return putInt(AKEY_BATT_CAPMAH, m < 100 ? 100 : (m > 20000 ? 20000 : m)); }
bool setBattChem(const String& slug) { return putStr(AKEY_BATT_CHEM, slug == "lifepo4" ? "lifepo4" : "liion"); }
// Clamp the override to the board's physical cell count so a 1S board can never be
// told it is 2S (CUM-371): the web handler clamps + toasts, this guards the AI
// config path and any other caller so an impossible value never lands in NVS.
bool setBattCells(uint8_t cells) {
  const uint8_t ok = nimbus::power::clampBattCellsOverride(cells, solide::board().batt.cells);
  return putInt(AKEY_BATT_CELLS, (ok == 1 || ok == 2) ? ok : 0);
}
bool setBattCurve(const String& csv) { return putOrClear(AKEY_BATT_CURVE, csv); }
bool setSleepMv(uint16_t v)  { const uint16_t ceil = nimbus::power::sleepMvCeilFor(battCellsEff()); return putInt(AKEY_SLEEP_MV, v > ceil ? ceil : v); }
bool setWakeMv(uint16_t v)   { const uint16_t ceil = nimbus::power::wakeMvCeilFor(battCellsEff());  return putInt(AKEY_WAKE_MV,  v > ceil ? ceil : v); }
bool setSleepOvr(bool on)    { return putInt(AKEY_SLEEP_OVR, on ? 1 : 0); }
bool setBrightOvr(bool on)   { return putInt(AKEY_BRIGHT_OVR, on ? 1 : 0); }
bool setTftFlip(bool on)      { return putInt(AKEY_TFT_FLIP, on ? 1 : 0); }
bool setLowBattRing(bool on)  { return putInt(AKEY_LOWBATT_RING, on ? 1 : 0); }
bool setLowBattSaver(bool on) { return putInt(AKEY_LOWBATT_SAVER, on ? 1 : 0); }
bool setBattMon(bool on) { return putInt(AKEY_BATT_MON, on ? 1 : 0); }
String connectorsJson() { return getStrQuiet(AKEY_CONNECTORS, ""); }
// Shrink-safe: removing a connector is one of the two remedies the storage-full
// message names, so a delete must land even on a 100% full partition (CUM-479).
bool   setConnectorsJson(const String& v) { return putStrShrinkSafe(AKEY_CONNECTORS, v); }
bool   setTheme(const String& v)       { return putStr(AKEY_THEME, v); }
bool   setScreenModel(const String& v) {
  // Clamp at the store layer (same reasoning as setSfxTheme): this value picks
  // a display AND an input driver at boot, so an unrecognised slug must fall
  // back to the shipped panel rather than leave the device with no UI at all.
  return putStr(AKEY_SCREEN_MODEL, v == "tft" ? "tft" : "eink");
}
bool   setSttProvider(const String& v) { return putStr(AKEY_STT_PROVIDER, v); }
bool   setTtsProvider(const String& v) { return putStr(AKEY_TTS_PROVIDER, v); }
bool   setTtsVoice(const String& v)    { return putOrClear(AKEY_TTS_VOICE, v); }

// ---- embedding config (set-once; the caller enforces the reset-before-change) -
String embedProvider() { return getStrQuiet(AKEY_EMBED_PROVIDER, EMBED_DEFAULT_PROVIDER); }
String embedModel()    { return getStrQuiet(AKEY_EMBED_MODEL, EMBED_DEFAULT_MODEL); }
int    embedDims()     { return solide::memory::getInt(AKEY_EMBED_DIMS, EMBED_DEFAULT_DIMS); }
bool   embedLocked()   { return solide::memory::getBool(AKEY_EMBED_LOCKED, false); }
// The previous value of one embed key, so a partial write can be undone exactly: an
// absent key goes back to absent (an erase that always lands), never to its default.
struct PrevStr { bool had; String v; };
static PrevStr prevStr(const char* key) { return {solide::memory::has(key), getStrQuiet(key, "")}; }
static void restoreStr(const char* key, const PrevStr& p) {
  if (p.had) putStr(key, p.v); else clearKey(key);
}
bool   setEmbedConfig(const String& provider, const String& model, int dims) {
  // All or nothing (best effort): the three values describe ONE embedding space, so a
  // write that lands only partly would leave a config no model matches. On a partial
  // failure the previous values go back and the caller gets false - it must then act
  // on nothing (POST /api/mem/embedcfg answers 507 and wipes no memories).
  const PrevStr oldProv = prevStr(AKEY_EMBED_PROVIDER), oldModel = prevStr(AKEY_EMBED_MODEL);
  const bool hadDims = solide::memory::has(AKEY_EMBED_DIMS);
  const int32_t oldDims = solide::memory::getInt(AKEY_EMBED_DIMS, EMBED_DEFAULT_DIMS);
  const bool a = putStr(AKEY_EMBED_PROVIDER, provider);
  const bool b = putStr(AKEY_EMBED_MODEL, model);
  const bool c = putInt(AKEY_EMBED_DIMS, dims);
  if (a && b && c) return true;
  restoreStr(AKEY_EMBED_PROVIDER, oldProv);
  restoreStr(AKEY_EMBED_MODEL, oldModel);
  if (hadDims) putInt(AKEY_EMBED_DIMS, oldDims); else clearKey(AKEY_EMBED_DIMS);
  return false;
}
bool   setEmbedLocked(bool v) { return putBool(AKEY_EMBED_LOCKED, v); }

// ---- per-provider usage ledger + budgets -------------------------------------
// The ledger is cached in RAM and re-serialized after each mutation. Since v4.5.14
// it lives in a LittleFS file (/data/usage_ledger.txt): in NVS its per-turn rewrite
// failed on a full partition and most of a month's spend was lost. The move follows
// nimbus/store/moved_store.h (the file, else the legacy usgLedger key; an atomic write
// read back; the NVS copy erased only on a boot whose image was already valid). A
// write that did not land leaves the ledger DIRTY: every later record retries, the
// main loop retries once a minute, and /api/state shows usage.saved false. The mutex
// serializes the turn task's record vs the web task's read.
namespace {
std::mutex             g_ledgerMx;
nimbus::orch::UsageLedger g_ledger;
bool                   g_ledgerLoaded = false;
nimbus::store::DirtyRetry g_ledgerState;      // dirty: RAM holds spend the file does not
constexpr const char*  kUsageLedgerPath = "/data/usage_ledger.txt";

// The ledger's moved-store binding (the file, and the legacy NVS key).
struct LedgerIo {
  bool readFile(std::string& out) {
    if (!LittleFS.exists(kUsageLedgerPath)) return false;
    File f = LittleFS.open(kUsageLedgerPath, "r");
    if (!f) return false;
    String v = f.readString();
    f.close();
    out.assign(v.c_str(), v.length());
    return true;
  }
  bool writeFileAtomic(const std::string& v) {
    LittleFS.mkdir("/data");
    return agent::writeFileAtomic(kUsageLedgerPath, v, "usage_ledger", false);
  }
  bool readNvs(std::string& out) {
    if (!solide::memory::has(AKEY_USAGE_LEDGER)) return false;
    const String v = getStrQuiet(AKEY_USAGE_LEDGER, "");
    out.assign(v.c_str(), v.length());
    return true;
  }
  bool eraseNvs() { return clearKey(AKEY_USAGE_LEDGER); }   // an erase: always lands
};

// ---- per-model spend + prices (2026-10) ------------------------------------------
// Three bounded LittleFS files beside usage_hist.txt, never NVS (usgLedger stays
// byte-identical): the spend rows (32, rewritten each turn that names a model), the
// owner's model prices (32, only an owner edit writes them) and the Cumulo router's
// prices (24, only the verify task writes them). Loaded with the ledger, under the
// same mutex. Until LittleFS is mounted (memory::begin, Orchestrator mode) they are neither
// read nor written, so an early read can never pass for an empty ledger and then
// overwrite the stored one.
constexpr const char* kUsageModelsPath  = "/data/usage_models.txt";
constexpr const char* kUsageMratesPath  = "/data/usage_mrates.txt";
constexpr const char* kUsageRpricesPath = "/data/usage_rprices.txt";
bool g_modelsLoaded = false;

nimbus::orch::UsageHistory& histLocked();   // the daily history (below), same mutex

std::string readFileQuiet(const char* path) {
  if (!LittleFS.exists(path)) return std::string();   // absent = empty, no open() error line
  File f = LittleFS.open(path, "r");
  if (!f) return std::string();
  String s = f.readString();
  f.close();
  return std::string(s.c_str(), s.length());
}

void loadModelsLocked() {  // caller holds g_ledgerMx
  if (g_modelsLoaded || LittleFS.totalBytes() == 0) return;   // 0 = not mounted yet
  auto& M = g_ledger.models();
  M.deserializeSpend(readFileQuiet(kUsageModelsPath));
  M.deserializeOwnerRates(readFileQuiet(kUsageMratesPath));
  M.deserializeRouterRates(readFileQuiet(kUsageRpricesPath));
  // Tracking inferred from the history, or started now with the stored counters as
  // earlier usage (gate M2), then the rebase: rows and NVS counters are stored apart,
  // and when they disagree the provider's spend since tracking began is priced at its
  // highest known price, never a cheaper one (gate G1, G3, M1).
  const auto settled = g_ledger.settleModels(histLocked());
  // Stored now, so it outlives what made it true: tracking inferred or started (a
  // later boot may see different history), and counters the file described that are
  // gone (a later boot may no longer see it, gate M1). When that write fails after
  // such counters, the file is removed only while the history shows tracking
  // (removing needs no free space; the next load then infers it); otherwise it stays
  // and the next load decides again. While the file on flash does not hold this
  // load's result, the next record writes it first (ModelLedger::spendFileFirst), and
  // while one is there that lacks it, history buckets name no model (historyKey).
  bool current = true;
  if (settled.mustStore() &&
      !agent::writeFileAtomic(kUsageModelsPath, M.serializeSpend(), "usage_models", false)) {
    current = false;
    if (settled.removable) LittleFS.remove(kUsageModelsPath);
  }
  const bool present = LittleFS.exists(kUsageModelsPath);
  M.setSpendFileStored(current && present);
  M.setSpendFileStale(!current && present);
  g_modelsLoaded = true;
}

// A per-turn file write that failed: one log line a minute, never one per turn.
void logUsageFileFailure(const char* what) {
  static uint32_t lastMs = 0;
  static bool logged = false;
  const uint32_t now = millis();
  if (logged && (uint32_t)(now - lastMs) < 60000) return;
  logged = true;
  lastMs = now;
  alogf("usage: %s write failed - keeping the previous copy", what);
}

nimbus::orch::UsageLedger& ledgerLocked() {  // caller holds g_ledgerMx
  if (!g_ledgerLoaded) {
    LedgerIo io;
    if (LittleFS.totalBytes() == 0) {
      // Flash storage not mounted yet (or Notifier mode): serve the legacy NVS copy
      // read-only and load again once it is mounted, so an early read can never pass
      // for the stored ledger and later overwrite the file.
      std::string v;
      io.readNvs(v);
      g_ledger.deserialize(v);
      // Still on the NVS fallback: no new (provider, tag) audit row may grow it (lane
      // T; per-host filing creates pairs a single-host turn never did). New tags are
      // allowed again in the file branch below (usgLedger lives on LittleFS there).
      g_ledger.setAllowNewTags(false);
    } else {
      const nimbus::store::MovedLoad m = nimbus::store::loadMoved(io);
      g_ledger.deserialize(m.value);
      g_ledgerLoaded = true;
      // Spend that so far lives only in NVS goes to the file at the next persist.
      if (m.src == nimbus::store::MovedSource::Nvs) g_ledgerState.markDirty(millis());
      if (m.src == nimbus::store::MovedSource::File &&
          nimbus::store::retireLegacy(io, otaupd::imageValidAtBoot()))
        alogf("usage: legacy settings-storage copy of the ledger removed (the file is in use)");
    }
  }
  loadModelsLocked();
  return g_ledger;
}

// A deleted routine's attribution row is kept only while the routine exists (set by
// the main-loop pass from the live loop ids; empty = not known yet, keep everything).
std::vector<std::string> g_liveLoopTags;
bool keepLedgerTag(const std::string& tag) {
  if (g_liveLoopTags.empty() || tag.compare(0, 5, "loop:") != 0) return true;
  if (tag == "loop:dream") return true;   // the reserved nightly routine never goes
  for (const auto& t : g_liveLoopTags)
    if (t == tag) return true;
  return false;
}

bool persistLedgerLocked() {  // caller holds g_ledgerMx
  // Never write before the stored ledger was read (flash storage not mounted).
  if (!g_ledgerLoaded) return notePersist(false, "lfs_ledger");
  // The byte guard: the serialized ledger stays under 3,800 B (a deleted routine's
  // row goes first, then the smallest rows).
  g_ledger.fitSerialized(nimbus::orch::UsageLedger::kSerializedMaxBytes, keepLedgerTag);
  const std::string s = g_ledger.serialize();
  LedgerIo io;
  const bool ok = nimbus::store::persistMoved(io, s);
  g_ledgerState.noteWrite(ok, millis());
  // Reported like an NVS write: an owner budget edit that did not store answers 507.
  notePersist(ok, "lfs_ledger");
  // Rides the next spend-file write: a load can then tell a spend file that is only
  // behind the stored counters (rows kept) from one that may be ahead (gate G1, G3),
  // and, from the totals a landed write stored, re-created counters from ones that
  // grew (gate M1).
  g_ledger.models().setCountersBehind(!ok);
  if (ok) g_ledger.noteCountersStored();
  return ok;
}

// An OWNER edit of budgets/rates (CUM-479): if it cannot be stored, put the RAM ledger
// back too, so a 507 really means "nothing changed" - never a limit enforced until
// the next reboot and then silently gone. Caller holds g_ledgerMx.
bool persistOrRevertLocked(const nimbus::orch::UsageLedger& before) {
  if (persistLedgerLocked()) return true;
  g_ledger = before;
  return false;
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

// Atomic since 2026-10 (tmp + rename): a power loss mid-write keeps the previous
// history instead of truncating it.
void persistHistLocked() {  // caller holds g_ledgerMx
  LittleFS.mkdir("/data");
  if (!agent::writeFileAtomic(kUsageHistPath, g_hist.serialize(), "usage_hist", false))
    logUsageFileFailure("usage_hist");
}

void persistModelsLocked() {  // caller holds g_ledgerMx
  if (!g_modelsLoaded) return;   // never overwrite a file that was not read
  if (agent::writeFileAtomic(kUsageModelsPath, g_ledger.models().serializeSpend(),
                             "usage_models", false))
    g_ledger.models().setSpendFileStored(true);
  else
    logUsageFileFailure("usage_models");
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
  nimbus::orch::TokenDelta d;
  d.in = tokensIn;
  d.out = tokensOut;
  d.cacheR = cacheRead;
  d.cacheW = cacheWrite;
  recordProviderTokens(provider, d, tag, "");   // provider level only
}

static void recordProviderTokensLocked(const String& provider,
                                       const nimbus::orch::TokenDelta& d, const char* tag,
                                       const char* model) {  // caller holds g_ledgerMx
  auto& L = ledgerLocked();
  const std::string prov(provider.c_str());
  const auto* e = L.find(prov);
  uint8_t rd = e ? e->resetDay : 1;
  const uint32_t day = currentDayKey();
  // 1. The provider ledger -> NVS, unchanged (the budget's source of truth).
  // 2. The model row -> /data/usage_models.txt (only once that file was read). Once
  //    tracking has started, spend with no model files under ~other (spendKey), so
  //    the rows always add up to the counters and a mismatch at load means a store
  //    failed (gate G1-G3).
  const char* key = g_modelsLoaded ? L.models().spendKey(model) : "";
  L.recordTokens(prov, d, currentPeriodKey(rd), std::string(tag ? tag : ""), key, day);
  // Gate M1, M2: while the spend file on flash does not hold the load's result (none
  // is there, or the load could not store it), it goes first, marked as ahead of the
  // counters ("!n": this record's usgLedger write has not landed yet).
  if (*key && L.models().spendFileFirst()) {
    L.models().setCountersBehind(true);
    persistModelsLocked();
  }
  persistLedgerLocked();
  if (*key) persistModelsLocked();
  // 3. Daily (day, provider, model) bucket for the graphs (skipped pre-SNTP: dayKey 0
  //    is refused by the core; no model while a stale spend file is on flash, gate M1).
  if (day) {
    auto& H = histLocked();
    H.record(prov, L.models().historyKey(key), day, d, 0);
    H.prune(day, 60);
    persistHistLocked();
  }
}

void recordProviderTokens(const String& provider, const nimbus::orch::TokenDelta& d,
                          const char* tag, const char* model) {
  if (provider.length() == 0 || d.empty()) return;
  if (!model) model = "";
  uint64_t micro = 0;
  {
    std::lock_guard<std::mutex> lk(g_ledgerMx);
    recordProviderTokensLocked(provider, d, tag, model);
    micro = ledgerLocked().priceDeltaMicro(std::string(provider.c_str()), model, d);
  }
  // The monthly spend limits count the record at its price, after the ledger lock (the
  // limits store may read the ledger on its first load).
  const agent::spend::Part part{provider.c_str(), micro};
  agent::spend::commit(&part, 1, false);
}

// Each slot of `spend` at the ledger's price, by provider (caller holds g_ledgerMx).
static size_t spendPartsLocked(const nimbus::orch::TurnSpend& spend, agent::spend::Part* parts,
                               size_t cap) {
  auto& L = ledgerLocked();
  size_t n = 0;
  for (size_t i = 0; i < spend.n && n < cap; ++i) {
    const nimbus::orch::SpendSlot& sl = spend.slot[i];
    parts[n++] = agent::spend::Part{sl.host, L.priceDeltaMicro(sl.host, sl.model, nimbus::orch::ledgerDelta(sl))};
  }
  return n;
}

// recordProviderSpend's ledger work (caller holds g_ledgerMx): every slot filed, each
// store persisted once; fills the spend at the ledger's prices by provider (micro-USD).
static size_t recordProviderSpendLocked(const nimbus::orch::TurnSpend& spend, const char* tag,
                                        agent::spend::Part* parts) {
  auto& L = ledgerLocked();
  const uint32_t day = currentDayKey();
  const std::string t(tag ? tag : "");
  bool anyKey = false;
  // Each host's slot under its own provider and model, its cache traffic split the way
  // that provider bills it (ledgerDelta, owner ruling R4). Same three stores and gates
  // as recordProviderTokens, but each persisted ONCE for the whole turn.
  for (size_t i = 0; i < spend.n; ++i) {
    const nimbus::orch::SpendSlot& sl = spend.slot[i];
    const nimbus::orch::TokenDelta d = nimbus::orch::ledgerDelta(sl);
    if (d.empty()) continue;
    const std::string prov(sl.host);
    const auto* e = L.find(prov);
    const uint8_t rd = e ? e->resetDay : 1;
    const char* key = g_modelsLoaded ? L.models().spendKey(sl.model) : "";
    L.recordTokens(prov, d, currentPeriodKey(rd), t, key, day);
    if (*key) anyKey = true;
    if (day) histLocked().record(prov, L.models().historyKey(key), day, d, 0);
  }
  if (anyKey && L.models().spendFileFirst()) {   // gate M1, M2 (see recordProviderTokens)
    L.models().setCountersBehind(true);
    persistModelsLocked();
  }
  persistLedgerLocked();
  if (anyKey) persistModelsLocked();
  if (day) {
    histLocked().prune(day, 60);
    persistHistLocked();
  }
  return spendPartsLocked(spend, parts, nimbus::orch::TurnSpend::kSlots);
}

void recordProviderSpend(const nimbus::orch::TurnSpend& spend, const char* tag) {
  agent::spend::Part parts[nimbus::orch::TurnSpend::kSlots];
  size_t n = 0;
  if (!spend.empty()) {
    std::lock_guard<std::mutex> lk(g_ledgerMx);
    n = recordProviderSpendLocked(spend, tag, parts);
  }
  // The limits count the turn after the ledger lock. A fold ("compact") runs inside a
  // turn: only the turn's own record (billed or not) ends its pending rounds.
  agent::spend::commit(parts, n, !(tag && std::strcmp(tag, "compact") == 0));
}

static void recordProviderCallLocked(const String& provider) {  // caller holds g_ledgerMx
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

void recordProviderCall(const String& provider) {
  if (provider.length() == 0) return;
  uint64_t micro = 0;
  {
    std::lock_guard<std::mutex> lk(g_ledgerMx);
    recordProviderCallLocked(provider);
    micro = ledgerLocked().priceDeltaMicro(std::string(provider.c_str()), "",
                                           nimbus::orch::TokenDelta{}, 1);
  }
  const agent::spend::Part part{provider.c_str(), micro};
  agent::spend::commit(&part, 1, false);   // a search call counts toward the limits too
}

// ---- the monthly spend ceiling's inputs (lane DEF) -----------------------------

uint64_t priceSpendMicro(const nimbus::orch::TurnSpend& spend) {
  if (spend.empty()) return 0;
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  return ledgerLocked().priceTurnMicro(spend);
}

void recordSideSpend(const nimbus::orch::TurnSpend& spend, const char* tag) {
  if (spend.empty()) return;
  agent::spend::Part parts[nimbus::orch::TurnSpend::kSlots];
  size_t n = 0;
  {
    std::lock_guard<std::mutex> lk(g_ledgerMx);
    n = recordProviderSpendLocked(spend, tag, parts);
  }
  agent::spend::commit(parts, n, false);   // never ends the turn in flight
}

size_t priceSpendParts(const nimbus::orch::TurnSpend& spend, agent::spend::Part* parts, size_t cap) {
  if (spend.empty()) return 0;
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  return spendPartsLocked(spend, parts, cap);
}

// ---- the per-provider monthly limits' inputs (lane DEF2) ----------------------------

void providerCapRules(std::vector<nimbus::orch::CapRule>& out) {
  out.clear();
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  for (const auto& e : ledgerLocked().entries()) {
    if (!e.centsLimit) continue;
    nimbus::orch::CapRule c;
    c.provider = e.name;
    c.capMicro = e.centsLimit > UINT64_MAX / 10000ULL ? UINT64_MAX : e.centsLimit * 10000ULL;
    c.resetDay = e.resetDay ? e.resetDay : 1;
    out.push_back(c);
  }
}

void providerPeriodSeeds(std::vector<std::pair<std::string, uint64_t>>& out) {
  out.clear();
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  auto& L = ledgerLocked();
  for (const auto& e : L.entries()) {
    if (e.periodKey == 0 || e.periodKey != currentPeriodKey(e.resetDay)) continue;
    const uint64_t cents = L.priceProvider(e.name);
    if (cents) out.push_back({e.name, cents * 10000ULL});
  }
}

uint64_t todayMicro() {
  const uint32_t day = currentDayKey();
  if (!day) return 0;
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  auto& L = ledgerLocked();
  uint64_t micro = 0;
  for (const auto& e : L.entries())
    micro = nimbus::orch::addSatMicro(micro, L.priceDay(histLocked(), e.name, day) * 10000ULL);
  return micro;
}

bool usageHasHistory() {
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  for (const auto& e : ledgerLocked().entries())
    if (e.totalIn || e.totalOut || e.totalCalls) return true;
  return false;
}

uint64_t monthToDateMicro() {
  const time_t now = time(nullptr);
  struct tm tmv;
  if (now < 1609459200 || !localtime_r(&now, &tmv)) return 0;
  // The first day of this month (device timezone) as a UTC day key: the history's
  // buckets are UTC days, so the one boundary day counts whole (fail-closed).
  struct tm first = tmv;
  first.tm_mday = 1;
  first.tm_hour = first.tm_min = first.tm_sec = 0;
  first.tm_isdst = -1;
  const time_t firstAt = mktime(&first);
  const uint32_t firstDay = firstAt > 0 ? (uint32_t)(firstAt / 86400) : 0;
  const uint32_t monthPeriod = currentPeriodKey(1);
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  auto& L = ledgerLocked();
  uint64_t micro = 0;
  auto addSat = [&micro](uint64_t v) { micro = nimbus::orch::addSatMicro(micro, v); };
  for (const auto& e : L.entries()) {
    if (e.resetDay == 1 && e.periodKey == monthPeriod) {
      // The ledger's own period IS this calendar month: the Usage page's estimate.
      addSat(L.priceProvider(e.name) * 10000ULL);
      continue;
    }
    // Another reset day: this month's daily buckets of that provider.
    for (const auto& b : histLocked().entries()) {
      if (b.prov != e.name || b.dayKey < firstDay) continue;
      nimbus::orch::TokenDelta d;
      d.in = b.tokIn;
      d.out = b.tokOut;
      d.cacheR = b.cacheR;
      d.cacheW = b.cacheW;
      addSat(L.priceDeltaMicro(b.prov, b.model, d, b.calls));
    }
  }
  return micro;
}

bool setProviderRates(const String& provider, uint32_t centsPerMIn,
                      uint32_t centsPerMOut, uint32_t centsPerKCalls) {
  if (provider.length() == 0) return true;   // nothing to store
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  auto& L = ledgerLocked();
  const nimbus::orch::UsageLedger before = L;
  L.setRates(std::string(provider.c_str()), centsPerMIn, centsPerMOut, centsPerKCalls);
  return persistOrRevertLocked(before);
}

nimbus::orch::RateEdit setModelRate(const String& provider, const char* model,
                                    uint32_t microIn, uint32_t microOut) {
  using nimbus::orch::RateEdit;
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  auto& L = ledgerLocked();
  RateEdit r = RateEdit::WriteFailed;
  if (g_modelsLoaded) {   // storage not mounted: nothing can be stored
    // Only the owner table is snapshotted; a failed write puts it back exactly.
    r = L.models().setOwnerRateDurable(
        provider.c_str(), model, microIn, microOut,
        [](const std::string& blob) { return agent::writeFileAtomic(kUsageMratesPath, blob, "usage_mrates"); });
  }
  // CUM-479: report a store failure to the persist ledger, so the route's SaveScope
  // answers 507 storage_full instead of {"ok":true} over a dropped price.
  if (r == RateEdit::WriteFailed) notePersist(false, "lfs_mrates");
  return r;
}

bool replaceRouterPrices(const std::vector<nimbus::orch::ModelRate>& rows) {
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  auto& L = ledgerLocked();
  if (!g_modelsLoaded) return false;
  L.models().replaceRouterRates(rows);
  return agent::writeFileAtomic(kUsageRpricesPath, L.models().serializeRouterRates(),
                                "usage_rprices");
}

int8_t routerPriceState() {
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  auto& L = ledgerLocked();
  if (!g_modelsLoaded) return -1;
  return L.models().routerRates().empty() ? 1 : 0;
}

std::string editableModels(const String& provider) {
  // The NVS reads first, outside the ledger lock.
  const char* p = provider.c_str();
  const std::string choices =
      nimbus::orch::isProviderSlug(p)
          ? nimbus::orch::effectiveModelChoices(p, std::string(modelChoices(provider).c_str()))
          : std::string();
  const std::string orchSel(orchModel(provider).c_str());
  const std::string subSel(subModel(provider).c_str());
  const std::string extra(provider == "custom" ? customModel().c_str() : "");
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  return nimbus::orch::editableModelKeys(p, choices, orchSel, subSel, extra,
                                         ledgerLocked().models());
}

namespace {
// ONE PSRAM allocation for a big JSON body (review HIGH: an internal-heap String of
// tens of KB, plus the response copy, was exactly the near-OOM class v2.5.1 fixed;
// big dynamic payloads go to PSRAM, streamed chunked).
char* jsonBuffer(size_t cap) {
  char* buf = (char*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  if (!buf) buf = (char*)heap_caps_malloc(cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return buf;
}
}  // namespace

char* usageHistoryJsonPs(size_t& outLen) {
  outLen = 0;
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  auto& H = histLocked();
  // kHistoryJsonEntryMax upper-bounds the widest bucket object (host-tested at its
  // worst case); +48 for the envelope.
  const size_t cap = 48 + H.entries().size() * nimbus::orch::kHistoryJsonEntryMax;
  char* buf = jsonBuffer(cap);
  if (!buf) return nullptr;
  size_t off = (size_t)snprintf(buf, cap, "{\"today\":%lu,\"days\":[",
                                (unsigned long)currentDayKey());
  bool first = true;
  for (const auto& e : H.entries()) {
    const size_t n = nimbus::orch::historyEntryJson(e, first, buf + off, cap - off - 3);
    if (n == 0) break;   // bound respected by sizing; belt anyway
    off += n;
    first = false;
  }
  off += (size_t)snprintf(buf + off, cap - off, "]}");
  outLen = off;
  return buf;
}

char* usageModelsJsonPs(size_t& outLen) {
  outLen = 0;
  std::vector<nimbus::orch::EditableModels> ed;   // NVS reads outside the ledger lock
  ed.reserve(nimbus::orch::kProviderSlotCount + 1);
  for (const auto& slot : nimbus::orch::kProviderSlots)
    ed.push_back({slot.slug, editableModels(slot.slug)});
  ed.push_back({"custom", editableModels("custom")});
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  auto& L = ledgerLocked();
  const size_t cap = nimbus::orch::usageModelsJsonCap(L, ed);
  char* buf = jsonBuffer(cap);
  if (!buf) return nullptr;
  outLen = nimbus::orch::writeUsageModelsJson(L, ed, buf, cap);
  if (outLen == 0) {   // never a truncated document
    free(buf);
    return nullptr;
  }
  return buf;
}

bool usageLedgerSaved() {
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  return g_ledgerState.saved();
}

void retryUsageLedger() {
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  if (g_ledgerLoaded && g_ledgerState.retryDue(millis())) persistLedgerLocked();
}

void noteLiveLoopIds(const std::vector<std::string>& ids) {
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  g_liveLoopTags.clear();
  for (const auto& id : ids) g_liveLoopTags.push_back("loop:" + id);
  if (g_ledgerLoaded && g_ledger.pruneTags(keepLedgerTag)) persistLedgerLocked();
}

// L19-2: the default daily spend warning. Run at the start of every turn (through
// providerOverBudget, the turn's budget gate): when today's spend at `provider`
// crosses its threshold (nimbus::orch::DailyWarn), the owner hears once that day.
// Priced from the usage history's day; latched on the device's local date.
static void checkDailySpend(const String& provider) {
  // Lane DEF2: the owner's own daily warning (every provider together, the limits store's
  // notice) replaces this default per-provider rule while it is set.
  if (agent::spend::dailyWarnMicro() > 0) return;
  const time_t now = time(nullptr);
  const uint32_t day = currentDayKey();
  if (!day || !agent::gate::ownerReachable()) return;   // undated, or nobody to tell now
  struct tm tmv;
  localtime_r(&now, &tmv);
  const uint32_t localDay = nimbus::orch::civilDayKey(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
  std::string notice;
  bool due;
  {
    std::lock_guard<std::mutex> lk(g_ledgerMx);
    due = nimbus::orch::dailyWarnDue(ledgerLocked(), histLocked(), std::string(provider.c_str()), day, localDay,
                                     notice);
  }
  if (due) agent::gate::sendOwnerNotice(provider.c_str(), String(notice.c_str()));
}

// Caller holds g_ledgerMx. The ledger's own ceilings: tokens and calls, and the dollar
// limit only while the limits file has not loaded (lane DEF2: once it has, the dollar limit
// is its hard freeze, one count for every seam, so the turn failover, the search refusal and
// the Budgets row never trip a cent before the freeze or after a raise lifts it).
static bool overLedgerBudgetLocked(const std::string& name, bool dollarKnown) {
  const nimbus::orch::UsageLedger& L = ledgerLocked();
  return L.overTokenBudget(name) || L.overCallBudget(name) || (!dollarKnown && L.overCostBudget(name));
}

// Caller holds g_ledgerMx: a Budgets row's "over" (the park read takes only its own lock).
static bool overLocked(const std::string& name) {
  bool known = false;
  return agent::spend::providerAtLimit(name.c_str(), known) || overLedgerBudgetLocked(name, known);
}

bool providerOverBudget(const String& provider) {
  if (provider.length() == 0) return false;
  checkDailySpend(provider);
  bool known = false;
  if (agent::spend::providerAtLimit(provider.c_str(), known)) return true;
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  const std::string name(provider.c_str());
  // Dated form (prism: the forever-gated wedge) - a stale periodKey reads as a
  // FRESH period, so "wait for the reset day" actually comes true even though
  // a gated provider never records. Unsynced clock (pre-SNTP) falls back to
  // the strict form: never un-gate on a clock we don't trust.
  time_t now = time(nullptr);
  if (now >= 1600000000) {
    struct tm tmv;
    localtime_r(&now, &tmv);
    const nimbus::orch::ProviderBudget* e = ledgerLocked().find(name);
    if (!e || e->periodKey != nimbus::orch::usagePeriodKey(tmv.tm_year + 1900, tmv.tm_mon + 1,
                                                           tmv.tm_mday, e->resetDay))
      return false;   // a new period: the roll happens on the next record
  }
  return overLedgerBudgetLocked(name, known);
}

bool setProviderBudget(const String& provider, uint64_t tokenLimit,
                       uint32_t callLimit, uint8_t resetDay, uint64_t centsLimit) {
  if (provider.length() == 0) return true;   // nothing to store
  bool ok;
  {
    std::lock_guard<std::mutex> lk(g_ledgerMx);
    auto& L = ledgerLocked();
    const nimbus::orch::UsageLedger before = L;
    L.setLimits(std::string(provider.c_str()), tokenLimit, callLimit, resetDay, centsLimit);
    ok = persistOrRevertLocked(before);
  }
  // Lane DEF2: the cents limit is a hard monthly limit; after the ledger lock, the limits
  // store re-reads it, so a raised one lifts that provider's pause at once.
  agent::spend::capsChanged();
  return ok;
}

// CUM-708: the ledger's price for ranking `model` of `provider` (rankPrice): the
// owner's and the Cumulo router's prices count, not only the built-in table.
uint64_t modelRankPrice(const std::string& provider, const std::string& model) {
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  return ledgerLocked().rankPrice(provider, model.c_str());
}

String providerBudgetJson() {
  std::lock_guard<std::mutex> lk(g_ledgerMx);
  auto& L = ledgerLocked();
  String out = "[";
  bool first = true;
  for (const auto& e : L.entries()) {
    if (!first) out += ",";
    first = false;
    // The one formula (per-model prices, 2026-10): each model at its own price.
    nimbus::orch::PriceSummary ps;
    const uint64_t est = L.priceProvider(e.name, &ps);
    out += "{\"prov\":\"";     out += e.name.c_str();
    out += "\",\"in\":";       out += String((double)e.tokensIn, 0);
    out += ",\"out\":";         out += String((double)e.tokensOut, 0);
    out += ",\"calls\":";       out += String(e.calls);
    out += ",\"estCents\":";    out += String((double)est, 0);
    out += ",\"approx\":";      out += ps.approx ? "true" : "false";   // a built-in estimate is in it
    out += ",\"centsLimit\":";  out += String((double)e.centsLimit, 0);
    out += ",\"tokenLimit\":";  out += String((double)e.tokenLimit, 0);
    out += ",\"callLimit\":";   out += String(e.callLimit);
    out += ",\"resetDay\":";    out += String(e.resetDay);
    out += ",\"over\":";        out += (overLocked(e.name) ? "true" : "false");
    // L19-2: today's spend and the daily warning threshold it is held to.
    const uint32_t day = currentDayKey();
    if (day) {
      const nimbus::orch::DailyWarn w = L.dailyWarnCheck(histLocked(), e.name, day);
      out += ",\"todayCents\":";  out += String((double)w.spentCents, 0);
      out += ",\"warnCents\":";   out += String((double)w.thresholdCents, 0);
    }
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
    nimbus::orch::PriceSummary ps;
    const uint64_t est = L.priceProvider(e.name, &ps);
    out += "{\"prov\":\"";
    out += e.name.c_str();
    out += "\",\"tokens\":";     out += String((double)e.tokens, 0);
    out += ",\"calls\":";        out += String(e.calls);
    out += ",\"tokenLimit\":";   out += String((double)e.tokenLimit, 0);
    out += ",\"callLimit\":";    out += String(e.callLimit);
    out += ",\"resetDay\":";     out += String(e.resetDay);
    out += ",\"over\":";         out += (overLocked(e.name) ? "true" : "false");
    out += ",\"tokIn\":";        out += String((double)e.tokensIn, 0);
    out += ",\"tokOut\":";       out += String((double)e.tokensOut, 0);
    out += ",\"totIn\":";        out += String((double)e.totalIn, 0);
    out += ",\"totOut\":";       out += String((double)e.totalOut, 0);
    out += ",\"totCalls\":";     out += String((double)e.totalCalls, 0);
    out += ",\"rateIn\":";       out += String(e.centsPerMIn);
    out += ",\"rateOut\":";      out += String(e.centsPerMOut);
    out += ",\"rateCall\":";     out += String(e.centsPerKCalls);
    // W16: the $ ceiling + the SERVER-computed period spend estimate (cents) -
    // one formula (UsageLedger::estCents: each model at its own price, 2026-10)
    // shared with the budget gate and device.status, so no surface can disagree
    // with another. approx = some part of it is a built-in estimate ('~').
    out += ",\"centsLimit\":";   out += String((double)e.centsLimit, 0);
    out += ",\"estCents\":";     out += String((double)est, 0);
    out += ",\"approx\":";       out += ps.approx ? "true" : "false";
    // unpriced: spend this period of which no part resolves to a price, so a $ cap
    // cannot count it (the Budgets row says so).
    const bool spent = e.tokens || e.calls || e.cacheRead || e.cacheWrite;
    out += ",\"unpriced\":";     out += (spent && !ps.anyPriced) ? "true" : "false";
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
