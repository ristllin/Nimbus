#pragma once
#include <Arduino.h>

#include <string>
#include <vector>

#include "nimbus/orch/model_ledger.h"   // TokenDelta, ModelRate, RateEdit (per-model prices)
#include "nimbus/orch/turn_spend.h"     // TurnSpend - recordProviderSpend
#include "nimbus/profile.h"   // nimbus::ProfileId for applyProfileDefaults (CUM-395)

// store - device configuration accessors for the Orchestrator subsystem, backed
// by solide::memory (NVS). Mirrors the subset of Nuage-Solide's storage.h that
// the ported agent code (adapters + orchestrator + telegram) depends on, so the
// port reads/writes config the same way. Keys live in agent_config.h.
//
// WRITE VERDICTS (CUM-479): every setter returns true iff its write LANDED in NVS.
// A full partition makes solide::memory refuse a write; the old `void` setters threw
// that away, so save routes answered {"ok":true} over dropped changes. Each write also
// reports to the persist ledger (nimbus::sys::notePersist), which logs the failing key
// and lets a route's nimbus::sys::SaveScope answer HTTP 507 storage_full. An empty
// value for a key whose default is "" is stored as an ERASE, so clearing a key or
// connector lands even on a 100% full partition (it frees entries, never needs one).
// Every setter is [[nodiscard]]: a dropped verdict is how the Telegram offset, the
// scratchpad and the OTA guard failed silently (the fifth recurrence of the class).
// There is no -Werror, so test_nvs_full enforces it: a call whose result is thrown
// away fails there unless it is in that test's reviewed baseline (which only shrinks).
//
// THE RESERVE (nimbus/store/nvs_reserve.h): a write that would leave fewer than
// kNvsReserveEntries free entries is refused unless its key is critical (the offset,
// the OTA guard, the credentials); a refusal reads as a failed write, so a route
// answers 507. Erases and shrinking rewrites are never refused.
//
// CREDENTIAL GATE (live-gated): every getter that returns a key/token/base is ""
// until the user provisions it via the web UI / NVS. All orchTurn* / adapter
// dispatch/poll / the Telegram loop are dead without these - the has*() helpers
// let the turn loop degrade gracefully (skip a provider with no key, reply "no
// provider configured", etc.) instead of firing a doomed TLS call.
namespace agent {
namespace store {

// ---- provider credentials + capability checks ----
String openaiKey();          bool hasOpenaiKey();
String anthropicKey();       bool hasAnthropicKey();
String mistralKey();         bool hasMistralKey();   // == Nuage store::hasKey()
String tavilyKey();          bool hasTavilyKey();     // web-search tool (orchestrator)
String webAuthToken();       // per-device web/MCP auth token (gen-on-first-use, NVS, no setter)
String regenWebAuthToken();  // rotate the token (owner-triggered); logs out every browser
String setupApPass();        // per-device setup-AP passphrase (gen-on-first-use, NVS, no
                             // setter - regenerates only via factory reset). Shown on the
                             // device's setup screen; the softAP + Wi-Fi QR use it.
String deviceTz();           // POSIX TZ for Local Loops wall-clock schedules (default UTC0)
[[nodiscard]] bool   setDeviceTz(const String& tz);

// Cloud relay (cumulo-nimbus) pairing state. Orchestrator-only; ships dark
// (cloudOptIn default false). The credential is cloud-minted at pairing (unlike
// webAuthToken it HAS a setter) and wiped on unpair / a "bye:unpaired" from the relay.
bool   cloudOptIn();
[[nodiscard]] bool   setCloudOptIn(bool on);
String cloudDeviceId();
String cloudCred();
String cloudHost();          // relay host (default "app.cumulo-nimbus.ai")
String cloudName();          // paired device display name ("" = none)
bool   cloudPaired();        // has both a device id and a credential
// Persist the pairing. Returns false if the credential did NOT land (a full 20 KB
// NVS). On a starved write it evicts any legacy mcat_ catalog cache (a backstop: since
// CUM-479 the catalogs live in flash files and normally nothing is left to evict) and
// retries once; false means the credential still did not fit.
[[nodiscard]] bool   setCloudPairing(const String& deviceId, const String& cred, const String& host,
                       const String& name);
[[nodiscard]] bool   clearCloudPairing();  // wipe id + credential + name (keeps the opt-in flag)

// Legacy (read once to migrate into /data/dream.json; never written since v4.5.14):
// fnv64-hex of the scratchpad after the last dream ("" = none).
String dreamScratchHash();

// custom / proxy endpoint (registered as backend "custom" when base non-empty)
String customBase();
String customKey();
String customConv();         // "openai"|"mistral"|"anthropic" (wire convention)
String customModel();
bool   hasCustom();          // customBase() non-empty

// Z.ai (GLM) - OpenAI-compatible provider. zaiBase is the probed working host.
String zaiKey();             bool hasZaiKey();
String zaiBase();            [[nodiscard]] bool setZaiBase(const String& host);   // "" until probed
// Cumulo Nimbus router - one key, per-role upstream chosen via the model id.
String cumuloKey();          bool hasCumuloKey();
String cumuloBase();         // "" -> CUMULO_HOST_DEFAULT
// Fallback rule set (device engine; shared v1 schema with the cloud). "" = use the
// shipped size-class defaults. syncTs>0 means the set was pulled from the cloud.
String   fallbackRulesJson();      [[nodiscard]] bool setFallbackRulesJson(const String& json);
uint32_t fallbackSyncTs();         [[nodiscard]] bool setFallbackSyncTs(uint32_t ts);

// ---- orchestrator routing + models ----
String orchHost();                          // explicit host provider ("" => top of priority)
bool   providerHasKey(const String& name);  // is an API key stored for openai|anthropic|mistral
String resolvedOrchHost();                  // orchHost, else the first KEYED provider in priority - use
                                            // this whenever the model WINDOW is resolved
int    effectiveToolResultCap();            // the per-result clamp a turn will apply (derived|owner)
String orchConvId();                        // "host|convId" (per-host conversation state)
[[nodiscard]] bool   setOrchConvId(const String& v);
String providerPriority();                  // orchestrator-host priority list
String orchPriority();                      // alias of providerPriority
String subPriority();                       // sub-session provider priority list
String orchModel(const String& provider);   // per-provider orchestrator model (defaults per provider)
String subModel(const String& provider);    // per-provider sub-session model
// Live model list harvested from the provider's /v1/models by the verify pass;
// "" until a verify has run. Callers fall back to the static compile-time list.
String modelChoices(const String& provider);
[[nodiscard]] bool   setModelChoices(const String& provider, const String& csv);
// Rich capability-aware catalog per provider (GET /api/models model array as JSON).
// A REBUILDABLE cache, so it lives in a LittleFS file (/data/mcat_<provider>.json),
// never in NVS (CUM-479: five ~3.8 KB catalogs exhausted the 20 KB partition). ""
// = never harvested. Survives a reboot; the CSV above stays for the /api/orch
// dropdowns (bounded: at most 8 ids, the one catalog-ish value left in NVS).
String modelCatalogJson(const String& provider);
[[nodiscard]] bool   setModelCatalogJson(const String& provider, const String& json);  // false = file write failed
// Erase every legacy NVS catalog key (mcat_<provider>, pre-CUM-479). Erasing only
// frees entries, so it works on a full partition. Returns how many were removed.
int    evictLegacyCatalogCaches();
// Boot heal (called once from provider_verify::begin): copy each legacy NVS catalog
// to its file when the file is missing, then evict every legacy key, so a device that
// is full today gets its owner-config room back on the first boot of this build.
int    migrateCatalogCachesOffNvs();
// The NVS reserve's eviction pass (main loop only; see nimbus/store/nvs_reserve.h):
// at most every 30 s, or right after a write was refused, erase rebuildable caches
// until the reserve is back, else mark the device starved. Called from otaupd::tick.
void   nvsMaintain(uint32_t nowMs);
String agentFabricCfg();                    // legacy category bindings "code:openai,..."

// ---- user directive + TTS ----
String sysPrompt();          // the user directive (immutable by the model)
bool   ttsEnabled();         // "Voice replies" master toggle (default ON, N12 - see
                             // TTS_ENABLED_DEFAULT): gates the tts action, reply.speak, and
                             // reply.telegram voice - OFF = text only
bool   orchToolLoop();       // head multi-turn tool-use loop enabled (default ON, P6)
bool   codeSandbox();        // Assistant > Tools: Code sandbox (code_interpreter/code_execution) (default OFF)
bool   midTurnFailover();    // mid-turn provider failover on loop turns (default ON)
bool   orchPromptV2();       // N11: simplified v2 system prompt A/B (default OFF)
bool   orchTrace();          [[nodiscard]] bool setOrchTrace(bool v);   // glass-box trace rows (default ON, A4)
// Head tool-loop caps (P6), user-tunable + clamped. Deadline in seconds.
int    orchLoopRounds();     // 1..32   (default 8)
int    orchLoopDeadlineS();  // 30..3600 (default 600)
int    orchLoopResultCap();  // 512..65536 (default 8192)
int    orchLoopTotalCap();   // 2048..1048576 (~256K tok ceiling; default 65536)
[[nodiscard]] bool   setOrchLoopRounds(int v);
[[nodiscard]] bool   setOrchLoopDeadlineS(int v);
[[nodiscard]] bool   setOrchLoopResultCap(int v);
[[nodiscard]] bool   setOrchLoopTotalCap(int v);
// Local Loops governor (routines) owner overrides (CUM-73). 0 = no override,
// keep the caps.h default; a set value may only TIGHTEN (the actual folding is
// nimbus::orch::clampLoopCaps). Getters report the raw stored override.
int    loopCapMaxCount();          // 0 or a stricter max routine count
int    loopCapMinIntervalS();      // 0 or a longer minimum fire interval (s)
int    loopCapFiresPerDay();       // 0 or a lower per-routine daily fire ceiling
int    loopCapTokensPerDay();      // 0 or a lower per-routine daily token ceiling
int    loopCapDevTokensPerDay();   // 0 or a lower device-wide daily token ceiling
int    loopCapDevFiresWindow();    // 0 or a lower device-wide fires-per-window
[[nodiscard]] bool   setLoopCapMaxCount(int v);
[[nodiscard]] bool   setLoopCapMinIntervalS(int v);
[[nodiscard]] bool   setLoopCapFiresPerDay(int v);
[[nodiscard]] bool   setLoopCapTokensPerDay(int v);
[[nodiscard]] bool   setLoopCapDevTokensPerDay(int v);
[[nodiscard]] bool   setLoopCapDevFiresWindow(int v);
bool   allowHwTests();       // orchestrator may run device hardware self-tests (default ON)
bool   onboarded();          // first-run onboarding completed (plain NVS bool; false => show the setup wizard)
[[nodiscard]] bool   setOnboarded(bool v); // set true when the wizard finishes; cleared by an NVS-wipe factory reset
int    tlsSlots();           // concurrent work-TLS sessions 1..2 (default 1; latched at boot)
[[nodiscard]] bool   setTlsSlots(int v);   // clamped 1..2; applies on next boot (arbiter latches at begin)
bool   tlsVerify();          // validate provider certs vs the CA bundle (default ON) - tlsSetup() reads this
[[nodiscard]] bool   setTlsVerify(bool v); // false = fall back to setInsecure (self-signed host / unbundled root)
// ---- capability validation (W3b): how the device confirms provider access -----
// 0 = off (trust key presence, no VERIFIED claims), 1 = passive (default - use the
// verify cache, refreshed by the web Verify button and by a real turn SUCCEEDING
// on that provider; failures don't demote - most turn failures aren't auth),
// 2 = active (passive + a periodic free provider re-verify to keep the marking fresh).
int    capProbe();           [[nodiscard]] bool setCapProbe(int v);      // clamped 0..2 (default 1)
int    fetchPolicy();        [[nodiscard]] bool setFetchPolicy(int v);   // W18 URL downloads: 0 off/1 approve(default)/2 scan/3 yolo
// Moderation gate switches (non-admin only; each a paid classifier call). Default off.
bool   modInbound();         [[nodiscard]] bool setModInbound(bool on);   // inbound guest text pre-turn (fail-closed)
bool   modOutbound();        [[nodiscard]] bool setModOutbound(bool on);  // outbound replies to guests (fail-open)
bool   modInjection();       [[nodiscard]] bool setModInjection(bool on); // injection-screen world content (fail-open + mark)
int    capProbeHours();      [[nodiscard]] bool setCapProbeHours(int v); // active re-verify interval, clamped 1..168 (default 24)
// ---- OTA firmware update (src/sys/ota_update) ----
int    otaPending();             [[nodiscard]] bool setOtaPending(int v);        // 1 = image awaiting validation
int    otaBootCount();           [[nodiscard]] bool setOtaBootCount(int v);      // attempts since the flip
String otaPrevSlot();            [[nodiscard]] bool setOtaPrevSlot(const String& v);  // "app0"/"app1"
String otaLastResult();          [[nodiscard]] bool setOtaLastResult(const String& v);
bool   otaAutoUpdate();          [[nodiscard]] bool setOtaAutoUpdate(bool v);    // idle-window auto-install (default OFF)
bool   usbUpdateConfirm();       [[nodiscard]] bool setUsbUpdateConfirm(bool v); // require on-device confirm for a USB update (default OFF)
String otaNotifiedVersion();     [[nodiscard]] bool setOtaNotifiedVersion(const String& v);  // Telegram no-re-nag
String otaPendingNotes();        [[nodiscard]] bool setOtaPendingNotes(const String& v);     // "ver|notes" across the install reboot
String batteryModelState();  // battery analytics learned state (CSV; "" if none)
[[nodiscard]] bool   setBatteryModelState(const String& v);
uint32_t lowBattPingEpoch();               // last low-battery owner ping (0 = armed)
[[nodiscard]] bool     setLowBattPingEpoch(uint32_t e);
// Benign config-action target: the model's `priority` knob tunes the SUB-SESSION
// provider preference only. It must NOT touch providerPriority()/orchPriority()
// (the orchestrator-HOST list) - that would let the model redirect its own brain,
// the exact control blocking the `orchHost` protected key is meant to deny.
[[nodiscard]] bool   setSubPriority(const String& v);
int    ledBright();          [[nodiscard]] bool setLedBright(int v);

// ---- Telegram ----
String   telegramToken();
String   telegramAllowlist();
String   telegramOwners();                   // OWNER chat ids (comma-sep; subset of allow). Empty => first allow entry is owner
[[nodiscard]] bool     setTelegramOwners(const String& v);
String   telegramNames();                    // "id:name,id:name" display sidecar (P8)
String   tgBotName();                        // connected bot's @username (set by the getMe verify)
[[nodiscard]] bool     setTgBotName(const String& v);
[[nodiscard]] bool     setTelegramNames(const String& v);
// Rename/upsert one chat's display name: rebuilds the sidecar dropping every
// exact-id entry, then appends id:name ("" name = just remove). Caller sanitizes.
[[nodiscard]] bool     replaceTelegramName(const String& id, const String& name);
bool     telegramPublic();                   // DANGER: accept anyone (default off, P8)
[[nodiscard]] bool     setTelegramPublic(bool v);
int32_t  telegramOffset();   [[nodiscard]] bool setTelegramOffset(int32_t v);

// ---- Anthropic managed-agents caches ----
String antEnvId();           [[nodiscard]] bool setAntEnvId(const String& v);
String antAgentMap();        [[nodiscard]] bool setAntAgentMap(const String& v);
String antOrchAgent();       [[nodiscard]] bool setAntOrchAgent(const String& v);

// ---- HUMAN-ONLY setters (web UI / provisioning console) ---------------------
// The write side of the CREDENTIAL GATE. These may be called ONLY from surfaces
// a human drives directly (the config web page, the provision/test consoles) -
// never from any code path the model can reach (device-action executor, turn
// loop, tools). Keys, the orchestrator-host list and orchHost redirect the
// model's own brain; exposing a setter to the model is the exact attack the
// protected-key list in device_actions exists to block. setSubPriority() above
// stays the ONLY model-writable routing knob.
[[nodiscard]] bool setOpenaiKey(const String& v);        // "" clears
[[nodiscard]] bool setAnthropicKey(const String& v);
[[nodiscard]] bool setMistralKey(const String& v);
[[nodiscard]] bool setTavilyKey(const String& v);        // web-search tool key ("" clears)
[[nodiscard]] bool setCustomBase(const String& v);
[[nodiscard]] bool setCustomKey(const String& v);
[[nodiscard]] bool setCustomConv(const String& v);       // "openai"|"mistral"|"anthropic"
[[nodiscard]] bool setCustomModel(const String& v);
[[nodiscard]] bool setZaiKey(const String& v);           // Z.ai (GLM) token ("" clears)
[[nodiscard]] bool setCumuloKey(const String& v);        // Cumulo router key ("" clears)
[[nodiscard]] bool setCumuloBase(const String& v);       // router base URL/host ("" -> default)
[[nodiscard]] bool setOrchHost(const String& v);         // ""=auto (top of priority)
[[nodiscard]] bool setProviderPriority(const String& v); // orchestrator-HOST list (human only)
[[nodiscard]] bool setSysPrompt(const String& v);        // the user directive
[[nodiscard]] bool setOrchModel(const String& provider, const String& v);  // "" -> default
[[nodiscard]] bool setSubModel(const String& provider, const String& v);
[[nodiscard]] bool setTelegramToken(const String& v);    // WRITE-ONLY on the web surface
[[nodiscard]] bool setTelegramAllowlist(const String& v);
[[nodiscard]] bool setTtsEnabled(bool v);
[[nodiscard]] bool setOrchToolLoop(bool v);               // enable/disable the head tool-use loop
[[nodiscard]] bool setCodeSandbox(bool v);                // enable/disable the code sandbox tool
[[nodiscard]] bool setMidTurnFailover(bool v);            // enable/disable mid-turn provider failover
[[nodiscard]] bool setAllowHwTests(bool v);               // owner gate for orchestrator-triggered hw tests

// ---- provider key-verification cache (written by provider_verify) -----------
// result: 1 = verified (HTTP 200), 0 = rejected (401/403), -1 = couldn't verify
// (no key / connect failed / never attempted - ts 0 distinguishes "never").
int8_t   verifyResult(const String& provider);
uint32_t verifyTs(const String& provider);
[[nodiscard]] bool     setVerify(const String& provider, int8_t r, uint32_t ts);

// ---- embedding config for the vector memory (SET-ONCE; see agent_config.h) --
// The embed config is immutable once any vector has been embedded (embedLocked),
// because vectors from different provider/model/dims are incomparable. The web
// UI must require an explicit VDB reset before setEmbedConfig() on a locked
// config; setEmbedConfig() itself just writes (the reset/lock policy lives with
// the caller so this stays a plain accessor).
String embedProvider();      // default "openai"
String embedModel();         // default "text-embedding-3-small"
int    embedDims();          // default 256; must equal the model's output width. POST
                             // /api/mem/embedcfg resolves dims=0 to it before storing
                             // (CUM-469), so 0 is only ever a legacy stored value.
bool   embedLocked();        // true once the first vector was embedded
// True iff all three landed; on a partial failure the previous values are put back
// (an absent key back to absent), so the stored embedding space is never half old.
[[nodiscard]] bool   setEmbedConfig(const String& provider, const String& model, int dims);
[[nodiscard]] bool   setEmbedLocked(bool v);

// Voice providers (freely changeable). Default "mistral" (Voxtral STT + TTS).
String sttProvider();
String ttsProvider();
String ttsVoice();       // selected TTS voice id/slug ("" = provider default)
String theme();          // LED colour theme slug (default "teal")
// Display panel fitted to this device: "eink" (default) | "tft". Hardware
// identity - selects the display AND input driver at boot, so a change needs a
// restart. Exempt from "Revert to Defaults" (not a Config override).
String screenModel();
// RAW stored scrModel ("" when the key is absent) - the boot seam tells a fresh
// device from an explicit legacy value with it (nimbus::display::showsUnsupportedNotice).
String screenModelStored();
// Touch calibration blob ("" = the driver's defaults); see nimbus/touch_cal.h.
String touchCal();
[[nodiscard]] bool   setTouchCal(const String& v);
bool   screenIsTft();    // convenience: screenModel() == "tft" (STORED preference)
void   setBootScreenIsTft(bool v);  // main.cpp records the boot-bound driver (RAM-only)
int    bootScreenIsTft();           // 1/0 = bound tft/other, -1 = not yet recorded
// Sound cues: per-mode sound levels (0-3) + shared sound theme (src/sfx).
uint8_t sfxLevelNotif(); // default 0 (none - a silent Notifier out of the box)
uint8_t sfxLevelOrch();  // default 2 (medium)
String  sfxTheme();      // "pulse" (default) | legacy themes fall back to the general pool
uint8_t sfxVolume();     // master speaker volume 0-100 (default 50 - the amp+speaker overdrive hot)
uint16_t saverMin();     // screensaver idle minutes, 0 = off (default 5)
// CUM-395: did the OWNER explicitly set this key (vs. it holding a shipped or a
// profile-seeded default)? An explicit owner value always wins over a battery-mode
// default, so applyProfileDefaults() only seeds a key whose has*() is false.
bool     hasSaverMin();
bool     hasSfxLevelNotif();
bool     hasSfxLevelOrch();
// Seed the screen-rest + sound-level defaults for a battery mode (CUM-395). Writes a
// profile default ONLY into a key the owner has not set explicitly, so an owner value
// is never clobbered and a re-selected profile re-seeds the untouched keys. Called on
// a battery-mode switch. Precedence: explicit owner value > profile default > hard
// default (see nimbus::effectiveWithProfileDefault).
void     applyProfileDefaults(nimbus::ProfileId id);
// CUM-408: true when the one-time profile-seed migration could not persist its "done"
// marker because NVS is full, so it has been deferred (latched off for this boot rather
// than re-running on every battery-mode switch). Surfaced as a health hint; clears on a
// reboot that finds NVS headroom. Lock-free RAM read.
bool     profileSeedDeferredNvsFull();
uint16_t compactAtKB();  // fold trigger: KB of chat since last fold, 0 = off (default 48, clamp 8..512)
[[nodiscard]] bool     setCompactAtKB(uint16_t v);
// ---- battery/LED protection (owner feature 2026-07-17; study-grounded) -------
// sleepMv: pack mV that triggers the low-battery deep sleep (debounced in the
// power policy). Default + clamp scale with the series-cell count (CUM-202): 2S =
// 6000 (~10% REAL SoC from the measured discharge study; resting 10% ≈ 6034 mV
// pack, BMS let it fall to 5574 live), 1S = 3000. Cell-aware so a full 1S pack
// (~4200 mV) does not sit permanently below a 2S floor and insta-sleep. 0 = off.
// ---- battery HARDWARE config (owner feature 2026-07-17; boards differ: 220/100
// vs 270/120 dividers, LiitoKala 3500 vs reclaimed vape/18650 ~500 mAh; always 2S) --
uint32_t battRtop();     // divider R_top ohms (default 220000)
uint32_t battRbot();     // divider R_bottom ohms (default 100000)
uint16_t battDividerX100();  // (Rtop+Rbot)/Rbot * 100, clamped 100-2000 - what begin() wants
uint16_t battCapMah();   // pack capacity mAh, 100-20000 (default 3500)
String   battChem();     // chemistry slug "liion" (default) | "lifepo4" - picks the SoC curve
uint8_t  battCellsOvr(); // owner series-cell override (1/2); 0 = board-derived default
uint8_t  battCellsEff(); // effective series-cell count: override, else board default (guard to 1)
String   battCurve();    // optional custom SoC curve "mv:pct,..." (high-mV first); "" = chemistry default
[[nodiscard]] bool     setBattChem(const String& slug);
[[nodiscard]] bool     setBattCells(uint8_t cells);
[[nodiscard]] bool     setBattCurve(const String& csv);
uint16_t sleepMv();
// wakeMv: after a low-batt sleep, stay awake only at/above THIS (a drained pack
// RESTS UPWARD to 6918-6992 mV - same-threshold wake tests oscillate forever).
uint16_t wakeMv();
bool     sleepOvr();     // skip the sleep entirely - deep-discharge risk accepted
bool     brightOvr();    // lift the 60% LED cap to 100% - panel/shell heat risk accepted
bool     tftFlip();      // colour panel rotated 180 deg (which end of landscape is up)
bool     lowBattRing();  // show the low-battery ring light (default OFF - see ring_plan)
bool     lowBattSaver(); // low battery switches the battery mode (default ON - shipped)
bool     battMon(bool def); // battery monitoring enabled (def = board-derived default)
// Connector registry (Phase C): one JSON blob, OWNER-ONLY writes (token-gated
// web endpoint; the model's `connector` config key is protected-BLOCKED).
// Parsed by agent::connectors::list(); may contain secrets - never echo raw.
String connectorsJson();
[[nodiscard]] bool   setConnectorsJson(const String& v);
[[nodiscard]] bool   setSttProvider(const String& v);
[[nodiscard]] bool   setTtsProvider(const String& v);
[[nodiscard]] bool   setTtsVoice(const String& v);
[[nodiscard]] bool   setTheme(const String& v);
[[nodiscard]] bool   setScreenModel(const String& v);  // allowlisted to "eink"|"tft"
[[nodiscard]] bool   setSfxLevelNotif(uint8_t v);   // clamped 0-3
[[nodiscard]] bool   setSfxLevelOrch(uint8_t v);    // clamped 0-3
[[nodiscard]] bool   setSfxTheme(const String& v);
[[nodiscard]] bool   setSfxVolume(uint8_t v);       // clamped 0-100
[[nodiscard]] bool   setSaverMin(uint16_t v);       // clamped 0-1440 (a day)
[[nodiscard]] bool   setBattRtop(uint32_t ohms);    // clamped 1k-10M
[[nodiscard]] bool   setBattRbot(uint32_t ohms);    // clamped 1k-10M
[[nodiscard]] bool   setBattCapMah(uint16_t mah);   // clamped 100-20000
[[nodiscard]] bool   setSleepMv(uint16_t v);        // clamped 0 - (3400 x cells) (2S 6800; a truly-full pack reads ~3950/cell RAW - a pack-wide ceiling was always-true = soft-brick)
[[nodiscard]] bool   setWakeMv(uint16_t v);         // clamped 0 - (3800 x cells) (2S 7600)
[[nodiscard]] bool   setSleepOvr(bool on);
[[nodiscard]] bool   setBrightOvr(bool on);
[[nodiscard]] bool   setTftFlip(bool on);
[[nodiscard]] bool   setLowBattRing(bool on);
[[nodiscard]] bool   setLowBattSaver(bool on);
[[nodiscard]] bool   setBattMon(bool on);

// ---- per-provider usage ledger + budgets (owner: "limit budget, budget per
// provider - Tavily monthly calls, LLM monthly tokens, per-provider month reset").
// Backed by the portable nimbus::orch::UsageLedger, cached in RAM + persisted to NVS
// (AKEY_USAGE_LEDGER). All accessors are mutex-guarded (web GET reads on the AsyncTCP
// task race the turn task's record). Rolling month keyed off the SNTP wall clock.
void   recordProviderTokens(const String& provider, uint32_t tokensIn,
                            uint32_t tokensOut);                        // per LLM turn (in/out split)
// Attribution overload (additive): also files the spend under a tag -
// "turn" | "synthesis" | "loop:<id>" | "spawn:<backend>" - in the ledger's
// all-time per-(provider, tag) audit counters. The untagged form above stays
// for callers with no source context (records as plain budget spend).
void   recordProviderTokens(const String& provider, uint32_t tokensIn,
                            uint32_t tokensOut, const char* tag);
// + prompt-cache counters (v4.1.3): pass ONLY for providers whose usage
// excludes them from tokensIn (Anthropic) - see UsageLedger::ProviderBudget.
void   recordProviderTokens(const String& provider, uint32_t tokensIn,
                            uint32_t tokensOut, uint32_t cacheRead,
                            uint32_t cacheWrite, const char* tag);
// + the model (2026-10, per-model prices): `model` is the usageModelKey ("" =
// provider level only). Files the same spend under (provider, model) in the model
// ledger (/data/usage_models.txt) and the (day, provider, model) history bucket.
// Bounded: O(32 + 720) per call, no allocation that outlives it, no new task.
void   recordProviderTokens(const String& provider, const nimbus::orch::TokenDelta& d,
                            const char* tag, const char* model);
// One turn's spend (lane T): every host that billed, each under the model it billed
// with, through nimbus::orch::ledgerDelta (the R4 cache split), then ONE persist of
// each store - the NVS usgLedger once per turn, whatever the number of hosts.
void   recordProviderSpend(const nimbus::orch::TurnSpend& spend, const char* tag);
void   recordProviderCall(const String& provider);                     // per search call
bool   providerOverBudget(const String& provider);                     // gate before spend
// CUM-708: the ledger's ranking price of a model (micro-USD per 1M in + out; UINT64_MAX
// when unpriced) - what the spawn-model fallback picks the cheapest by.
uint64_t modelRankPrice(const std::string& provider, const std::string& model);
[[nodiscard]] bool   setProviderBudget(const String& provider, uint64_t tokenLimit,
                         uint32_t callLimit, uint8_t resetDay,
                         uint64_t centsLimit = 0);                      // owner web edit (W16: + $ cap in cents)
// The owner's provider FALLBACK prices (cents per 1M input / 1M output tokens /
// 1000 calls; 0 = unset): they price every model of the provider with no price of
// its own. They feed the $ estimate and so arm the $ cap (enforcement inputs).
[[nodiscard]] bool   setProviderRates(const String& provider, uint32_t centsPerMIn,
                        uint32_t centsPerMOut, uint32_t centsPerKCalls);
// The owner's price for one model (micro-USD per 1M tokens; 0:0 clears it), stored
// in /data/usage_mrates.txt (never NVS). Full answers without changing anything;
// WriteFailed restores the table and reports "lfs_mrates" to the persist ledger, so
// the route's SaveScope answers 507 (CUM-479).
nimbus::orch::RateEdit setModelRate(const String& provider, const char* model,
                                    uint32_t microIn, uint32_t microOut);
// The Cumulo router's prices from the verify harvest (verify task only, never the
// turn path), stored in /data/usage_rprices.txt. False when the file did not store.
[[nodiscard]] bool   replaceRouterPrices(const std::vector<nimbus::orch::ModelRate>& rows);
// Whether a Cumulo router price is stored (/data/usage_rprices.txt): 1 = none (the
// file missing or empty), 0 = stored, -1 = flash storage not loaded yet (cannot tell).
// Boot asks for one Cumulo verify while it reads 1.
int8_t routerPriceState();
// The models an owner may price for `provider`: comma-separated keys (the mrate
// allowlist, nimbus::orch::editableHas).
std::string editableModels(const String& provider);
// The usage ledger file (/data/usage_ledger.txt since v4.5.14): false while RAM holds
// spend the file did not take (a failed write; every record and the main loop retry).
bool   usageLedgerSaved();
void   retryUsageLedger();
// The live routine ids (main loop): attribution rows of deleted routines are dropped.
void   noteLiveLoopIds(const std::vector<std::string>& ids);
String providerUsageJson();   // JSON array: [{prov,tokens,calls,...,tokIn,tokOut,totIn,totOut,totCalls,rateIn,rateOut,rateCall,centsLimit,estCents,approx}]
// W16: compact per-provider budget view for the MODEL (device.status usage.budget):
// period counters + $ estimate vs ceilings + reset day. No tags/all-time detail -
// that stays on the web Usage page; this is what PLANNING needs.
String providerBudgetJson();
// Daily usage buckets for the Usage-pane graphs: {"today":<dayKey>,"days":[{d,prov,in,out,calls[,m,cr,cw]}]}
// (UTC days-since-epoch; ~60 days retained; LittleFS-backed). Returns a
// heap_caps_malloc'd buffer - PSRAM-preferred, up to ~170 KB at the bucket cap, so
// it must NEVER be copied into an internal-heap String; stream it chunked and
// free() it when done (nullptr on alloc failure). outLen = payload bytes.
char* usageHistoryJsonPs(size_t& outLen);
// GET /api/usage/models (nimbus::orch::writeUsageModelsJson): per-model spend and
// prices, the residual, the Prices table rows. Same buffer contract as above.
char* usageModelsJsonPs(size_t& outLen);

}  // namespace store
}  // namespace agent
