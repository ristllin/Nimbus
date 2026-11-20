#include "orchestrator.h"
#include "orchestrator_common.h"   // the half both device seam files share (RB-SEAM1a)
#include "agent_config.h"
#include "nimbus/harness/log.h"         // hlog - the portable harness log seam
#include "nimbus/harness/compose.h"     // portable World prompt composition (Stage D)
#include "nimbus/harness/apply.h"       // portable turn-application policy (Stage E)
#include "nimbus/harness/jobs.h"        // portable sub-agent job machinery (Stage F)
#include "nimbus/harness/engine.h"      // portable turn orchestration (Stage G)
#include "nimbus/harness/turn_memory.h" // L24: a turn keeps its small blocks in PSRAM
#include "nimbus/harness/turn_progress.h"  // lane PROG: the round-boundary signal plan
#include "../sys/heap_watch.h"          // heapwatch::psramFirst - the turn's malloc placement
#include "nimbus/harness/turn_inbox.h"  // histV2 switch (the turn-structured history)
#include "nimbus/harness/turn_anatomy.h" // the /api/lastturn page head (provider chain, device history)
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
#include "truth_device.h"   // RB-G0: the truth check's binding
#include "nimbus/harness/providers.h"   // providers::orchTurnCustom
#include "nimbus/harness/turn_contract.h"   // kNoMessageAfterAction
#include "loops_subsystem.h"            // loops::checkDue - Local Loops tick (on tg_poll)
#include "nimbus/orch/self_wakeup.h"    // WakeNote / armSelfWakeupNote - the owner design (9/11 Nov)
#include "dream_subsystem.h"            // DREAMING: noteTurnEnd (idle-gate quiet clock)
#include "degraded_device.h"            // lane DEGRADE: the served-by memory behind the degraded line
#include "connectors.h"                 // per-provider connector catalog + attach
#include "nimbus/orch/action_rules.h"   // who may run each tool and device action (CUM-813)
#include "model_refresh.h"   // R5: the orch_model miss answer
#include "provider_verify.h"            // queueWorkspaceProbeIfStale() at boot (CUM-537)
#include "nimbus/orch/connectors_wire.h" // portable attach*Wire (builtinsOnly cumulo route)
#include "files_subsystem.h"            // files::available - E1 capability manifest
#include "skills.h"                     // skills::spawnCapsule - per-spawn injection (P2)
#include "prefs_subsystem.h"            // standing rules: the block, /prefs, the delivery filter
#include "budget_command.h"             // lane BUD: /usage, /budget, /model (the spending commands)
#include "../sys/ota_update.h"          // RB-OTA2: /ota, the owner's update channel switch
#include "effect_ring_device.h"         // lane RB-FX: each chat's recent effects on the card
#include "memory_subsystem.h"           // memory::registry/scratchpad/vectors (live World)
#include "nimbus/orch/moderation.h"     // portable gate decision core (fail-open/closed, admin-exempt)
#include "nimbus/orch/media.h"          // CUM-40 validMusicName (for /play)
#include "../sfx/music.h"               // CUM-40 music player control (/play)
#include "adapters/provider_file_fetch.h"  // captureProviderFile - v4.1 code_interpreter file capture
#include "pdf_tool.h"                        // files.pdf - the device PDF writer
#include <solide/audio.h>               // reply.speak - play WAV on the device speaker (P6)
#include <LittleFS.h>
#include "fs_atomic.h"   // writeFileAtomic - the stop list (lane QW4)
#include <atomic>                       // voiceTurnSeq - hold-to-talk completion edge
#include <memory>                       // unique_ptr - heap Info[] (kMaxConnectors)
#include <new>       // std::nothrow - alloc failure degrades, never panics
#include <time.h>    // localtime_r - the self-wakeup note's local minute of day
#include "telegram.h"                   // telegram::enabled - capability manifest
#include "../sys/config_nvs.h"          // sys::deviceName - prompt identity (P2)
#include "../hw/hal_status.h"           // live HAL health -> capability manifest (P5)
#include "../hw/selftest.h"             // localTimeStr - temporal grounding (prompt + briefs)
#include "version.h"                    // NIMBUS_FW_VERSION -> prompt identity + cmd replies
#include "nimbus/fault.h"               // fault mask -> live capability manifest
#include <solide/storage.h>             // SD mount -> hw.sd
#include <solide/board.h>        // the build's board profile -> hw.ledCount (W10, CUM-514)
#include "nimbus/orch/world.h"          // SessionInfo / Hardware (composeInputs closure)
#include "nimbus/orch/router_route.h"   // CUM-369: shared router "<upstream>/<model>" resolution
#include "nimbus/orch/spend_ceiling.h"  // meterSeamUsage - a sub-agent's spend (lane DEF)
#include "nimbus/harness/router_refusal.h" // CUM-539/530: a router refusal, read and answered like the hosted head
#include "nimbus/orch/safety_activity.h"  // cumuloHostFromBase - the router host the refusal tap watches
#include "../sys/ps_json.h"              // PsramJsonAllocator - the refusal tap's filter copy stays off internal heap
#include "../sys/psram_static.h"         // long-lived buffers and the two engines in PSRAM, not internal .bss

#include "nimbus/orch/budget.h"         // headContextBudget - brief/fold-slice bytes from the head window
#include "nimbus/orch/request_budget.h" // requestBudgetFor - the fold's trigger and step (lane B)
#include "nimbus/harness/fold_driver.h" // FoldDriver - the one fold driver (lane F: never a permanent pause)
#include <map>
#include <mutex>
#include "nimbus/orch/caps.h"
#include "nimbus/orch/compact.h"        // modelCtxTokens (window table)
#include "nimbus/orch/owner_first.h"    // OwnerFirstGate - background work waits for the owner
#include "nimbus/orch/servedby.h"       // CUM-236 turn-chip served-by disclosure
#include "nimbus/orch/served_model.h"   // the model each provider really answered with
#include "nimbus/orch/head_switch.h"    // HeadPause - what holds a head off
#include "nimbus/orch/provider_park.h"  // the park the head pause reads
#include "nimbus/orch/failure_rows.h"   // turnEndRowJson / errorEventText - lane T rows
#include "nimbus/orch/trace_rows.h"    // the tool trace row + the lost-sidecar mark (L19-24)
#include "nimbus/orch/command.h"
#include "nimbus/orch/memory.h"
#include "nimbus/orch/journal.h"
#include "nimbus/store/persist.h"        // kStorageFullMessage - an /mcp approval that did not store
#include "nimbus/store/verdict_memo.h"   // vfy_ written only when the verdict changes
#include "nimbus/orch/turn.h"
#include "nimbus/orch/device_actions.h"
#include "nimbus/mem_cap.h"
#include "nimbus/harness/dream.h"   // buildConversationWindow (RECENT CONVERSATION) + buildEpisodicDigest
#include "nimbus/harness/conversation.h"   // the /clear boundary + live conversation window (CUM-511)
#include "../sfx/sound_fx.h"      // SFX hooks: spawn ack / turn start / reply / mem save
#include "../sys/crash_report.h"  // crash UX: the in-flight turn journal
#include "../sys/poll_watch.h"    // lane TGSTALL: turn, round and tool beats of the poll task
#include "nimbus/sys/log_window.h"   // forensic window: routine headloop lines stay in RAM

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
// It is the old engine's seam (lane RB-SEAM1b): it defines the calls of engine_api.h and is
// the only device file that includes nimbus/harness/engine.h; the engine-neutral half lives
// in orchestrator_common.cpp.

namespace agent {
namespace orchestrator {

namespace orch = nimbus::orch;
namespace attn = nimbus::attn;

// ---- Module state -----------------------------------------------------------
static constexpr const char* kStopListPath = "/data/jobs_stopping.txt";   // lane QW4: the stop list
// The portable turn orchestration (Stage G lift) - constructed in begin().
static TurnEngine*  g_engine = nullptr;
// The owner's message of the turn in flight (empty for a turn the owner did not write):
// the one thing that may confirm moving the head away from a paused provider (head_switch.h).
// Written on the turn task (handleMessage); read by the apply under it.
static nimbus::orch::OwnerMessage g_turnOwner;   // empty = no owner message in flight
static uint32_t     g_ownerMsgs = 0;              // owner messages numbered since boot
static std::mutex   g_turnOwnerMx;                // device.control may also run on the /mcp task
// The harness's read-mostly config view over NVS (store_config.cpp).
static HarnessConfig g_cfg;

static orch::OrchMemory      g_mem;
static LittleFsMemoryStore   g_memStore;
// v3.6.0 fold state (plans/compaction-plan.md). All accessed on tg_poll ONLY
// (capture hooks, onTurnEnd, pollJobs, handleMessage) - no lock needed.
static LittleFsFoldStoreIO      g_foldIO;
static nimbus::orch::FoldStore  g_folds;
// An auto fold and the staged system turn wait for the owner (owner_first.h): onTurnEnd
// and pollJobs feed it, compactTick and drainUpdateNotice ask it, tg_poll only.
static nimbus::orch::OwnerFirstGate g_ownerFirst;
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
String withTurnTag(const char* base) {
  String t(base);
  if (g_turnTag[0]) { if (t.length()) t += ","; t += g_turnTag; }
  return t;
}
// Staged manual requests (/compact, Summarize now, the console): a few slots, so two
// requests between pump passes both reach the driver's persisted queue.
static constexpr int kFoldStageSlots = 4;
static char g_manualFoldChat[kFoldStageSlots][32] = {};
static portMUX_TYPE g_foldSpin = portMUX_INITIALIZER_UNLOCKED;   // console (main) stages too
static volatile bool g_foldResetStaged = false;   // NIMBUS_TEST FOLDRESET (console task)
// Lane F: the one fold driver (fold_driver.h), tg_poll only; built in begin().
static agent::FoldDriver* g_foldDriver = nullptr;
// The web task's copy of the fold status (GET /api/fold/status): written on tg_poll
// whenever the driver's state moves, read under the mutex by the route. A std::mutex,
// never the spinlock: the copy holds strings.
static std::mutex g_foldStatusMx;
static std::map<std::string, agent::FoldStatus> g_foldStatusSnap;   // under g_foldStatusMx
static std::string g_foldDefaultChat;                               // under g_foldStatusMx
// Cross-task fold-state ECHO (prism v3.6.0): foldStatusText runs on AsyncTCP /
// the console task, but FoldStore is tg_poll-owned and its ChatFold carries a
// heap std::string - copying it while applyFold reassigns the summary (or
// upsert moves the slot vector) is a use-after-free, not a skewed counter. Only
// tg_poll writes this fixed buffer; readers copy it under the spinlock. Same
// pattern as g_memEcho.
static char (&g_foldEcho)[320] = ::nimbus::psramStatic<char[320]>();   // PSRAM (psram_static.h)
static void publishFoldStatus(const std::string& chat);   // fwd (defined below)
static void publishFoldAll(const std::string& chat);      // fwd: the echo and the web status
static void publishFoldWebStatus();                        // fwd: the web status copy
static agent::FoldDriverHooks foldHooks();                 // fwd: the fold driver over this device

// Cross-task model-memory echo for the web UI. g_mem's std::string may be
// reassigned mid-turn on THIS (poll) task; letting the AsyncTCP task copy it
// directly risks a dangling read across a realloc. So the turn path mirrors it
// into this fixed buffer (strlcpy, always NUL-terminated) and the web accessor
// reads the buffer: worst case a torn *text*, never a torn pointer.
// Lives in PSRAM (psram_static.h): about 1.2 KB that internal RAM no longer holds.
static char (&g_memEcho)[nimbus::orch::kMemModelMax + 1] =
    ::nimbus::psramStatic<char[nimbus::orch::kMemModelMax + 1]>();
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
static volatile bool g_convClearReq = false;   // web "Clear conversation": clear the web chat (CUM-511)
static void clearConversation(const String& chatId);   // fwd (defined below)
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
    // The web app's "Clear conversation" (staged from AsyncTCP): the web chat's
    // /clear. Same between-turns discipline as the conv reset above so an
    // in-flight turn's convId write-back is already landed.
    clearConversation("web");
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

// The ONE head-window budget for every per-turn size that follows the head model
// (the RECENT CONVERSATION window, the sub-agent brief, the fold slice): sized for
// the head the ENGINE will run (headHost: the router fallback included), never a
// store-side guess - store::resolvedOrchHost does not know the router fallback,
// and on a Z.ai-only device it sized a 272K window for a 100K model (CUM-485).
// Before the engine exists (boot), the store's resolution is the best answer.
// test_orch_budget pins that this is the only headContextBudget call in this file.
static nimbus::orch::ContextBudget headBudget() {
  const String h = g_engine ? String(g_engine->headHost().c_str()) : store::resolvedOrchHost();
  return nimbus::orch::headContextBudget(h.c_str(), store::orchModel(h).c_str(), store::orchToolLoop(), {});
}

// The head's request context (lane B: deriveRequestContext over the head's turn budget,
// lane L), which the fold reads directly (lane F): its trigger, one fold's step. On the
// floor branch (a head with no room, or one whose turn goes out flat: the tool loop off on
// Mistral or Z.ai) it is v5.1.1's (the 48 KB compactAtKB trigger, the v5.1.1 slice).
static nimbus::orch::ContextBudget headRequestContext() {
  const String h = g_engine ? String(g_engine->headHost().c_str()) : String();   // the fold runs with an engine
  return nimbus::orch::deriveRequestContext(
      nimbus::orch::turnBudgetFor(h.c_str(), store::orchModel(h).c_str(), store::orchToolLoop()), {});
}
// The automatic fold's trigger: the owner's Compact threshold over the head's budget. One
// figure for the fold driver (when a chat is due) and the engine's fold slice (the tail it
// keeps fits it).
static uint32_t foldTriggerNow() {
  return (uint32_t)nimbus::orch::foldTriggerBytes(headRequestContext(), store::compactAtKB());
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

// CUM-486: the ONE model-choice derivation (nimbus::orch::effectiveModelChoices:
// the model store's short list first - every verify fetches the provider's list,
// keeping current-generation ids selectable without a firmware release - else the
// cold-start list). The prompt's [AVAILABLE MODELS] line and the spawn fallback
// (d.modelChoices below) read it; it is a subset of what modelIsValid accepts, so the
// head is never offered a model that validation then coerces away.
static std::string modelChoicesFor(const std::string& p) {
  return nimbus::orch::effectiveModelChoices(p.c_str(),
                                             std::string(store::modelChoices(String(p.c_str())).c_str()));
}

// Lane F: orch_model and spawns ask THE check (store::modelSelectable over the model
// store, the cold-start list and the compiled default) - the same one the web pick
// asks, so a model the picker or the prompt offers is never refused here.
static bool modelIsValid(const String& provider, const char* model) {
  return model && store::modelSelectable(provider, String(model), nimbus::orch::PickRole::Any);
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
  // The head (provider chain, the history the device sent, the section legend,
  // and - lane T - a failed turn's causes, first cause first) is portable and
  // host-tested (turn_anatomy.h), shared with the hosted daemon.
  const std::string head = agent::turnAnatomyHeader(ev);
  const size_t need =
      head.size() + instructions.size() + inputs.size() + outJson.size() + brief.size() + 1024;
  char* b = (char*)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
  if (!b) return;   // no PSRAM -> skip capture, never the turn
  size_t off = 0;
  auto app = [&](const char* s, size_t n) {
    if (off + n >= need - 1) n = need - 1 - off;
    memcpy(b + off, s, n);
    off += n;
  };
  auto appStr = [&](const std::string& s) { app(s.c_str(), s.size()); };
  auto appLit = [&](const char* s) { app(s, strlen(s)); };
  appStr(head);
  appStr(instructions);
  appLit("\n\n===== 2. INPUT - the per-turn message block =====\n"
         "(optional [MEMORY RESULTS] / [FRESH RESULTS] + live session digest +\n"
         "[CHANNEL] + [USER] - the current message; earlier conversation reaches\n"
         "the model only through the device history in section 1, or the provider\n"
         "chain when the header says CONTINUED)\n\n");
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

// ---- the last failed turn (device.status.lastFail, lane T) --------------------
// RAM only, reset at boot: the turn id, its source, when, and the first cause first -
// so the head can answer "why did that fail" from the tool it already calls first.
// Written on the turn task, read by device.status on the turn task or over /mcp.
static std::mutex  g_lastFailMx;
static std::string g_lastFailTurn, g_lastFailSrc, g_lastFailCause;
static uint32_t    g_lastFailMs = 0;

static void noteLastFail(const char* src, const TurnEndEv& ev) {
  std::string cause = ev.chain && !ev.chain->empty()
                          ? ev.chain->render(nimbus::orch::FailureChain::kTextMax)
                          : nimbus::orch::sanitizeFailText(ev.error.data(), ev.error.size(),
                                                           nimbus::orch::FailureChain::kTextMax);
  std::lock_guard<std::mutex> lk(g_lastFailMx);
  g_lastFailTurn = g_turnTag[0] ? std::string(g_turnTag + 5) : std::string();   // skip "turn:"
  g_lastFailSrc = src ? src : "";
  g_lastFailCause = std::move(cause);
  g_lastFailMs = millis();
}

String lastFailJson() {
  std::lock_guard<std::mutex> lk(g_lastFailMx);
  if (!g_lastFailMs && g_lastFailCause.empty()) return "null";
  JsonDocument d;
  d["turn"] = g_lastFailTurn;
  d["src"] = g_lastFailSrc;
  d["agoS"] = (uint32_t)((millis() - g_lastFailMs) / 1000u);
  d["cause"] = g_lastFailCause;
  String out;
  serializeJson(d, out);
  return out;
}

// The ev:error row of a failed turn, tagged with the turn's id so the "system" row
// joins the turn's own rows (before the clock syncs it is held untagged by
// captureEvent, which stamps it at first sync).
static void captureTurnError(const String& text) {
  if (!g_turnTag[0] || !memory::clockSynced()) {
    memory::captureEvent("error", text);
    return;
  }
  memory::flushPendingEvents();   // older held rows first, preserving order
  memory::captureSession("system", "device", "Device timeline");
  memory::captureMessage("system", "system", orch::MsgKind::Log, text, "",
                         String("ev:error,") + g_turnTag);
}

// A context overflow anywhere in the chain (the first host may have overflowed and a
// later one failed for another reason): the next fold must still run.
static bool anyContextOverflow(const TurnEndEv& ev) {
  if (nimbus::orch::isContextOverflowError(ev.error)) return true;
  if (!ev.chain) return false;
  for (size_t i = 0; i < ev.chain->size(); ++i)
    if (nimbus::orch::isContextOverflowError(ev.chain->at(i).text)) return true;
  return false;
}

// ---- the JobEngine wiring (spawn/dispatch/poll logic lives in lib/harness) --

// Execution-only closures for the portable job machinery - every decision
// (dispatch gates, backoff, refusal strings, synthesis clock) lives in
// lib/harness jobs.cpp and is host-tested (test_harness_jobs).
// The whole principal for a chat (namespace, role and quota) from the tenant table:
// the one answer to "who is this chat" for the turn (ApplyDeps) and for the
// sub-agents it starts (JobEngine), so the two can never disagree (CUM-828).
static nimbus::orch::Principal chatPrincipal(const std::string& chatId) {
  nimbus::orch::Quota q;
  tenantQuotaOf(chatId, q);
  return nimbus::orch::principalForRole(chatId, roleOfChat(String(chatId.c_str())), q);
}

static JobEngine::Deps buildJobDeps() {
  JobEngine::Deps d;
  d.principalFor = chatPrincipal;   // a sub-agent inherits its chat's principal (CUM-828/830)
  d.platform.nowMs    = [] { return (uint32_t)millis(); };
  d.platform.freeHeap = [] { return (uint32_t)ESP.getFreeHeap(); };
  d.fabric  = g_fabric;      // set by begin() before the engine is constructed
  d.journal = &g_journal;
  // CUM-482: every job-engine message (spawn acks, refusals, sub failures, raw
  // fallbacks) also lands in the chat's episodic history, so the web chat - which
  // renders that history - shows it and it survives a reload. Untagged by turn
  // when it arrives between turns (tg_poll pump), so it can never be grouped
  // under, or returned as, a later turn's reply. kJobDeliveryTag marks
  // provenance: these rows render in the chat but never enter a model-facing
  // conversation read (window, brief, fold) - a raw fallback carries sub-agent
  // web content (prism injection + input lenses). Role "tool", never "assistant".
  d.deliver = agent::capturingDelivery(
      [](const std::string& chatId, const std::string& text) {
        return deliver(String(chatId.c_str()), String(text.c_str()));
      },
      [](const std::string& chatId, const std::string& text) {
        memory::captureMessage(chatId.c_str(), "tool", orch::MsgKind::Message,
                               String(text.c_str()), "", withTurnTag(agent::kJobDeliveryTag));
        if (chatId != "system") g_folds.noteMessage(chatId, text.size());   // fold trigger
      });
  d.event = [](const attn::Event& e) { if (g_sinks.event) g_sinks.event(e); };
  d.fire = [](const char* cue) {
    if (strcmp(cue, "spawn") == 0) ::sfx::fire(nimbus::sfx::Ev::AgentSpawn);   // "Right away, sir."
  };
  d.backendHue = [](const char* b) { return backendHue(b); };
  d.subPriority = [] { return std::string(store::subPriority().c_str()); };
  d.providerHasKey = [](const std::string& p) { return providerHasKey(String(p.c_str())); };
  d.overBudget = [](const std::string& p) { return store::providerOverBudget(String(p.c_str())); };
  d.subModel = [](const std::string& p) {
    return std::string(store::subModel(String(p.c_str())).c_str());
  };
  d.modelIsValid = [](const std::string& p, const std::string& m) {
    return modelIsValid(String(p.c_str()), m.c_str());
  };
  // Lane L (validated coercion): the same list, so an unlisted spawn model falls
  // back to a listed one, never to an unchecked default.
  d.modelChoices = [](const std::string& p) { return modelChoicesFor(p); };
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
  d.nowEpoch = [] {   // the finished-job record (L19-35); 0 before the clock syncs
    const time_t t = time(nullptr);
    return t > 1600000000 ? (uint32_t)t : 0u;
  };
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
    q.limit = 12;   // 6 shown; headroom for the skipped job-delivery rows
    std::vector<EpisodicMessage> rows;
    { memory::Lock lk; rows = memory::episodic().query(q); }
    rows = agent::liveConversation(rows, std::string(), 6);   // not past a /clear, no job rows
    if (rows.empty()) return std::string();
    std::string block = "[CONTEXT] (the conversation this task came from, oldest first)\n";
    // Brief bytes derive from the HEAD's window (the spawning context; 1200 at
    // the 200K anchor). Per-sub-model derivation deferred - this closure has no
    // provider argument today.
    block += dream::buildEpisodicDigest(rows, headBudget().briefBytes);
    return block;
  };
  // A file task runs where its file can come back: the device's PDF route.
  d.fileSpawnProvider = [] {
    return std::string(nimbus::orch::pdfRouteProvider(connectors::pdfRoute()));
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
      if (match) return std::string(cs[i].prov.c_str());   // "any": no routing, never the router
    }
    return std::string();
  };
  // Sub-session spend attribution: real usage from a terminal poll (OpenAI
  // Responses reports it) files under "spawn:<backend>" in the usage ledger, and
  // (per-model prices, 2026-10) under the job's model: "default" is the model the
  // adapter actually ran, store::subModel(backend).
  // Lane DEF: through the seam meter, so the monthly spend ceiling counts it with every
  // other sub-agent's (the sink files it under "spawn:<backend>" as before).
  d.recordSpawnUsage = [](const std::string& backend, const std::string& model,
                          const nimbus::orch::TokenUsage& u) {
    const String be(backend.c_str());
    const String resolved = (model.empty() || model == "default") ? store::subModel(be)
                                                                  : String(model.c_str());
    nimbus::orch::meterSeamUsage(nimbus::orch::SpendSeam::Sub, backend.c_str(),
                                 resolved.c_str(), u);
  };
  // Lane QW4 round 3: the background stop list (helpers whose stop is not yet confirmed)
  // in flash, so a restart keeps asking them to stop and counts them.
  d.loadStopping = [] {
    std::string out;
    File f = LittleFS.open(kStopListPath, "r");
    if (!f) return out;
    while (f.available() && out.size() < 8192) out += (char)f.read();
    f.close();
    return out;
  };
  d.saveStopping = [](const std::string& text) {
    LittleFS.mkdir("/data");
    return agent::writeFileAtomic(kStopListPath, text, "jobs_stopping", false);
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

// What holds paid requests to `p` off now on `seam` (nimbus::orch::HeadPause): the pause switch
// (the owner, the overall ceiling, the provider's monthly limit), a refusal park covering that
// seam (parkPause -> headPauseIn, host-tested), the monthly budget check. The park's re-check
// request is never taken; the budget check is the one every turn makes at its start. Chat for
// the head; Spawn for the catalog (providerPause).
static nimbus::orch::HeadPause headPauseFor(const std::string& p,
                                            nimbus::orch::PaidSeam seam = nimbus::orch::PaidSeam::Chat,
                                            bool dueIsOpen = false) {
  nimbus::orch::HeadPause hp = nimbus::orch::parkPause(p, seam, millis(), dueIsOpen);
  hp.overBudget = store::providerOverBudget(String(p.c_str()));
  return hp;
}

// The prompt's reads: a park whose re-check is due is open (the turn that makes the
// re-check never reads its provider as paused).
nimbus::orch::HeadPause providerPause(const std::string& p, nimbus::orch::PaidSeam seam) {
  return headPauseFor(p, seam, /*dueIsOpen=*/true);
}

static agent::ApplyDeps buildApplyDeps() {
  agent::ApplyDeps d;
  // Execution-only closures - every POLICY decision (scheduled-turn refusals,
  // risk notes, tts gate, orch_model validation order) lives in the portable
  // harness apply.cpp and is host-tested (test_harness_apply).
  d.execConfig = [](const orch::ValidatedAction& va) -> bool {
    // Returns whether a stored value CHANGED (before != after), so a no-op
    // action reports "no change" and never backs a "switched" reply (23 Oct).
    bool changed = false;
    if (va.hasLedBrightness) changed = store::setLedBright(va.ledBrightness) || changed;
    // `priority` tunes the SUB-SESSION provider preference only (never the
    // orchestrator-host list - see the protected-key rail).
    if (va.hasPriority)  changed = store::setSubPriority(String(va.priority.c_str())) || changed;
    if (va.hasTtsVoice)  changed = store::setTtsVoice(String(va.ttsVoice.c_str())) || changed;
    if (va.hasTtsProv)   changed = store::setTtsProvider(String(va.ttsProv.c_str())) || changed;
    if (va.hasTtsOn)     changed = store::setTtsEnabled(va.ttsOn) || changed;
    if (va.hasSleepOvr)  changed = store::setSleepOvr(va.sleepOvr) || changed;
    if (va.hasBrightOvr) changed = store::setBrightOvr(va.brightOvr) || changed;
    if (va.hasSttProv)   changed = store::setSttProvider(String(va.sttProv.c_str())) || changed;
    if (va.hasSfxLvlN || va.hasSfxLvlO || va.hasSfxVol || va.hasSfxTheme) {
      if (va.hasSfxLvlN)  changed = store::setSfxLevelNotif(va.sfxLvlN) || changed;
      if (va.hasSfxLvlO)  changed = store::setSfxLevelOrch(va.sfxLvlO) || changed;
      if (va.hasSfxVol)   changed = store::setSfxVolume(va.sfxVol) || changed;
      if (va.hasSfxTheme) changed = store::setSfxTheme(String(va.sfxTheme.c_str())) || changed;
      ::sfx::refreshConfig();   // live-apply, same seam the web setters use
    }
    if (va.hasDevName) {
      const String before = nimbus::sys::deviceName();
      changed = nimbus::sys::saveDeviceName(String(va.devName.c_str())) != before || changed;
    }
    return changed;
  };
  if (g_sinks.device)
    d.stageDevice = [](const orch::ValidatedAction& va) { g_sinks.device(va); };
  if (g_sinks.speak)
    d.speak = [](const std::string& chat, const std::string& s) {
      return g_sinks.speak(String(chat.c_str()), String(s.c_str()));
    };
  d.speakerVolume = [] { return (int)store::sfxVolume(); };   // live: a tts at volume 0 plays silence
  d.modelIsValid = [](const std::string& p, const std::string& m) {
    return modelIsValid(String(p.c_str()), m.c_str());
  };
  // R5 (lane H): a miss queues one list refresh and answers with the cache date.
  d.modelMiss = [](const std::string& p, const std::string& m) { return agent::model_refresh::missText(p, m); };
  d.providerHasKey = [](const std::string& p) { return providerHasKey(String(p.c_str())); };
  d.setOrchHostModel = [](const std::string& p, const std::string& m) {
    store::setOrchHost(String(p.c_str()));
    store::setOrchModel(String(p.c_str()), String(m.c_str()));
  };
  // A pause never moves the owner's head (the policy is apply.cpp's): the facts.
  d.headHost = [] { return g_engine ? g_engine->headHost() : std::string(store::orchHost().c_str()); };
  d.headPause = [](const std::string& p) { return headPauseFor(p); };
  d.ownerMessage = [] {
    std::lock_guard<std::mutex> lk(g_turnOwnerMx);
    return g_turnOwner;
  };
  d.nowMs = [] { return (uint32_t)millis(); };
  d.setModelMemory = [](const std::string& chatId, const std::string& v) {
    return g_mem.setModel(chatId, v);   // per-chat running memory (B3)
  };
  d.syncMemEcho = [] { syncMemEcho(); };
  d.applyScratch = [](const nimbus::orch::ScratchUpdate& u) -> agent::ScratchOutcome {
    // Inline scratchpad update: replace only the tiers the model
    // returned, under the memory Lock, then persist. A non-null tier REPLACES it
    // (the model returns the full desired tier each time - same model as running
    // memory), so clearing = an empty array, unchanged = a null field.
    using nimbus::orch::Tier;
    agent::memory::Lock memg;
    auto& sp = memory::scratchpad();
    const std::string before = sp.serialize();
    if (u.hasActive) sp.setActiveTask(u.active);
    if (u.hasShort)  sp.replace(Tier::Short, u.shortItems);
    if (u.hasMid)    sp.replace(Tier::Mid,   u.midItems);
    if (u.hasLong)   sp.replace(Tier::Long,  u.longItems);
    // An update that changes nothing writes nothing and backs no claim.
    if (sp.serialize() == before) return agent::ScratchOutcome::Skipped;
    return memory::persistScratchpad() ? agent::ScratchOutcome::Applied
                                       : agent::ScratchOutcome::AppliedRamOnly;
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
  d.principalFor = chatPrincipal;
  d.persistMemory = [] { memory::persistVectors(); memory::persistScratchpad(); };
  d.enqueueSpawn = [](const orch::Spawn& s, const std::string& chatId, bool quiet) {
    return g_jobs && g_jobs->enqueueSpawn(s, chatId, quiet);
  };
  d.cancelSession = [](const std::string& id) { return cancel(id.c_str()); };
  d.sessionStopping = [](const std::string& id) { return g_jobs && g_jobs->stopping(id.c_str()); };
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
    // A loop's own turn is unattended and now delivers to the web chat too (CUM-505):
    // nobody is polling for it, so it stays silent there as well (no 03:30 "Done.").
    if (loops::firing()) return;
    if (chatId == "web" || chatId == "voice" || chatId == "serial")
      deliver(String(chatId.c_str()), String(kNoMessageAfterAction));   // a background turn only
  };
  d.emitAsk = [] { emitAsk(); };
  d.captureAssistant = [](const std::string& chatId, const std::string& text) {
    // The row carries what the turn actually did ("fx:mem,file" or "fx:none",
    // at most 24 B on the card), so a later turn can tell a promise from a
    // deliverable (rendered in the history by a later lane).
    // histV2 adds the row's out: kind (reply, ask or tts) after the fx: tag.
    const std::string fx = g_engine ? g_engine->assistantRowTags() : std::string();
    memory::captureMessage(chatId.c_str(), "assistant", orch::MsgKind::Message,
                           String(text.c_str()), "", withTurnTag(fx.c_str()));
    if (chatId != "system") g_folds.noteMessage(chatId, text.size());   // fold trigger
  };
  d.fire = [](const char* cue) {
    if (strcmp(cue, "reply") == 0)    ::sfx::fire(nimbus::sfx::Ev::ReplySent);
    if (strcmp(cue, "memsaved") == 0) ::sfx::fire(nimbus::sfx::Ev::MemSaved);
  };
  return d;
}

// CUM-369: the Cumulo head's picked selector, read fresh every turn (the selected
// model can change via NVS after boot) and split by the SAME rule the sub-session
// CumuloAdapter uses (resolveRouterRoute), so head and sub-session address the
// router identically: base /router/<upstream>/v1 and the bare model the router
// prices. A bare id (no '/') keeps the openai upstream. store::orchModel already
// defaults an empty selector to CUMULO_MODEL; the belt-and-suspenders default
// keeps it non-empty. A fold rung pinned on another router model (a routed forced
// reasoner folds on Small) wins: the pin is the fold task's own, so a turn never sees
// it. noinline (lane STACK): the lookups stay out of cumuloTurn's frame on tg_poll.
__attribute__((noinline)) static std::string cumuloPick() {
  const std::string pinned = providers::modelOverrideFor("cumulo");
  if (!pinned.empty()) return pinned;
  String m = store::orchModel("cumulo");
  return std::string((m.length() ? m : String(CUMULO_MODEL)).c_str());
}

__attribute__((noinline)) static bool cumuloFoldPinned() { return !providers::modelOverrideFor("cumulo").empty(); }

// CUM-539 on the device (bench CUM-530: a router id the upstream refused read as
// "cumulo is unavailable" and the bad pick stayed): the cumulo head reads WHY the
// router refused a request and answers a refused model on one that works, by the
// rule the hosted head runs (nimbus/harness/router_refusal.h). The tap is static
// (no per-turn object), and the host it watches and the selector that last
// answered (RAM only, like the hosted head's) are fixed buffers: nothing on the
// turn path grows the heap (the tap's filter copy is transient and in PSRAM).
static char g_cumuloRouterHost[64] = "";   // the host the tap watches (set per turn)
static char g_cumuloLastGood[72] = "";     // a router id is at most 64 characters (cleanModelId)
// Copy `s` whole into `dst`, or clear it when it does not fit: a cut host or
// selector would name a different one.
template <size_t N>
static void copyWholeOrClear(char (&dst)[N], const std::string& s) {
  if (s.size() < N) strlcpy(dst, s.c_str(), N);
  else dst[0] = '\0';
}
static agent::router_refusal::RouterTap& cumuloTap() {
  static agent::router_refusal::RouterTap tap(nullptr, g_cumuloRouterHost, &PsramJsonAllocator::instance());
  static const bool kClockSet = [] {   // the fail_over cooling table's clock
    tap.setClock([] { return (uint32_t)millis(); });
    return true;
  }();
  (void)kClockSet;
  return tap;
}

// CUM-242 x1 item 2: the connector list a CUMULO-routed head advertises. It runs
// under the shared router org key, so the owner's PRIVATE connectors (remote MCP,
// first-party) must NOT ride the request - only the generic provider built-ins.
// The attach builders filter with builtinsOnly=true; this rebuilds the portable
// list from the blob so an enabled built-in still attaches over the router, minus
// code_interpreter (below). No secrets (built-ins authenticate provider-side, so
// no bearer is resolved).
static std::vector<nimbus::orch::ConnectorInfo> cumuloRouteConnectors() {
  std::vector<nimbus::orch::ConnectorInfo> cs;
  nimbus::orch::parseConnectorsJson(store::connectorsJson().c_str(), cs,
                                    connectors::kMaxConnectors, nullptr);
  // No code_interpreter over the router (wf_plan 3, Q8): a file it built could not
  // come back (no router capture path yet), so it would only cost container time.
  nimbus::orch::applyCodeSandbox(cs, false);
  return cs;
}

// One cumulo head call's arguments (the ProviderHosts signature), passed as one.
struct CumuloCall {
  std::string& cv;
  const std::string& ins;
  const std::string& inp;
  std::string& out;
  std::string& err;
  const agent::HeadTools* tools;
  nimbus::orch::TokenUsage* usage;
};

// The bare host the router requests go to (wire::applyRouter's rule: no scheme,
// path or port), so the refusal tap watches exactly those ("" when it does not
// fit: then every request counts, and every request of this head is a router one).
static void setCumuloRouterHost() {
  const std::string h = nimbus::orch::cumuloHostFromBase(std::string(store::cumuloBase().c_str()),
                                                         CUMULO_HOST_DEFAULT);
  copyWholeOrClear(g_cumuloRouterHost, h.substr(0, h.find(':')));
}

// One cumulo head attempt on router selector `sel` (CUM-242 tool loop, built ON
// CUM-369's shared route resolution): a Cumulo head runs the SELECTED upstream's
// NATIVE head (its real mid-turn tool loop for anthropic/openai/mistral) over the
// router, instead of the old single-shot chat-completions that ignored `tools`.
// The "<upstream>/<model>" selector is split by the SAME resolveRouterRoute the
// sub-session uses, so head and sub-session never drift. Then a router-override
// pd (base = cumuloBase, key = cumuloKey, orchModel = the bare model) dispatches
// to that upstream's orchTurn*, which picks the tool loop vs a single shot from
// `tools` + the loop toggle; wire::applyRouter reroutes every request through
// /router/<upstream>, and every request passes the refusal tap. zai runs its own tool
// loop, and any name the native fabric does not drive stays the single-shot
// chat-completions path (both orchTurnCustom over /router/<upstream>).
static bool cumuloAttempt(const std::string& sel, CumuloCall& c) {
  const nimbus::orch::RouterRoute rr = nimbus::orch::resolveRouterRoute(sel);
  const std::string upstream = rr.upstream;
  const std::string model = rr.model;
  // On the heap (lane STACK): ProviderDeps is 416 B of hooks, and this runs inside a turn
  // on the Telegram poll task. Built in place from deviceProviderDeps().
  const std::unique_ptr<providers::ProviderDeps> deps(new providers::ProviderDeps(deviceProviderDeps()));
  providers::ProviderDeps& pd = *deps;
  cumuloTap().setInner(pd.http);
  pd.http = &cumuloTap();
  pd.routerBase = [] { String b = store::cumuloBase();
                       return std::string((b.length() ? b : String(CUMULO_HOST_DEFAULT)).c_str()); };
  pd.routerKey  = [] { return std::string(store::cumuloKey().c_str()); };
  // The upstream loop steps read pd.key(<upstream>) / pd.orchModel(<upstream>);
  // over the router BOTH resolve to the router credential + the selected model
  // regardless of the host arg (applyRouter then swaps in Bearer cumuloKey).
  pd.key       = [](const char*) { return std::string(store::cumuloKey().c_str()); };
  pd.orchModel = [model](const char*) { return model; };
  // Per-route tool advertisement (item 2): a cumulo head advertises provider
  // BUILT-IN tools only - the account's private connectors stay off the shared
  // router key. attach*Wire with builtinsOnly=true does the filtering.
  pd.attachOpenAI = [](JsonDocument& doc) {
    nimbus::orch::attachOpenAIWire(doc, cumuloRouteConnectors(), nullptr, /*builtinsOnly=*/true);
  };
  pd.attachMistral = [](JsonDocument& doc) {
    nimbus::orch::attachMistralWire(doc, cumuloRouteConnectors(), /*builtinsOnly=*/true);
  };
  pd.attachAnthropic = [](JsonDocument& doc) {
    nimbus::orch::attachAnthropicWire(doc, cumuloRouteConnectors(), nullptr, /*builtinsOnly=*/true);
  };
  if (upstream == "anthropic")
    return providers::orchTurnAnthropic(pd, c.cv, c.ins, c.inp, c.out, c.err, c.tools, c.usage);
  if (upstream == "mistral") {
    // The router admits only POST /v1/conversations (a new conversation), never an
    // append to /v1/conversations/<id>: start fresh every turn, as the hosted head
    // does. The prompt already carries the recent conversation.
    c.cv.clear();
    return providers::orchTurnMistral(pd, c.cv, c.ins, c.inp, c.out, c.err, c.tools, c.usage);
  }
  if (upstream == "openai")
    return providers::orchTurnOpenAI(pd, c.cv, c.ins, c.inp, c.out, c.err, c.tools, c.usage);
  // zai / unknown upstream: single-shot chat-completions over /router/<upstream>
  // (NOT rr.basePath - the zai upstream path carries no /v1, unlike the fabric
  // upstreams; custRequest maps "/v1/chat/completions" to prefix + "/chat/...").
  // json_object for every such upstream (GLM behind the router ignores
  // json_schema exactly like direct Z.ai, CUM-495), and for zai the same bounded
  // reasoning effort + 50 s budget as the direct Z.ai head (CUM-544) - one
  // portable helper, host-tested, so the two Z.ai paths cannot drift.
  providers::configureRouterCompatHead(pd, upstream, model);
  return providers::orchTurnCustom(pd, c.cv, c.ins, c.inp, c.out, c.err, c.tools, c.usage);
}

// The cumulo head turn with the hosted head's refusal ladder (CUM-539): a model
// the router does not offer (model_not_priced, a 404, or the upstream's own 400
// "Invalid model") is answered in the same turn on the last selector that worked,
// else on the router default, with a first line naming the refused model and the
// fix, and the pick moves to the model that answered; a paused upstream or a
// route the router does not allow answers on the default and keeps the pick. A
// refusal no model can answer fails with its own cause (never "unavailable").
// Only a refusal of an attempt's FIRST request is answered again, so a tool that
// already ran is never repeated. The fold (a background compaction through this
// same head) gets the answer only: no notice, and the pick never moves.
static bool cumuloTurn(CumuloCall& c) {
  namespace rf = agent::router_refusal;
  rf::RouterTap& tap = cumuloTap();
  setCumuloRouterHost();
  const std::string picked = cumuloPick();
  auto attempt = [&c](const std::string& sel, bool fresh) {
    if (fresh) {   // a fresh conversation and a clean output for another model
      c.cv.clear();
      c.out.clear();
      c.err.clear();
    }
    return cumuloAttempt(sel, c);
  };
  const rf::LadderResult r =
      rf::runLadder(tap, picked, std::string(g_cumuloLastGood), std::string(CUMULO_MODEL), attempt);
  if (!r.ok) {
    const std::string cause = rf::refusalCause(r.lastKind, r.lastTried);
    if (!cause.empty()) c.err = cause;
    return false;
  }
  // A pinned fold rung leaves the head's last good model alone (the pick never moves).
  if (!cumuloFoldPinned()) copyWholeOrClear(g_cumuloLastGood, r.used);
  if (g_engine && g_engine->turnInFlight()) rf::settleClamp(tap.servedCapped(), (uint32_t)millis(), c.out);
  if (!r.fellBack() || !g_engine || !g_engine->turnInFlight()) return true;
  rf::settleFallback(r, picked, std::string(CUMULO_MODEL), c.out, [] { return cumuloPick(); },
                     [](const std::string& p) { store::setOrchModel("cumulo", String(p.c_str())); });
  // The conversation marker is the answering model's: it carries over only when
  // that model is now the pick (a kept pick on another upstream starts fresh).
  if (!rf::sameRoute(cumuloPick(), r.used)) c.cv.clear();
  return true;
}

// Execution-only closures for the portable turn orchestration - every decision
// (recall gate, host pick, budget/retry/failover ladder, salvage, scheduled-turn
// rails) lives in lib/harness engine.cpp and is host-tested (test_harness_turn).
static TurnEngine::Deps buildTurnDeps() {
  TurnEngine::Deps d;
  d.cfg = g_cfg;
  d.platform.nowMs    = [] { return (uint32_t)millis(); };
  // Epoch seconds (0 until SNTP): the history stamps each owner and routine row with it (at:).
  d.platform.nowEpoch = [] {
    const time_t t = time(nullptr);
    return t > 1000000000 ? (uint64_t)t : (uint64_t)0;
  };
  d.platform.freeHeap = [] { return (uint32_t)ESP.getFreeHeap(); };
  // CUM-404: the real turn-admission signal - the largest contiguous INTERNAL block
  // (fragmentation, not total free, fails the mbedTLS handshake alloc). Same read the
  // relay dial + provider verify use. Without this the chat gate's largest-block guard
  // is a no-op on device (the harness treats an unwired closure as ample).
  d.platform.largestFreeBlock = [] {
    return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  };
  // L24 (turn_memory.h): a turn keeps its small blocks in PSRAM - the switch drops the
  // general-malloc spill limit to 0 for the turn's length and puts the boot limit back
  // after - so the turn's setup costs no internal SRAM and the admission floors above
  // still hold at its provider request, where the engine checks them again.
  setPsramFirstSwitch([](bool on) { nimbus::heapwatch::psramFirst(on); });
  d.platform.delayMs  = [](uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); };
  d.jobs  = g_jobs;
  d.apply = buildApplyDeps();
  d.deliver = [](const std::string& chatId, const std::string& text) {
    deliver(String(chatId.c_str()), String(text.c_str()));
  };
  d.deliveryNeedsRow = [](const std::string&) { return s_lastDeliveryNeedsRow; };
  d.routineGrants = [] { return loops::routineGrants(); };   // RB-Q2a, ruling 76 item 7
  d.truth = truthdev::binding();   // RB-G0: the truth check (truth_device.h)
  // The chat's recent effects for its probe: the effect ring (RB-FX), whole only when the
  // ring lives on the card.
  truthdev::useRecent([](const std::string& chatId, std::vector<nimbus::orch::fx::RingEntry>& out) {
    const PsramFirstScope psramFirst;
    fxdev::ring().recent(chatId, nimbus::orch::fx::kRingEntries, out);
    return fxdev::ring().backing(chatId) == nimbus::orch::fx::Backing::Storage;
  });
  d.event = [](const attn::Event& e) { if (g_sinks.event) g_sinks.event(e); };
  d.fire = [](const char* cue) {
    if (strcmp(cue, "turnstart") == 0) ::sfx::fire(nimbus::sfx::Ev::TurnStart);   // "Roger that."
  };
  d.recall = [](const std::string& userText, const nimbus::orch::Principal& who) {
    return memory::recallFor(String(userText.c_str()), 0, who);
  };
  // Queued memory writes stored by the drain since this chat's last turn (lane P).
  d.memNotes = [](const std::string& chatId) {
    const bool admin = roleOfChat(String(chatId.c_str())) == nimbus::orch::Role::Admin;
    return memory::pendingNotes(nimbus::orch::nsForChat(chatId, admin));
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
    // Headroom past the 12-row window: the in-flight row, job-delivery rows and a
    // /clear boundary are skipped by recentConversationBlock, not shown.
    q.limit = 2 * kRecentTurnsMax + 1;
    std::vector<EpisodicMessage> rows;
    { memory::Lock lk; rows = memory::episodic().query(q); }   // newest-first, ring-served
    // CUM-511: the window stops at the chat's newest /clear boundary (a cleared
    // chat carries no prior conversation); the in-flight row is excluded.
    // CUM-485: the byte budget derives from the window of the head the ENGINE
    // runs (headBudget; 8192 B at the 200K anchor; the old fixed 3000 B cut the
    // previous reply's code), and the newest rows are kept WHOLE first - on a
    // stateless tool-loop turn this window is the only place the model sees its
    // last answer.
    return agent::recentConversationBlock(rows, std::string(g_curUserRowId),
                                          headBudget().recentConvBytes);
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
    // histV2: the engine renders the turn-structured history itself.
    if (!agent::history::v2Enabled()) in.recentConversation = buildRecentWindow(chatId);
    in.devName   = std::string(nimbus::sys::deviceName().c_str());
    in.hostLabel = agent::hostForPrompt(g_cfg.provider);
    in.now       = std::string(nimbus::hw::localTimeStr().c_str());  // "" = unsynced
    in.fw        = NIMBUS_FW_VERSION " (" NIMBUS_FW_BUILD ")";
    in.directive = g_mem.directive();
    // Lane J: the owner's standing rules ride the never-dropped band beside the
    // directive, on every owner-facing turn whatever started it (a member or
    // guest turn never sees them - the rules are the owner's).
    in.standingPrefs = prefs::blockForRole(roleOfChat(String(chatId.c_str())) == nimbus::orch::Role::Admin);
    // Lane POL2: the owner's open questions ride the same band, owner-facing
    // turns only, like the standing rules.
    in.openQuestions = prefs::openQBlockForRole(roleOfChat(String(chatId.c_str())) == nimbus::orch::Role::Admin);
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
    // CUM-523: the bot is running; the engine narrows this with the turn's one
    // deliverability sample (the same one its tool list follows).
    in.hw.telegram = agent::telegram::enabled();
    in.hw.voiceReplies = agent::store::ttsEnabled();   // owner's "Voice replies" toggle (P2.5)
    in.hw.files = agent::files::available();            // E1 artifact store (SD-backed)
    // CUM-514: the BUILD's board (the same -DSOLIDE_BOARD /api/state reports), not
    // the Solide S3 constant - a Freenove all-in-one has one status pixel and draws
    // the ring on its screen, and must not describe a 45-LED ring to its owner.
    in.hw.ledCount = solide::board().led.count;
    in.board = &agent::buildBoardIdentity();
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
    // CUM-830: the sub-agents this chat may see (the owner every one, a member or a
    // guest only its own chat's).
    in.sessions   = nimbus::orch::visibleSessions(
        sessionInfos(), nimbus::orch::actionCaller(chatPrincipal(chatId), false, chatId));
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
  d.connectorsCatalog = [](bool headTools) { return std::string(connectors::catalog(headTools).c_str()); };
  // [SPAWN CAPACITY] and the priority marking read the Spawn seam, as the catalog does
  // (check of c962b5d0: unwired, they fell back to the Chat read).
  d.spawnPause = [](const std::string& p) { return providerPause(p, nimbus::orch::PaidSeam::Spawn); };
  // Whether this head's turn runs the tool loop (web.search on its own turn): the native
  // hosts, and the Cumulo head when its pick's upstream is one; never Z.ai or custom.
  d.headRunsTools = [](const std::string& h) { return nimbus::orch::headRunsToolLoop(h, cumuloPick()); };
  // W15: the ambient skills index - every turn NAMES the playbooks (id + one-line
  // desc) so the model pulls the procedure with skill.get instead of the recipes
  // riding every prompt. Lane I: the per-chat attach surface - the [ACTIVE
  // SKILLS] block the engine renders into the per-turn INPUT, and the (chat, id)
  // report of a successful skill.get the RAM table notes.
  d.skillsIndex = [] { return skills::indexText(); };
  d.skillsPicks = [](const std::string& chatId) { return skills::picksFor(chatId); };
  d.skillBody = [](const std::string& id) { return skills::attachBody(id); };
  d.skillUsed = [](const std::string& chat, const std::string& id) {
    skills::noteUsed(chat, id);
  };
  // [AVAILABLE MODELS] reads the SAME derivation the spawn validator accepts
  // (CUM-486). It used compile-time lists for three providers and "" for the
  // rest, so the head saw "zai: ;" / "cumulo: ;" and refused those sub-agents,
  // and kept offering o4-mini-deep-research, which validation coerced away.
  d.modelChoices = [](const std::string& p) { return modelChoicesFor(p); };
  // Lane F: how old that list is ("list from <date>, may miss newer models").
  d.modelListNote = [](const std::string& p) {
    return std::string(store::modelListNote(String(p.c_str())).c_str());
  };
  d.takeModelNotice = [] { return store::takeModelNotice(); };   // a dropped stored pick, told once
  // The owner's row: `tags` is the sanitized "from:<sender>" plus, under histV2,
  // the att:/src:/sent: items the engine adds. Returns the row id.
  auto captureOwner = [](const std::string& chatId, const std::string& text,
                         const std::string& tags) -> std::string {
    memory::captureSession(chatId.c_str(), store::orchHost().c_str(), String(text.c_str()));
    String id = memory::captureMessage(chatId.c_str(), "user", orch::MsgKind::Message,
                                       String(text.c_str()), "", String(tags.c_str()));
    strncpy(g_curUserRowId, id.c_str(), sizeof(g_curUserRowId) - 1);
    g_curUserRowId[sizeof(g_curUserRowId) - 1] = 0;
    nimbus::crash::noteTurnText(text);   // the crash journal names a photo as a photo
    // The user row IS this turn's identity - re-key the turn tag off it (the
    // onTurnStart fallback minted a placeholder). The row does NOT carry a
    // turn: tag itself: its own `id` is the key.
    if (id.length()) snprintf(g_turnTag, sizeof(g_turnTag), "turn:%s", id.c_str());
    // Fold trigger on content (L19-26): a re-served copy of the last message adds nothing.
    if (chatId != "system") g_folds.noteOwnerMessage(chatId, text);
    return std::string(id.c_str());
  };
  d.episodicCaptureUser = [captureOwner](const std::string& chatId, const std::string& text,
                                         const std::string& fromTag) {
    captureOwner(chatId, text, fromTag);
  };
  // histV2 (conversation.h): the turn-structured history. A RAM switch, on by
  // default (no settings key); the test console's HISTV2 0 restores the
  // previous window and capture byte for byte.
  d.histV2 = [] { return agent::history::v2Enabled(); };
  d.captureUserRow = captureOwner;
  d.historyRows = [](const std::string& chatId, const agent::HistoryQuery& hq) {
    using namespace nimbus::orch;
    std::vector<EpisodicMessage> rows;
    MsgQuery q;
    q.sessionId = chatId;
    q.haveKind = true; q.kind = MsgKind::Message;
    q.alsoKinds = {MsgKind::Image, MsgKind::File, MsgKind::Audio};
    q.limit = (int)hq.limit;
    q.before = hq.before;
    q.after = hq.after;
    { memory::Lock lk; rows = memory::episodic().query(q, hq.info); }   // ring-served when it can
    return rows;
  };
  // Ruling 90 (RB-Q2b): the store's word search for the earlier-conversation recall (index only).
  d.searchStore = [](const nimbus::orch::MsgQuery& q) {
    std::vector<nimbus::orch::EpisodicMessage> rows;
    { memory::Lock lk; rows = memory::episodic().query(q); }
    return rows;
  };
  // A seedless turn's trigger row joins the turn by its turn: tag.
  d.captureTrigger = [](const std::string& chatId, const std::string& text, const std::string& tags) {
    memory::captureMessage(chatId.c_str(), "system", orch::MsgKind::Message, String(text.c_str()), "",
                           withTurnTag(tags.c_str()));
  };
  d.historyMarks = [](const std::string& chatId) {
    const nimbus::orch::ChatFold f = g_folds.get(chatId);
    return agent::HistoryMarks{f.thru, f.softStart, 0, f.lastFoldEpoch};
  };
  d.noteHistoryShift = [](const std::string& chatId, const std::string& softStart) {
    g_folds.setSoftStart(chatId, softStart);   // persisted with the turn's blob write
  };
  d.noteHistoryBytes = [](const std::string& chatId, size_t bytes) { g_folds.noteHistoryBytes(chatId, bytes); };
  d.foldTriggerBytes = [] { return foldTriggerNow(); };   // the fold driver's own (foldHooks)
  // Minutes east of UTC from the device's own TZ, now and at any row's time (the TZ rule
  // decides DST for a past time, so a row stored before a clock change keeps its offset).
  d.utcOffsetMin = []() -> int32_t { return agent::utcOffsetAtUnix((uint32_t)time(nullptr)); };
  d.utcOffsetAt = [](uint32_t unixSec) -> int32_t { return agent::utcOffsetAtUnix(unixSec); };
  d.dayStartMin = [] { return agent::store::dayStartMin(); };   // RB-REC5: the setting day_starts
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
  static bool s_progLineSent = false;   // lane PROG: the ONE progress line went out
  static int s_traceBlobs = 0;   // sidecars written this turn (P4 budget)
  static int s_turnBlobFails = 0;   // sidecar writes that failed this turn (L19-24)
  d.hooks.onTurnStart = [](const TurnStartEv& ev) {
    nimbus::pollwatch::beat("turn");   // lane TGSTALL: a turn's steps are the poll task's beats
    // Lane PROG: the ONE progress line is per turn.
    s_progLineSent = false;
    // Turn identity = the turn's USER row id (boot-stable, resumed past history
    // via nextIdHint) - so the chat UI joins a bubble to its trace rows by ID
    // instead of chronological adjacency (which mis-bucketed an interleaved
    // Telegram/voice turn's rows into the wrong web bubble). Seedless turns
    // (scheduled loops, synthesis/dream) have no user row, so they mint one from
    // the same counter here; episodicCaptureUser overwrites this with the real
    // row id when a user row does get captured, whichever order the hooks fire.
    s_traceBlobs = 0;
    s_turnBlobFails = 0;
    // The user row is captured BEFORE this hook fires (durable-first), so prefer
    // the id it already stashed - minting here would consume a fresh id and
    // clobber the real one, leaving the turn tagged with an id no row owns
    // (caught on hardware: user row m…85 vs tag turn:m…86). Only a SEEDLESS turn
    // (scheduled loop, synthesis/dream) has no user row and needs a minted id;
    // episodicCaptureUser still overwrites this if it lands after us.
    String id = g_curUserRowId[0] ? String(g_curUserRowId) : memory::mintRowId();
    snprintf(g_turnTag, sizeof(g_turnTag), "turn:%s", id.length() ? id.c_str() : "t0");
    s_traceArgs.clear();
    // Crash UX: the in-flight turn is journaled (/crash/turn.json) until it ends.
    nimbus::crash::turnStarted((int)ev.source, ev.chatId, std::string());
  };
  // Lane ECHO (owner design, 9/11 Nov): is the local time inside quiet hours
  // right now? The engine reads this to capture a background turn's reply into
  // TurnEndEv.wakeReply instead of delivering it at night.
  d.hooks.quietHoursNow = [] { return loops::quietHoursNow(); };
  d.hooks.onEffect = fxdev::record;   // lane RB-FX: the chat's effect ring (the truth check reads it)
  // Lane RB-Q3a: the same ring's newest receipts ride each turn as [RECENT ACTIONS].
  static agent::RingRecentActions recentFx(fxdev::ring());
  d.recentActions = &recentFx;
  d.hooks.onToolCall = [](const orch::HeadToolCall& call) {
    nimbus::pollwatch::beat("tool");
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
    nimbus::pollwatch::beat("tool done");
    if (!memory::traceActive()) return;
    std::string args;
    auto it = s_traceArgs.find(r.id);
    if (it != s_traceArgs.end()) { args = it->second; s_traceArgs.erase(it); }
    // The row keeps the SHORT args clip; the sidecar keeps what was really sent.
    // ONE merged row per call: name(args) -> output. Halves row count and keeps
    // args + result together for the chat disclosure. P4: when anything was
    // clipped, park the FULL call+result as a blob so the owner can open what
    // the model actually saw (the model has results.get; the web had nothing).
    // Bounded per turn so a tool-heavy turn can't churn the SD. The row itself
    // is built in one portable place (trace_rows) so the L19-24 lost-sidecar
    // mark is host-tested: a clipped row whose sidecar write failed says so.
    orch::TraceToolRow tr;
    tr.name = r.name;
    tr.args = args;
    tr.output = r.output;
    tr.blobWanted = s_traceBlobs < ORCH_TRACE_BLOBS_PER_TURN;
    const orch::TraceToolRowOut row = orch::buildTraceToolRow(
        tr, [](const std::string& full, bool* failed) {
          return std::string(memory::persistTraceBlob(String(full.c_str()), failed).c_str());
        });
    if (row.blobPath.length()) s_traceBlobs++;
    if (row.blobFailed) s_turnBlobFails++;
    String tags = String(g_turnTag) + ",tool:" + r.name.c_str() +
                  (r.isError ? ",err" : "");
    std::string chat(currentChat().c_str());
    memory::captureMessage(chat.c_str(), "tool", orch::MsgKind::ToolOutput,
                           String(row.text.c_str()), String(row.blobPath.c_str()), tags);
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
  // Lane PROG: every round boundary of the running turn reaches the Telegram
  // seam - typing stays alive between every provider request, ONE plain
  // progress line past 30 s or at the third round, and a message that arrived
  // mid-turn is acknowledged immediately instead of waiting out the turn in
  // silence. The plan is the portable, host-tested turn_progress.h; this just
  // executes it on the poll task (the turn's own task, no new concurrency).
  // s_progLineSent is reset per turn in onTurnStart above.
  d.hooks.onTurnProgress = [](const TurnProgressEv& ev) {
    nimbus::pollwatch::beat("round");
    orch::HeadProgress hp;
    hp.round = ev.round;
    hp.elapsedMs = ev.elapsedMs;
    hp.lastRound = ev.lastRound;
    hp.tools = ev.tools;
    // Lane ECHO fix pass: only a turn a person started (owner/member message,
    // serial console) may signal the chat; a dream, routine, loop or synthesis
    // turn stays quiet, so the owner never gets an unexplained progress line.
    const nimbus::harness::TurnProgressPlan plan =
        nimbus::harness::planTurnProgress(hp, ev.maxRounds, s_progLineSent, ev.source);
    if (plan.keepAlive) telegram::turnTyping(String(ev.chatId.c_str()));
    if (plan.sayLine) {
      if (telegram::turnProgressLine(String(ev.chatId.c_str()), String(plan.line.c_str())))
        s_progLineSent = true;
    }
    if (plan.checkQueue) telegram::turnAckQueued();
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
    nimbus::pollwatch::beat("turn end");
    nimbus::crash::turnEnded();   // the turn finished: no crash notice will name it
    g_ownerFirst.noteTurnEnd(millis());   // background work waits for the owner
    // Hold-to-talk (CUM-456): the voice message's own turn answered? A salvaged
    // reply (parse failed, text recovered) still answered the owner; the honest
    // failure paths deliver their own line and pass 0 bytes.
    if (s_inVoiceMessage && ev.chatId == "voice")
      s_voiceOk.store(ev.ok || ev.replyBytes > 0, std::memory_order_relaxed);
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
    // Lane T: the portable renderer SHRINKS the row to ORCH_TRACE_END_MAX instead of
    // the old silent drop; err is the failure chain (first cause first), "via" the
    // per-host split when more than one host billed, and the model comes from the
    // event (frozen when the spend was filed), never a store read that this turn's
    // own device action may already have changed.
    static_assert(ORCH_TRACE_END_MAX >= nimbus::orch::kTurnEndRowMin,
                  "the turn-end row is always produced only within kTurnEndRowMin");
    const char* src = agent::turnSourceSlug(ev.source, ev.attribution);
    // Lane BUD: /model's "last reply" (the served provider and the model it billed under).
    budget::noteTurnEnd(ev.host, ev.servedModel, ev.requestedHost, src, ev.ok);
    // Lane DEGRADE: which provider answered and which ones the turn went past, for the
    // degraded line ("Anthropic down, Mistral answers"). Every turn source counts.
    degraded::noteTurn(ev.requestedHost, ev.host, ev.path, ev.billed);
    if (memory::traceActive() && g_turnTag[0] && !ev.chatId.empty()) {
      // Served-by disclosure (CUM-236 device leg): the turn view (_turnChip) reads
      // model+fallback off this row. The shared rule decides it; it never names a
      // model that billed nothing.
      const nimbus::orch::TurnChipDisclosure disc = nimbus::orch::turnChipDisclosure(
          ev.requestedHost, ev.requestedModel, ev.host, ev.servedModel, ev.model);
      nimbus::orch::TurnEndRowIn row;
      row.ok = ev.ok;
      row.src = src;
      row.host = ev.host;
      row.model = disc.model;
      row.fallback = ev.fallback;   // false when no host billed (nothing was served)
      row.tools = ev.rounds;
      row.in = ev.usage.promptTokens;
      row.out = ev.usage.completionTokens;
      row.cacheR = ev.usage.cacheReadTokens;
      row.cacheW = ev.usage.cacheWriteTokens;
      row.reply = ev.replyBytes;
      row.spend = ev.spend;
      row.chain = ev.chain;
      row.error = ev.error;
      row.blobFails = (uint32_t)s_turnBlobFails;   // L19-24: the trace view sees the loss
      const std::string js = nimbus::orch::turnEndRowJson(row, ORCH_TRACE_END_MAX);
      // blobPath -> this turn's dossier, so the chat can offer "turn anatomy"
      // without probing for a file that may have been evicted by the ring.
      String dossier = g_turnDossier
                           ? String("/mem/trace/") + (g_turnTag + 5) + ".txt"
                           : String();
      memory::captureMessage(ev.chatId.c_str(), "system", orch::MsgKind::Log, String(js.c_str()),
                             dossier, String("ev:turnend,") + g_turnTag);
    }
    // Timeline row for FAILED turns only (Glass Box A3) - successes are already
    // visible as chat history; failures previously left no queryable trace. Lane T:
    // it names the turn's source, where it started and ended, the first cause first
    // and the tool CALLS that ran, and carries this turn's id (written before the
    // tag is cleared) so it joins the turn's own rows.
    if (!ev.ok) {
      nimbus::orch::ErrorEventIn ein;
      ein.src = src;
      ein.requestedHost = ev.requestedHost;
      ein.finalHost = ev.host;
      ein.chain = ev.chain;
      ein.error = ev.error;
      ein.toolCalls = ev.rounds;
      const std::string txt = nimbus::orch::errorEventText(ein);
      captureTurnError(String(txt.c_str()));
      noteLastFail(src, ev);
    }
    g_turnTag[0] = 0;   // same reason: no async row may inherit this turn's id
    g_turnDossier = false;
    alogf("turn end: %s host=%s tokens in=%u out=%u tools=%d reply=%uB src=%s",
          ev.ok ? "ok" : "FAIL", ev.host.c_str(), (unsigned)ev.usage.promptTokens,
          (unsigned)ev.usage.completionTokens, ev.rounds, (unsigned)ev.replyBytes, src);
    // W3b passive validation: a SUCCESSFUL real turn is the strongest proof a
    // provider key works - refresh the verify cache from it, so "verified"
    // doesn't depend on the manual Verify button (the UI hint promises this).
    // Write only on a state CHANGE (no steady-state NVS wear). Failures are NOT
    // marked rejected here: most turn failures are not auth failures. One attempt per
    // host and verdict per boot: on a full NVS the stored verdict never reads back as
    // 1, and retrying it made a failed write at every turn end.
    static nimbus::store::VerdictMemo s_verifyMemo;   // the verdict tried per host this boot
    if (ev.ok && (ev.host == "openai" || ev.host == "anthropic" || ev.host == "mistral") &&
        store::verifyResult(ev.host.c_str()) != 1 && s_verifyMemo.shouldWrite(ev.host.c_str(), 1))
      (void)store::setVerify(ev.host.c_str(), 1, millis());   // a failure is in the ledger
    // Lane ECHO (owner design, 9/11 Nov): a background turn (routine, loop,
    // sub-agent synthesis, the upkeep) that ran during quiet hours captured its
    // reply into ev.wakeReply - never delivered at night, never held for the
    // owner's next message. Arm ONE self-wakeup for the end of quiet hours; the
    // kind comes from the turn's source (the class rule in turn_progress.h), the
    // one-pending-per-kind bound lives in orch::armSelfWakeup.
    if (!ev.wakeReply.empty()) {
      nimbus::orch::WakeKind kind;
      if (nimbus::harness::wakeKindForSource(ev.source, ev.attribution, kind)) {
        nimbus::orch::WakeNote note;
        note.kind = kind;
        note.text = ev.wakeReply;
        note.noticedEpoch = (uint64_t)time(nullptr);
        time_t t = (time_t)note.noticedEpoch;
        struct tm lt;
        if (localtime_r(&t, &lt)) note.noticedMinute = (uint16_t)(lt.tm_hour * 60 + lt.tm_min);
        note.dedupKey = nimbus::orch::fnv64(ev.wakeReply);
        const std::string wid = loops::armSelfWakeupNote(note);
        if (!wid.empty())
          alogf("turn end: %s reply captured during quiet hours - self-wakeup %s armed (%s)",
                src, wid.c_str(), nimbus::orch::wakeKindSlug(kind));
      }
    }
    dream::noteTurnEnd(millis());   // the nightly upkeep's idle gate: its quiet clock
    // v3.6.0 fold accounting: one persist per turn + the reactive overflow path.
    if (!ev.chatId.empty() && ev.chatId != "system") {
      if (g_foldDriver) g_foldDriver->noteTurnChat(ev.chatId);
      g_folds.noteTurn(ev.chatId);
      publishFoldAll(ev.chatId);
      if (!ev.ok && anyContextOverflow(ev)) {
        g_folds.markOverflow(ev.chatId);   // next compactTick() folds this chat
        alogf("fold: context-overflow classified on %s - fold queued", ev.chatId.c_str());
      } else if (ev.trimmedForSize) {
        g_folds.noteHistoryTrimmed(ev.chatId);   // the next compactTick() folds, outside the thrash window
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
                      const agent::HeadTools& ht, nimbus::orch::TurnSpend* spend,
                      const std::function<void(const std::string&, const std::string&)>& notify) {
    return providers::runFabricLoop(deviceProviderDeps(), hostList, ins, inp, out, err,
                                    ht, spend, notify);
  };
  d.hosts.fabricSupports = [](const std::string& h) { return providers::fabricSupports(h); };
  // Lane POL2: the open-question list observes every completed turn in the
  // owner's own chat (raise-once bookkeeping, open_questions.h). A member's
  // turn never touches the owner's items.
  d.openqObserve = [](const std::string& chatId, const std::string& ownerText,
                      const std::string& reply) {
    if (roleOfChat(String(chatId.c_str())) != nimbus::orch::Role::Admin) return std::string();
    return prefs::observeOwnerTurn(ownerText, reply);
  };
  // Lane K's park, one store for the fabric and the single-shot ladder and fold,
  // over the same ProviderDeps (the park's key and model fingerprint).
  d.hosts.parked = [](const std::string& h, providers::HostPark* why) {
    return providers::hostParked(deviceProviderDeps(), h, why);
  };
  d.hosts.failed = [](const std::string& h, nimbus::orch::StepErr kind, int httpStatus) {
    providers::noteHostFailed(deviceProviderDeps(), h, kind, httpStatus);
  };
  d.hosts.answered = [](const std::string& h) { providers::unparkHost(h); };
  // Head on the custom/LAN endpoint (Stage H): a single-shot chat-completions
  // structured turn (no tool loop v1 - the portable fn ignores `tools`), so a
  // fully keyless LAN setup (e.g. Ollama) can run the whole orchestrator.
  // The head is registered ONLY when configured at boot (restart to use a newly
  // set endpoint as the head; custom HELPERS need no restart since CUM-541), and
  // hostHasKey('custom') maps to store::hasCustom() via the HarnessConfig view, so
  // priority/failover just work.
  if (store::hasCustom())
    d.hosts.add("custom", [](std::string& cv, const std::string& ins, const std::string& inp,
                             std::string& out, std::string& err, const agent::HeadTools* tools,
                             nimbus::orch::TokenUsage* usage) {
      return providers::orchTurnCustom(deviceProviderDeps(), cv, ins, inp, out, err,
                                       tools, usage);
    });
  // CUM-242: the router providers as first-class orchestrator HEADS, over a FIXED base
  // + path prefix and the router key, so a device holding only a verified Cumulo (or
  // Z.ai) key runs the whole assistant - the flagship "one key, one balance" path. The
  // Z.ai head runs the tool loop through orchTurnCustom (pd.customToolLoop). Registered UNCONDITIONALLY (unlike custom's boot-time
  // hasCustom() gate) so a key added AFTER boot works with no reboot: selection is
  // gated live by routerFallbackHost()/hasKey() (both check the key at turn time),
  // and orchTurnCustom refuses honestly if the base/key is somehow empty.
  d.hosts.add("cumulo", [](std::string& cv, const std::string& ins, const std::string& inp,
                           std::string& out, std::string& err, const agent::HeadTools* tools,
                           nimbus::orch::TokenUsage* usage) {
    CumuloCall c{cv, ins, inp, out, err, tools, usage};
    return cumuloTurn(c);
  });
  d.hosts.add("zai", [](std::string& cv, const std::string& ins, const std::string& inp,
                        std::string& out, std::string& err, const agent::HeadTools* tools,
                        nimbus::orch::TokenUsage* usage) {
    const std::unique_ptr<providers::ProviderDeps> deps(new providers::ProviderDeps(deviceProviderDeps()));
    providers::ProviderDeps& pd = *deps;   // on the heap (lane STACK, as in cumuloAttempt)
    pd.zaiHead(pd);
    return providers::orchTurnCustom(pd, cv, ins, inp, out, err, tools, usage);
  });
  return d;
}

// P7 (/api/tools): does this registry tool ride the mid-turn loop right now?
bool toolRidesLoop(const std::string& name) {
  return g_engine && g_engine->loopActiveNow() && !loopToolHidden(name);
}

static void registerDeviceControl() {
  // Mid-turn device control (owner, 2026-07-24: "the model must be able to run as
  // many tool calls as necessary in a single turn"). The tool loop already gives
  // it up to orchLoopRounds() tool rounds - what was missing is a TOOL that
  // touches the device: config/led/lights/tts existed only as end-of-turn
  // device[] contract actions. This registers the same element shape, validator,
  // policy rails (the caller rule table, protected-key block, risk notes), and
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
      [](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) -> nimbus::orch::ToolResult {
        // WHO asked (CUM-813): the dispatch Principal, never the ambient current chat -
        // a LAN /mcp call can arrive while the owner's turn runs, and currentChat()
        // would then be the owner's. The registry's access rule refuses the LAN and
        // non-owners before this handler; this keeps the handler safe on its own.
        const std::string refused = nimbus::orch::actionRefusal(
            "device.control", nimbus::orch::actionCaller(who, inScheduledTurn()));
        if (!refused.empty()) return nimbus::orch::ToolResult::fail(refused);
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
        const agent::DeviceOutcome dev = agent::applyDeviceAction(
            json, who, inScheduledTurn(), std::string(currentChat().c_str()), s_devDeps, &risk);
        // A refused action is an error the head reads, in plain words (never "applied").
        if (dev.refusedWhole) return nimbus::orch::ToolResult::fail("not applied: " + dev.refusals);
        std::string out = "applied: " + dev.summary;
        if (!dev.refusals.empty()) out += "\nnot applied: " + dev.refusals;
        if (!risk.empty()) out += risk;
        out += dev.replyNote;   // the head relays it: this path has no reply of its own to ride
        return nimbus::orch::ToolResult::ok(out);
      },
      R"JS({"type":"object","properties":{"action":{"type":"object","description":"one device[] element, verbatim (config/led/lights/tts)"}},"required":["action"]})JS");
}

// ---- public API -------------------------------------------------------------

void begin(HeavyFabric* fabric, const Sinks& sinks) {
  g_fabric = fabric;
  g_sinks  = sinks;
  // Route the portable harness library's log seam into the device agent log
  // (alog -> RAM ring -> GET /api/log), same surface the rest of the agent uses.
  // Forensic window (crash forensics, rows 44/57): a routine per-round headloop line
  // stays in the RAM ring + serial; a capped or low-memory round stays durable. The
  // round number also moves the crash journal, so a crash notice can name the step.
  hlog::setSink([](const char* line) {
    const std::string l(line);
    int round = 0;
    if (nimbus::sys::parseHeadloopRound(l, round) && turnInFlight()) nimbus::crash::turnRound(round);
    if (nimbus::sys::headloopRoutine(l)) alogi(line);
    else alog(line);
  });
  // The journal's file store (/data/agjournal.json, orch_persist.h), in PSRAM: its slot
  // image is only touched from task context (begin here, then tg_poll).
  static FlashJournalStore& journalStore = ::nimbus::psramObject<FlashJournalStore>();
  journalStore.begin();
  g_journal.begin(&journalStore);
  // Construct the portable job machinery over the freshly loaded journal (the
  // engine's ctor arms the poll gate at "now", replacing g_nextPollAt = millis()).
  // The job and turn engines live in PSRAM (psram_static.h, psramObject): about 3 KB
  // of task-only state that internal RAM no longer holds (no lock, no ISR access).
  static JobEngine& engine = ::nimbus::psramObject<JobEngine>(buildJobDeps(),
      JobEngine::Tuning{AGENT_MAX_ACTIVE_INFLIGHT, ORCH_DISPATCH_MIN_HEAP,
                        AGENT_POLL_INTERVAL_MS});
  g_jobs = &engine;
  g_jobs->setAttnHoldMs(g_attnHoldMs);   // apply a pre-begin Param::AttnHoldMs latch
  if (!g_sessMux) g_sessMux = xSemaphoreCreateMutex();   // guards the session snapshot
  refreshSessionSnapshot();   // prime it from the restored journal (pre-first-pump reads)
  // The harness config view + the portable turn orchestration over it. Heap
  // floors come from agent_config.h so the device numbers stay authoritative.
  static_assert(ORCH_LOOP_MAX_ROUNDS == agent::kLoopRoundsDefault,
                "the prompt's stated round cap must be the device default (CUM-514)");
  g_cfg = harnessConfigFromStore();
  // CUM-523: a turn that cannot deliver over Telegram is never offered a Telegram
  // leg (the engine's one per-turn sample also narrows the manifest's hw.telegram).
  g_cfg.telegramDeliverable = telegramDeliverable;
  static TurnEngine& turnEngine = ::nimbus::psramObject<TurnEngine>(buildTurnDeps(),
      TurnEngine::Tuning{ORCH_TURN_HARD_FLOOR, ORCH_RECALL_MIN_HEAP,
                         ORCH_AUTO_TURN_MIN_HEAP, ORCH_LOOP_MIN_HEAP,
                         ORCH_TURN_MIN_LARGEST_BLOCK});
  g_engine = &turnEngine;
  // A lazily created mutex allocates from internal RAM on first use: create these now.
  { std::lock_guard<std::mutex> prime(g_turnOwnerMx); }
  nimbus::orch::headSwitchGuard().reset();
  nimbus::orch::clearServedModels();
  g_mem.begin(&g_memStore, std::string(store::sysPrompt().c_str()));
  g_folds.begin(&g_foldIO);   // v3.6.0 per-chat fold state (/data/chatsum.txt)
  // Lane F: the one fold driver over the store and /data/fold_ctl.txt (PSRAM, task-only).
  static agent::FoldDriver& foldDriver =
      ::nimbus::psramObject<agent::FoldDriver>(turnEngine, g_folds, LittleFsFoldCtlIO::io(), foldHooks());
  g_foldDriver = &foldDriver;
  foldDriver.begin();
  publishFoldWebStatus();
  loadTenants();              // v3.7.0 RBAC roles/quotas (+ legacy adoption)
  syncMemEcho();
  registerDeviceControl();
  // CUM-538: an unknown tool name gets a way back - a configured connector names
  // the provider whose sub-agents run it, and a spawn field carries the REAL
  // remaining room (the same spawnRoomFor the [SPAWN CAPACITY] line uses). Both
  // hooks are evaluated only on an unknown name, never on the normal tool path.
  {
    nimbus::orch::ToolRegistry::UnknownToolHints hints;
    hints.connectorProvider = [](const std::string& name) {
      return connectors::providerForName(name);
    };
    hints.spawnRoom = [] {
      return nimbus::orch::spawnRoomFor(g_jobs ? g_jobs->pendingCount() : 0);
    };
    memory::registry().setUnknownToolHints(hints);
  }
  // wf_plan 3: a stored Code Interpreter card moves into the Code sandbox switch
  // (once; a no-op on every later boot), and the device PDF writer is registered.
  connectors::migrateCodeInterpreterCards();
  pdf_tool::registerTool(memory::registry());
  // CUM-537: queue a workspace probe once, 90 s out so the first turns after a
  // restart (queued Telegram messages) get the work slot first; the pump runs it
  // when idle. A stored answer (the last good one, kept on flash) covers the wait,
  // so a confirmed connector stays usable across the restart.
  provider_verify::queueWorkspaceProbeIfStale(90000);
  // Per-model prices: an upgraded device with a verified Cumulo key has no router
  // prices until a Cumulo verify fetches them; ask for one, after the probe above.
  provider_verify::queueRouterPriceRefill(150000);
  int n = g_journal.count();
  alogf("orchestrator: ready (%d active job%s)", n, n == 1 ? "" : "s");
}

static String (*g_otaInstallHook)() = nullptr;
void setOtaInstallHook(String (*fn)()) { g_otaInstallHook = fn; }

void handleMessage(const String& text, const String& fromName, const String& chatId) {
  // Hold-to-talk (CUM-456): count an on-device voice message as finished on EVERY
  // exit path below (turn, early refusal, command), after its reply was delivered.
  struct VoiceDone {
    bool on;
    ~VoiceDone() {
      if (!on) return;
      s_inVoiceMessage = false;
      s_voiceHandled.fetch_add(1, std::memory_order_release);
    }
  } voiceDone{chatId == "voice"};
  if (voiceDone.on) {
    s_voiceOk.store(false, std::memory_order_relaxed);   // until its turn answers
    s_inVoiceMessage = true;
  }
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
    // Lane BUD: a spending change waits for "/budget confirm" from the same chat; any
    // other message from that chat cancels it (before anything else runs).
    budget::noteMessage(chatId, cmd);
    if (cmd.isCommand && (isChatAllowed(chatId) || pseudo)) {
      const std::string& v = cmd.verb;
      // Owner-only verbs: they schedule unattended turns, install firmware, or
      // approve the owner's downloads - an allow-listed MEMBER may converse but
      // not run these. One table (nimbus::orch::kCommandVerbs) says which; the
      // batcher's command boundary and the dispatcher guard iterate the same table.
      const bool ownerCmd = nimbus::orch::isOwnerVerb(v);
      // The tenant role decides (the table every conversational rail reads): a demoted
      // or revoked chat loses the owner verbs, an Admin row has them wherever it sits on
      // the allowlist. A chat with no row follows the Telegram owner rule (roleOfChat).
      const bool owner = pseudo || nimbus::orch::mayRunOwnerCommands(roleOfChat(chatId));
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
      if (v == "ota") {   // RB-OTA2: the update channel (owner only, by the table)
        say(selfTag + nimbus::ota::channelCommandReply(cmd.args.data(), cmd.args.size(), otaupd::channelOps()));
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
          clearConversation(chatId);   // tg_poll, before any turn: nothing in flight
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
        // Reply only after the change landed: an approval a full NVS refused would
        // otherwise read as done and silently revert at the next restart.
        if (!store::setConnectorsJson(out)) {
          say(selfTag + nimbus::store::kStorageFullMessage);
          return;
        }
        say(selfTag + (action == "approve"
                  ? ("Approved MCP server \"" + name + "\". Its tools will be discovered shortly.")
                  : ("Removed approval for MCP server \"" + name + "\". Its tools are retracted.")));
        return;
      }
      if (v == "compact") {
        // Async by design: the fold runs on the next pollJobs pass (~1 s), off
        // the command path - the same staged pattern as every heavy owner op.
        stageManualFold(chatId.c_str());
        say(selfTag + "Compacting this conversation in the background - I'll confirm shortly.");
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
          say(selfTag + "Usage: /remind <when> <what> - e.g. "
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
            em = "that time is out of range - pick between 2 minutes and 7 days";
          say(selfTag + "Couldn't set that reminder: " + em);
          return;
        }
        const long mins = secs / 60;
        String when = (secs < 3600) ? (String(mins) + " min")
                    : (secs < 86400) ? (String(secs / 3600) + "h" +
                                        (secs % 3600 ? " " + String((secs % 3600) / 60) + "m" : ""))
                    : (String(secs / 86400) + "d");
        say(selfTag + "Reminder set - in " + when + " I'll remind you: " +
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
        if (action == "grants" && (id == "on" || id == "off")) {   // RB-Q2a, ruling 76 item 7
          loops::setRoutineGrants(id == "on");
          say(selfTag + (id == "on" ? "Routines may now use your connectors and start sub-agents."
                                    : "Routines now only read, set their follow-up and reply."));
          return;
        }
        String msg;
        if      (action == "approve") msg = loops::approveLoop(id) ? "Approved - it will run on schedule." : "No routine with that ID.";
        else if (action == "deny" || action == "kill" || action == "delete")
          msg = loops::isReservedId(id)
                    ? "This is a system routine - use /loop off " + id + " to pause it."
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
      if (v == "prefs") {
        // Lane J: the owner's standing rules - deterministic, owner-only, never
        // a paid turn. The parse and the words come from the portable command
        // handler (prefs_tools.cpp), pinned host-side.
        say(selfTag +
            nimbus::orch::prefsCommand(prefs::context(), cmd.args.c_str()).c_str());
        return;
      }
      if (v == "skill") {
        // Lane I: the owner's /skill sub-commands over the store - deterministic,
        // owner-only, never a paid turn (intercepted before the LLM like /loop).
        // The parse and the words come from the portable command table
        // (skill_store.h), pinned by test_skill_commands_burst_and_parse.
        std::string arg;
        const agent::SkillCmd sub = agent::parseSkillCmd(cmd.args.c_str(), arg);
        const std::string id = arg;
        if (sub == agent::SkillCmd::List) {
          say(selfTag + agent::skillListText(skills::store(), skills::selfApproveOn()).c_str());
          return;
        }
        if (sub == agent::SkillCmd::Show) {
          say(selfTag + agent::skillShowText(skills::store(), id).c_str());
          return;
        }
        if (sub == agent::SkillCmd::Auto) {
          const bool on = arg == "on";
          if (arg != "on" && arg != "off") {
            say(selfTag + agent::skillCmdUsage().c_str());
            return;
          }
          if (!skills::setSelfApprove(on)) {
            say(selfTag + "The switch could not be written.");
            return;
          }
          say(selfTag + String("The assistant may approve its own skill changes: ") +
              (on ? "on" : "off") + ".");
          return;
        }
        const agent::SkillCaller who = agent::SkillCaller::ownerCall();
        agent::SkillResult r;
        if (sub == agent::SkillCmd::Approve)      r = skills::approve(id, who);
        else if (sub == agent::SkillCmd::Deny)    r = skills::deny(id, who);
        else if (sub == agent::SkillCmd::Delete)  r = skills::remove(id, who);
        else { say(selfTag + agent::skillCmdUsage().c_str()); return; }
        say(selfTag + agent::skillResultText(id, r).c_str());
        return;
      }
      if (v == "usage" || v == "budget" || v == "model") {
        // Lane BUD: the spending commands and /model - deterministic, owner-only, never
        // a paid turn, so they work while every provider is paused. The module refuses a
        // group chat and a change by voice on its own; an applied change is copied once into
        // the web chat's history (a change made in the web chat is already there). History
        // only: a delivery to "web" here would fill the web chat's reply slot of a turn
        // that is not running.
        // A Telegram message carries its send time and whether it is a voice note's
        // transcript (a confirm is held to its proposal's date; speech never changes a
        // limit); the web chat, serial and hold-to-talk carry neither.
        nimbus::orch::BudgetMsg facts;
        if (!pseudo) {
          facts.date = agent::telegram::turnMessageDate();
          facts.voiceNote = agent::telegram::turnIsVoiceNote();
        }
        const budget::Reply r = budget::handle(v, cmd.args, chatId, owner, facts);
        say(selfTag + String(r.text.c_str()));
        if (!r.mirror.empty() && chatId != "web")
          memory::captureMessage("web", "assistant", orch::MsgKind::Message, String(r.mirror.c_str()), "",
                                 "ev:budget");
        return;
      }
      if (v == "help" || v == "start") {
        say("Hi - I'm " + String(nimbus::sys::deviceName().c_str()) +
                        " (firmware " NIMBUS_FW_VERSION
                        "). Send a message and I'll reply here.\n\nOwner commands:\n"
                        "/update - install a pending firmware update\n"
                        "/loops - list routines\n"
                        "/remind <when> <what> - one-time reminder (e.g. /remind 30m ...)\n"
                        "/compact - summarize this conversation into memory\n"
                        "/clear - forget this conversation (keeps memory and files)\n"
                        "/mcp approve|deny <name> - approve a tool server\n"
                        "/play [track|stop|pause] - play music from the SD card\n"
                        "/loop approve|deny|off|on <id> - manage a routine\n"
                        "/loop grants on|off - may routines use your connectors and start sub-agents\n"
                        "/skill list|show <id> - see saved skills and what waits for you\n"
                        "/skill approve|deny|delete <id> - decide on the assistant's skill changes\n"
                        "/skill auto on|off - may the assistant approve its own skill changes\n"
                        "/prefs - list your standing rules; /prefs add <rule> saves one\n"
                        "/usage - this month's spending, limits and what is paused\n"
                        "/budget <provider|all> <USD> - change a monthly limit (asks you to confirm)\n"
                        "/model - which provider and model the assistant uses, and what is paused");
        return;
      }
      // Any other slash command: deterministic reply, never a paid LLM turn.
      say(selfTag + "I don't recognize /" + String(v.c_str()) +
                      " - try /help. To chat, just write without the slash.");
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
  // Only the owner's own words may confirm a head switch (head_switch.h): web, serial and
  // voice are the owner's channels, a Telegram chat counts when it is the admin's.
  if (nimbus::orch::ownerChannel(std::string(chatId.c_str()), roleOfChat(chatId) == nimbus::orch::Role::Admin)) {
    std::lock_guard<std::mutex> lk(g_turnOwnerMx);
    g_turnOwner.text = text.c_str();
    g_turnOwner.number = ++g_ownerMsgs;
  }
  g_engine->handleMessage(std::string(text.c_str()), std::string(fromName.c_str()),
                          std::string(chatId.c_str()));
  std::lock_guard<std::mutex> lk(g_turnOwnerMx);
  g_turnOwner = nimbus::orch::OwnerMessage();
}

// ---- output-channel helpers (P6: reply.speak / reply.telegram tools) --------
// These back the registry tools so the MODEL chooses its output channel, instead
// of the old global ttsEnabled checkbox. reply.speak reads TTS out on the device
// speaker (loopback-verified working); reply.telegram delivers text to a chat.
String currentChat() { return g_engine ? String(g_engine->currentChat().c_str()) : String(); }

void noteTurnSpoke() {
  if (g_engine) g_engine->noteSpoke();
}

// ---- Local Loops: scheduled-turn executor -----------------------------------
bool telegramForTurn() {
  if (g_engine && g_engine->turnInFlight()) return g_engine->turnTelegram();
  return telegramDeliverable(std::string());
}
bool fileSendForTurn() {
  if (g_engine && g_engine->turnInFlight()) return g_engine->turnFileSend();
  return telegramDeliverable(std::string());   // outside a turn: a chat is allowlisted
}
bool inScheduledTurn() { return g_engine && g_engine->inScheduledTurn(); }
// Read on the turn task only (the turn's state): a web page's save while a routine runs on
// tg_poll is not the routine's call.
nimbus::orch::UnattendedFacts unattendedFacts() {
  return g_engine && telegram::onTurnTask() ? g_engine->unattendedFacts() : nimbus::orch::UnattendedFacts();
}

// Lane J: did the running turn read outside content (a folded-in media row, or
// an external-tainted tool result)? The standing rules' taint rail reads it: a
// rule a tainted turn adds waits for the owner's approval instead of going live.
bool turnReadOutsideContent() {
  return g_engine && g_engine->externalContentSeen();
}

nimbus::orch::FireOutcome injectScheduledTurn(const String& chatId, const String& prompt,
                                              const String& name, const String& loopId,
                                              bool quietOk, bool once, bool ownerReminder,
                                              bool selfWakeup) {
  if (!g_engine) {   // loops only fire after begin(); belt-and-braces
    nimbus::orch::FireOutcome o;
    o.deferred = true;   // not a failure: it must not roll the breaker (lane M)
    o.detail = "deferred: not running";
    return o;
  }
  return g_engine->injectScheduledTurn(std::string(chatId.c_str()),
                                       std::string(prompt.c_str()),
                                       std::string(name.c_str()),
                                       std::string(loopId.c_str()), quietOk, once,
                                       ownerReminder, selfWakeup);
}

// The update notice (RB-Q2a, ruling 76 item 6): the OTA hook stages its fixed text from its
// own task (orch::updateNoticeText, no model call); here on tg_poll the owner's chat is
// resolved, the notice sent and its history row written at once, with no effect tags, so a
// question right after the update sees it. One slot; the latest stager wins.
static char          (&s_noticeText)[512] = ::nimbus::psramStatic<char[512]>();   // PSRAM
static volatile bool s_noticePending = false;
static portMUX_TYPE  s_noticeMux = portMUX_INITIALIZER_UNLOCKED;

void stageUpdateNotice(const String& text) {
  portENTER_CRITICAL(&s_noticeMux);
  strncpy(s_noticeText, text.c_str(), sizeof(s_noticeText) - 1);
  s_noticeText[sizeof(s_noticeText) - 1] = 0;
  s_noticePending = true;
  portEXIT_CRITICAL(&s_noticeMux);
}

static void drainUpdateNotice() {
  if (!s_noticePending) return;
  String text;
  portENTER_CRITICAL(&s_noticeMux);
  text = s_noticeText;
  s_noticePending = false;
  portEXIT_CRITICAL(&s_noticeMux);
  const String chat = agent::telegram::ownerChat();   // the poll task's own lists
  if (chat.length() == 0 || !g_engine) return;          // nowhere to deliver - drop quietly
  if (loops::hasRoutines())   // ruling 76 item 7: the owner learns his routines' new default
    text += "\nRoutines no longer use your connectors or start sub-agents; /loop grants on allows it.";
  telegram::send(chat, text, /*block=*/false);
  g_engine->recordNotice(std::string(chat.c_str()), std::string(text.c_str()));
}


bool stageManualFold(const char* chatId) {
  if (!chatId || !chatId[0] || strlen(chatId) >= sizeof(g_manualFoldChat[0])) return false;
  bool staged = false;
  portENTER_CRITICAL(&g_foldSpin);
  for (int i = 0; i < kFoldStageSlots && !staged; i++) {
    if (g_manualFoldChat[i][0] && strncmp(g_manualFoldChat[i], chatId, sizeof(g_manualFoldChat[i])) != 0) continue;
    strncpy(g_manualFoldChat[i], chatId, sizeof(g_manualFoldChat[i]) - 1);
    g_manualFoldChat[i][sizeof(g_manualFoldChat[i]) - 1] = 0;
    staged = true;
  }
  portEXIT_CRITICAL(&g_foldSpin);
  if (!staged) alogf("fold(%s): manual request not staged - %d others are waiting", chatId, kFoldStageSlots);
  return staged;
}

#ifdef NIMBUS_TEST
void stageFoldDailyReset() { g_foldResetStaged = true; }
#endif

// Refresh the cross-task fold echo. tg_poll ONLY (every turn end + every fold
// outcome) - the sole reader of the live FoldStore off-task is the echo. Lane F
// appends the schedule (paused stays 0: nothing pauses since v5.1.2).
static void publishFoldStatus(const std::string& chat) {
  if (chat.empty()) return;
  const nimbus::orch::ChatFold* f = g_folds.peek(chat);
  const nimbus::orch::ChatFold none;
  const nimbus::orch::ChatFold& c = f ? *f : none;
  const uint32_t kb = store::compactAtKB();
  agent::FoldStatus st;
  if (g_foldDriver) st = g_foldDriver->status(chat, millis(), (uint32_t)(memory::clockSynced() ? time(nullptr) : 0));
  char buf[sizeof(g_foldEcho)];
  snprintf(buf, sizeof(buf),
           "CTX chat=%s bytes=%lu msgs=%lu turns=%lu paused=0 fails=%u lastFold=%lu "
           "sumBytes=%u threshKB=%u nextTry=%lu firstFail=%lu rung=%u/%u paidToday=%u cap=%u queued=%d",
           chat.c_str(), (unsigned long)c.bytesSinceFold, (unsigned long)c.msgsSinceFold,
           (unsigned long)c.turnsSinceFold, (unsigned)c.failedCycles, (unsigned long)c.lastFoldEpoch,
           (unsigned)c.summary.size(), (unsigned)kb, (unsigned long)c.nextTry, (unsigned long)c.firstFail,
           (unsigned)st.rungIndex, (unsigned)st.rungCount, (unsigned)st.paidToday, (unsigned)st.cap, (int)st.queued);
  portENTER_CRITICAL(&g_foldSpin);
  strncpy(g_foldEcho, buf, sizeof(g_foldEcho) - 1);
  g_foldEcho[sizeof(g_foldEcho) - 1] = 0;
  portEXIT_CRITICAL(&g_foldSpin);
}

// The web task's status copy: every chat with a fold state, the queued ones and the
// default (the owner's Telegram chat, else the web chat). tg_poll only.
static void publishFoldWebStatus() {
  if (!g_foldDriver) return;
  const uint32_t epoch = memory::clockSynced() ? (uint32_t)time(nullptr) : 0;
  std::map<std::string, agent::FoldStatus> snap;
  std::vector<std::string> chats = g_folds.chatIds();
  for (const std::string& q : g_foldDriver->ctl().manualPendingChat) chats.push_back(q);
  const String owner = agent::telegram::ownerChat();
  std::string def = owner.length() ? std::string(owner.c_str()) : std::string("web");
  chats.push_back(def);
  chats.push_back("web");
  for (const std::string& c : chats)
    if (!c.empty() && c != "system" && !snap.count(c)) snap[c] = g_foldDriver->status(c, millis(), epoch);
  std::lock_guard<std::mutex> lk(g_foldStatusMx);
  g_foldStatusSnap.swap(snap);
  g_foldDefaultChat.swap(def);
}

static void publishFoldAll(const std::string& chat) {
  publishFoldStatus(chat);
  publishFoldWebStatus();
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

bool foldStatusJsonFor(const std::string& chatArg, std::string& out) {
  agent::FoldStatus st;
  std::vector<agent::FoldStatus> all;
  {
    std::lock_guard<std::mutex> lk(g_foldStatusMx);
    const auto it = g_foldStatusSnap.find(chatArg.empty() ? g_foldDefaultChat : chatArg);
    if (it == g_foldStatusSnap.end()) return false;
    st = it->second;
    for (const auto& c : g_foldStatusSnap) all.push_back(c.second);
  }
  const uint32_t epoch = memory::clockSynced() ? (uint32_t)time(nullptr) : 0;
  out = agent::foldStatusJson(st, epoch, agent::utcOffsetAtUnix(epoch), &all);
  return true;
}

// The fold's owner notices. A Telegram chat gets a message; the web chat a history row
// (its reply slot belongs to the turn being polled, live-caught: an async notice there
// displaced every later web reply); serial and voice only the log. True when it reached a
// chat a person reads.
static bool foldSay(const std::string& chat, const std::string& text) {
  if (chat == "web")
    return memory::captureMessage("web", "assistant", nimbus::orch::MsgKind::Message, String(text.c_str()), "",
                                  "ev:foldnote").length() > 0;
  if (chat == "serial" || chat == "voice") {
    alogf("fold(%s): %s", chat.c_str(), text.c_str());
    return false;
  }
  bool queued = false;   // delivered only when the reply sink took it (a null sink or a full queue is not)
  return deliver(String(chat.c_str()), String(text.c_str()), /*system=*/true, &queued) && queued;
}
static void foldTell(const std::string& chat, const std::string& text) { foldSay(chat, text); }

// The owner's Telegram chat the daily alert of the serial console or the screen goes to
// ("" with no bot token or no owner): the owner-default rule the routines use.
static std::string foldOwnerTelegram() {
  const nimbus::orch::DeliveryWorld w =
      nimbus::orch::deliveryWorld(std::string(store::telegramToken().c_str()),
                                  std::string(store::telegramAllowlist().c_str()),
                                  std::string(store::telegramOwners().c_str()));
  return w.botToken ? w.ownerTelegram : std::string();
}

// A summary landed (the store is written): the chain reset, then the boundary row.
static void foldLanded(const agent::FoldDone& d) {
  g_engine->clearChatConv(d.chat);
  if (const nimbus::orch::ChatFold* after = g_folds.peek(d.chat))
    alogf("fold(%s): digest %u B, anchor %s, shift %s, on %s/%s", d.chat.c_str(), (unsigned)d.slice.digest.size(),
          after->thru.empty() ? "-" : after->thru.c_str(), after->softStart.empty() ? "-" : after->softStart.c_str(),
          d.host.c_str(), d.model.c_str());
  const std::string& thru = d.slice.thru;
  memory::captureMessage(d.chat.c_str(), "system", nimbus::orch::MsgKind::Log,
      String("Conversation compacted: ") + String((unsigned)d.bytesFolded) + " B / " + String((unsigned)d.msgsFolded) +
          " msgs folded into a " + String((unsigned)d.summary.size()) + " B summary (produced " +
          String((unsigned)d.producedBytes) + " B, kept " + String((unsigned)d.summary.size()) + " B) (" +
          String((unsigned)(d.elapsedMs / 1000)) + " s)",
      "", thru.empty() ? String("ev:compact") : String("ev:compact,thru:") + thru.c_str());
}

// The fold slice when the turn-structured history is off: the live conversation's
// rows, repeats collapsed, as one digest (no anchor).
static agent::FoldSlice legacyFoldSlice(const std::string& chat, size_t cap) {
  using namespace nimbus::orch;
  MsgQuery q;
  q.sessionId = chat;
  q.haveKind = true; q.kind = MsgKind::Message;
  q.alsoKinds = {MsgKind::Image, MsgKind::File, MsgKind::Audio};   // media is conversation
  q.limit = 400;
  // NO sinceHours bound (prism v3.6.0): row stamps are boot-relative until SNTP syncs;
  // the 400-row limit and the digest budget bound the slice, and the summary is
  // anchored, so re-including already-folded rows costs a few tokens.
  std::vector<EpisodicMessage> rows;
  { memory::Lock lk; rows = memory::episodic().query(q); }
  // CUM-511: only the live conversation; L19-26: a run of one repeated owner message is one line.
  rows = agent::collapseRepeats(agent::liveConversation(rows, std::string(), 0), agent::utcOffsetAtUnix);
  agent::FoldSlice s;
  s.digest = dream::buildEpisodicDigest(rows, cap);
  s.rows = s.digest.empty() ? 0 : rows.size();
  return s;
}

// The owner's chats (the daily alert, the fold's order and the guests' share of the cap).
// By role, as every rail reads it (bench B11, candidate 2): the device's own surfaces (web,
// serial, voice) and every Admin chat are the owner's. The serial console counted as a guest
// once, so after the guests' 6 paid folds of the day its automatic fold never started.
static bool foldOwnerChat(const std::string& chat) {
  return roleOfChat(String(chat.c_str())) == nimbus::orch::Role::Admin;
}

static agent::FoldDriverHooks foldHooks() {
  agent::FoldDriverHooks h;
  h.online = [] { return nimbus::net::staConnected(); };   // an offline fold is a guaranteed failure
  // A due automatic fold waits for the owner (owner_first.h: two quiet minutes, or at
  // most 10 minutes / 6 turns and then right after a reply went out). An overflow fold
  // is needed by the next turn.
  h.quiet = [](bool overflow) {
    return overflow || g_ownerFirst.foldMayStart(telegram::guardStatus().holding, millis());
  };
  h.notDue = [] { g_ownerFirst.noteFoldNotDue(); };
  h.triggerBytes = [] { return foldTriggerNow(); };
  // What one fold input can carry (lane CTXR1): a backlog is paid off in large bites.
  h.foldCycleBytes = [] { return nimbus::orch::foldCycleBytes(headRequestContext()); };
  h.slice = [](const std::string& chat, size_t cap, bool manual) {
    return g_engine->histV2On() ? g_engine->historyFoldSlice(chat, cap, manual) : legacyFoldSlice(chat, cap);
  };
  h.utcOffsetMin = [](uint32_t epoch) -> int32_t { return agent::utcOffsetAtUnix(epoch); };
  h.folded = foldLanded;
  h.tell = foldTell;
  // The daily alert reaches a chat a person reads, or the day's alert is not spent
  // (candidate 4, F2): an owner's Telegram chat or the web chat directly; the serial
  // console's and the screen's through the owner's Telegram chat, else the web chat; a
  // guest chat's is only logged (agent::foldAlertRoute, host-tested).
  h.alert = [](const std::string& chat, const std::string& text) {
    return agent::foldAlertSend(chat, text, foldOwnerChat(chat), foldOwnerTelegram(), "web", foldSay);
  };
  h.ownerChat = foldOwnerChat;   // the owner's chats fold first; guests share half the cap
  h.changed = [](const std::string& chat) {
    if (!chat.empty()) publishFoldStatus(chat);   // the CTX echo, then the web copy
    publishFoldWebStatus();
  };
  h.firmware = NIMBUS_FW_BUILD;
  return h;
}

// Staged manual requests (web, console, /compact) into the driver's persisted queue.
static void drainStagedFolds() {
  char staged[kFoldStageSlots][32];
  portENTER_CRITICAL(&g_foldSpin);
  memcpy(staged, g_manualFoldChat, sizeof(staged));
  memset(g_manualFoldChat, 0, sizeof(g_manualFoldChat));
  portEXIT_CRITICAL(&g_foldSpin);
  for (int i = 0; i < kFoldStageSlots; i++) {
    if (!staged[i][0]) continue;
    if (g_foldDriver->requestManual(staged[i]) == agent::FoldDriver::Manual::Full)
      foldTell(staged[i], "\xE2\x9A\xA0 Other summaries are waiting. Try again in a few minutes.");
  }
  if (g_foldResetStaged) {
    g_foldResetStaged = false;
    g_foldDriver->resetDailyCount();
    alog("fold: FOLDRESET - today's paid fold count restarts at 0");
  }
}

// The fold pump (lane F of the history fix): at most one rung of one fold cycle per
// pass, tg_poll only (fold_driver.h). Runs at the END of pollJobs (after
// loops::checkDue consumed its FireOutcome, so fold spend can never leak into loop
// metering). Write order on success: summary blob FIRST, then the chain reset, then
// the ev:compact boundary row (foldLanded). noinline (lane STACK): inlined, its locals
// grew pollJobs' frame from 48 to 240 B, and pollJobs sits on tg_poll's worst path (a
// scheduled turn's head round), 288 B over the floor before this lane.
__attribute__((noinline)) static void compactTick() {
  if (!g_engine || !g_foldDriver) return;
  drainStagedFolds();
  if (g_engine->turnInFlight()) return;
  // A parked provider nothing else is calling is asked here, so it recovers by itself
  // (one request at most per pass; the fold waits for the next pass). It waits for the
  // owner: the request blocks this task, and this task reads the owner's messages.
  if (nimbus::net::staConnected() &&
      g_ownerFirst.parkRecheckMayStart(telegram::guardStatus().holding, millis()) && g_engine->recheckParkedHost())
    return;
  g_foldDriver->tick(millis(), memory::clockSynced() ? (uint32_t)time(nullptr) : 0);
  // The status's clock-derived parts (the manual cooldown, the badge's age) move by
  // themselves: refresh the web copy now and then.
  static uint32_t lastPublishMs = 0;
  if (millis() - lastPublishMs >= 5000) {
    lastPublishMs = millis();
    publishFoldWebStatus();
  }
}

// The conversation cache's idle fill (lane H, conv_cache.h): after a restart a chat's rows
// after thru are read back newest first, between turns (a turn reads at most one page
// itself): up to four pages of 24 rows a pass, within a quarter second (an idle pass comes
// once per Telegram long poll, so one page a pass would take over 15 minutes for a 200 KB
// history). Card reads only, no request: not gated on the owner. noinline: pollJobs sits
// on tg_poll's worst path, so its frame stays flat.
__attribute__((noinline)) static void historyCacheTick() {
  if (!g_engine) return;
  const uint32_t t0 = millis();
  for (int page = 0; page < 4 && g_engine->historyIdleStep(); page++)
    if (millis() - t0 >= 250) break;
}

int pollJobs() {
  if (!g_fabric || !g_jobs) return 0;
  drainStaged();      // apply web-staged directive/memory edits between turns
  drainTerminates();  // apply web/MCP-staged session.terminate on the journal's writer task
  drainUpdateNotice();  // the fixed post-update notice - tg_poll only
  dream::pumpDrill(); // a queued console DREAM drill - tg_poll only, never the loop task

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
  historyCacheTick();        // a restart's conversation cache fill, a page a pass
  g_ownerFirst.notePumpEnd();   // the next pass follows this cycle's poll and reply drain
  // Lane QW4 round 3: the stop list makes its own TLS requests, so it counts as active
  // work: the Telegram loop then frees the TLS arena and keeps the pump coming.
  return n + g_jobs->stoppingCount();
}

bool turnInFlight()   { return g_engine && g_engine->turnInFlight(); }

// The engine seam the common half calls (engine_api.h): ring arcs and the reply sinks.
bool engineRunning() { return g_engine != nullptr; }
bool engineReapStuckTurn(uint32_t nowMs) { return g_engine && g_engine->reapStuckTurn(nowMs); }
void engineSetHeadArc(bool on) { g_engine->setHeadArc(on); }
void engineNoteToolReplied(const std::string& text) { g_engine->noteToolReplied(text); }

String memorySnapshot() { return String(g_memEcho); }

String lastInstructions() {
  return g_engine ? String(g_engine->lastInstructions().c_str()) : String();
}

void requestConvReset() { g_convResetReq = true; }

// /clear (CUM-511): start a genuinely fresh conversation for THIS chat. The old
// clear only dropped the provider thread and the active task, while the head
// kept reading the chat's last 12 messages (rebuilt from episodic every turn),
// its compacted summary, and its running notes - "Cleared" with nothing reset.
// Now: a boundary row in the chat's own history (every conversation read stops
// there), this chat's provider thread, fold summary and running notes reset, and
// the scratchpad's active task cleared. Kept: long-term memory, the scratchpad
// tiers, files, and the episodic history itself (the owner's record - still
// searchable by memory.episodic, a recall path like vector memory). tg_poll only.
static void clearConversation(const String& chatId) {
  const std::string cid(chatId.c_str());
  const String boundary = memory::captureMessage(chatId.c_str(), "system", orch::MsgKind::Message,
                                                 String(agent::kClearBoundaryText), "",
                                                 String(agent::kClearBoundaryTag));
  if (g_engine) g_engine->clearChatConv(cid);   // this chat's provider-side thread only
  skills::dropChat(cid);   // lane I: /clear drops this chat's sticky skill attachments too
  g_mem.setModel(cid, std::string());            // this chat's running notes
  syncMemEcho();
  // A clear is not a fold: empty summary, zeroed counters, epoch 0 (never folded,
  // so the thrash guard cannot mistake the reset for a fold that failed to shrink).
  // The anchor is the boundary row (lane CTXR1): no later fold reads a row before it.
  g_folds.applyFold(cid, std::string(), 0, std::string(boundary.c_str()));
  if (g_foldDriver) g_foldDriver->noteCleared(cid);   // a cycle's slice is gone with it
  publishFoldAll(cid);
  {
    memory::Lock lk;
    memory::scratchpad().setActiveTask("");   // clears just the active line, tiers untouched
  }
  memory::persistScratchpad();
  alogf("orchestrator: /clear - %s conversation + active task cleared (memory/files kept)",
        chatId.c_str());
}

// The web app's "Clear conversation" button: the web chat's /clear, staged from
// any task (a flag the turn/poll task drains); see drainStaged().
void requestConvClear() { g_convClearReq = true; }

void requestMemoryClear() {
  // Zero the echo immediately so the UI reflects the clear without waiting for
  // the drain; the authoritative g_mem.clear() runs on the turn task.
  g_memEcho[0] = 0;
  g_memClearReq = true;
}

void noteConfigChanged() { g_cfgReloadReq = true; }

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
