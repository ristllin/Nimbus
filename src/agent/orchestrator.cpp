#include "orchestrator.h"
#include "agent_config.h"
#include "nimbus/harness/log.h"         // hlog - the portable harness log seam
#include "nimbus/harness/compose.h"     // portable World prompt composition (Stage D)
#include "nimbus/harness/apply.h"       // portable turn-application policy (Stage E)
#include "nimbus/harness/jobs.h"        // portable sub-agent job machinery (Stage F)
#include "nimbus/harness/engine.h"      // portable turn orchestration (Stage G)
#include "nimbus/harness/head_arc.h"    // portable head-arc reconciler (W6)
#include "store.h"
#include "store_config.h"               // harnessConfigFromStore - the HarnessConfig view
#include "../sys/agent_log.h"
#include "orch_persist.h"
#include "net/wifi_portal.h"   // net::staConnected - fold-tick network gate
#include "adapters/adapter_factory.h"   // backendHue
#include "adapters/openai_adapter.h"    // openai_responses::orchTurn - OpenAI host
#include "adapters/anthropic_adapter.h" // orchTurnAnthropic - Anthropic host
#include "adapters/mistral_adapter.h"   // orchTurnMistral - Mistral host (Conversations)
#include "transport_tls.h"              // deviceProviderDeps - head-custom host (Stage H)
#include "nimbus/harness/providers.h"   // providers::orchTurnCustom
#include "loops_subsystem.h"            // loops::checkDue - Local Loops tick (on tg_poll)
#include "dream_subsystem.h"            // DREAMING: noteTurnEnd (idle-gate quiet clock)
#include "connectors.h"                 // per-provider connector catalog + attach
#include "files_subsystem.h"            // files::available - E1 capability manifest
#include "skills.h"                     // skills::spawnCapsule - per-spawn injection (P2)
#include "memory_subsystem.h"           // memory::registry/scratchpad/vectors (live World)
#include "adapters/moderation.h"        // CUM-69 device classifier (behind the gate decision core)
#include "nimbus/orch/moderation.h"     // portable gate decision core (fail-open/closed, admin-exempt)
#include "nimbus/orch/media.h"          // CUM-40 validMusicName (for /play)
#include "../sfx/music.h"               // CUM-40 music player control (/play)
#include "adapters/audio_tts.h"         // spoken replies (TTS -> Telegram audio / device speaker)
#include "nimbus/tts_catalog.h"         // core::speakerTtsFormat - provider -> speaker format (host-tested)
#include "adapters/provider_file_fetch.h"  // captureProviderFile - v4.1 code_interpreter file capture
#include <solide/audio.h>               // reply.speak - play WAV on the device speaker (P6)
#include <LittleFS.h>
#include <memory>                       // unique_ptr - heap Info[] (kMaxConnectors)
#include <new>       // std::nothrow - alloc failure degrades, never panics
#include "telegram.h"                   // telegram::enabled - capability manifest
#include "../sys/config_nvs.h"          // sys::deviceName - prompt identity (P2)
#include "../hw/hal_status.h"           // live HAL health -> capability manifest (P5)
#include "../hw/selftest.h"             // localTimeStr - temporal grounding (prompt + briefs)
#include "version.h"                    // NIMBUS_FW_VERSION -> prompt identity + cmd replies
#include "nimbus/fault.h"               // fault mask -> live capability manifest
#include <solide/storage.h>             // SD mount -> hw.sd
#include <solide/boards/board_solide_s3.h>  // board pixel count -> hw.ledCount (W10)
#include "nimbus/orch/world.h"          // SessionInfo / Hardware (composeInputs closure)

#include "nimbus/orch/budget.h"         // deriveBudget - brief/fold-slice bytes from the head window
#include "nimbus/orch/result_store.h"   // recent-results ring (results.get/list)
#include "nimbus/orch/caps.h"
#include "nimbus/orch/compact.h"        // modelCtxTokens (window table)
#include "nimbus/orch/servedby.h"       // CUM-236 turn-chip served-by disclosure
#include "nimbus/orch/command.h"
#include "nimbus/orch/memory.h"
#include "nimbus/orch/journal.h"
#include "nimbus/orch/turn.h"
#include "nimbus/orch/device_actions.h"
#include "nimbus/mem_cap.h"
#include "nimbus/harness/dream.h"   // buildEpisodicDigest - the RECENT CONVERSATION window (B1)
#include "../sfx/sound_fx.h"      // SFX hooks: spawn ack / turn start / reply / mem save

#include <ArduinoJson.h>
#include <mutex>       // last-turn introspection buffer guard
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>   // vTaskDelay
#include <freertos/semphr.h> // session-snapshot mutex (cross-task journal read)

// Single-task orchestrator turn loop - now THIN GLUE over the portable harness
// (Stage G). The full turn pipeline (TurnGuard/recall/compose/host pick/budget
// failover/tool-loop wiring/retry ladder/parse+salvage/applyTurn), the
// auto-synthesis turn, handleMessage's turn core, injectScheduledTurn, and the
// stuck-turn reaper all live in lib/harness engine.cpp (host-tested by
// test_harness_turn). This file keeps only what is genuinely device-bound:
// sinks, NVS/PSRAM surfaces, the Telegram /loops owner-command intercept, the
// web-staged mailbox, the /api/lastturn PSRAM blob, deps builders, and thin
// public-API forwards (the agent::orchestrator:: surface is unchanged).

namespace agent {
namespace orchestrator {

namespace orch = nimbus::orch;
namespace attn = nimbus::attn;

// ---- Module state -----------------------------------------------------------

static HeavyFabric* g_fabric = nullptr;
static Sinks        g_sinks;
// The portable sub-agent job machinery (spawn queue / dispatch gate / round-robin
// poll / fresh results / reap scheduler) - constructed in begin(), Stage F lift.
static JobEngine*   g_jobs = nullptr;
// The portable turn orchestration (Stage G lift) - constructed in begin().
static TurnEngine*  g_engine = nullptr;
// The harness's read-mostly config view over NVS (store_config.cpp).
static HarnessConfig g_cfg;

static orch::OrchMemory      g_mem;
static orch::Journal         g_journal;
static LittleFsMemoryStore   g_memStore;
// v3.6.0 fold state (plans/compaction-plan.md). All accessed on tg_poll ONLY
// (capture hooks, onTurnEnd, pollJobs, handleMessage) - no lock needed.
static LittleFsFoldStoreIO      g_foldIO;
static nimbus::orch::FoldStore  g_folds;
static char g_lastTurnChat[32]  = {};   // the chat compactTick() watches
// Glass Box turn identity: "turn:<user row id>" - stamped on every trace row and
// on the turn's assistant reply rows, so the chat UI groups a bubble with its
// trace by ID instead of chronological adjacency (adjacency mis-bucketed rows
// whenever a Telegram/voice turn interleaved with a web one). File scope because
// the reply sinks (speak/telegram, below) capture from outside buildTurnDeps().
// Empty outside a turn (cleared in onTurnEnd) so an async sub-agent delivery can
// never inherit a stale turn's id. tg_poll-only - no lock needed.
static char g_turnTag[24] = {};
// True once this turn's dossier file landed (Glass Box P3) - the turn-summary
// row then carries its path, so the chat knows a dossier exists without probing.
static bool g_turnDossier = false;
// Append the live turn tag to a row's tags, if we're inside a turn.
static String withTurnTag(const char* base) {
  String t(base);
  if (g_turnTag[0]) { if (t.length()) t += ","; t += g_turnTag; }
  return t;
}
// v3.7.0 RBAC: the tenant table (roles + quotas). Unlike the rest of the
// orchestrator state this genuinely has TWO writers - the web surface
// (/api/tenant, AsyncTCP) and the assistant's tenant.* tools (tg_poll, or
// AsyncTCP again via POST /mcp) - and both need a SYNCHRONOUS answer, because
// "that is the only admin" must come back as a 409 rather than land silently a
// second later. So it gets a real mutex instead of the staged-flag pattern: a
// push_back reallocating the vector under a concurrent find() is a
// use-after-free, not a skewed counter.
//
// Every access goes through the tenant* helpers below, which return values, not
// references - nothing may hold a pointer into the table across a task switch.
static nimbus::orch::TenantStore g_tenants;
// STATIC allocation, not lazy creation. "if (!mux) mux = create()" is itself a
// race: two tasks reaching it together both create one, each takes a different
// mutex, and the lock silently stops excluding anything - the exact bug this
// lock exists to prevent, now invisible. A static mutex needs no heap and
// exists before the first task runs.
static StaticSemaphore_t g_tenantMuxBuf;
static SemaphoreHandle_t g_tenantMux =
    xSemaphoreCreateRecursiveMutexStatic(&g_tenantMuxBuf);
struct TenantLock {
  TenantLock() { xSemaphoreTakeRecursive(g_tenantMux, portMAX_DELAY); }
  ~TenantLock() { xSemaphoreGiveRecursive(g_tenantMux); }
  TenantLock(const TenantLock&) = delete;
  TenantLock& operator=(const TenantLock&) = delete;
};
static char g_manualFoldChat[32] = {};  // staged /compact target (bypasses thresholds)
static portMUX_TYPE g_foldSpin = portMUX_INITIALIZER_UNLOCKED;   // console (main) stages too
// Cross-task fold-state ECHO (prism v3.6.0): foldStatusText runs on AsyncTCP /
// the console task, but FoldStore is tg_poll-owned and its ChatFold carries a
// heap std::string - copying it while applyFold reassigns the summary (or
// upsert moves the slot vector) is a use-after-free, not a skewed counter. Only
// tg_poll writes this fixed buffer; readers copy it under the spinlock. Same
// pattern as g_memEcho.
static char g_foldEcho[224] = {};
static void publishFoldStatus(const std::string& chat);   // fwd (defined below)
static NvsJournalStore       g_journalStore;

// Cross-task model-memory echo for the web UI. g_mem's std::string may be
// reassigned mid-turn on THIS (poll) task; letting the AsyncTCP task copy it
// directly risks a dangling read across a realloc. So the turn path mirrors it
// into this fixed buffer (strlcpy, always NUL-terminated) and the web accessor
// reads the buffer: worst case a torn *text*, never a torn pointer.
static char g_memEcho[nimbus::orch::kMemModelMax + 1] = {};
static void syncMemEcho() {
  // Web echo: the NEWEST chat's running memory (B3 - the store is per-chat now).
  strlcpy(g_memEcho, g_mem.modelAny().c_str(), sizeof(g_memEcho));
}

// Web-staged maintenance flags, drained on the turn task (pollJobs/handleMessage)
// so g_mem is only ever mutated from one task. Directive edits + memory clears
// take effect at the next drain - i.e. before the next turn uses them.
static volatile bool g_memClearReq = false;
static volatile bool g_cfgReloadReq = false;
static volatile bool g_convResetReq = false;   // prism B: reset must land BETWEEN turns
static volatile bool g_convClearReq = false;   // /clear: drop conversation + active task, keep memory/files
static void drainStaged() {
  if (g_memClearReq) {
    g_memClearReq = false;
    g_mem.clear();
    syncMemEcho();
    alog("orchestrator: model memory cleared (web)");
  }
  if (g_convResetReq) {
    g_convResetReq = false;
    // Draining here (tg_poll, between turns) means an in-flight turn's convId
    // write-back has ALREADY landed - so the wipe truly clears every chat,
    // including the one the owner reset mid-turn (the AsyncTCP-direct write was
    // silently re-added by that turn's end-of-turn upsert).
    store::setOrchConvId("");
    alog("orchestrator: provider conversations reset (web)");
  }
  if (g_convClearReq) {
    g_convClearReq = false;
    // /clear: forget the current conversation (provider continuity) AND the
    // scratchpad's ACTIVE TASK line, but keep long-term memory, the memory tiers,
    // vectors, episodic history, and files. Same between-turns discipline as the
    // conv reset above so an in-flight turn's convId write-back is already landed.
    store::setOrchConvId("");
    {
      memory::Lock lk;
      memory::scratchpad().setActiveTask("");   // clears just the active line, tiers untouched
    }
    memory::persistScratchpad();
    alog("orchestrator: /clear - conversation + active task cleared (memory/files kept)");
  }
  if (g_cfgReloadReq) {
    g_cfgReloadReq = false;
    // Re-begin re-reads the directive from NVS and reloads the persisted model
    // memory from the store (which the clear above already wiped if both were
    // staged in the same pass).
    g_mem.begin(&g_memStore, std::string(store::sysPrompt().c_str()));
    syncMemEcho();
    alog("orchestrator: directive reloaded (web)");
  }
}

// ---- sub-agent journal: cross-task snapshot + staged terminate (prism F24) ---
// The sub-agent journal (g_journal) is SINGLE-WRITER on the tg_poll task - pump()
// update/markSeen/gc it every cycle. External MCP clients reach session.list and
// session.terminate on the AsyncTCP web task, which must NEVER touch the journal
// concurrently (a poll firing mid-request could tear a record / corrupt job state).
// So, mirroring loops:: web-staging and the g_memEcho model:
//   * READS  (session.list, sessionsJson) go through a snapshot rebuilt on tg_poll
//     after each pump(), copied out under a short mutex - never the journal;
//   * WRITES (session.terminate) are STAGED from the web task and drained on
//     tg_poll (terminate becomes async - the remote cancel is best-effort anyway).
// The turn's OWN session_ops[] terminate stays synchronous: it runs on tg_poll via
// d.cancelSession -> cancel(), already the journal's writer task, so it's untouched.
static SemaphoreHandle_t        g_sessMux = nullptr;   // guards g_sessSnapshot
static std::vector<nimbus::orch::SessionInfo> g_sessSnapshot;
static portMUX_TYPE             g_termSpin = portMUX_INITIALIZER_UNLOCKED;
static std::vector<std::string> g_termQueue;           // guarded by g_termSpin

// Rebuild the AsyncTCP-readable snapshot from the journal. Called ONLY on tg_poll
// (same task as pump), so the sessionInfos() journal read is race-free; the vector
// is built outside the lock and swapped in (O(1), no alloc while the mutex is held).
static void refreshSessionSnapshot() {
  std::vector<nimbus::orch::SessionInfo> fresh =
      g_jobs ? g_jobs->sessionInfos() : std::vector<nimbus::orch::SessionInfo>{};
  if (g_sessMux) xSemaphoreTake(g_sessMux, portMAX_DELAY);
  g_sessSnapshot.swap(fresh);
  if (g_sessMux) xSemaphoreGive(g_sessMux);
}

// Apply web-staged terminates on tg_poll (the journal's single writer). Swap the
// queue out under the spinlock (O(1)), then cancel() each outside it. A bad/stale
// id is a harmless no-op (the job already finished between staging and draining).
static void drainTerminates() {
  std::vector<std::string> work;
  portENTER_CRITICAL(&g_termSpin);
  work.swap(g_termQueue);
  portEXIT_CRITICAL(&g_termSpin);
  for (const std::string& id : work) {
    bool ok = g_jobs && g_jobs->cancel(id.c_str());
    alogf("orchestrator: staged terminate %s -> %s", id.c_str(), ok ? "cancelled" : "not found");
  }
}

// ---- attention-event helpers (the plan §3.6 integration seam) ---------------
// keyFromTag / ringStatusFor / emitJobState / emitJobCleared live in the portable
// JobEngine; the head-turn Running/Offline arc + the stuck-turn reaper live in
// the portable TurnEngine. emitAsk stays: it's turn-application, not job
// machinery, and rides the device ApplyDeps.

static void emitAsk() {
  if (!g_sinks.event) return;
  attn::Event e; e.type = attn::Event::Type::IncomingAsk;
  g_sinks.event(e);
}

// Clear a pending "needs you" ask. Nothing used to fire this on the normal path, so
// an ask latched the Dark/Calm attention LED (and masked every other attention
// source) until the router's ~6-min backstop aged it out. The owner engaging - a
// message or a voice reply - IS the answer, so clear it then. (A scheduled loop
// turn is NOT the owner answering, so it deliberately doesn't call this.)
void clearAsk() {
  if (!g_sinks.event) return;
  attn::Event e; e.type = attn::Event::Type::AskCleared;
  g_sinks.event(e);
}

// Error arcs are calls-to-action: hold them for the tunable attention window
// (Param::AttnHoldMs, fed by main.cpp's applyConfig). Latched here because
// setAttnHoldMs can fire from applyConfig BEFORE begin() constructs the engine.
static uint32_t g_attnHoldMs = 300000;  // 5 min default, mirrors the Param preset

// ---- moderation gates (CUM-69) ---------------------------------------------
// The portable decision core (lib/core/orch_moderation) owns the fail-open/closed
// policy + admin exemption; the device adapter runs the classifier. Every gate is
// owner opt-in (default off) so with the switches off NO classifier call is made
// and the turn path is byte-identical to before.

nimbus::orch::Role roleOfChat(const String& chatId);   // fwd (defined below)

static nimbus::orch::ModConfig modConfig() {
  nimbus::orch::ModConfig c;
  c.inbound   = store::modInbound();
  c.outbound  = store::modOutbound();
  c.injection = store::modInjection();
  return c;
}

// Run one gate: apply the role/switch gate; if it applies, classify + decide. The
// outbound gate can run while a turn holds the single TLS slot, so it uses a SHORT
// acquire timeout (skips = fail-open if busy) rather than stalling the reply; the
// inbound gate runs pre-turn with the slot free, so it waits the full window.
static nimbus::orch::ModAction moderateGate(nimbus::orch::ModGate gate, const String& chatId,
                                            const String& text) {
  using namespace nimbus::orch;
  ModConfig cfg = modConfig();
  Role role = roleOfChat(chatId);
  if (!gateApplies(gate, role, cfg)) return ModAction::Allow;
  const uint32_t acquireMs = (gate == ModGate::OutboundReply) ? 250u : 15000u;
  ClassifierVerdict v = agent::moderation::classify(std::string(text.c_str()), gate, acquireMs);
  return decide(gate, v);
}

// ---- delivery helpers -------------------------------------------------------

// `system` marks GENUINE device-authored deterministic copy (command replies,
// confirmations, the inbound-block notice) - an out-of-band provenance signal set by
// the emitting call site, never inferred from the text. It is the ONLY thing that
// exempts a reply from the outbound screen. Everything guest-steerable - a model
// reply, a job result - is delivered with system=false (the default) and always
// screened. (CUM-275: the old exemption keyed on text.startsWith(deviceName), which
// a guest could prompt-inject the model into reproducing.)
static void deliver(const String& chatId, const String& text, bool system = false) {
  // Gate 2 (outbound to guests, fail-open): when the owner turns it on, a reply to
  // a non-admin chat is screened; a flagged reply is suppressed (a classifier
  // outage fails open, so an outage never silences the assistant). Admin/web/serial
  // /voice resolve to Admin and are never screened (gateApplies returns Allow fast,
  // no classifier call). Runs on the delivering task, same TLS-slot discipline as
  // the fetch scan. Genuine device system copy is exempt so we never pay a classifier
  // call to screen our own known-safe strings - but only on provenance, not content.
  if (!nimbus::orch::outboundExempt(system) &&
      moderateGate(nimbus::orch::ModGate::OutboundReply, chatId, text) == nimbus::orch::ModAction::Block) {
    alogf("moderation: outbound reply to %s suppressed (flagged)", chatId.c_str());
    return;
  }
  // The send sink enqueues onto a bounded reply queue and returns false if it was
  // still full after its short block. The queue is now sized for a full turn's burst
  // (see REPLY_QUEUE_DEPTH), but if it ever overflows anyway, log it rather than drop
  // the owner's message silently - a dropped reply is otherwise invisible.
  if (g_sinks.send && !g_sinks.send(chatId, text))
    alogf("orchestrator: reply queue full - dropped %d-byte message to %s",
          (int)text.length(), chatId.c_str());
}

// Speak a short confirmation of a job result aloud (opt-in via store::ttsEnabled).
// LIVE-GATED + BENCH-BROKEN: the audio path is unverified here (no key, broken
// bench audio). Compile-clean; the sink is nullable and no-ops when unset.
static void ttsAnnounce(const String& text) {
  if (!g_sinks.speak || !store::ttsEnabled() || text.length() == 0) return;
  int end = -1;
  for (int i = 0; i < (int)text.length() && i < 180; i++) {
    char c = text[i];
    if (c == '.' || c == '!' || c == '?' || c == '\n') { end = i + 1; break; }
  }
  String snippet = (end > 0) ? text.substring(0, end)
                             : text.substring(0, min((int)text.length(), 180));
  g_sinks.speak(snippet);
}

// hostForPrompt moved to the harness (agent::hostForPrompt over ProviderConfig,
// compose.h) - the composeInputs closure below delegates to it.

static bool providerHasKey(const String& p) {
  if (p == "openai")    return store::hasOpenaiKey();
  if (p == "anthropic") return store::hasAnthropicKey();
  if (p == "custom")    return store::hasCustom();
  if (p == "mistral")   return store::hasMistralKey();
  if (p == "zai")       return store::hasZaiKey();
  if (p == "cumulo")    return store::hasCumuloKey();
  return false;
}

static String modelChoicesFor(const String& p) {
  // Live-harvested list first (the verify pass reads the provider's /v1/models -
  // keeps current-generation ids selectable without a firmware release); the
  // compile-time list stays the fallback for never-verified providers.
  String dyn = store::modelChoices(p);
  if (dyn.length()) return dyn;
  if (p == "openai")    return OPENAI_MODEL_CHOICES;
  if (p == "anthropic") return ANT_MODEL_CHOICES;
  if (p == "mistral")   return MISTRAL_MODEL_CHOICES;
  if (p == "cumulo")    return CUMULO_MODEL_CHOICES;
  if (p == "zai")       return ZAI_MODEL_CHOICES;
  return "";
}

static bool modelIsValid(const String& provider, const char* model) {
  if (!model || !*model) return false;
  String list = modelChoicesFor(provider);
  if (list.length() == 0) return false;
  int start = 0;
  while (start < (int)list.length()) {
    int comma = list.indexOf(',', start);
    if (comma < 0) comma = list.length();
    String item = list.substring(start, comma); item.trim();
    if (item == model) return true;
    start = comma + 1;
  }
  return false;
}

// ---- last-turn introspection (owner ask 2026-07-16: "create a file where I can
// see the raw convo that led to this response"). Captures the COMPLETE anatomy of
// the most recent turn - the exact system prompt, the exact per-turn input block,
// and the model's raw orch_turn JSON - into ONE PSRAM text blob, served token-gated
// at GET /api/lastturn. Written on the turn task (via the engine's onTurnDebug
// hook), read on the AsyncTCP task.
static std::mutex g_dbgMx;
static char*      g_dbgBuf = nullptr;   // PSRAM
static size_t     g_dbgLen = 0;

// Takes the borrowed-pointer TurnDebugEv straight from the onTurnDebug hook (one param,
// instead of unpacking its seven fields at the call site). The pointers are valid only
// during the hook call, which is where this runs.
static void captureTurnDebug(const TurnDebugEv& ev) {
  static const std::string kNoBrief;
  const std::string& instructions = *ev.instructions;
  const std::string& inputs = *ev.inputs;
  const std::string& outJson = *ev.rawOut;
  const std::string& brief = ev.transcriptBrief ? *ev.transcriptBrief : kNoBrief;
  const size_t need =
      instructions.size() + inputs.size() + outJson.size() + brief.size() + 1024;
  char* b = (char*)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
  if (!b) return;   // no PSRAM -> skip capture, never the turn
  size_t off = (size_t)snprintf(b, need,
      "===== NIMBUS TURN ANATOMY - the last completed turn =====\n"
      "host: %s\n"
      "provider conversation: %s\n"
      "result: %s\n"
      "sections: 1 system prompt · 2 per-turn input · 3 raw model output ·\n"
      "4 tool-loop transcript (the mid-turn rounds, when the turn used tools).\n"
      "\n===== 1. INSTRUCTIONS - the system prompt sent this turn =====\n"
      "(role + HOW YOU RUN + capability manifest + owner directive + [RUNNING\n"
      "MEMORY] + [RELEVANT MEMORIES] associative recall)\n\n",
      ev.host.c_str(),
      ev.convContinued ? "CONTINUED - the provider held history from prior turns"
                       : "FRESH - no prior history visible to the model",
      ev.ok ? "ok" : "FAILED");
  auto app = [&](const char* s, size_t n) {
    if (off + n >= need - 1) n = need - 1 - off;
    memcpy(b + off, s, n);
    off += n;
  };
  auto appStr = [&](const std::string& s) { app(s.c_str(), s.size()); };
  auto appLit = [&](const char* s) { app(s, strlen(s)); };
  appStr(instructions);
  appLit("\n\n===== 2. INPUT - the per-turn message block =====\n"
         "(optional [MEMORY RESULTS] / [FRESH RESULTS] + live session digest +\n"
         "[CHANNEL] + [USER] - the ONLY user-message content the model sees\n"
         "unless the provider conversation above is CONTINUED)\n\n");
  appStr(inputs);
  appLit("\n\n===== 3. MODEL RAW OUTPUT - the orch_turn JSON =====\n\n");
  if (outJson.size()) appStr(outJson);
  else appLit("(none - the turn failed before an output)");
  appLit("\n\n===== 4. TOOL-LOOP TRANSCRIPT - the mid-turn rounds =====\n"
         "([user] seed, [assistant] per-round prose, [tool] calls with their\n"
         "arguments, [result] outputs - each line folded to 200 chars)\n\n");
  if (brief.size()) appStr(brief);
  else appLit("(single-shot turn - the model answered without calling a tool)");
  appLit("\n");
  b[off] = 0;
  std::lock_guard<std::mutex> lk(g_dbgMx);
  if (g_dbgBuf) free(g_dbgBuf);
  g_dbgBuf = b;
  g_dbgLen = off;
}

char* lastTurnDebugPs(size_t& outLen) {
  outLen = 0;
  std::lock_guard<std::mutex> lk(g_dbgMx);
  if (!g_dbgBuf || !g_dbgLen) return nullptr;
  char* copy = (char*)heap_caps_malloc(g_dbgLen, MALLOC_CAP_SPIRAM);
  if (!copy) return nullptr;
  memcpy(copy, g_dbgBuf, g_dbgLen);
  outLen = g_dbgLen;
  return copy;
}

// ---- the JobEngine wiring (spawn/dispatch/poll logic lives in lib/harness) --

// Execution-only closures for the portable job machinery - every decision
// (dispatch gates, backoff, refusal strings, synthesis clock) lives in
// lib/harness jobs.cpp and is host-tested (test_harness_jobs).
static JobEngine::Deps buildJobDeps() {
  JobEngine::Deps d;
  d.platform.nowMs    = [] { return (uint32_t)millis(); };
  d.platform.freeHeap = [] { return (uint32_t)ESP.getFreeHeap(); };
  d.fabric  = g_fabric;      // set by begin() before the engine is constructed
  d.journal = &g_journal;
  d.deliver = [](const std::string& chatId, const std::string& text) {
    deliver(String(chatId.c_str()), String(text.c_str()));
  };
  d.event = [](const attn::Event& e) { if (g_sinks.event) g_sinks.event(e); };
  d.fire = [](const char* cue) {
    if (strcmp(cue, "spawn") == 0) ::sfx::fire(nimbus::sfx::Ev::AgentSpawn);   // "Right away, sir."
  };
  d.backendHue = [](const char* b) { return backendHue(b); };
  d.subPriority = [] { return std::string(store::subPriority().c_str()); };
  d.providerHasKey = [](const std::string& p) { return providerHasKey(String(p.c_str())); };
  d.subModel = [](const std::string& p) {
    return std::string(store::subModel(String(p.c_str())).c_str());
  };
  d.modelIsValid = [](const std::string& p, const std::string& m) {
    return modelIsValid(String(p.c_str()), m.c_str());
  };
  // Dynamic-skill capsule resolver (roadmap P2): SD capsules with
  // inject: spawn|both are prepended to the sub-agent instruction at dispatch;
  // built-ins / unknown ids return "" and keep the provider-hint passthrough.
  d.resolveSkill = [](const std::string& id) { return skills::spawnCapsule(id); };
  // v4.0.0 attachments + auto-persist (the fan-out file plumbing):
  d.readDoc = [](const std::string& chatId, const std::string& project,
                 const std::string& name) {
    // Enforce the SAME read boundary as the files.read tool: a member cannot
    // attach a doc its principal can't read. Resolve the spawning chat to its
    // real role (owner vs member vs guest) so an admin still reads everything
    // and a member is confined to its own namespace + shared docs.
    const nimbus::orch::Role role = roleOfChat(String(chatId.c_str()));
    const nimbus::orch::Principal who =
        nimbus::orch::principalForRole(chatId, role);
    return files::readDocText(project, name, who);
  };
  d.persistResult = [](const std::string& project, const std::string& name,
                       const std::string& tag, const std::string& text,
                       const std::string& chatId) {
    return files::persistSubResult(project, name, tag, text,
                                   nimbus::orch::nsForChat(chatId, /*admin=*/false));
  };
  // v4.1 provider file capture: a sub-agent that produced a binary FILE (a
  // Mistral code_interpreter PDF/image) has its file streamed to SD + registered
  // here, owned by the spawning chat's namespace (same boundary as persistResult
  // + the memory tools). Runs on tg_poll, one TLS at a time.
  d.fetchArtifact = [](const std::string& backend, const std::string& fileId,
                       const std::string& fileName, const std::string& project,
                       const std::string& name, const std::string& tag,
                       const std::string& chatId) {
    return agent::captureProviderFile(backend, fileId, fileName, project, name, tag,
                                      nimbus::orch::nsForChat(chatId, /*admin=*/false));
  };
  d.nowString = [] { return std::string(nimbus::hw::localTimeStr().c_str()); };
  // B4: the spawning chat's recent window rides the sub-agent brief - smaller
  // caps than the head's own window (the brief also carries task + skill capsule
  // and PendingSpawn.task is 1024 B... the CONTEXT block rides `instruction`
  // AFTER the copy, so it is bounded here, not by the task buffer).
  d.chatContext = [](const std::string& chatId) -> std::string {
    using namespace nimbus::orch;
    if (chatId.empty() || chatId == "system") return std::string();
    MsgQuery q;
    q.sessionId = chatId;
    // Message + the media kinds: a photo's row IS its description, and a
    // sub-agent asked about "the picture they sent" needs to see it. Trace rows
    // (ToolOutput / LlmResponse) stay out - the brief is the conversation, not
    // the machinery.
    q.haveKind = true; q.kind = MsgKind::Message;
    q.alsoKinds = {MsgKind::Image, MsgKind::File, MsgKind::Audio};
    q.limit = 6;
    std::vector<EpisodicMessage> rows;
    { memory::Lock lk; rows = memory::episodic().query(q); }
    if (rows.empty()) return std::string();
    std::string block = "[CONTEXT] (the conversation this task came from, oldest first)\n";
    // Brief bytes derive from the HEAD's window (the spawning context; 1200 at
    // the 200K anchor). Per-sub-model derivation deferred - this closure has no
    // provider argument today.
    const String h = store::resolvedOrchHost();
    const auto bb = deriveBudget(modelCtxTokens(h.c_str(), store::orchModel(h).c_str()), {});
    block += dream::buildEpisodicDigest(rows, bb.briefBytes);
    return block;
  };
  // Connector-aware routing: a spawn whose skill names an enabled connector routes
  // to the provider that hosts it (e.g. a Notion task -> mistral), so heavy
  // connector work runs on lab compute. Empty/ambiguous ("any") -> no override.
  d.connectorProvider = [](const std::string& skill) -> std::string {
    // Heap, not stack (tg_poll): see kMaxConnectors in connectors.h - the old
    // Info[8] made a spawn whose skill named the 9th+ connector route nowhere.
    std::unique_ptr<connectors::Info[]> cs(
        new (std::nothrow) connectors::Info[connectors::kMaxConnectors]);
    if (!cs) return std::string();   // no routing override beats a panic
    const int n = connectors::list(cs.get(), connectors::kMaxConnectors);
    for (int i = 0; i < n; i++) {
      if (!cs[i].enabled) continue;
      const bool match = skill == cs[i].name.c_str() ||
                         (cs[i].type.length() && skill == cs[i].type.c_str());
      if (match && cs[i].prov != "any") return std::string(cs[i].prov.c_str());
    }
    return std::string();
  };
  // Sub-session spend attribution: real usage from a terminal poll (OpenAI
  // Responses reports it) files under "spawn:<backend>" in the usage ledger.
  d.recordSpawnUsage = [](const std::string& backend, uint32_t in, uint32_t out) {
    store::recordProviderTokens(String(backend.c_str()), in, out,
                                ("spawn:" + backend).c_str());
  };
  // Recent-results spill: every finished sub-agent's FULL text into the ring,
  // so [FRESH RESULTS] stubs/clips can point at results.get("sub:<tag>").
  d.spillResult = [](const char* tag, const char* model, const std::string& fullText,
                     const std::string& ns) {
    return resultsPut("sub", model ? model : "", fullText, tag ? tag : "", ns);
  };
  d.synthesize = [](const std::string& chatId) {
    if (g_engine) g_engine->maybeConsolidate(chatId);
  };
  d.turnInFlight = [] { return g_engine && g_engine->turnInFlight(); };
  return d;
}

// ---- the TurnEngine wiring --------------------------------------------------

static agent::ApplyDeps buildApplyDeps() {
  agent::ApplyDeps d;
  // Execution-only closures - every POLICY decision (scheduled-turn refusals,
  // risk notes, tts gate, orch_model validation order) lives in the portable
  // harness apply.cpp and is host-tested (test_harness_apply).
  d.execConfig = [](const orch::ValidatedAction& va) {
    if (va.hasLedBrightness) store::setLedBright(va.ledBrightness);
    // `priority` tunes the SUB-SESSION provider preference only (never the
    // orchestrator-host list - see the protected-key rail).
    if (va.hasPriority)  store::setSubPriority(String(va.priority.c_str()));
    if (va.hasTtsVoice)  store::setTtsVoice(String(va.ttsVoice.c_str()));
    if (va.hasTtsProv)   store::setTtsProvider(String(va.ttsProv.c_str()));
    if (va.hasTtsOn)     store::setTtsEnabled(va.ttsOn);
    if (va.hasSleepOvr)  store::setSleepOvr(va.sleepOvr);
    if (va.hasBrightOvr) store::setBrightOvr(va.brightOvr);
    if (va.hasSttProv)   store::setSttProvider(String(va.sttProv.c_str()));
    if (va.hasSfxLvlN || va.hasSfxLvlO || va.hasSfxVol || va.hasSfxTheme) {
      if (va.hasSfxLvlN)  store::setSfxLevelNotif(va.sfxLvlN);
      if (va.hasSfxLvlO)  store::setSfxLevelOrch(va.sfxLvlO);
      if (va.hasSfxVol)   store::setSfxVolume(va.sfxVol);
      if (va.hasSfxTheme) store::setSfxTheme(String(va.sfxTheme.c_str()));
      ::sfx::refreshConfig();   // live-apply, same seam the web setters use
    }
    if (va.hasDevName) nimbus::sys::saveDeviceName(String(va.devName.c_str()));
  };
  if (g_sinks.device)
    d.stageDevice = [](const orch::ValidatedAction& va) { g_sinks.device(va); };
  if (g_sinks.speak)
    d.speak = [](const std::string& s) { g_sinks.speak(String(s.c_str())); };
  d.modelIsValid = [](const std::string& p, const std::string& m) {
    return modelIsValid(String(p.c_str()), m.c_str());
  };
  d.providerHasKey = [](const std::string& p) { return providerHasKey(String(p.c_str())); };
  d.setOrchHostModel = [](const std::string& p, const std::string& m) {
    store::setOrchHost(String(p.c_str()));
    store::setOrchModel(String(p.c_str()), String(m.c_str()));
  };
  d.setModelMemory = [](const std::string& chatId, const std::string& v) {
    return g_mem.setModel(chatId, v);   // per-chat running memory (B3)
  };
  d.syncMemEcho = [] { syncMemEcho(); };
  d.applyScratch = [](const nimbus::orch::ScratchUpdate& u) {
    // Inline scratchpad update: replace only the tiers the model
    // returned, under the memory Lock, then persist. A non-null tier REPLACES it
    // (the model returns the full desired tier each time - same model as running
    // memory), so clearing = an empty array, unchanged = a null field.
    using nimbus::orch::Tier;
    agent::memory::Lock memg;
    auto& sp = memory::scratchpad();
    if (u.hasActive) sp.setActiveTask(u.active);
    if (u.hasShort)  sp.replace(Tier::Short, u.shortItems);
    if (u.hasMid)    sp.replace(Tier::Mid,   u.midItems);
    if (u.hasLong)   sp.replace(Tier::Long,  u.longItems);
    memory::persistScratchpad();
  };
  d.withMemoryLock = [](const std::function<void()>& fn) {
    // Recursive, so the nested persist* re-enter safely. (memory.write embeds via
    // TLS while held - a rare brief stall for a concurrent dashboard request,
    // never a deadlock.)
    agent::memory::Lock memg;
    fn();
  };
  d.memDispatch = [](const char* name, ArduinoJson::JsonObjectConst args,
                     const nimbus::orch::Principal& who) {
    return memory::registry().dispatch(name, args, who);
  };
  // v3.7.0: the WHOLE principal for a chat - namespace, role and quota - read
  // from the device's tenant table. This is the only place the turn path learns
  // who is speaking, so changing someone's role or their storage limit takes
  // effect on their very next message.
  //
  // It used to be a bool ("is this the owner?"), which meant the harness had to
  // synthesize a role and could only ever produce Admin or User with no quota.
  // Every rail then enforced defaults nobody had chosen: a Guest wrote like a
  // User, and revoking someone changed nothing. The bool could not carry the
  // answer, so it had to go.
  d.principalFor = [](const std::string& chatId) {
    const String cid(chatId.c_str());
    nimbus::orch::Quota q;
    tenantQuotaOf(chatId, q);
    return nimbus::orch::principalForRole(chatId, roleOfChat(cid), q);
  };
  d.persistMemory = [] { memory::persistVectors(); memory::persistScratchpad(); };
  d.enqueueSpawn = [](const orch::Spawn& s, const std::string& chatId, bool quiet) {
    if (g_jobs) g_jobs->enqueueSpawn(s, chatId, quiet);
  };
  d.cancelSession = [](const std::string& id) { return cancel(id.c_str()); };
  d.awaitTag = [](const std::string& firstTag) {
    if (g_jobs) g_jobs->awaitTag(firstTag);
  };
  d.noteSpawned = [] { if (g_jobs) g_jobs->noteSpawned(); };
  d.deliver = [](const std::string& chatId, const std::string& text) {
    deliver(String(chatId.c_str()), String(text.c_str()));
  };
  d.turnComplete = [](const std::string& chatId) {
    // Only the SYNCHRONOUS channels (web chat / on-device voice / serial console)
    // need a completion to resolve their poll; Telegram (async) stays silent so a
    // scheduled loop never pings a fabricated "Done." A device/spawn turn with no
    // reply still confirms it finished on the channel the owner is watching live.
    if (chatId == "web" || chatId == "voice" || chatId == "serial")
      deliver(String(chatId.c_str()), String("Done."));
  };
  d.emitAsk = [] { emitAsk(); };
  d.captureAssistant = [](const std::string& chatId, const std::string& text) {
    memory::captureMessage(chatId.c_str(), "assistant", orch::MsgKind::Message,
                           String(text.c_str()), "", withTurnTag(""));
    if (chatId != "system") g_folds.noteMessage(chatId, text.size());   // fold trigger
  };
  d.fire = [](const char* cue) {
    if (strcmp(cue, "reply") == 0)    ::sfx::fire(nimbus::sfx::Ev::ReplySent);
    if (strcmp(cue, "memsaved") == 0) ::sfx::fire(nimbus::sfx::Ev::MemSaved);
  };
  return d;
}

// Execution-only closures for the portable turn orchestration - every decision
// (recall gate, host pick, budget/retry/failover ladder, salvage, scheduled-turn
// rails) lives in lib/harness engine.cpp and is host-tested (test_harness_turn).
static TurnEngine::Deps buildTurnDeps() {
  TurnEngine::Deps d;
  d.cfg = g_cfg;
  d.platform.nowMs    = [] { return (uint32_t)millis(); };
  d.platform.freeHeap = [] { return (uint32_t)ESP.getFreeHeap(); };
  d.platform.delayMs  = [](uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); };
  d.jobs  = g_jobs;
  d.apply = buildApplyDeps();
  d.deliver = [](const std::string& chatId, const std::string& text) {
    deliver(String(chatId.c_str()), String(text.c_str()));
  };
  d.event = [](const attn::Event& e) { if (g_sinks.event) g_sinks.event(e); };
  d.fire = [](const char* cue) {
    if (strcmp(cue, "turnstart") == 0) ::sfx::fire(nimbus::sfx::Ev::TurnStart);   // "Roger that."
  };
  d.recall = [](const std::string& userText, const nimbus::orch::Principal& who) {
    return memory::recall(String(userText.c_str()), 0, who);
  };
  // Gather the LIVE prompt inputs; the engine fills tools/loopOn/recalled and
  // every model-visible string lives in the portable harness (compose.cpp),
  // golden-pinned by test_harness_compose.
  // Release B1: the per-chat conversation window. The current user row is
  // captured BEFORE compose (durable-first, engine.cpp order) - its id is
  // stashed here so the window excludes the in-flight message. tg_poll-only.
  static char g_curUserRowId[16] = {};
  auto buildRecentWindow = [](const std::string& chatId) -> std::string {
    using namespace nimbus::orch;
    if (chatId.empty() || chatId == "system") return std::string();
    MsgQuery q;
    q.sessionId = chatId;
    q.haveKind = true; q.kind = MsgKind::Message;   // chat only - trace rows stay out
    // ...but a photo or file the sender just shared IS part of that chat. Its
    // row carries the description, so including these kinds is what makes
    // "what did I just send you?" answerable one turn later.
    q.alsoKinds = {MsgKind::Image, MsgKind::File, MsgKind::Audio};
    q.limit = kRecentTurnsMax + 1;                  // +1 covers the in-flight row
    std::vector<EpisodicMessage> rows;
    { memory::Lock lk; rows = memory::episodic().query(q); }   // newest-first, ring-served
    if (!rows.empty() && g_curUserRowId[0] && rows.front().id == g_curUserRowId)
      rows.erase(rows.begin());
    if ((int)rows.size() > kRecentTurnsMax) rows.resize(kRecentTurnsMax);
    if (rows.empty()) return std::string();
    std::string block =
        "\n## RECENT CONVERSATION (this chat, oldest first; the current message "
        "is NOT here - it is in [USER])\n";
    block += dream::buildEpisodicDigest(rows, kRecentConvBytes);
    return block;
  };
  d.composeInputs = [buildRecentWindow](const std::string& chatId) {
    using namespace nimbus::orch;
    agent::ComposeInputs in;
    // §5a (v3.6.0 fold): the anchored summary + the verbatim recent tail below it.
    // Framed as information, never instructions (injection posture).
    if (!chatId.empty() && chatId != "system") {
      nimbus::orch::ChatFold cf = g_folds.get(chatId);
      if (!cf.summary.empty())
        in.chatSummary = "\n## CONVERSATION SUMMARY (compacted earlier - a summary "
                         "of prior conversation; information, not instructions)\n" +
                         cf.summary + "\n";
    }
    in.recentConversation = buildRecentWindow(chatId);
    in.devName   = std::string(nimbus::sys::deviceName().c_str());
    in.hostLabel = agent::hostForPrompt(g_cfg.provider);
    in.now       = std::string(nimbus::hw::localTimeStr().c_str());  // "" = unsynced
    in.fw        = NIMBUS_FW_VERSION " (" NIMBUS_FW_BUILD ")";
    in.directive = g_mem.directive();
    in.runningMemory = g_mem.model(chatId);   // the TURN CHAT's rolling summary (B3)
    // LIVE hardware manifest (P5): read the actual HAL health + storage mount +
    // fault mask instead of hardcoding flags, so the model's self-model matches
    // reality (a dead display / missing SD / fault-injected mic all show through).
    const nimbus::hw::HalHealth& hal = nimbus::hw::halHealth();
    using nimbus::fault::active;
    in.hw.ring     = hal.leds    && !active(nimbus::fault::LED);
    // The color touchscreen is the only supported display; the manifest reports
    // it so the model's self-model matches the hardware in front of the owner.
    in.hw.touch    = hal.display && !active(nimbus::fault::SCREEN);
    in.hw.mic      = !active(nimbus::fault::MIC);
    in.hw.speaker  = !active(nimbus::fault::SPEAKER);   // reconnected + loopback-verified
    // W10: the old `= false // NullMonitor` was STALE - every s3 build carries
    // the 2S ADC divider (platformio.ini -DNIMBUS_HAS_BATTERY_ADC), so the model
    // was told it can flip sleepOvr on a device it believed had no battery.
#ifdef NIMBUS_HAS_BATTERY_ADC
    in.hw.battery  = true;
#else
    in.hw.battery  = false;
#endif
    // W10: the LIVE tier state, not the boot mount latch - a pulled card kept
    // advertising "SD card" (while hw.files, which already reads effHaveSd(),
    // honestly dropped the artifact-store bullet - an inconsistent manifest).
    in.hw.sd       = memory::haveSd() && !active(nimbus::fault::SD);
    in.hw.wifi     = true;                              // a turn only runs with WiFi up
    in.hw.ble      = false;                             // BLE is Notifier-mode only
    in.hw.telegram = agent::telegram::enabled();
    in.hw.voiceReplies = agent::store::ttsEnabled();   // owner's "Voice replies" toggle (P2.5)
    in.hw.files = agent::files::available();            // E1 artifact store (SD-backed)
    in.hw.ledCount = solide::kBoardSolideS3.led.count;  // board pixel count, not a prose literal
    // W10: who this turn's message is from (multi-tenant honesty - the prompt
    // used to call everyone "your owner"). Pseudo-channels (web/serial/voice)
    // resolve to Admin via roleOfChat; a synthesis/scheduled turn keeps the
    // speaker line only if the chat maps to a person.
    {
      nimbus::orch::Role r = roleOfChat(String(chatId.c_str()));
      in.speakerRole = nimbus::orch::roleName(r);
      for (const auto& t : tenantSnapshot())
        if (t.chatId == chatId) { in.speakerLabel = t.label; break; }
    }
    in.sessions   = sessionInfos();
    in.scratchpad = &memory::scratchpad();
    in.budgetBytes = memory::config().maxContextBytes;  // was a dead knob (compile-time default won)
    return in;
  };
  // W14: advertise only what THIS caller can actually call (admin-only tools
  // are omitted for a member/guest turn).
  d.toolSpecs = [](const nimbus::orch::Principal& who) {
    return memory::registry().toolSpecsFor(who);
  };
  d.mcpDispatch = [](const std::string& req, const nimbus::orch::Principal& who) {
    return memory::handleMcp(req, who);
  };
  d.connectorsCatalog = [] { return std::string(connectors::catalog().c_str()); };
  // W15: the ambient skills index - every turn NAMES the playbooks (id + one-line
  // desc) so the model pulls the procedure with skill.get instead of the recipes
  // riding every prompt.
  d.skillsIndex = [] { return skills::indexText(); };
  // [AVAILABLE MODELS] uses the COMPILE-TIME lists (the pre-lift behavior - the
  // live-harvested store::modelChoices list feeds validation, not this block).
  d.modelChoices = [](const std::string& p) {
    if (p == "openai")    return std::string(OPENAI_MODEL_CHOICES);
    if (p == "anthropic") return std::string(ANT_MODEL_CHOICES);
    if (p == "mistral")   return std::string(MISTRAL_MODEL_CHOICES);
    return std::string();
  };
  d.episodicCaptureUser = [](const std::string& chatId, const std::string& text,
                             const std::string& fromTag) {
    memory::captureSession(chatId.c_str(), store::orchHost().c_str(), String(text.c_str()));
    String id = memory::captureMessage(chatId.c_str(), "user", orch::MsgKind::Message,
                                       String(text.c_str()), "", String(fromTag.c_str()));
    strncpy(g_curUserRowId, id.c_str(), sizeof(g_curUserRowId) - 1);
    g_curUserRowId[sizeof(g_curUserRowId) - 1] = 0;
    // The user row IS this turn's identity - re-key the turn tag off it (the
    // onTurnStart fallback minted a placeholder). The row does NOT carry a
    // turn: tag itself: its own `id` is the key, and appending one would
    // corrupt the "from:<sender>" label the chat view slices out of tags.
    if (id.length()) snprintf(g_turnTag, sizeof(g_turnTag), "turn:%s", id.c_str());
    if (chatId != "system") g_folds.noteMessage(chatId, text.size());   // fold trigger
  };
  d.firstAllowedChat = [] { return std::string(firstAllowedChat().c_str()); };
  d.journalGc = [] { g_journal.gc(); };
  // Clamped-tool-result spill (Context Fabric): the loop's truncation marker
  // then carries a results.get handle instead of a dead "…[truncated]".
  d.spillResult = [](const std::string& name, const std::string& fullText,
                     const std::string& ns) {
    return resultsPut("tool", name, fullText, std::string(), ns);
  };
  // Introspection snapshot - success AND failure, so /api/lastturn always shows
  // what the model actually received on the most recent attempt.
  // ---- glass-box trace capture (A4) ----------------------------------------
  // All four hooks fire on tg_poll only (the loop's task), so the statics below
  // need no lock. Rows are UTF-8-safe-clipped BEFORE building the String, so the
  // internal-heap transient per row stays ~2 KB; memory::traceActive() gates on
  // the SD append-log + the owner `otrace` knob.
  static std::map<std::string, std::string> s_traceArgs;   // call id -> args (full cap)
  static int s_traceBlobs = 0;   // sidecars written this turn (P4 budget)
  d.hooks.onTurnStart = [](const TurnStartEv& ev) {
    // Turn identity = the turn's USER row id (boot-stable, resumed past history
    // via nextIdHint) - so the chat UI joins a bubble to its trace rows by ID
    // instead of chronological adjacency (which mis-bucketed an interleaved
    // Telegram/voice turn's rows into the wrong web bubble). Seedless turns
    // (scheduled loops, synthesis/dream) have no user row, so they mint one from
    // the same counter here; episodicCaptureUser overwrites this with the real
    // row id when a user row does get captured, whichever order the hooks fire.
    s_traceBlobs = 0;
    // The user row is captured BEFORE this hook fires (durable-first), so prefer
    // the id it already stashed - minting here would consume a fresh id and
    // clobber the real one, leaving the turn tagged with an id no row owns
    // (caught on hardware: user row m…85 vs tag turn:m…86). Only a SEEDLESS turn
    // (scheduled loop, synthesis/dream) has no user row and needs a minted id;
    // episodicCaptureUser still overwrites this if it lands after us.
    String id = g_curUserRowId[0] ? String(g_curUserRowId) : memory::mintRowId();
    snprintf(g_turnTag, sizeof(g_turnTag), "turn:%s", id.length() ? id.c_str() : "t0");
    s_traceArgs.clear();
  };
  d.hooks.onToolCall = [](const orch::HeadToolCall& call) {
    if (!memory::traceActive()) return;
    // Keep the args at the FULL cap here (P4): the row text still shows the
    // short clip, but the sidecar needs what the model actually sent. >=128 B
    // strings spill to PSRAM, and the map is bounded at 24 entries.
    int keep = nimbus::utf8CapLen(call.argsJson.c_str(), (int)call.argsJson.size(),
                                  ORCH_TRACE_ARGS_FULL);
    std::string a = keep < (int)call.argsJson.size()
                        ? call.argsJson.substr(0, (size_t)keep) + "\xE2\x80\xA6"
                        : call.argsJson;
    if (s_traceArgs.size() < 24) s_traceArgs[call.id] = std::move(a);
  };
  d.hooks.onToolResult = [](const orch::HeadToolResult& r) {
    if (!memory::traceActive()) return;
    std::string args;
    auto it = s_traceArgs.find(r.id);
    if (it != s_traceArgs.end()) { args = it->second; s_traceArgs.erase(it); }
    int keep = nimbus::utf8CapLen(r.output.c_str(), (int)r.output.size(),
                                  ORCH_TRACE_OUT_MAX);
    const bool outClipped  = keep < (int)r.output.size();
    std::string out = outClipped
                          ? r.output.substr(0, (size_t)keep) + "\xE2\x80\xA6[truncated]"
                          : r.output;
    // The row keeps the SHORT args clip; the sidecar keeps what was really sent.
    int akeep = nimbus::utf8CapLen(args.c_str(), (int)args.size(), ORCH_TRACE_ARGS_MAX);
    const bool argsClipped = akeep < (int)args.size();
    std::string argsRow = argsClipped ? args.substr(0, (size_t)akeep) + "\xE2\x80\xA6" : args;
    // ONE merged row per call: name(args) -> output. Halves row count and keeps
    // args + result together for the chat disclosure.
    String text = String(r.name.c_str()) + "(" + argsRow.c_str() + ")\n\xE2\x86\x92 " +
                  out.c_str();
    String tags = String(g_turnTag) + ",tool:" + r.name.c_str() +
                  (r.isError ? ",err" : "");
    // P4: when anything was clipped, park the FULL call+result as a blob so the
    // owner can open what the model actually saw (the model has results.get; the
    // web had nothing). Bounded per turn so a tool-heavy turn can't churn the SD.
    String blob;
    if ((outClipped || argsClipped) && s_traceBlobs < ORCH_TRACE_BLOBS_PER_TURN) {
      std::string full = r.name + "(" + args + ")\n\xE2\x86\x92 " + r.output;
      int fkeep = nimbus::utf8CapLen(full.c_str(), (int)full.size(), ORCH_TRACE_BLOB_MAX);
      if (fkeep < (int)full.size()) full = full.substr(0, (size_t)fkeep);
      blob = memory::persistTraceBlob(String(full.c_str()));
      if (blob.length()) s_traceBlobs++;
    }
    std::string chat(currentChat().c_str());
    memory::captureMessage(chat.c_str(), "tool", orch::MsgKind::ToolOutput, text, blob, tags);
  };
  d.hooks.onThinking = [](const ThinkingEv& ev) {
    if (!memory::traceActive()) return;
    int keep = nimbus::utf8CapLen(ev.text.c_str(), (int)ev.text.size(),
                                  ORCH_TRACE_THINK_MAX);
    std::string t = keep < (int)ev.text.size()
                        ? ev.text.substr(0, (size_t)keep) + "\xE2\x80\xA6"
                        : ev.text;
    char tags[40];
    snprintf(tags, sizeof(tags), "%s,round:%d", g_turnTag, ev.round);
    memory::captureMessage(ev.chatId.c_str(), "assistant", orch::MsgKind::LlmResponse,
                           String(t.c_str()), "", tags);
  };
  // B5: sub-agent COMMS into the glass box - the spawn brief and the terminal
  // result land as kind=log rows in the SPAWNING chat's session, so the debug
  // disclosure shows the full delegation round-trip. (The sub-agent's INTERNAL
  // provider-side transcript/tool events remain a documented follow-up.)
  d.hooks.onSpawn = [](const SpawnEv& ev) {
    if (!memory::traceActive() || ev.chatId.empty()) return;
    String text = String("spawned ") + ev.tag.c_str() + " (" + ev.backend.c_str() +
                  "/" + ev.model.c_str() + "): " + ev.task.c_str();
    // Turn tag when the spawn happened INSIDE a turn (the usual case) so the
    // delegation groups with the reply that ordered it; bare when it didn't.
    String tags = String("ev:spawn,sub:") + ev.tag.c_str();
    if (g_turnTag[0]) tags += String(",") + g_turnTag;
    memory::captureMessage(ev.chatId.c_str(), "system", orch::MsgKind::Log, text, "",
                           tags);
  };
  d.hooks.onResult = [](const ResultEv& ev) {
    if (!ev.terminal || !memory::traceActive() || ev.chatId.empty()) return;
    // FULL result into the episodic row (Context Fabric Stage 1 - the SD
    // day-stream is the durable copy behind results.get's RAM ring; the old
    // 512-char clip silently lost the tail of every sub-agent result). The
    // ResultEnvelope already bounds ev.reply at 4 KB, and >=128 B strings spill
    // to PSRAM, so no internal-SRAM concern.
    std::string reply = ev.reply;
    String text = String("sub-agent ") + ev.tag.c_str() +
                  (ev.state == (uint8_t)orch::JobState::Done ? " finished: " : " FAILED: ") +
                  reply.c_str();
    memory::captureMessage(ev.chatId.c_str(), "system", orch::MsgKind::Log, text, "",
                           String("ev:subresult,sub:") + ev.tag.c_str());
  };
  d.hooks.onTurnDebug = [](const TurnDebugEv& ev) {
    captureTurnDebug(ev);
    // Glass Box P3: persist the same anatomy per TURN, so the chat can open any
    // past turn - /api/lastturn is one RAM slot, overwritten by the next turn.
    // Bounded copy + a file ring; skipped entirely when trace is off / no SD.
    if (memory::traceActive() && g_turnTag[0]) {
      size_t n = 0;
      char* blob = lastTurnDebugPs(n);
      if (blob) {
        if (n > ORCH_TRACE_FILE_MAX)
          n = (size_t)nimbus::utf8CapLen(blob, (int)n, ORCH_TRACE_FILE_MAX);
        g_turnDossier = memory::writeTraceFile(String(g_turnTag + 5), blob, n);  // skip "turn:"
        free(blob);
      }
    }
  };
  // Lifecycle breadcrumb (observer-only): one log line per completed turn with
  // real billed tokens + tool-call count. onSpawn/onResult stay UNWIRED here on
  // purpose - sfx + the ring already cover those via the event/fire sinks.
  d.hooks.onTurnEnd = [](const TurnEndEv& ev) {
    // Prism B: the in-flight-user-row id must not outlive its turn - a stale id
    // matching the chat's newest row made SYNTHESIS/loop turns silently drop the
    // owner's real last message from the RECENT CONVERSATION window (exactly the
    // request a spawn-synthesis turn is answering).
    g_curUserRowId[0] = 0;
    // Glass Box P2: one compact per-turn summary row BEFORE the tag is cleared -
    // the only PERSISTED per-turn token record (the usage ledger aggregates by
    // provider+tag and can't answer "what did THIS reply cost?"). Written as JSON
    // in the text field so the UI parses it without a schema change.
    // NB `ev.rounds` counts EXECUTED TOOL CALLS, not provider rounds (see
    // TurnEndEv) - it ships as "tools" so no surface can misreport it as rounds.
    if (memory::traceActive() && g_turnTag[0] && !ev.chatId.empty()) {
      std::string err = ev.error;
      if (err.size()) {
        int keep = nimbus::utf8CapLen(err.c_str(), (int)err.size(), ORCH_TRACE_ENDERR_MAX);
        if (keep < (int)err.size()) err = err.substr(0, (size_t)keep);
      }
      JsonDocument doc;
      doc["host"]  = ev.host;
      // Served-by disclosure (CUM-236 device leg): the turn view (_turnChip) reads
      // model+fallback off this row. On a provider/model substitution show the model
      // that actually answered and flag the fallback; a plain turn is unchanged. The
      // shared servedByDisclosure rule decides it, so a new fallback path discloses
      // here without a device-side re-implementation.
      const nimbus::orch::TurnChipDisclosure disc = nimbus::orch::turnChipDisclosure(
          ev.requestedHost, ev.requestedModel, ev.host, ev.servedModel,
          std::string(agent::store::orchModel(String(ev.host.c_str())).c_str()));
      doc["model"] = disc.model;
      if (disc.fallback) doc["fallback"] = true;
      doc["tools"] = ev.rounds;
      doc["ok"]    = ev.ok;
      doc["in"]    = (unsigned)ev.usage.promptTokens;
      doc["out"]   = (unsigned)ev.usage.completionTokens;
      doc["reply"] = (unsigned)ev.replyBytes;
      if (err.size()) doc["err"] = err;
      String js;
      serializeJson(doc, js);
      // blobPath -> this turn's dossier, so the chat can offer "turn anatomy"
      // without probing for a file that may have been evicted by the ring.
      String dossier = g_turnDossier
                           ? String("/mem/trace/") + (g_turnTag + 5) + ".txt"
                           : String();
      if (js.length() <= ORCH_TRACE_END_MAX)
        memory::captureMessage(ev.chatId.c_str(), "system", orch::MsgKind::Log, js, dossier,
                               String("ev:turnend,") + g_turnTag);
    }
    g_turnTag[0] = 0;   // same reason: no async row may inherit this turn's id
    g_turnDossier = false;
    alogf("turn end: %s host=%s tokens in=%u out=%u tools=%d reply=%uB",
          ev.ok ? "ok" : "FAIL", ev.host.c_str(), (unsigned)ev.usage.promptTokens,
          (unsigned)ev.usage.completionTokens, ev.rounds, (unsigned)ev.replyBytes);
    // W3b passive validation: a SUCCESSFUL real turn is the strongest proof a
    // provider key works - refresh the verify cache from it, so "verified"
    // doesn't depend on the manual Verify button (the UI hint promises this).
    // Write only on a state CHANGE (no steady-state NVS wear). Failures are NOT
    // marked rejected here: most turn failures are not auth failures.
    if (ev.ok && (ev.host == "openai" || ev.host == "anthropic" || ev.host == "mistral") &&
        store::verifyResult(ev.host.c_str()) != 1)
      store::setVerify(ev.host.c_str(), 1, millis());
    // Timeline row for FAILED turns only (Glass Box A3) - successes are already
    // visible as chat history; failures previously left no queryable trace.
    if (!ev.ok)
      memory::captureEvent("error", String("Turn FAILED on ") + ev.host.c_str() +
                                    " after " + ev.rounds + " tool round(s)");
    dream::noteTurnEnd(millis());   // DREAMING idle gate: quiet clock
    // v3.6.0 fold accounting: one persist per turn + the reactive overflow path.
    if (!ev.chatId.empty() && ev.chatId != "system") {
      strncpy(g_lastTurnChat, ev.chatId.c_str(), sizeof(g_lastTurnChat) - 1);
      g_lastTurnChat[sizeof(g_lastTurnChat) - 1] = 0;
      g_folds.noteTurn(ev.chatId);
      publishFoldStatus(ev.chatId);
      if (!ev.ok && nimbus::orch::isContextOverflowError(ev.error)) {
        g_folds.markOverflow(ev.chatId);   // next compactTick() folds this chat
        alogf("fold: context-overflow classified on %s - fold queued", ev.chatId.c_str());
      }
    }
  };
  // ProviderHosts: String<->std::string at the wrapper; the orchTurn* adapters
  // themselves are UNTOUCHED. An unknown host fails over inside the engine.
  auto wrap = [](bool (*fn)(String&, const String&, const String&, String&, String&,
                            const agent::HeadTools*, nimbus::orch::TokenUsage*)) {
    return [fn](std::string& cv, const std::string& ins, const std::string& inp,
                std::string& out, std::string& err, const agent::HeadTools* tools,
                nimbus::orch::TokenUsage* usage) {
      String cvS(cv.c_str()), outS, errS;
      const bool ok = fn(cvS, String(ins.c_str()), String(inp.c_str()), outS, errS,
                         tools, usage);
      cv  = cvS.c_str();
      out = outS.c_str();
      err = errS.c_str();
      return ok;
    };
  };
  d.hosts.add("openai",    wrap(&openai_responses::orchTurn));
  d.hosts.add("anthropic", wrap(&orchTurnAnthropic));
  d.hosts.add("mistral",   wrap(&orchTurnMistral));
  // Stage 2 phase 5: the engine-owned multi-provider fabric loop. The engine
  // routes LOOP turns here when store::midTurnFailover() is on - a failed round
  // switches provider and re-runs against the shared canonical transcript.
  d.hosts.fabric = [](const std::vector<std::string>& hostList, const std::string& ins,
                      const std::string& inp, std::string& out, std::string& err,
                      const agent::HeadTools& ht, nimbus::orch::TokenUsage* usage,
                      const std::function<void(const std::string&, const std::string&)>& notify) {
    return providers::runFabricLoop(deviceProviderDeps(), hostList, ins, inp, out, err,
                                    ht, usage, notify);
  };
  d.hosts.fabricSupports = [](const std::string& h) { return providers::fabricSupports(h); };
  // Head on the custom/LAN endpoint (Stage H): a single-shot chat-completions
  // structured turn (no tool loop v1 - the portable fn ignores `tools`), so a
  // fully keyless LAN setup (e.g. Ollama) can run the whole orchestrator.
  // Registered ONLY when configured - mirrors adapter_factory's boot-time
  // hasCustom() gate (reboot to apply), and hostHasKey('custom') maps to
  // store::hasCustom() via the HarnessConfig view, so priority/failover just work.
  if (store::hasCustom())
    d.hosts.add("custom", [](std::string& cv, const std::string& ins, const std::string& inp,
                             std::string& out, std::string& err, const agent::HeadTools* tools,
                             nimbus::orch::TokenUsage* usage) {
      return providers::orchTurnCustom(deviceProviderDeps(), cv, ins, inp, out, err,
                                       tools, usage);
    });
  // CUM-242: the router providers as first-class orchestrator HEADS. Same
  // single-shot chat-completions turn as the custom head (orchTurnCustom), but
  // over a FIXED base + path prefix and the router key, so a device holding only
  // a verified Cumulo (or Z.ai) key runs the whole assistant - the flagship "one
  // key, one balance" path. Registered UNCONDITIONALLY (unlike custom's boot-time
  // hasCustom() gate) so a key added AFTER boot works with no reboot: selection is
  // gated live by routerFallbackHost()/hasKey() (both check the key at turn time),
  // and orchTurnCustom refuses honestly if the base/key is somehow empty.
  d.hosts.add("cumulo", [](std::string& cv, const std::string& ins, const std::string& inp,
                           std::string& out, std::string& err, const agent::HeadTools* tools,
                           nimbus::orch::TokenUsage* usage) {
    providers::ProviderDeps pd = deviceProviderDeps();
    pd.customBase       = [] { String b = store::cumuloBase();
                               return std::string((b.length() ? b : String(CUMULO_HOST_DEFAULT)).c_str()); };
    pd.customPathPrefix = [] { return std::string("/router/openai/v1"); };
    pd.customKey        = [] { return std::string(store::cumuloKey().c_str()); };
    pd.customConv       = [] { return std::string("openai"); };
    pd.customModel      = [] { String m = store::orchModel("cumulo");
                               return std::string((m.length() ? m : String(CUMULO_MODEL)).c_str()); };
    return providers::orchTurnCustom(pd, cv, ins, inp, out, err, tools, usage);
  });
  d.hosts.add("zai", [](std::string& cv, const std::string& ins, const std::string& inp,
                        std::string& out, std::string& err, const agent::HeadTools* tools,
                        nimbus::orch::TokenUsage* usage) {
    providers::ProviderDeps pd = deviceProviderDeps();
    pd.customBase       = [] { String b = store::zaiBase();
                               return std::string((b.length() ? b : String(ZAI_HOST_PRIMARY)).c_str()); };
    pd.customPathPrefix = [] { return std::string(ZAI_BASE_PATH); };
    pd.customKey        = [] { return std::string(store::zaiKey().c_str()); };
    pd.customConv       = [] { return std::string("openai"); };
    pd.customModel      = [] { String m = store::orchModel("zai");
                               return std::string((m.length() ? m : String(ZAI_MODEL)).c_str()); };
    return providers::orchTurnCustom(pd, cv, ins, inp, out, err, tools, usage);
  });
  return d;
}

// P7 (/api/tools): does this registry tool ride the mid-turn loop right now?
bool toolRidesLoop(const std::string& name) {
  return g_engine && g_engine->loopActiveNow() && !loopToolHidden(name);
}

// ---- public API -------------------------------------------------------------

void begin(HeavyFabric* fabric, const Sinks& sinks) {
  g_fabric = fabric;
  g_sinks  = sinks;
  // Route the portable harness library's log seam into the device agent log
  // (alog -> RAM ring -> GET /api/log), same surface the rest of the agent uses.
  hlog::setSink([](const char* line) { alog(line); });
  g_journalStore.begin();
  g_journal.begin(&g_journalStore);
  // Construct the portable job machinery over the freshly loaded journal (the
  // engine's ctor arms the poll gate at "now", replacing g_nextPollAt = millis()).
  static JobEngine engine(buildJobDeps(),
      JobEngine::Tuning{AGENT_MAX_ACTIVE_INFLIGHT, ORCH_DISPATCH_MIN_HEAP,
                        AGENT_POLL_INTERVAL_MS});
  g_jobs = &engine;
  g_jobs->setAttnHoldMs(g_attnHoldMs);   // apply a pre-begin Param::AttnHoldMs latch
  if (!g_sessMux) g_sessMux = xSemaphoreCreateMutex();   // guards the session snapshot
  refreshSessionSnapshot();   // prime it from the restored journal (pre-first-pump reads)
  // The harness config view + the portable turn orchestration over it. Heap
  // floors come from agent_config.h so the device numbers stay authoritative.
  g_cfg = harnessConfigFromStore();
  static TurnEngine turnEngine(buildTurnDeps(),
      TurnEngine::Tuning{ORCH_TURN_HARD_FLOOR, ORCH_RECALL_MIN_HEAP,
                         ORCH_AUTO_TURN_MIN_HEAP, ORCH_LOOP_MIN_HEAP});
  g_engine = &turnEngine;
  g_mem.begin(&g_memStore, std::string(store::sysPrompt().c_str()));
  g_folds.begin(&g_foldIO);   // v3.6.0 per-chat fold state (/data/chatsum.txt)
  loadTenants();              // v3.7.0 RBAC roles/quotas (+ legacy adoption)
  syncMemEcho();
  // Mid-turn device control (owner, 2026-07-24: "the model must be able to run as
  // many tool calls as necessary in a single turn"). The tool loop already gives
  // it up to orchLoopRounds() tool rounds - what was missing is a TOOL that
  // touches the device: config/led/lights/tts existed only as end-of-turn
  // device[] contract actions. This registers the same element shape, validator,
  // policy rails (scheduled-turn strips, protected-key block, risk notes), and
  // executor as device[] - but applied NOW, so a sequenced flow (read volume ->
  // set volume 10 -> speak -> set volume back) completes inside ONE turn.
  memory::registry().add(
      "device.control",
      "Apply ONE device action IMMEDIATELY, mid-turn. 'action' is the same JSON "
      "shape as a device[] element: {\"type\":\"config\"|\"led\"|\"lights\"|"
      "\"tts\", ...}. Use when steps must happen in ORDER within one turn - e.g. "
      "device.status to read cfg.sfxVol, then {\"type\":\"config\",\"sfxVol\":10}, "
      "then reply.speak, then {\"type\":\"config\",\"sfxVol\":0} to restore. "
      "Changes are real and persist (identical knobs + limits to device[]). "
      "reboot is refused here - put {\"type\":\"reboot\"} in the end-of-turn "
      "device[] instead.",
      [](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) -> nimbus::orch::ToolResult {
        if (!a["action"].is<ArduinoJson::JsonObjectConst>())
          return nimbus::orch::ToolResult::fail("missing 'action' object");
        const char* ty = a["action"]["type"] | "";
        if (strcmp(ty, "reboot") == 0)
          return nimbus::orch::ToolResult::fail(
              "reboot is end-of-turn only - put {\"type\":\"reboot\"} in the "
              "turn's device[] actions (it applies after this turn finishes)");
        std::string json;
        serializeJson(a["action"], json);
        static agent::ApplyDeps s_devDeps = buildApplyDeps();
        s_devDeps.ttsEnabled = store::ttsEnabled();   // live gate, not begin()-time
        std::string risk;
        std::string sum = agent::applyDeviceElement(json, inScheduledTurn(),
                                                    std::string(currentChat().c_str()),
                                                    s_devDeps, &risk);
        std::string out = "applied: " + sum;
        if (!risk.empty()) out += risk;
        return nimbus::orch::ToolResult::ok(out);
      },
      R"JS({"type":"object","properties":{"action":{"type":"object","description":"one device[] element, verbatim (config/led/lights/tts)"}},"required":["action"]})JS");
  int n = g_journal.count();
  alogf("orchestrator: ready (%d active job%s)", n, n == 1 ? "" : "s");
}

static String (*g_otaInstallHook)() = nullptr;
void setOtaInstallHook(String (*fn)()) { g_otaInstallHook = fn; }

void handleMessage(const String& text, const String& fromName, const String& chatId) {
  clearAsk();     // the owner responded -> resolve any pending "needs you" ask
  drainStaged();  // apply web-staged directive/memory edits before the turn

  // Deterministic commands, handled before the turn so the owner can always manage
  // the device with the model out of the path. Web/serial/voice are physically /
  // token-authenticated pseudo-channels, so they count as the owner here; a Telegram
  // MEMBER can converse but not run owner commands. Parsing is EXACT-verb (no loose
  // prefix): "/updates", "/update now", "/loops@Bot" all resolve correctly, and an
  // unknown "/typo" gets a deterministic reply instead of a paid LLM turn.
  {
    const bool pseudo = (chatId == "web" || chatId == "serial" || chatId == "voice");
    nimbus::orch::Command cmd = nimbus::orch::parseCommand(std::string(text.c_str()));
    if (cmd.isCommand && (isChatAllowed(chatId) || pseudo)) {
      const std::string& v = cmd.verb;
      // Owner-only verbs: they schedule unattended turns, install firmware, or
      // approve the owner's downloads - an allow-listed MEMBER may converse but
      // not run these. (`fetch` was missing here - a member could approve/deny
      // the owner's queued URL downloads; closed alongside adding `remind`.)
      const bool ownerCmd = (v == "loops" || v == "loop" || v == "update" || v == "compact" ||
                             v == "skill" || v == "fetch" || v == "remind" || v == "clear" ||
                             v == "mcp" || v == "play");
      const bool owner = pseudo || agent::telegram::isOwner(chatId);
      // Every deterministic command reply SELF-IDENTIFIES (device name + fw).
      // Live confusion 2026-07-24: two devices sharing one bot token take turns
      // consuming messages, so an update notice from one board and the /update
      // "you're on the latest" from the other looked like a single schizophrenic
      // device. With the tag, a mixed conversation is instantly visible.
      const String selfTag = String(nimbus::sys::deviceName().c_str()) +
                             " \xC2\xB7 " NIMBUS_FW_VERSION ": ";
      // Deterministic command replies are the device's own copy: deliver with the
      // system-provenance flag so the outbound screen exempts them by provenance
      // (not by any prefix a model could reproduce - CUM-275).
      auto say = [&](const String& body) { deliver(chatId, body, /*system=*/true); };
      if (ownerCmd && !owner) {
        say(selfTag + "Only the device's owner can use that command. "
                        "You're welcome to keep chatting with me here.");
        return;
      }
      if (v == "update") {
        if (!cmd.args.empty()) {
          say(selfTag + "Send /update on its own to install a pending update.");
          return;
        }
        say(selfTag + (g_otaInstallHook ? g_otaInstallHook()
                                                    : String("Updates aren't available on this build.")));
        return;
      }
      if (v == "play") {
        // Music control (CUM-40): "/play" all of /music, "/play <name>.mp3" one
        // track, "/play stop|pause". WAV plays today; MP3 needs the decoder build.
        String arg = String(cmd.args.c_str()); arg.trim();
        String low = arg; low.toLowerCase();
        if (low == "stop")      { music::stop();  say(selfTag + "Stopped music."); return; }
        if (low == "pause")     { music::pause(); say(selfTag + "Paused music."); return; }
        if (arg.length() == 0) {
          int n = music::playAll();
          say(selfTag + (n ? ("Playing " + String(n) + " track" + (n == 1 ? "" : "s") + " from the music folder.")
                                        : String("No tracks in the music folder (SD /music).")));
          return;
        }
        if (!nimbus::orch::validMusicName(arg.c_str())) {
          say(selfTag + "That is not a valid track name. Use a .wav or .mp3 file in the music folder.");
          return;
        }
        music::playNow({std::string(arg.c_str())});
        say(selfTag + "Playing " + arg + ".");
        return;
      }
      if (v == "clear") {
        // Drop the current conversation + the scratchpad's active task, keeping
        // long-term memory and files. Light two-step confirm (this is recoverable,
        // not a danger-zone erase): "/clear" explains + asks, "/clear yes" does it.
        String arg = String(cmd.args.c_str()); arg.trim(); arg.toLowerCase();
        if (arg == "yes" || arg == "confirm") {
          requestConvClear();
          say(selfTag + "Cleared this conversation and its active task. "
                          "Long-term memory and files are kept.");
        } else {
          say(selfTag + "Clear this conversation and the current active task? "
                          "Long-term memory and files are kept. Send /clear yes to confirm.");
        }
        return;
      }
      if (v == "mcp") {
        // Cross-lane (CUM-33): owner approves/denies a device-dialed MCP server so
        // its tools can be discovered. Flips the `appr` bit on the mcp entry in the
        // connectors blob - the SAME NVS rail as the token-gated /api/connectors
        // write. Enforcement (discovery/retraction) lives in N4's connectors sync.
        String rest = String(cmd.args.c_str()); rest.trim();
        int sp = rest.indexOf(' ');
        String action = (sp < 0) ? rest : rest.substring(0, sp); action.toLowerCase();
        String name = (sp < 0) ? String("") : rest.substring(sp + 1); name.trim();
        if ((action != "approve" && action != "deny") || name.length() == 0) {
          say(selfTag + "Usage: /mcp approve <name>  |  /mcp deny <name>");
          return;
        }
        JsonDocument blob;
        bool ok = !deserializeJson(blob, store::connectorsJson());   // truthy == error
        bool found = false;
        if (ok && blob.is<ArduinoJson::JsonArray>()) {
          for (ArduinoJson::JsonObject e : blob.as<ArduinoJson::JsonArray>()) {
            if (String((const char*)(e["kind"] | "")) == "mcp" &&
                String((const char*)(e["name"] | "")) == name) {
              e["appr"] = (action == "approve") ? 1 : 0;
              found = true;
              break;
            }
          }
        }
        if (!found) {
          say(selfTag + "No MCP server named \"" + name + "\". Add it on the web page first.");
          return;
        }
        String out; serializeJson(blob, out);
        store::setConnectorsJson(out);
        say(selfTag + (action == "approve"
                  ? ("Approved MCP server \"" + name + "\". Its tools will be discovered shortly.")
                  : ("Removed approval for MCP server \"" + name + "\". Its tools are retracted.")));
        return;
      }
      if (v == "compact") {
        // Async by design: the fold runs on the next pollJobs pass (~1 s), off
        // the command path - the same staged pattern as every heavy owner op.
        stageManualFold(chatId.c_str());
        say(selfTag + "Compacting this conversation in the background \xE2\x80\x94 I'll confirm shortly.");
        return;
      }
      if (v == "remind") {
        // W22: the OWNER's deterministic one-time wakeup - the human counterpart
        // of the model's wakeup.set. "/remind <when> <what>" schedules ONE future
        // turn carrying <what>, framed as a [REMINDER] (createdBy=Owner). Fires
        // once, no approval, under the same daily governor as any routine.
        String rest = String(cmd.args.c_str());
        int sp = rest.indexOf(' ');
        String durStr = (sp < 0) ? rest : rest.substring(0, sp);
        String note = (sp < 0) ? String("") : rest.substring(sp + 1); note.trim();
        const long secs = nimbus::orch::parseDurationSecs(std::string(durStr.c_str()));
        if (secs < 0 || note.length() == 0) {
          say(selfTag + "Usage: /remind <when> <what> \xE2\x80\x94 e.g. "
                          "/remind 30m take the cake out. Time is a span like 45s, 30m, "
                          "2h, or 1d.");
          return;
        }
        JsonDocument sd;
        sd["kind"] = "once";
        sd["in_seconds"] = (int)secs;
        auto rr = loops::createLoop("reminder", note, chatId,
                                    sd.as<ArduinoJson::JsonObjectConst>(),
                                    /*byAgent=*/false, inScheduledTurn());
        if (!rr.ok) {
          // Translate the core's in_seconds bounds message into /remind's vocabulary.
          String em = String(rr.err.c_str());
          if (em.indexOf("in_seconds") >= 0)
            em = "that time is out of range \xE2\x80\x94 pick between 2 minutes and 7 days";
          say(selfTag + "Couldn't set that reminder: " + em);
          return;
        }
        const long mins = secs / 60;
        String when = (secs < 3600) ? (String(mins) + " min")
                    : (secs < 86400) ? (String(secs / 3600) + "h" +
                                        (secs % 3600 ? " " + String((secs % 3600) / 60) + "m" : ""))
                    : (String(secs / 86400) + "d");
        say(selfTag + "Reminder set \xE2\x80\x94 in " + when + " I'll remind you: " +
                        note + "  (cancel with /loop deny " + String(rr.id.c_str()) + ")");
        return;
      }
      if (v == "loops") { say(selfTag + loops::loopsText()); return; }
      if (v == "loop") {
        String rest = String(cmd.args.c_str());   // "action [id]"
        int sp = rest.indexOf(' ');
        String action = (sp < 0) ? rest : rest.substring(0, sp);
        String id = (sp < 0) ? String("") : rest.substring(sp + 1); id.trim();
        if (action.length() == 0) { say(selfTag + "Usage: /loops \xC2\xB7 /loop approve|deny|off|on <id>"); return; }
        String msg;
        if      (action == "approve") msg = loops::approveLoop(id) ? "Approved \xE2\x80\x94 it will run on schedule." : "No routine with that ID.";
        else if (action == "deny" || action == "kill" || action == "delete")
          msg = loops::isReservedId(id)
                    ? "This is a system routine \xE2\x80\x94 use /loop off " + id + " to pause it."
                    : (loops::cancelLoop(id) ? "Removed." : "No routine with that ID.");
        else if (action == "off"  || action == "pause")  msg = loops::setEnabled(id, false) ? "Paused." : "No routine with that ID.";
        else if (action == "on"   || action == "resume") msg = loops::setEnabled(id, true)  ? "Resumed." : "No routine with that ID.";
        else { say(selfTag + "Usage: /loops  |  /loop approve|deny|off|on <id>"); return; }
        say(selfTag + "Routine " + id + ": " + msg);
        return;
      }
      if (v == "fetch") {
        // W18: approve/deny a queued URL download - the Telegram counterpart of
        // the web Memory & Files card. Deterministic, owner-only (this branch
        // already runs under the owner gate like /update and /loop).
        String rest = String(cmd.args.c_str());
        int sp = rest.indexOf(' ');
        String action = (sp < 0) ? rest : rest.substring(0, sp);
        String id = (sp < 0) ? String("") : rest.substring(sp + 1); id.trim();
        uint32_t n = (uint32_t)id.toInt();
        if (action == "approve" && n) {
          say(selfTag + (files::fetchApprove(n)
              ? "Approved - downloading in the background; I'll confirm when it lands."
              : "No pending download with that ID."));
        } else if (action == "deny" && n) {
          say(selfTag + (files::fetchDeny(n)
              ? "Denied - it will not be downloaded."
              : "No pending download with that ID."));
        } else {
          say(selfTag + "Usage: /fetch approve|deny <id>");
        }
        return;
      }
      if (v == "skill") {
        // v4.0.0: approve/deny an agent-authored skill capsule - the Telegram
        // counterpart of the web approve button. Deterministic, owner-only,
        // never a paid turn (intercepted before the LLM like /loop).
        String rest = String(cmd.args.c_str());
        int sp = rest.indexOf(' ');
        String action = (sp < 0) ? rest : rest.substring(0, sp);
        String id = (sp < 0) ? String("") : rest.substring(sp + 1); id.trim();
        if (action.length() == 0 || id.length() == 0) {
          say(selfTag + "Usage: /skill approve|deny <id>");
          return;
        }
        std::string err;
        String msg;
        if (action == "approve")
          msg = skills::approve(id.c_str(), err) ? "Approved \xE2\x80\x94 it can now be used."
                                                  : String(err.c_str());
        else if (action == "deny" || action == "delete")
          msg = skills::remove(id.c_str(), err) ? "Removed." : String(err.c_str());
        else { say(selfTag + "Usage: /skill approve|deny <id>"); return; }
        say(selfTag + "Skill " + id + ": " + msg);
        return;
      }
      if (v == "help" || v == "start") {
        say("Hi \xE2\x80\x94 I'm " + String(nimbus::sys::deviceName().c_str()) +
                        " (firmware " NIMBUS_FW_VERSION
                        "). Send a message and I'll reply here.\n\nOwner commands:\n"
                        "/update \xE2\x80\x94 install a pending firmware update\n"
                        "/loops \xE2\x80\x94 list routines\n"
                        "/remind <when> <what> \xE2\x80\x94 one-time reminder (e.g. /remind 30m ...)\n"
                        "/compact \xE2\x80\x94 summarize this conversation into memory\n"
                        "/clear - forget this conversation (keeps memory and files)\n"
                        "/mcp approve|deny <name> - approve a tool server\n"
                        "/play [track|stop|pause] - play music from the SD card\n"
                        "/loop approve|deny|off|on <id> \xE2\x80\x94 manage a routine\n"
                        "/skill approve|deny <id> \xE2\x80\x94 manage a saved skill");
        return;
      }
      // Any other slash command: deterministic reply, never a paid LLM turn.
      say(selfTag + "I don't recognize /" + String(v.c_str()) +
                      " \xE2\x80\x94 try /help. To chat, just write without the slash.");
      return;
    }
  }
  if (!g_engine) return;
  // Gate 1 (CUM-69): inbound guest/member text screened PRE-turn, fail-CLOSED.
  // Admin (owner / web / serial / voice) is never classified. Runs on tg_poll with
  // the TLS slot free (like the fetch scan); a block stops the paid turn entirely.
  if (moderateGate(nimbus::orch::ModGate::InboundText, chatId, text) == nimbus::orch::ModAction::Block) {
    alogf("moderation: inbound blocked (chat=%s)", chatId.c_str());
    deliver(chatId, String(nimbus::sys::deviceName().c_str()) + " \xC2\xB7 " NIMBUS_FW_VERSION
                    ": That message couldn't be processed here.", /*system=*/true);
    return;
  }
  g_engine->handleMessage(std::string(text.c_str()), std::string(fromName.c_str()),
                          std::string(chatId.c_str()));
}

// ---- output-channel helpers (P6: reply.speak / reply.telegram tools) --------
// These back the registry tools so the MODEL chooses its output channel, instead
// of the old global ttsEnabled checkbox. reply.speak reads TTS out on the device
// speaker (loopback-verified working); reply.telegram delivers text to a chat.
String currentChat() { return g_engine ? String(g_engine->currentChat().c_str()) : String(); }

bool speakOnDevice(const String& text, bool capture) {
  if (text.length() == 0) return false;
  if (nimbus::fault::active(nimbus::fault::SPEAKER)) return false;
  // Synthesize in the format the CONFIGURED provider actually emits, then play it
  // on the speaker (I2S TX, independent of the mic). OpenAI -> WAV (played directly);
  // Mistral/Voxtral -> MP3 (decoded by the vendored minimp3). This is the field-bug
  // fix: the old path forced "wav", but a Mistral-only device can't make WAV, so
  // synth returned 0 and the device NEVER spoke. speakerTtsFormat() is host-tested.
  bool mp3 = false;
  // Use the EFFECTIVE provider (accounts for a key-driven fallback to the other
  // provider), so the format we synthesize matches the provider that will voice it.
  const char* fmt = core::speakerTtsFormat(std::string(agent::tts::activeProvider().c_str()), &mp3);
  const char* path = mp3 ? "/reply.mp3" : "/reply.wav";
  size_t n = agent::tts::synthesizeToFile(text, path, fmt);
  if (!n) return false;
  // QUEUE the playback on the sfx task - never play inline. This runs on
  // tg_poll inside a tool round; the synth fetch above is bounded (~4 s) but
  // fetch + an 8 s clip held the tool dispatch locks past loopTask's 8 s
  // watchdog budget: task_wdt abort, observed in the field as "the harness
  // reset itself" on 'count to ten out loud'.
  const bool played = ::sfx::speakReply(mp3);
  if (!played) alogf("speak: could not queue %s (%u bytes) - sfx queue full/off?", path, (unsigned)n);
  // Capture spoken output so a voice-only reply is still in history ("what did you
  // just say?" works on stateless hosts once device history reads these rows). Skip
  // when the caller already records it (the tts device action captures in apply.cpp),
  // so the same utterance is not written to history twice.
  if (played && capture) memory::captureMessage(currentChat().c_str(), "assistant",
                                                orch::MsgKind::Message, text, "",
                                                withTurnTag("via:speaker"));
  return played;
}

// Is `cid` in the persisted Telegram allowlist? Read from NVS (thread-safe) - NOT
// telegram's poll-task-owned g_allowlist, since this runs on the turn task.
static bool chatAllowed(const String& cid) {
  String al = store::telegramAllowlist();
  int s = 0;
  while (s < (int)al.length()) {
    int e = al.indexOf(',', s); if (e < 0) e = al.length();
    String t = al.substring(s, e); t.trim();
    if (t.length() && t == cid) return true;
    s = e + 1;
  }
  return false;
}

bool sendToChat(const String& chatId, const String& text, bool asVoice) {
  String cur = currentChat();
  String cid = chatId.length() ? chatId : cur;
  // serial/voice/web aren't Telegram-deliverable channels.
  if (cid.length() == 0 || cid == "serial" || cid == "voice" || cid == "web") return false;
  if (text.length() == 0) return false;
  // SECURITY (prism): the model must not be able to exfiltrate to an arbitrary
  // chat. Only the CURRENT turn's chat (implicit) or an ALLOWLISTED chat may be
  // messaged - the allowlist is the Telegram auth boundary, inbound AND outbound.
  if (cid != cur && !chatAllowed(cid)) return false;
  if (asVoice && g_sinks.speak) {
    g_sinks.speak(text);
    // Capture spoken-via-Telegram output to the TARGET chat's history.
    memory::captureMessage(cid.c_str(), "assistant", orch::MsgKind::Message, text, "",
                           withTurnTag("via:telegram"));
    if (cid == cur && g_engine) g_engine->noteToolReplied();
    return true;
  }
  const bool sent = g_sinks.send && g_sinks.send(cid, text);
  // The current turn's chat already got a real answer mid-loop - remember it so
  // applyTurn doesn't append a contentless "Done." as a second message.
  if (sent) {
    // reply.telegram output was invisible to history - capture it (to the TARGET
    // chat, so a cross-chat send lands in the right conversation).
    memory::captureMessage(cid.c_str(), "assistant", orch::MsgKind::Message, text, "",
                           withTurnTag("via:telegram"));
    if (cid == cur && g_engine) g_engine->noteToolReplied();
  }
  return sent;
}

// ---- Local Loops: scheduled-turn executor -----------------------------------
String firstAllowedChat() {
  String al = store::telegramAllowlist();
  int e = al.indexOf(','); if (e < 0) e = al.length();
  String t = al.substring(0, e); t.trim();
  return t;
}
bool isChatAllowed(const String& cid) { return chatAllowed(cid); }
bool inScheduledTurn() { return g_engine && g_engine->inScheduledTurn(); }

nimbus::orch::FireOutcome injectScheduledTurn(const String& chatId, const String& prompt,
                                              const String& name, const String& loopId,
                                              bool quietOk, bool once, bool ownerReminder) {
  if (!g_engine) {   // loops only fire after begin(); belt-and-braces
    nimbus::orch::FireOutcome o;
    o.detail = "deferred: not running";
    return o;
  }
  return g_engine->injectScheduledTurn(std::string(chatId.c_str()),
                                       std::string(prompt.c_str()),
                                       std::string(name.c_str()),
                                       std::string(loopId.c_str()), quietOk, once,
                                       ownerReminder);
}

// Staged system turn (Glass Box A2): set from any task (the OTA hook fires in
// its own task context, where a synchronous LLM turn would trip the watchdog),
// drained here on tg_poll - the same single-producer/consumer char-buffer+flag
// handoff as the telegram token swap. One slot; the latest stager wins.
static char          s_sysTurnPrompt[512] = {};
static char          s_sysTurnName[32] = {};
static volatile bool s_sysTurnPending = false;
static portMUX_TYPE  s_sysTurnMux = portMUX_INITIALIZER_UNLOCKED;

void stageSystemTurn(const String& prompt, const String& name) {
  portENTER_CRITICAL(&s_sysTurnMux);
  strncpy(s_sysTurnPrompt, prompt.c_str(), sizeof(s_sysTurnPrompt) - 1);
  s_sysTurnPrompt[sizeof(s_sysTurnPrompt) - 1] = 0;
  strncpy(s_sysTurnName, name.c_str(), sizeof(s_sysTurnName) - 1);
  s_sysTurnName[sizeof(s_sysTurnName) - 1] = 0;
  s_sysTurnPending = true;
  portEXIT_CRITICAL(&s_sysTurnMux);
}

static void drainSystemTurn() {
  if (!s_sysTurnPending) return;
  String prompt, name;
  portENTER_CRITICAL(&s_sysTurnMux);
  prompt = s_sysTurnPrompt; name = s_sysTurnName;
  s_sysTurnPending = false;
  portEXIT_CRITICAL(&s_sysTurnMux);
  String chat = firstAllowedChat();
  if (chat.length() == 0 || !g_engine) return;   // nowhere to deliver - drop quietly
  injectScheduledTurn(chat, prompt, name, "system", /*quietOk=*/true);
}

#ifdef NIMBUS_TEST
static volatile bool g_testReboot = false;
void stageTestReboot() { g_testReboot = true; }
bool testRebootRequested() { return g_testReboot; }
#endif

// ---- v3.7.0 tenant table ----------------------------------------------------
static std::vector<std::string> splitIds(const String& csv) {
  std::vector<std::string> out;
  int start = 0;
  while (start < (int)csv.length()) {
    int end = csv.indexOf(',', start);
    if (end < 0) end = csv.length();
    String one = csv.substring(start, end);
    one.trim();
    if (one.length()) out.push_back(std::string(one.c_str()));
    start = end + 1;
  }
  return out;
}

void loadTenants() {
  TenantLock lk;
  g_tenants.load(LittleFsTenantStoreIO::load());
  // First boot after the upgrade (or after a rollback wrote nothing): rebuild
  // from the legacy lists so nobody's access changes by surprise and the device
  // always has an admin.
  if (g_tenants.all().empty()) {
    g_tenants.adoptLegacy(splitIds(store::telegramOwners()),
                          splitIds(store::telegramAllowlist()));
    LittleFsTenantStoreIO::save(g_tenants.dump());
    alogf("rbac: adopted %u legacy tenants (%u admin)",
          (unsigned)g_tenants.all().size(), (unsigned)g_tenants.adminCount());
  }
}

void persistTenants() {
  TenantLock lk;
  if (!LittleFsTenantStoreIO::saveChecked(g_tenants.dump()))
    alog("rbac: tenant table did not persist - roles may revert on restart");
}

nimbus::orch::Role roleOfChat(const String& chatId) {
  using nimbus::orch::Role;
  // The token/physically-authenticated local surfaces are the device itself.
  if (chatId == "web" || chatId == "serial" || chatId == "voice") return Role::Admin;
  const std::string id(chatId.c_str());
  Role r;
  bool known;
  { TenantLock lk; known = g_tenants.known(id); r = g_tenants.roleOf(id); }
  // An EXPLICIT row is the answer, including an explicit Unknown - that is what
  // revoking someone writes, and revoking must not be undone by the fallback
  // below. (A revoked chat deliberately stays on the Telegram allowlist so they
  // can still be told they no longer have access.)
  if (known) return r;
  // No row at all: an allow-listed chat that predates the table is a User (it
  // already had conversational access); anyone else is Unknown until approved.
  return isChatAllowed(chatId) ? Role::User : Role::Unknown;
}

// ---- tenant mutations (locked; every surface goes through these) -------------
// Each takes the lock, mutates, persists, and returns a plain value. Nothing
// hands out a reference into the table, so a caller on another task cannot hold
// a pointer across a reallocation.

std::vector<nimbus::orch::Tenant> tenantSnapshot(size_t* adminsOut) {
  TenantLock lk;
  if (adminsOut) *adminsOut = g_tenants.adminCount();
  return g_tenants.all();                      // by value, under the lock
}

bool tenantQuotaOf(const std::string& chatId, nimbus::orch::Quota& out) {
  TenantLock lk;
  const nimbus::orch::Tenant* t = g_tenants.find(chatId);
  if (!t) return false;
  out = t->quota;
  return true;
}

bool tenantSetRole(const std::string& chatId, nimbus::orch::Role r, std::string& err) {
  TenantLock lk;
  if (!g_tenants.setRole(chatId, r, err)) return false;
  // A change that did not reach the disk must not report success: the next boot
  // would load the OLD table, silently undoing a demotion the admin believes
  // landed. Roll the in-RAM table back so it never disagrees with storage.
  if (!LittleFsTenantStoreIO::saveChecked(g_tenants.dump())) {
    g_tenants.load(LittleFsTenantStoreIO::load());
    err = "couldn't save that change (storage full?) - nothing was changed";
    return false;
  }
  return true;
}

bool tenantSetQuota(const std::string& chatId, const nimbus::orch::Quota& q,
                    std::string& err) {
  TenantLock lk;
  if (!g_tenants.setQuota(chatId, q, err)) return false;
  // A change that did not reach the disk must not report success: the next boot
  // would load the OLD table, silently undoing a demotion the admin believes
  // landed. Roll the in-RAM table back so it never disagrees with storage.
  if (!LittleFsTenantStoreIO::saveChecked(g_tenants.dump())) {
    g_tenants.load(LittleFsTenantStoreIO::load());
    err = "couldn't save that change (storage full?) - nothing was changed";
    return false;
  }
  return true;
}

bool tenantRemove(const std::string& chatId, std::string& err) {
  TenantLock lk;
  // Removing the ROW revokes access and frees a slot; it deliberately does NOT
  // erase what the person stored. An admin can still read those namespaces, and
  // silently destroying data as a side effect of a role change would be a nasty
  // surprise. Deleting the data is a separate, explicit act.
  if (!g_tenants.remove(chatId, err)) return false;
  // A change that did not reach the disk must not report success: the next boot
  // would load the OLD table, silently undoing a demotion the admin believes
  // landed. Roll the in-RAM table back so it never disagrees with storage.
  if (!LittleFsTenantStoreIO::saveChecked(g_tenants.dump())) {
    g_tenants.load(LittleFsTenantStoreIO::load());
    err = "couldn't save that change (storage full?) - nothing was changed";
    return false;
  }
  return true;
}

void stageManualFold(const char* chatId) {
  if (!chatId || !chatId[0]) return;
  portENTER_CRITICAL(&g_foldSpin);
  strncpy(g_manualFoldChat, chatId, sizeof(g_manualFoldChat) - 1);
  g_manualFoldChat[sizeof(g_manualFoldChat) - 1] = 0;
  portEXIT_CRITICAL(&g_foldSpin);
}

// Refresh the cross-task fold echo. tg_poll ONLY (every turn end + every fold
// outcome) - the sole reader of the live FoldStore off-task is the echo.
static void publishFoldStatus(const std::string& chat) {
  if (chat.empty()) return;
  nimbus::orch::ChatFold f = g_folds.get(chat);
  const uint32_t kb = store::compactAtKB();
  char buf[sizeof(g_foldEcho)];
  snprintf(buf, sizeof(buf),
           "CTX chat=%s bytes=%lu msgs=%lu turns=%lu paused=%d fails=%u lastFold=%lu "
           "sumBytes=%u threshKB=%u",
           chat.c_str(), (unsigned long)f.bytesSinceFold, (unsigned long)f.msgsSinceFold,
           (unsigned long)f.turnsSinceFold, (int)f.breakerPaused, (unsigned)f.breakerFails,
           (unsigned long)f.lastFoldEpoch, (unsigned)f.summary.size(), (unsigned)kb);
  portENTER_CRITICAL(&g_foldSpin);
  strncpy(g_foldEcho, buf, sizeof(g_foldEcho) - 1);
  g_foldEcho[sizeof(g_foldEcho) - 1] = 0;
  portEXIT_CRITICAL(&g_foldSpin);
}

// One-line fold state for the HIL harness (CTX? console / GET /api/test/ctx).
// Reads the tg_poll-published ECHO - never the live store (see g_foldEcho).
String foldStatusText(const char* chatArg) {
  char snap[sizeof(g_foldEcho)];
  portENTER_CRITICAL(&g_foldSpin);
  memcpy(snap, g_foldEcho, sizeof(snap));
  portEXIT_CRITICAL(&g_foldSpin);
  if (!snap[0]) return String("CTX no chat yet");
  // The echo tracks the last-active chat; an explicit mismatched request says so
  // rather than returning another chat's numbers.
  if (chatArg && chatArg[0]) {
    String want = String("CTX chat=") + chatArg + " ";
    if (strncmp(snap, want.c_str(), want.length()) != 0)
      return String("CTX chat=") + chatArg + " (not the active chat; echo: " + snap + ")";
  }
  return String(snap);
}

// v3.6.0 fold pump - one auto/manual compaction cycle per pass, tg_poll only.
// Runs at the END of pollJobs (after loops::checkDue consumed its FireOutcome,
// so fold spend can never leak into loop metering; runFold also never touches
// lastTurnUsage_). The cycle: notice -> episodic digest (64 KB budget) ->
// engine runFold (single-shot, no side effects) -> write order summary-blob
// FIRST, then chain reset, then the ev:compact boundary row + completion notice.
static void compactTick() {
  using nimbus::orch::FoldDue;
  if (!g_engine || g_engine->turnInFlight()) return;
  if (!nimbus::net::staConnected()) return;   // offline fold = guaranteed breaker burn

  // Gate FIRST (prism v3.6.0 HIGH): runFold's own heap/key/budget gates fire
  // AFTER the owner-visible notice and the 64 KB digest would already be paid,
  // and a Deferred leaves the trigger state intact - so a persistent defer
  // (board resting under the 30 K floor, a month-long budget cap) turned into a
  // notice + SD-digest loop once per pump pass. Nothing visible or expensive
  // happens until a fold can actually run. The staged manual slot is NOT
  // consumed here, so /compact survives the wait instead of vanishing.
  if (!g_engine->canFoldNow()) return;

  std::string chat;
  bool manual = false;
  portENTER_CRITICAL(&g_foldSpin);
  if (g_manualFoldChat[0]) { chat = g_manualFoldChat; manual = true; g_manualFoldChat[0] = 0; }
  portEXIT_CRITICAL(&g_foldSpin);
  if (!manual && g_lastTurnChat[0]) chat = g_lastTurnChat;
  if (chat.empty()) return;

  // Pseudo channels (web/serial/voice) hold ONE reply slot: any async fold
  // notice lands in it and offsets the request/reply pairing by one (live-
  // caught: "✓ Compacted." displaced every later web reply). They get NO fold
  // notices - the ev:compact row in the chat history is their visible record.
  const bool pseudo = (chat == "web" || chat == "serial" || chat == "voice");

  if (manual) {
    g_folds.resume(chat);   // manual /compact overrides a paused breaker
  } else {
    const uint32_t kb = store::compactAtKB();
    FoldDue due = g_folds.evaluateDue(chat, kb * 1024u, 200);
    if (due == FoldDue::ThrashPaused) {
      if (!pseudo)
        deliver(String(chat.c_str()),
                String("\xE2\x9A\xA0 Conversation compaction paused for this chat \xE2\x80\x94 it "
                       "kept refilling immediately. Send /compact to run it manually."),
                /*system=*/true);
      return;
    }
    if (due != FoldDue::Yes) return;
  }

  nimbus::orch::ChatFold prev = g_folds.get(chat);
  if (manual && prev.msgsSinceFold == 0 && prev.summary.empty()) {
    if (!pseudo) deliver(String(chat.c_str()), String("Nothing to compact yet."), /*system=*/true);
    return;
  }
  // No pre-notice (field 2026-08-11: "triple message on Compaction then
  // failure"). This ran on EVERY pump pass the fold was due - a failing fold
  // left the trigger counters intact, so the owner got "Compacting our
  // conversation" once per retry (3x before the breaker) and then the pause
  // alert. Auto folds are background maintenance: silent unless something
  // needs the owner (the breaker alert below). Manual /compact already ack'd
  // synchronously in handleMessage and gets its result message below.

  std::string digest;
  {
    using namespace nimbus::orch;
    MsgQuery q;
    q.sessionId = chat;
    q.haveKind = true; q.kind = MsgKind::Message;
    q.alsoKinds = {MsgKind::Image, MsgKind::File, MsgKind::Audio};   // media is conversation
    q.limit = 400;
    // NO sinceHours bound (prism v3.6.0): row stamps are nowHours(), which is
    // BOOT-RELATIVE until SNTP syncs, while lastFoldEpoch is a real epoch -
    // rows captured in a pre-sync window would sit below every later bound and
    // never reach any summary. The 400-row limit + the 64 KB digest budget
    // bound the slice instead, and the summary is ANCHORED (an update, not a
    // rewrite), so re-including already-folded rows costs a few tokens and can
    // never lose what the owner said.
    std::vector<EpisodicMessage> rows;
    { memory::Lock lk; rows = memory::episodic().query(q); }
    // Fold-slice bytes derive from the head model's window (65536 at the 200K
    // anchor - identical to the old constant on today's fleet).
    const String fh = store::resolvedOrchHost();
    const auto fb = deriveBudget(modelCtxTokens(fh.c_str(), store::orchModel(fh).c_str()), {});
    digest = dream::buildEpisodicDigest(rows, fb.foldSliceBytes);   // PSRAM via the 128 B spill
  }

  std::string summary;
  const uint32_t t0 = millis();
  const auto res = g_engine->runFold(chat, prev.summary, digest, summary);
  if (res == agent::TurnEngine::FoldResult::Deferred) {
    // A gate closed between canFoldNow() and the call - leave all state
    // untouched. A MANUAL request must not evaporate silently after the device
    // promised a confirmation: re-stage it for the next pass.
    if (manual) stageManualFold(chat.c_str());
    return;
  }
  if (res == agent::TurnEngine::FoldResult::Ok) {
    g_folds.applyFold(chat, summary, (uint32_t)time(nullptr));   // blob FIRST
    // ...and republish, which this function's own contract says to do on
    // "every turn end AND every fold outcome" - only the first half was ever
    // implemented. The echo has ONE writer, the onTurnEnd hook, so until now
    // the diagnostic always reported the stamp as of the last turn, i.e. the
    // value BEFORE this fold. Anything watching it saw a fold appear only when
    // the NEXT turn ended, which made a compaction look like it had not
    // happened. Safe here: compactTick already runs on tg_poll, the same task
    // that owns g_folds and performs the existing publish.
    publishFoldStatus(chat);
    g_engine->clearChatConv(chat);                               // then the chain
    memory::captureMessage(chat.c_str(), "system", nimbus::orch::MsgKind::Log,
        String("Conversation compacted: ") + String((unsigned)prev.bytesSinceFold) +
            " B / " + String((unsigned)prev.msgsSinceFold) + " msgs folded into a " +
            String((unsigned)summary.size()) + " B summary (" +
            String((unsigned)((millis() - t0) / 1000)) + " s)",
        "", String("ev:compact"));
    // Success confirmation only for MANUAL /compact (the owner asked and was
    // promised a result). Auto folds complete silently - the ev:compact history
    // row + publishFoldStatus are their record.
    if (!pseudo && manual) deliver(String(chat.c_str()), String("\xE2\x9C\x93 Compacted."), /*system=*/true);
  } else if (manual) {
    // Manual attempts reset the breaker (resume() above), so the 3-fail alert
    // edge can never fire for them - report the failure directly instead of
    // leaving "I'll confirm shortly" unanswered forever (prism v3.6.0).
    g_folds.noteFoldFailed(chat);
    publishFoldStatus(chat);   // a failed fold is an outcome too - make it readable
    if (!pseudo)
      deliver(String(chat.c_str()),
              String("\xE2\x9A\xA0 Couldn't compact this conversation just now. "
                     "Try /compact again in a moment."),
              /*system=*/true);
  } else if (g_folds.noteFoldFailed(chat)) {
    publishFoldStatus(chat);
    if (!pseudo) deliver(String(chat.c_str()),
            String("\xE2\x9A\xA0 Conversation compaction keeps failing for this "
                   "chat \xE2\x80\x94 automatic compaction is paused. /compact retries manually."),
            /*system=*/true);
  }
}

int pollJobs() {
  if (!g_fabric || !g_jobs) return 0;
  drainStaged();      // apply web-staged directive/memory edits between turns
  drainTerminates();  // apply web/MCP-staged session.terminate on the journal's writer task
  drainSystemTurn();  // post-OTA (etc.) unattended awareness turn - tg_poll only

  // Local Loops tick - fire at most one due scheduled loop. Internally 20 s-gated
  // and guarded on turn-in-flight / heap / WiFi; runs on THIS (tg_poll) task, so
  // a scheduled turn is watchdog-safe exactly like the engine's synthesis turn.
  loops::checkDue((uint64_t)time(nullptr), g_engine && g_engine->turnInFlight(),
                  ESP.getFreeHeap());

  // W18 files.fetch pump - one action per cycle (owner prompt OR one bounded
  // download) on THIS task, same single-TLS/watchdog discipline as everything
  // else here. Skipped while a turn runs (the turn owns the work TLS slot).
  if (!(g_engine && g_engine->turnInFlight())) files::tickFetch();

  // Everything job-shaped - reap, the per-completion synthesis clock, the
  // one-dispatch-per-cycle gate, loop-closure synthesis, and the round-robin
  // poll with backoff - lives in the portable JobEngine (Stage F lift).
  int n = g_jobs->pump();
  refreshSessionSnapshot();  // mirror the post-pump journal for AsyncTCP readers (F24)
  compactTick();             // v3.6.0 fold pump (after loops consumed their usage)
  return n;
}

bool cancel(const char* tagOrJobId) {
  return g_jobs && g_jobs->cancel(tagOrJobId);
}

int  activeJobCount() { return g_journal.count(); }
int  pendingSpawnCount() { return g_jobs ? g_jobs->pendingCount() : 0; }   // queued, not yet dispatched
bool turnInFlight()   { return g_engine && g_engine->turnInFlight(); }
uint32_t headJobKey() { return keyFromTag("head"); }

// CUM-11 backstop metric: every time a belt-and-braces path (not the primary edge)
// has to clear a stuck ring arc, this counts it. The 24 h wake-up soak acceptance is
// this == 0 - the backstops are insurance, never the mechanism. Surfaced in
// /api/state as ring.backstopFires so the soak can assert it stayed flat.
static uint32_t s_ringBackstopFires = 0;
uint32_t ringBackstopFires() { return s_ringBackstopFires; }
void noteRingBackstopFired() { ++s_ringBackstopFires; }

// Stuck-turn reaper - the decision + the arc-freeing event live in the portable
// TurnEngine (loop budget + 2 min margin). Called from the always-alive loop().
bool reapStuckTurn(uint32_t nowMs) {
  const bool fired = g_engine && g_engine->reapStuckTurn(nowMs);
  if (fired) ++s_ringBackstopFires;   // a dead turn slipped past the primary clear
  return fired;
}

// W6: keep the orchestrator's own "head" ring arc lit while its sub-agents run.
// A fan-out turn returns (and the TurnGuard would clear the arc) the instant it
// enqueues the children - long before they finish - so the head arc collapsed
// before the child arcs appeared. This runs on the always-alive main loop (the
// same watchdog block as reapStuckTurn), asks the pure tracker whether to light,
// refresh, or clear, and drives the arc through the engine. Returns true when it
// changed the arc (so the caller can refreshRing()).
bool reconcileHeadArc(uint32_t nowMs) {
  if (!g_engine) return false;
  static nimbus::harness::HeadArcTracker s_tracker;
  static uint32_t s_lastTurnCount = 0;
  // Completion EDGE: turnCount() bumps once per finished turn. A short wake-up turn
  // can start AND finish between two watchdog ticks, so turnInFlight below never
  // samples true - the edge is what tells the tracker a (possibly arc-leaving) turn
  // happened, so the primary clear path fires instead of a backstop (CUM-11).
  const uint32_t tc = g_engine->turnCount();
  const bool turnEnded = (tc != s_lastTurnCount);
  s_lastTurnCount = tc;
  const bool inTurn = g_engine->turnInFlight();
  const int children = g_jobs ? g_jobs->activeCount() : 0;
  const auto action = s_tracker.reconcile(inTurn, children, nowMs, turnEnded);
  // Fold the tracker's own frozen-children backstop into the aggregate metric.
  static uint32_t s_lastTrackerBackstops = 0;
  const uint32_t tb = s_tracker.backstopFires();
  if (tb != s_lastTrackerBackstops) { s_ringBackstopFires += tb - s_lastTrackerBackstops; s_lastTrackerBackstops = tb; }
  switch (action) {
    case nimbus::harness::HeadArcTracker::Action::Light:
      g_engine->setHeadArc(true);
      return true;
    case nimbus::harness::HeadArcTracker::Action::Clear:
      g_engine->setHeadArc(false);
      return true;
    case nimbus::harness::HeadArcTracker::Action::None:
    default:
      return false;
  }
}

void setAttnHoldMs(uint32_t ms) {   // fed from Param::AttnHoldMs
  g_attnHoldMs = ms;                // latched: applyConfig can fire before begin()
  if (g_jobs) g_jobs->setAttnHoldMs(ms);
}

static const char* stateStr(orch::JobState s) {
  switch (s) {
    case orch::JobState::Queued:     return "queued";
    case orch::JobState::Running:    return "running";
    case orch::JobState::NeedsInput: return "needs_input";
    case orch::JobState::Done:       return "done";
    case orch::JobState::Error:      return "error";
    case orch::JobState::Cancelled:  return "cancelled";
    default:                         return "unknown";
  }
}

String jobsSummary() {
  int n = g_journal.count();
  if (n == 0) return "No active jobs.";
  String s;
  for (int i = 0; i < n; i++) {
    orch::JobRecord rec;
    if (!g_journal.get(i, rec)) continue;
    s += String(rec.tag) + " (" + rec.backend + "/" + rec.model + ") " +
         rec.category + " [" + stateStr(rec.state) + "]\n";
  }
  return s;
}

String memorySnapshot() { return String(g_memEcho); }

String lastInstructions() {
  return g_engine ? String(g_engine->lastInstructions().c_str()) : String();
}

void requestConvReset() { g_convResetReq = true; }

// /clear: staged between-turns drop of the conversation context + scratchpad
// active task, keeping long-term memory and files. Safe from any task (a flag the
// turn/poll task drains); see drainStaged().
void requestConvClear() { g_convClearReq = true; }

void requestMemoryClear() {
  // Zero the echo immediately so the UI reflects the clear without waiting for
  // the drain; the authoritative g_mem.clear() runs on the turn task.
  g_memEcho[0] = 0;
  g_memClearReq = true;
}

void noteConfigChanged() { g_cfgReloadReq = true; }

String sessionsJson() {
  // AsyncTCP task: read the cross-task snapshot, NOT g_journal (single-writer on
  // tg_poll - a direct read here would race pump()). std::string fields are copied
  // into the doc, so the local `snap` staying alive through serializeJson is enough.
  auto snap = sessionInfosSnapshot();
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (const auto& s : snap) {
    JsonObject o = a.add<JsonObject>();
    o["tag"]      = s.id;
    o["backend"]  = s.provider;
    o["model"]    = s.model;
    o["category"] = s.title;
    o["state"]    = s.state;
  }
  String out; serializeJson(d, out); return out;
}

// tg_poll ONLY (the journal's writer task) - used by composeInputs on the turn.
// AsyncTCP/MCP callers must use sessionInfosSnapshot() instead.
std::vector<nimbus::orch::SessionInfo> sessionInfos() {
  return g_jobs ? g_jobs->sessionInfos() : std::vector<nimbus::orch::SessionInfo>{};
}

// Cross-task-safe read of the session snapshot (see refreshSessionSnapshot).
std::vector<nimbus::orch::SessionInfo> sessionInfosSnapshot() {
  std::vector<nimbus::orch::SessionInfo> out;
  if (g_sessMux) xSemaphoreTake(g_sessMux, portMAX_DELAY);
  out = g_sessSnapshot;
  if (g_sessMux) xSemaphoreGive(g_sessMux);
  return out;
}

bool sessionKnown(const std::string& id) {
  bool found = false;
  if (g_sessMux) xSemaphoreTake(g_sessMux, portMAX_DELAY);
  for (const auto& s : g_sessSnapshot)
    if (s.id == id) { found = true; break; }
  if (g_sessMux) xSemaphoreGive(g_sessMux);
  return found;
}

// ---- recent-results ring (results.get/list) ---------------------------------
// Own mutex (same pattern as g_sessMux): writers are tg_poll (clamp spill /
// fresh-result overflow), readers include the AsyncTCP /mcp task.
static nimbus::orch::ResultStore g_results;
static SemaphoreHandle_t g_resultsMux = xSemaphoreCreateMutex();

std::string resultsPut(const char* kind, const std::string& name, const std::string& fullText,
                       const std::string& jobTag, const std::string& ns) {
  if (g_resultsMux) xSemaphoreTake(g_resultsMux, portMAX_DELAY);
  std::string tag = g_results.put(kind, name, fullText, millis(), jobTag, ns);
  if (g_resultsMux) xSemaphoreGive(g_resultsMux);
  return tag;
}

bool resultsGet(const std::string& tag, size_t offset, size_t maxBytes, std::string& out,
                size_t& total, const nimbus::orch::Principal& who) {
  if (g_resultsMux) xSemaphoreTake(g_resultsMux, portMAX_DELAY);
  bool ok = g_results.get(tag, offset, maxBytes, out, total, who);
  if (g_resultsMux) xSemaphoreGive(g_resultsMux);
  return ok;
}

std::string resultsList(const nimbus::orch::Principal& who) {
  if (g_resultsMux) xSemaphoreTake(g_resultsMux, portMAX_DELAY);
  std::string o = g_results.list(who);
  if (g_resultsMux) xSemaphoreGive(g_resultsMux);
  return o;
}

void stageTerminate(const std::string& id) {
  portENTER_CRITICAL(&g_termSpin);
  g_termQueue.push_back(id);   // bounded (kAgentMaxJobs); drained on tg_poll
  portEXIT_CRITICAL(&g_termSpin);
}

// Phase 0 token seam: the real provider token usage of the most recently
// completed turn. Local Loops reads this right after injectScheduledTurn() to
// meter true billed spend against its per-loop + daily cost caps.
nimbus::orch::TokenUsage lastTurnUsage() {
  return g_engine ? g_engine->lastTurnUsage() : nimbus::orch::TokenUsage{};
}
nimbus::orch::TokenUsage sessionUsage() {
  return g_engine ? g_engine->sessionUsage() : nimbus::orch::TokenUsage{};
}
uint32_t turnCount() { return g_engine ? g_engine->turnCount() : 0; }

}  // namespace orchestrator
}  // namespace agent
