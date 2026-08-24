#pragma once
// agent_config - device-side constants for the Orchestrator subsystem, ported
// from Nuage-Solide include/config.h (the Head-Orchestrator / heavy-fabric block).
//
// The PORTABLE ceilings (memory caps, journal capacity, MAX_ACTIVE_INFLIGHT,
// spawn field sizes) are the source of truth in lib/core (nimbus/orch/caps.h);
// this header holds only the DEVICE-side numbers the portable core has no opinion
// on: provider REST endpoints, model choice lists, heap floors, poll cadence, and
// the NVS key names. The heap floors + poll cadence are the no-PSRAM survival
// values; keep as-is for P5 and relax behind a flag after on-hardware heap-watch
// (plan §3.7).
//
// VERIFY(2026-06): the model lists + endpoints below are copied from Nuage-Solide
// verified live 2026-06-25. Re-verify at implementation: OpenAI has no "-latest"
// flagship alias (needs a periodic bump), Anthropic managed-agents beta header +
// endpoints, model overview pages.

// ---- Provider model choice lists (comma-separated) --------------------------
#define MISTRAL_MODEL_CHOICES "mistral-large-latest,mistral-medium-latest,mistral-small-latest"
#define OPENAI_MODEL_CHOICES  "gpt-5.5,gpt-5.4-mini,o4-mini-deep-research"
#define ANT_MODEL_CHOICES     "claude-opus-4-8,claude-sonnet-4-6,claude-haiku-4-5"

// ---- Anthropic REST endpoints ----------------------------------------------
#define ANTHROPIC_HOST  "api.anthropic.com"
#define ANTHROPIC_PORT  443
#define ANTHROPIC_VER   "2023-06-01"
// Default Anthropic model (orchestrator + sub-session fallback).
#define ANT_MODEL       "claude-sonnet-4-6"

// ---- OpenAI REST endpoints -------------------------------------------------
#define OPENAI_HOST  "api.openai.com"
#define OPENAI_PORT  443
// No "-latest" flagship alias exists for OpenAI, so this needs a periodic bump.
#define OPENAI_MODEL "gpt-5.5"

// ---- Mistral REST endpoints (host ported 2026-07 - Conversations API) -------
#define MISTRAL_HOST "api.mistral.ai"

// Z.ai (GLM) - OpenAI-compatible, but the base path is /api/paas/v4 (NOT /v1). The
// token answers at one of two hosts; the device probes both and pins the winner.
#define ZAI_HOST_PRIMARY  "api.z.ai"
#define ZAI_HOST_FALLBACK "open.bigmodel.cn"
#define ZAI_BASE_PATH     "/api/paas/v4"

// Cumulo Nimbus router - one key, upstream selectable per role. OpenAI-compatible
// wire under /router/<upstream>/v1/...; verify hits /router/<upstream>/v1/models.
#define CUMULO_HOST_DEFAULT "app.cumulo-nimbus.ai"
#define MISTRAL_PORT 443
// Default Mistral model (orchestrator + sub-session fallback); the "-latest"
// alias tracks the flagship without a periodic bump.
#define MISTRAL_MODEL "mistral-large-latest"

// ---- Poll cadence + INTERNAL-heap floors -----------------------------------
// ⚠ MEMORY MODEL (read docs/memory-model.md before touching these): this S3 has
// TWO pools - ~266 KB INTERNAL SRAM (fast, DMA-capable, SCARCE) that ESP.getFreeHeap()
// measures, and 8 MB PSRAM (external, abundant, ~98% free). The floors below gate on
// INTERNAL heap ONLY. They are NOT a whole-device memory budget: a turn's heavy
// buffers (mbedTLS, request/response bodies, ArduinoJson, the VDB) are already routed
// to PSRAM, so a turn's true INTERNAL cost is just small HTTP/JSON transients + the
// lwIP/TLS stack. The real internal red line is the ~24 KB lwIP/TLS danger zone; the
// floors sit just above it with margin. Do NOT re-raise these to "survive with no
// PSRAM" - PSRAM is present and carries the churn (main.cpp routes it; the spill
// threshold heap_caps_malloc_extmem_enable governs how much).
#define AGENT_POLL_INTERVAL_MS  15000  // baseline running-job poll interval
// A turn's heavy TLS buffers (mbedTLS RX/TX ~16 KB each + session/cert) are routed
// to PSRAM (main.cpp installs a PSRAM-backed mbedTLS allocator), so a turn now costs
// only its small JSON/HTTP transients out of INTERNAL heap. The old 52 KB floor was
// tuned for the pre-PSRAM era (~68 KB resting); the richer round-4/5 firmware sits
// ~46 KB internal at rest (measured live: /api/state heap=45956, heapMin=13056 - it
// has safely dipped to 13 KB during work), so the 52 KB floor perpetually DEFERRED
// real turns ("finishing background work") even with nothing running. Floors lowered
// to the true internal-heap need + margin; PSRAM (8+ MB free) carries the TLS. (A turn
// that still OOMs fails soft - the orchestrator returns an error reply, no reboot.)
#define ORCH_AUTO_TURN_MIN_HEAP 30000U  // defer the SYNTHESIS turn below this (internal; TLS is PSRAM)
// Measured live 2026-07-06 (post Phase-C firmware): resting internal heap sits
// ~37-40 KB and a turn's heavy TLS is PSRAM-backed, so the old 38 KB floor
// deferred REAL user turns at rest. Aligned with the dispatch floor: the turn's
// internal-heap transients are the same small HTTP/JSON buffers.
// 28 KB = the same ~24 KB lwIP/TLS internal danger zone + ~4 KB margin the loop
// re-gate uses (ORCH_LOOP_MIN_HEAP). The turn's big buffers are PSRAM-routed, so this
// is the honest internal floor; the old 34 KB deferred real turns while PSRAM sat
// ~98% empty (proven live: at 16 KB internal turns deferred; freeing internal to
// ~48 KB via the PSRAM spill made them run at heap=51 KB).
#define ORCH_TURN_HARD_FLOOR    28000U  // user turn: reply "busy" below this (INTERNAL heap)
#define ORCH_DISPATCH_MIN_HEAP  28000U  // defer a single spawn dispatch below this
// In-turn associative recall does a SECOND TLS handshake (the embed) BEFORE the
// LLM call. With mbedTLS now backed by PSRAM (see main.cpp) the two handshakes no
// longer contend for internal heap - the embed's only internal cost is its small
// HTTP/JSON transient, which fits the ~64 KB the turn runs at. So recall runs at
// the normal turn floor. (Kept as a named floor so a future heavier build can
// raise it; the recall engine also stays reachable over /mcp regardless.)
#define ORCH_RECALL_MIN_HEAP    ORCH_TURN_HARD_FLOOR  // embed for recall above the turn floor

// ---- Head multi-turn tool-use loop (ReAct) bounds ---------------------------
// The head can call registry tools (memory.*/session.*/web.search) mid-turn, see
// each result, and iterate across model turns, stopping when it emits the terminal
// orch_turn. These cap the loop so it can't stall the tg_poll task (each round is a
// full TLS round-trip through the single tls_arbiter slot). See
// nimbus::orch::runHeadLoop; the accumulating conversation is routed to PSRAM.
// Gated at runtime by store::orchToolLoop() (default OFF - single-shot until on).
// P6 defaults: the loop is now THE turn path (default ON), so the caps are sized
// for real agent work - minutes, not 90 s - and are user-tunable (store::orchLoop*
// with these as fallbacks; the web UI clamps to the ranges noted on each key).
// The conversation accumulates in PSRAM so rounds are cheap; the deadline is the
// real ceiling on a runaway/hung provider.
#define ORCH_LOOP_MAX_ROUNDS      8       // tool-dispatch rounds before the forced final answer
#define ORCH_LOOP_DEADLINE_MS     600000U // wall-clock budget across the whole loop (10 min)
// Bumped 2026-08-03 (owner): the accumulating context is PSRAM-backed, so a bigger
// tool-output budget costs PSRAM not internal SRAM. 8 KB/result, 64 KB total (~16 K tokens)
// lets the loop gather more evidence per turn; the internal-SRAM re-gate + the wall-clock
// deadline remain the real ceilings. (Was 4096 / 24576.)
#define ORCH_LOOP_TOOL_RESULT_CAP 8192U   // clamp each tool result fed back to the model
#define ORCH_LOOP_TOOL_TOTAL_CAP  65536U  // cumulative tool-output budget -> force the final round
// Per-round free-INTERNAL-heap re-gate (rounds AFTER the first; round 0 is admitted
// by the caller's turn floor). Measured live 2026-07-11: with the request body
// serialized to a PSRAM buffer (one TLS write) a full tool round at 30.6 KB ended at
// 30.0 KB - a round's internal cost is a few hundred bytes of HTTP/JSON transients,
// not a handshake (mbedTLS is PSRAM-backed). The old 34 KB floor (== the turn hard
// floor) made every round-1 re-gate fire on this SD-mounted firmware (~30 KB at
// turn time), capping the loop at one tool round. 28 KB keeps real margin above the
// ~24 KB TLS/lwIP danger zone while letting multi-round loops actually run.
#define ORCH_LOOP_MIN_HEAP        28000U

// Max simultaneously-dispatched heavy jobs (one dispatched per poll cycle while
// active count < this). Mirrors nimbus::orch::kMaxActiveInflight; declared here so
// the device turn loop can gate without pulling the portable header into a macro.
// v2.0.0: raised 3 -> 4 with the 2-slot TLS arbiter (PSRAM-backed sessions); the
// per-cycle dispatch pacing (one spawn/cycle + heap floor) still bounds burst cost.
#define AGENT_MAX_ACTIVE_INFLIGHT 4

// Telegram long-poll (device-side; portable offset math lives in lib/core).
#define TELEGRAM_LONG_POLL_TIMEOUT_S  30
#define TELEGRAM_POLL_INTERVAL_MS     1000
// Backoff on consecutive poll failures (flood-control the failed-handshake loop).
#define TG_BACKOFF_STEP_MS   1500
#define TG_BACKOFF_MAX_STEPS 8

// ---- NVS key names (<=15 chars, solide::memory limit) -----------------------
// Provider credentials + routing config the user must supply (web UI / NVS). Every
// live path is dead without these - see the credential-gate notes in the adapters.
#define AKEY_OPENAI_KEY     "oaiKey"
#define AKEY_ANTHROPIC_KEY  "antKey"
#define AKEY_MISTRAL_KEY    "mistralKey"
#define AKEY_TAVILY_KEY     "tavilyKey"     // Tavily web-search API key (orchestrator web.search tool)
#define AKEY_WEB_TOKEN      "webTok"        // per-device web/MCP auth token (gen on first use, shown via Config QR)
#define AKEY_AP_PASS        "apPass"        // per-device setup-AP passphrase (gen on first use, shown on the setup screen)
#define AKEY_DEVICE_TZ      "devTz"         // POSIX TZ for Local Loops wall-clock schedules (default UTC0)
#define AKEY_CLOUD_OPTIN    "cloudOptIn"    // u8: 1 = cloud relay enabled (Orchestrator-only; default 0, ships dark)
#define AKEY_CLOUD_DEVID    "cloudDevId"    // cloud device id assigned at pairing
#define AKEY_CLOUD_CRED     "cloudCred"     // cloud device credential (bearer, cloud-minted; wiped on unpair)
#define AKEY_CLOUD_HOST     "cloudHost"     // relay host (default app.cumulo-nimbus.ai)
#define AKEY_CLOUD_NAME     "cloudName"     // paired device display name (for the web status line)
#define AKEY_DREAM_SCRATCH  "dreamScrHash"   // fnv64 of the scratchpad after the last dream (hex)
#define AKEY_CUSTOM_BASE    "custBase"
#define AKEY_CUSTOM_KEY     "custKey"
#define AKEY_ZAI_KEY        "zaiKey"      // Z.ai (GLM) token (Z_AI_TOKEN)
#define AKEY_ZAI_BASE       "zaiBase"     // probed working host (api.z.ai | open.bigmodel.cn)
#define AKEY_CUMULO_KEY     "cumuloKey"   // Cumulo router key (one key, all upstreams)
#define AKEY_CUMULO_BASE    "cumuloBase"  // router base URL/host ("" -> CUMULO_HOST_DEFAULT)
#define AKEY_FALLBACK_RULES "fbRules"     // fallback rule set JSON (v1 schema); "" -> shipped defaults
#define AKEY_FALLBACK_SYNC  "fbSyncTs"    // epoch s of last cloud sync (0 = never/local edit)
#define AKEY_CUSTOM_CONV    "custConv"      // "openai"|"mistral"|"anthropic"
#define AKEY_CUSTOM_MODEL   "custModel"
#define AKEY_ORCH_HOST      "orchHost"      // explicit host provider ("" => priority top)
#define AKEY_ORCH_CONVID    "orchConv"      // "host|convId" (per-host conversation state)
#define AKEY_PROV_PRIORITY  "provPrio"      // orchestrator-host priority (also orchPriority)
#define AKEY_SUB_PRIORITY   "subPrio"       // sub-session provider priority
#define AKEY_SYS_PROMPT     "sysPrompt"     // the user directive (immutable by model)
#define AKEY_TG_TOKEN       "tgToken"
#define AKEY_TG_ALLOWLIST   "tgAllow"       // comma-separated chat ids
#define AKEY_TG_OWNERS      "tgOwners"      // comma-separated OWNER chat ids (subset of allow); empty => the first allow entry is owner
#define AKEY_TG_OFFSET      "tgOffset"
#define AKEY_TG_NAMES       "tgNames"        // display sidecar "id:name,..." (P8)
#define AKEY_TG_BOTNAME     "tgBotName"      // connected bot's @username (getMe, display-only)
#define AKEY_TG_PUBLIC      "tgPublic"       // DANGER: accept anyone (default off, P8)
#define AKEY_TTS_ENABLED    "ttsEnabled"    // "Voice replies" master toggle (default OFF, P2.5)
#define AKEY_ORCH_TOOLLOOP  "orchLoop"      // head multi-turn tool-use loop on/off (default ON, P6)
#define AKEY_CODE_SANDBOX   "codeSbx"       // Assistant > Tools "Code sandbox" toggle (default OFF)
#define AKEY_MID_FAILOVER   "midFail"       // mid-turn provider failover on loop turns (default ON, Stage 2 ph5)
#define AKEY_ORCH_PROMPTV2  "orchPromptV2"  // N11: simplified v2 system prompt A/B (default OFF)
// Head tool-loop caps, user-tunable (P6). NVS keys <=15 chars. Deadline stored in
// SECONDS (fits uint16, avoids ms overflow). Empty/absent -> the macro defaults.
#define AKEY_LOOP_ROUNDS    "orchLoopRnds"  // max tool-dispatch rounds (default 8, 1..32)
#define AKEY_LOOP_DEADLINE  "orchLoopDlS"   // wall-clock budget seconds (default 600, 30..3600)
#define AKEY_LOOP_RESCAP    "orchLoopRCap"  // per-tool-result byte clamp (default 4096)
#define AKEY_LOOP_TOTCAP    "orchLoopTCap"  // cumulative tool-output byte budget (default 24576)
// Local Loops governor (routines/scheduler) owner overrides - may only TIGHTEN
// the caps.h defaults, never loosen; 0/absent = keep the default. Read at loops
// begin(), folded through nimbus::orch::clampLoopCaps (CUM-73). NVS keys <=15 ch.
#define AKEY_LOOP_MAXCNT    "loopMaxCnt"    // max concurrent routines (default 8)
#define AKEY_LOOP_MINIVL    "loopMinIvl"    // min seconds between fires (default 300)
#define AKEY_LOOP_FIRES     "loopFires"     // per-routine daily fire ceiling (default 24)
#define AKEY_LOOP_TOKENS    "loopTokens"    // per-routine daily token ceiling (default 120000)
#define AKEY_LOOP_DEVTOK    "loopDevTok"    // device-wide daily token ceiling (default 400000)
#define AKEY_LOOP_DEVFIR    "loopDevFir"    // device-wide fires per rate window (default 6)
#define AKEY_ALLOW_HW_TESTS "allowHwTest"   // orchestrator may run hardware self-tests (default ON)
// OTA firmware update (src/sys/ota_update). The pend/boots/prev trio is the
// app-level rollback guard (arduino sdkconfig lacks bootloader rollback):
// written BEFORE the otadata flip, consumed by otaupd::bootGuard() at boot.
#define AKEY_OTA_PENDING    "otaPend"        // u8: 1 = fresh image awaiting validation
#define AKEY_OTA_BOOTS      "otaBoots"       // u8: boot attempts since the flip
#define AKEY_OTA_PREV       "otaPrev"        // previous app slot label ("app0"/"app1")
#define AKEY_OTA_LASTRES    "otaLast"        // last OTA outcome ("ok vX"/"rollback vX"/...)
#define AKEY_OTA_AUTO       "autoUpd"        // auto-install in an idle window (default OFF)
#define AKEY_OTA_NOTIF      "otaNotif"       // last version Telegram-notified (no re-nag)
#define AKEY_OTA_TYPE       "otaType"        // typed-OTA device slug: "nimbus-tft" |
                                             // "freenove-28|35|40" | "" (untyped/e-ink:
                                             // no update). Set by the flasher or the
                                             // transition boot; frozen machine key.
#define AKEY_OTA_NOTES      "otaNotes"       // "ver|notes" persisted across the install reboot
                                             // (RAM-only notes died on the very reboot they
                                             // describe - Glass Box A2)
#define AKEY_ORCH_TRACE     "otrace"         // glass-box trace capture (default ON - A4)
// Glass-box per-row caps (A4): bound the INTERNAL-heap transient per trace row.
#define ORCH_TRACE_ARGS_MAX   256
#define ORCH_TRACE_OUT_MAX    1024
#define ORCH_TRACE_THINK_MAX  1024
// Per-turn metadata row (Glass Box P2): one compact JSON row per completed turn
// (host/model/tools/tokens). Bounded so the turn summary can never rival the
// content rows it describes; the error text inside it is clipped to fit.
#define ORCH_TRACE_END_MAX     256
#define ORCH_TRACE_ENDERR_MAX   96
// Per-turn dossier file (/mem/trace/<turnId>.txt): the full turn anatomy. Capped
// per file; the directory itself is ring-bounded (kTraceFilesMax/kTraceBytesMax).
#define ORCH_TRACE_FILE_MAX  (48 * 1024)
// Full (unclipped) tool args + result parked as a blob sidecar when the row's
// text had to be clipped, so the owner can open what the model actually saw.
// ARGS_FULL is what's kept in the in-flight RAM map (>=128 B strings are PSRAM).
#define ORCH_TRACE_ARGS_FULL   4096
#define ORCH_TRACE_BLOB_MAX  (64 * 1024)
#define ORCH_TRACE_BLOBS_PER_TURN  8
#define AKEY_ONBOARDED      "onbrded"        // first-run onboarding completed (plain NVS bool, NOT the SD override blob - survives reboot with no SD)
#define AKEY_BATT_MODEL     "battModel"     // battery analytics learned state (CSV blob)
#define AKEY_TLS_SLOTS      "tlsSlots"       // concurrent work-TLS sessions 1..2 (default 1 - the measured-stable value; latched at boot)
#define AKEY_TLS_VERIFY     "tlsVerify"      // validate provider certs vs the CA bundle (default ON)
#define AKEY_CAP_PROBE      "capProbe"       // capability validation: 0 off / 1 passive (default) / 2 active
#define AKEY_FETCH_POL      "fetchPol"       // files.fetch trust: 0 off / 1 approve (default) / 2 scan / 3 yolo
// Moderation gates (non-admin roles only; each a paid classifier call per item).
// Default OFF. Cumulo moderation endpoint on a Cumulo key, else Mistral on the user key.
#define AKEY_MOD_INBOUND    "modInbound"     // screen inbound guest/member text pre-turn (fail-closed)
#define AKEY_MOD_OUTBOUND   "modOutbound"    // screen outbound replies to guests (fail-open)
#define AKEY_MOD_INJECTION  "modInject"      // injection-screen fetched world content (fail-open + mark)
#define AKEY_CAP_PROBE_H    "capProbeH"      // active re-verify interval in hours (1..168, default 24)
#define AKEY_STT_PROVIDER   "sttProv"        // "mistral"|"openai" (voice -> text)
#define AKEY_TTS_PROVIDER   "ttsProv"        // "mistral"|"openai" (text -> voice)
#define AKEY_TTS_VOICE      "ttsVoice"       // voice id/slug ("" = provider default)
#define AKEY_THEME          "theme"          // LED colour theme (default "teal")
// Display panel fitted to THIS device: "eink" (2.9" SSD1680 + EC11 knob, the
// default and the shipped config) | "tft" (2.8" ILI9341 + XPT2046 touch). It is
// hardware identity, not a preference: it re-pins GPIO 1/2/48 and selects both
// the display driver and the input driver, so it is applied at boot (restart to
// take effect) and is exempt from "Revert to Defaults".
#define AKEY_SCREEN_MODEL   "scrModel"       // "eink" (default) | "tft"
// Resistive-touch calibration, "minX,maxX,minY,maxY,flags" (nimbus::touch::Cal).
// MEASURED per unit - the raw ADC range varies between panels, so without this
// a wrong guess is indistinguishable from broken touch. "" = driver defaults.
#define AKEY_TOUCH_CAL      "tchCal"
// Sound cues (src/sfx): PER-MODE sound levels (0 none / 1 light / 2 medium /
// 3 heavy - Notifier ships silent, Orchestrator at medium) + one shared sound theme.
// legacy sound-theme slugs kept as stored NVS values; the shipped theme is 'pulse'.
#define AKEY_SFX_LVL_NOTIF  "sfxLvlN"
#define AKEY_SFX_LVL_ORCH   "sfxLvlO"
#define AKEY_SFX_THEME      "sfxTheme"       // "terran" | "protoss" | "zerg"
#define AKEY_SFX_VOL        "sfxVol"         // master speaker volume 0-100 (%)
#define AKEY_SAVER_MIN      "saverMin"       // e-ink screensaver idle minutes (0 = off)
#define AKEY_COMPACT_KB     "compactKB"      // fold trigger: chat KB since last fold (0 = off)
#define AKEY_SLEEP_MV       "sleepMv"        // low-batt deep-sleep threshold, pack mV (0 = off; default 6000 = ~10% real SoC)
#define AKEY_BATT_RTOP      "battRtop"       // divider R_top (ohms); default 220000
#define AKEY_BATT_RBOT      "battRbot"       // divider R_bottom (ohms); default 100000
#define AKEY_BATT_CAPMAH    "battCapMah"     // pack capacity mAh (LiitoKala 3500, reclaimed vape ~500, ...)
#define AKEY_BATT_CHEM      "battChem"       // battery chemistry slug: "liion" (default) | "lifepo4"; picks the SoC curve
#define AKEY_BATT_CELLS     "battCells"      // series cell count override (1/2); 0/absent = board-derived default
#define AKEY_BATT_CURVE     "battCurve"      // optional custom SoC curve "mv:pct,mv:pct,..." (high-mV first); "" = chemistry default
#define AKEY_WAKE_MV        "wakeMv"         // stay-awake bar after a low-batt sleep (rested-empty packs read 6918-6992; default 7200)
#define AKEY_LOWBATT_PING   "lbPingEp"       // epoch of the last low-battery owner ping (AlertGate persistence - survives the 5-min wake-sniff boots)
#define AKEY_SLEEP_OVR      "sleepOvr"       // owner/AI override: skip the low-batt sleep (deep-discharge risk accepted)
#define AKEY_BRIGHT_OVR     "brightOvr"      // owner/AI override: lift the 60% LED cap to 100% (panel/shell heat risk accepted)
// ⚠ DIAGNOSTIC toggles for the TFT white-screen fault, persisted ONLY so a
// bisect can survive the reboot that a mode switch forces. They default to
// "everything on" (normal behaviour); a device that has never been bisected
// behaves exactly as before. Remove these once the fault is understood.
#define AKEY_LOWBATT_RING   "lbRing"         // owner opt-in: show the low-battery ring light at all (DEFAULT OFF)
#define AKEY_TFT_FLIP       "tftFlip"        // colour panel mounted 180 deg round (landscape only)
#define AKEY_LOWBATT_SAVER  "lbSaver"        // T1 switches the battery mode to save power (DEFAULT ON = shipped behaviour)
#define AKEY_BATT_MON       "battMon"        // battery monitoring enabled (default board-derived: ON for battery-native hand-built boards, OFF/opt-in for all-in-one desk boards). OFF => the ADC is never read, the glyph is hidden, and low-batt sleep never fires.
// SFX asset sync source (public repo; see tools/sounds + docs). The device GETs
// <SFX_SYNC_BASE>/manifest.json and the dist/sd/* WAVs it lists onto the SD card.
// OTA release manifest - served from the PUBLIC firmware-releases repo (the
// device downloads unauthenticated, so the source repo Nimbus being private
// can't host it - same reason the SFX assets live in the public nimbus-sounds).
// "latest" is a stable GitHub alias; each asset URL 302-redirects cross-host to
// objects.githubusercontent.com (the OTA HTTP client follows Location). See
// docs/ota.md + .github/workflows/release.yml.
#define OTA_MANIFEST_URL "https://github.com/ristllin/nimbus-fw-releases/releases/latest/download/manifest.json"
#define SFX_SYNC_HOST "raw.githubusercontent.com"
#define SFX_SYNC_BASE "/ristllin/nimbus-sounds/main/dist"
#define AKEY_CONNECTORS     "connectors"     // per-provider connector registry (JSON blob)
#define STT_DEFAULT_PROVIDER "mistral"       // Voxtral by default
#define TTS_DEFAULT_PROVIDER "mistral"
#define AKEY_AGENT_FABRIC   "agFabric"      // legacy category bindings
// Per-provider orchestrator/sub model overrides (prefix + provider initial).
#define AKEY_ORCH_MODEL_PFX "orchM_"        // + "openai"/"anthropic"/"mistral"
#define AKEY_SUB_MODEL_PFX  "subM_"
// Anthropic managed-agents caches.
#define AKEY_ANT_ENVID      "antEnv"
#define AKEY_ANT_AGENTMAP   "antAgents"
#define AKEY_ANT_ORCHAGENT  "antOrchAg"
// Provider key-verification cache (provider_verify): one key per provider,
// value "R:TS" where R = 1 verified / 0 rejected / -1 couldn't-verify and TS =
// millis() when the result landed. "vfy_" + provider stays <=15 chars for every
// provider name we use ("vfy_anthropic" = 13).
#define AKEY_VERIFY_PFX     "vfy_"

// ---- Embedding config for the vector memory (Part B Ph3) --------------------
// SET-ONCE: vectors from different provider/model/dims are incomparable, so
// changing any of these invalidates the whole VDB (wipe + re-embed). AKEY_EMBED_
// LOCKED flips true the first time a vector is embedded; the web UI must warn +
// require an explicit reset before changing a locked config. Defaults are the
// design's choice: OpenAI text-embedding-3-small truncated to 256 dims.
#define AKEY_EMBED_PROVIDER "embProv"     // "openai" | "mistral"
#define AKEY_EMBED_MODEL    "embModel"
#define AKEY_EMBED_DIMS     "embDims"     // int (0 = provider default width)
#define AKEY_EMBED_LOCKED   "embLocked"   // bool: a vector has been embedded
#define AKEY_USAGE_LEDGER   "usgLedger"   // per-provider monthly usage+budget blob (UsageLedger::serialize)
#define EMBED_DEFAULT_PROVIDER "openai"
#define EMBED_DEFAULT_MODEL    "text-embedding-3-small"
#define EMBED_DEFAULT_DIMS     256
// Provider embeddings endpoints (both OpenAI-compatible /v1/embeddings shape).
#define OPENAI_EMBED_PATH   "/v1/embeddings"
#define MISTRAL_EMBED_PATH  "/v1/embeddings"
