#pragma once
#include <Arduino.h>

// store - device configuration accessors for the Orchestrator subsystem, backed
// by solide::memory (NVS). Mirrors the subset of Nuage-Solide's storage.h that
// the ported agent code (adapters + orchestrator + telegram) depends on, so the
// port reads/writes config the same way. Keys live in agent_config.h.
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
void   setDeviceTz(const String& tz);

// Cloud relay (cumulo-nimbus) pairing state. Orchestrator-only; ships dark
// (cloudOptIn default false). The credential is cloud-minted at pairing (unlike
// webAuthToken it HAS a setter) and wiped on unpair / a "bye:unpaired" from the relay.
bool   cloudOptIn();
void   setCloudOptIn(bool on);
String cloudDeviceId();
String cloudCred();
String cloudHost();          // relay host (default "app.cumulo-nimbus.ai")
String cloudName();          // paired device display name ("" = none)
bool   cloudPaired();        // has both a device id and a credential
void   setCloudPairing(const String& deviceId, const String& cred, const String& host,
                       const String& name);
void   clearCloudPairing();  // wipe id + credential + name (keeps the opt-in flag)

String dreamScratchHash();   // fnv64-hex of the scratchpad after the last dream ("" = none)
void   setDreamScratchHash(const String& hex);

// custom / proxy endpoint (registered as backend "custom" when base non-empty)
String customBase();
String customKey();
String customConv();         // "openai"|"mistral"|"anthropic" (wire convention)
String customModel();
bool   hasCustom();          // customBase() non-empty

// ---- orchestrator routing + models ----
String orchHost();                          // explicit host provider ("" => top of priority)
String resolvedOrchHost();                  // orchHost, else the head of providerPriority - use
                                            // this whenever the model WINDOW is resolved
int    effectiveToolResultCap();            // the per-result clamp a turn will apply (derived|owner)
String orchConvId();                        // "host|convId" (per-host conversation state)
void   setOrchConvId(const String& v);
String providerPriority();                  // orchestrator-host priority list
String orchPriority();                      // alias of providerPriority
String subPriority();                       // sub-session provider priority list
String orchModel(const String& provider);   // per-provider orchestrator model (defaults per provider)
String subModel(const String& provider);    // per-provider sub-session model
// Live model list harvested from the provider's /v1/models by the verify pass;
// "" until a verify has run. Callers fall back to the static compile-time list.
String modelChoices(const String& provider);
void   setModelChoices(const String& provider, const String& csv);
String agentFabricCfg();                    // legacy category bindings "code:openai,..."

// ---- user directive + TTS ----
String sysPrompt();          // the user directive (immutable by the model)
bool   ttsEnabled();         // "Voice replies" master toggle (default OFF, P2.5): gates the
                             // tts action, reply.speak, and reply.telegram voice - OFF = text only
bool   orchToolLoop();       // head multi-turn tool-use loop enabled (default ON, P6)
bool   midTurnFailover();    // mid-turn provider failover on loop turns (default ON)
bool   orchTrace();          void setOrchTrace(bool v);   // glass-box trace rows (default ON, A4)
// Head tool-loop caps (P6), user-tunable + clamped. Deadline in seconds.
int    orchLoopRounds();     // 1..32   (default 12)
int    orchLoopDeadlineS();  // 30..3600 (default 600)
int    orchLoopResultCap();  // 512..65536 (default 8192)
int    orchLoopTotalCap();   // 2048..1048576 (~256K tok ceiling; default 65536)
void   setOrchLoopRounds(int v);
void   setOrchLoopDeadlineS(int v);
void   setOrchLoopResultCap(int v);
void   setOrchLoopTotalCap(int v);
bool   allowHwTests();       // orchestrator may run device hardware self-tests (default ON)
bool   onboarded();          // first-run onboarding completed (plain NVS bool; false => show the setup wizard)
void   setOnboarded(bool v); // set true when the wizard finishes; cleared by an NVS-wipe factory reset
int    tlsSlots();           // concurrent work-TLS sessions 1..2 (default 1; latched at boot)
void   setTlsSlots(int v);   // clamped 1..2; applies on next boot (arbiter latches at begin)
bool   tlsVerify();          // validate provider certs vs the CA bundle (default ON) - tlsSetup() reads this
void   setTlsVerify(bool v); // false = fall back to setInsecure (self-signed host / unbundled root)
// ---- capability validation (W3b): how the device confirms provider access -----
// 0 = off (trust key presence, no VERIFIED claims), 1 = passive (default - use the
// verify cache, refreshed by the web Verify button and by a real turn SUCCEEDING
// on that provider; failures don't demote - most turn failures aren't auth),
// 2 = active (passive + a periodic free provider re-verify to keep the marking fresh).
int    capProbe();           void setCapProbe(int v);      // clamped 0..2 (default 1)
int    fetchPolicy();        void setFetchPolicy(int v);   // W18 URL downloads: 0 off/1 approve(default)/2 scan/3 yolo
int    capProbeHours();      void setCapProbeHours(int v); // active re-verify interval, clamped 1..168 (default 24)
// ---- OTA firmware update (src/sys/ota_update) ----
int    otaPending();             void setOtaPending(int v);        // 1 = image awaiting validation
int    otaBootCount();           void setOtaBootCount(int v);      // attempts since the flip
String otaPrevSlot();            void setOtaPrevSlot(const String& v);  // "app0"/"app1"
String otaLastResult();          void setOtaLastResult(const String& v);
bool   otaAutoUpdate();          void setOtaAutoUpdate(bool v);    // idle-window auto-install (default OFF)
String otaNotifiedVersion();     void setOtaNotifiedVersion(const String& v);  // Telegram no-re-nag
String otaPendingNotes();        void setOtaPendingNotes(const String& v);     // "ver|notes" across the install reboot
String batteryModelState();  // battery analytics learned state (CSV; "" if none)
void   setBatteryModelState(const String& v);
uint32_t lowBattPingEpoch();               // last low-battery owner ping (0 = armed)
void     setLowBattPingEpoch(uint32_t e);
// Benign config-action target: the model's `priority` knob tunes the SUB-SESSION
// provider preference only. It must NOT touch providerPriority()/orchPriority()
// (the orchestrator-HOST list) - that would let the model redirect its own brain,
// the exact control blocking the `orchHost` protected key is meant to deny.
void   setSubPriority(const String& v);
int    ledBright();          void setLedBright(int v);

// ---- Telegram ----
String   telegramToken();
String   telegramAllowlist();
String   telegramOwners();                   // OWNER chat ids (comma-sep; subset of allow). Empty => first allow entry is owner
void     setTelegramOwners(const String& v);
String   telegramNames();                    // "id:name,id:name" display sidecar (P8)
String   tgBotName();                        // connected bot's @username (set by the getMe verify)
void     setTgBotName(const String& v);
void     setTelegramNames(const String& v);
// Rename/upsert one chat's display name: rebuilds the sidecar dropping every
// exact-id entry, then appends id:name ("" name = just remove). Caller sanitizes.
void     replaceTelegramName(const String& id, const String& name);
bool     telegramPublic();                   // DANGER: accept anyone (default off, P8)
void     setTelegramPublic(bool v);
int32_t  telegramOffset();   void setTelegramOffset(int32_t v);

// ---- Anthropic managed-agents caches ----
String antEnvId();           void setAntEnvId(const String& v);
String antAgentMap();        void setAntAgentMap(const String& v);
String antOrchAgent();       void setAntOrchAgent(const String& v);

// ---- HUMAN-ONLY setters (web UI / provisioning console) ---------------------
// The write side of the CREDENTIAL GATE. These may be called ONLY from surfaces
// a human drives directly (the config web page, the provision/test consoles) -
// never from any code path the model can reach (device-action executor, turn
// loop, tools). Keys, the orchestrator-host list and orchHost redirect the
// model's own brain; exposing a setter to the model is the exact attack the
// protected-key list in device_actions exists to block. setSubPriority() above
// stays the ONLY model-writable routing knob.
void setOpenaiKey(const String& v);        // "" clears
void setAnthropicKey(const String& v);
void setMistralKey(const String& v);
void setTavilyKey(const String& v);        // web-search tool key ("" clears)
void setCustomBase(const String& v);
void setCustomKey(const String& v);
void setCustomConv(const String& v);       // "openai"|"mistral"|"anthropic"
void setCustomModel(const String& v);
void setOrchHost(const String& v);         // ""=auto (top of priority)
void setProviderPriority(const String& v); // orchestrator-HOST list (human only)
void setSysPrompt(const String& v);        // the user directive
void setOrchModel(const String& provider, const String& v);  // "" -> default
void setSubModel(const String& provider, const String& v);
void setTelegramToken(const String& v);    // WRITE-ONLY on the web surface
void setTelegramAllowlist(const String& v);
void setTtsEnabled(bool v);
void setOrchToolLoop(bool v);               // enable/disable the head tool-use loop
void setMidTurnFailover(bool v);            // enable/disable mid-turn provider failover
void setAllowHwTests(bool v);               // owner gate for orchestrator-triggered hw tests

// ---- provider key-verification cache (written by provider_verify) -----------
// result: 1 = verified (HTTP 200), 0 = rejected (401/403), -1 = couldn't verify
// (no key / connect failed / never attempted - ts 0 distinguishes "never").
int8_t   verifyResult(const String& provider);
uint32_t verifyTs(const String& provider);
void     setVerify(const String& provider, int8_t r, uint32_t ts);

// ---- embedding config for the vector memory (SET-ONCE; see agent_config.h) --
// The embed config is immutable once any vector has been embedded (embedLocked),
// because vectors from different provider/model/dims are incomparable. The web
// UI must require an explicit VDB reset before setEmbedConfig() on a locked
// config; setEmbedConfig() itself just writes (the reset/lock policy lives with
// the caller so this stays a plain accessor).
String embedProvider();      // default "openai"
String embedModel();         // default "text-embedding-3-small"
int    embedDims();          // default 256 (0 = provider-native width)
bool   embedLocked();        // true once the first vector was embedded
void   setEmbedConfig(const String& provider, const String& model, int dims);
void   setEmbedLocked(bool v);

// Voice providers (freely changeable). Default "mistral" (Voxtral STT + TTS).
String sttProvider();
String ttsProvider();
String ttsVoice();       // selected TTS voice id/slug ("" = provider default)
String theme();          // LED colour theme slug (default "teal")
// Display panel fitted to this device: "eink" (default) | "tft". Hardware
// identity - selects the display AND input driver at boot, so a change needs a
// restart. Exempt from "Revert to Defaults" (not a Config override).
String screenModel();
// Touch calibration blob ("" = the driver's defaults); see nimbus/touch_cal.h.
String touchCal();
void   setTouchCal(const String& v);
bool   screenIsTft();    // convenience: screenModel() == "tft" (STORED preference)
void   setBootScreenIsTft(bool v);  // main.cpp records the boot-bound driver (RAM-only)
int    bootScreenIsTft();           // 1/0 = bound tft/eink, -1 = not yet recorded
// Sound cues: per-mode sound levels (0-3) + shared sound theme (src/sfx).
uint8_t sfxLevelNotif(); // default 0 (none - a silent Notifier out of the box)
uint8_t sfxLevelOrch();  // default 2 (medium)
String  sfxTheme();      // "pulse" (default) | legacy themes fall back to the general pool
uint8_t sfxVolume();     // master speaker volume 0-100 (default 50 - the amp+speaker overdrive hot)
uint16_t saverMin();     // e-ink screensaver idle minutes, 0 = off (default 60)
uint16_t compactAtKB();  // fold trigger: KB of chat since last fold, 0 = off (default 48, clamp 8..512)
void     setCompactAtKB(uint16_t v);
// ---- battery/LED protection (owner feature 2026-07-17; study-grounded) -------
// sleepMv: pack mV that triggers the low-battery deep sleep (debounced in the
// power policy). Default 6000 = ~10% REAL SoC from the measured discharge study
// (resting 10% ≈ 6034 mV pack; the pack's own BMS let it fall to 5574 live). 0 = off.
// ---- battery HARDWARE config (owner feature 2026-07-17; boards differ: 220/100
// vs 270/120 dividers, LiitoKala 3500 vs reclaimed vape/18650 ~500 mAh; always 2S) --
uint32_t battRtop();     // divider R_top ohms (default 220000)
uint32_t battRbot();     // divider R_bottom ohms (default 100000)
uint16_t battDividerX100();  // (Rtop+Rbot)/Rbot * 100, clamped 100-2000 - what begin() wants
uint16_t battCapMah();   // pack capacity mAh, 100-20000 (default 3500)
uint16_t sleepMv();
// wakeMv: after a low-batt sleep, stay awake only at/above THIS (a drained pack
// RESTS UPWARD to 6918-6992 mV - same-threshold wake tests oscillate forever).
uint16_t wakeMv();
bool     sleepOvr();     // skip the sleep entirely - deep-discharge risk accepted
bool     brightOvr();    // lift the 60% LED cap to 100% - panel/shell heat risk accepted
bool     tftFlip();      // colour panel rotated 180 deg (which end of landscape is up)
bool     lowBattRing();  // show the low-battery ring light (default OFF - see ring_plan)
bool     lowBattSaver(); // low battery switches the battery mode (default ON - shipped)
// Connector registry (Phase C): one JSON blob, OWNER-ONLY writes (token-gated
// web endpoint; the model's `connector` config key is protected-BLOCKED).
// Parsed by agent::connectors::list(); may contain secrets - never echo raw.
String connectorsJson();
void   setConnectorsJson(const String& v);
void   setSttProvider(const String& v);
void   setTtsProvider(const String& v);
void   setTtsVoice(const String& v);
void   setTheme(const String& v);
void   setScreenModel(const String& v);  // allowlisted to "eink"|"tft"
void   setSfxLevelNotif(uint8_t v);   // clamped 0-3
void   setSfxLevelOrch(uint8_t v);    // clamped 0-3
void   setSfxTheme(const String& v);
void   setSfxVolume(uint8_t v);       // clamped 0-100
void   setSaverMin(uint16_t v);       // clamped 0-1440 (a day)
void   setBattRtop(uint32_t ohms);    // clamped 1k-10M
void   setBattRbot(uint32_t ohms);    // clamped 1k-10M
void   setBattCapMah(uint16_t mah);   // clamped 100-20000
void   setSleepMv(uint16_t v);        // clamped 0-6800 (a truly-full pack reads ~7900 RAW - 8000 was always-true = soft-brick)
void   setWakeMv(uint16_t v);         // clamped 0-7600
void   setSleepOvr(bool on);
void   setBrightOvr(bool on);
void   setTftFlip(bool on);
void   setLowBattRing(bool on);
void   setLowBattSaver(bool on);

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
void   recordProviderCall(const String& provider);                     // per search call
bool   providerOverBudget(const String& provider);                     // gate before spend
void   setProviderBudget(const String& provider, uint64_t tokenLimit,
                         uint32_t callLimit, uint8_t resetDay,
                         uint64_t centsLimit = 0);                      // owner web edit (W16: + $ cap in cents)
// Owner-editable price rates for $ estimates (display-time only, never enforcement):
// cents per 1M input tokens / 1M output tokens / 1000 calls. 0 = unset.
void   setProviderRates(const String& provider, uint32_t centsPerMIn,
                        uint32_t centsPerMOut, uint32_t centsPerKCalls);
String providerUsageJson();   // JSON array: [{prov,tokens,calls,...,tokIn,tokOut,totIn,totOut,totCalls,rateIn,rateOut,rateCall,centsLimit,estCents}]
// W16: compact per-provider budget view for the MODEL (device.status usage.budget):
// period counters + $ estimate vs ceilings + reset day. No tags/all-time detail -
// that stays on the web Usage page; this is what PLANNING needs.
String providerBudgetJson();
// Daily usage buckets for the Usage-pane graphs: {"today":<dayKey>,"days":[{d,prov,in,out,calls}]}
// (UTC days-since-epoch; ~60 days retained; LittleFS-backed). Returns a
// heap_caps_malloc'd buffer - PSRAM-preferred, up to ~90 KB at the bucket cap, so
// it must NEVER be copied into an internal-heap String; stream it chunked and
// free() it when done (nullptr on alloc failure). outLen = payload bytes.
char* usageHistoryJsonPs(size_t& outLen);

}  // namespace store
}  // namespace agent
