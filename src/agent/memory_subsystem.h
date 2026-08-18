#pragma once
#include <Arduino.h>
#include <FS.h>

#include <string>

#include "nimbus/orch/episodic.h"
#include "nimbus/orch/mem_config.h"
#include "nimbus/orch/scratchpad.h"
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/vector_memory.h"

// memory_subsystem - the device integration hub for the orchestrator "World"
// memory (Part B). It owns the portable engines (VectorMemory + Scratchpad +
// MemConfig + episodic store), binds the real provider embedder
// (agent::embeddings), persists to flash, and registers the memory.* MCP tools.
// Deliberately DECOUPLED from the turn loop: it is driven by the web dashboard
// and the LAN MCP endpoint (handleMcp), so it goes live without touching the
// (watchdog-sensitive) orchestrator turn path. The turn loop can later read
// context()/registry() to inject recall + expose tools - that wiring is additive.
//
// Persistence: vectors -> LittleFS /data/orchvec.bin (binary blob), scratchpad
// -> NVS. SD is the design's scale target for the vectors; LittleFS works with no
// SD and holds the thousands of small int8 vectors a desk device accumulates.
namespace agent {
namespace memory {

// Choose the filesystem for the vector/episodic blobs (and, via dataFs(), voice
// clips). Call BEFORE begin() with the SD card's FS when a card is mounted (16 GB+
// vs LittleFS's few MB); defaults to LittleFS so a card-less device still works.
void      setDataFs(fs::FS& fs);
fs::FS&   dataFs();   // the active store (SD when mounted, else LittleFS)
// DANGER: recursively delete the durable store root (/mem on SD, else /data) -
// every vector blob, episodic day-stream, file, and media blob. A reboot must
// follow so the in-RAM engines reload empty. Web Danger-zone action only.
bool      eraseDurableStore();

// Load config + persisted state, bind the embedder, register tools. Safe to call
// once from setup() in either mode (Notifier can still browse an existing store).
void begin();

// Concurrency guard for the shared engines (VectorMemory / Scratchpad / episodic).
// They are touched by TWO tasks - the AsyncTCP web/MCP handler task and the Telegram
// turn/poll task - so unguarded access races (iterator invalidation / heap corruption).
// lock()/unlock() wrap a RECURSIVE FreeRTOS mutex (re-entrant, so nested calls like
// handleMcp -> persistVectors can't self-deadlock). Hold it only around in-RAM engine
// ops, NEVER across a blocking network embed. Prefer the RAII `Lock` guard.
void lock();
void unlock();
struct Lock {
  Lock() { lock(); }
  ~Lock() { unlock(); }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
};

// Storage-tier status (docs/orchestrator-storage.md). haveSd(): bulk blobs are on
// the SD card (/mem); false = degraded on internal flash (/data), vectors capped.
// flashFull(): a degraded vector persist hit the LittleFS free floor and paused
// (surfaced in STATUS / the dashboard banner). Both resolve after begin().
bool haveSd();
bool flashFull();

// SD graceful degradation (the HIL test spec). A card present at boot can
// vanish mid-run; the subsystem degrades to no-card behaviour WITHOUT a reboot and
// recovers when the card answers again - no data loss (episodic appends fall back
// to the RAM ring; vectors re-cap). sdLost(): the card was mounted at begin() but
// is currently unusable (demoted). tickSdHealth(): call from the main loop - an
// adaptive liveness probe that drives promote (and idle demote); watch the sdLost()
// edge for the user-facing CTA. noteSdIoResult(): fed by real SD writes for
// responsive demote. demoteSd()/promoteSd(): explicit transitions (probe/button).
bool sdLost();
void tickSdHealth(uint32_t nowMs);
void noteSdIoResult(bool ok);
void demoteSd();
bool promoteSd();
bool forceSdProbe();   // test/console: one immediate probe+note (bypasses the cadence gate)

// Append one battery telemetry row to the SD discharge log (/mem/battery/d<N>.jsonl,
// day-partitioned). SD-gated (no-op with no card / while demoted). The caller logs
// DISCHARGING ticks only - bounds growth + captures the prediction-relevant data.
void appendBatteryHistory(uint32_t hours, uint16_t mv, uint8_t pct, const char* state);

// The registry with the memory.* tools (device.*/session.* are added elsewhere).
nimbus::orch::ToolRegistry& registry();

// Engine accessors (for the web dashboard's browse/edit endpoints).
nimbus::orch::VectorMemory&  vectors();
nimbus::orch::Scratchpad&    scratchpad();
nimbus::orch::MemConfig&     config();
nimbus::orch::EpisodicStore& episodic();

// Associative recall for a live turn (Phase 2): embed `queryText` and return up to
// `k` recalled memories formatted as prompt bullets ("[NN%] content"), filtered by
// the configured relevance threshold. k<=0 uses the configured retrieval_count.
// Fail-OPEN: returns {} on empty query / no embed key / embed timeout / empty
// store, so a turn is never blocked by recall. Bounded by the embed timeout.
// `who` bounds what this turn may recall (v3.7.0). The prompt's
// [RELEVANT MEMORIES] block MUST obey the same boundary as memory.search -
// otherwise the tool refuses a fact while the prompt hands it over anyway.
std::vector<std::string> recall(const String& queryText, int k = 0,
                                const nimbus::orch::Principal& who = {});

// Persist current state. persistVectors() writes the whole blob (O(n), cheap for
// the device's scale); called automatically after a mutating MCP call.
void persistVectors();
void persistScratchpad();
void persistMemConfig();  // retrieval knobs -> NVS (fixes silent reset-on-reboot)
void persistEpisodic();   // whole-blob to LittleFS /data/episodic.bin
void applyConfig();       // push MemConfig -> engines (VDB capacity cap); call after a config change

// Episodic auto-capture (Q2): the orchestrator calls these each turn so past
// messages/sessions become queryable (memory.episodic / GET /api/mem/episodic) and
// survive reboot. captureMessage persists after each append. Safe no-ops if the
// subsystem isn't begun. kind defaults to a plain chat Message.
void captureSession(const char* sessionId, const char* provider, const String& title);
// tags: optional freeform labels on the row (the EpisodicMessage.tags field, which
// already round-trips both persistence formats). Convention: "from:<sender name>"
// on user rows - the unified chat view renders it as the sender label.
// Returns the captured row's id ("m%08x"), or "" when nothing was captured -
// the per-chat conversation window uses it to exclude the in-flight user row
// (captured BEFORE compose, deliberately durable-first). All prior callers
// ignore the return value (additive change).
String captureMessage(const char* sessionId, const char* role, nimbus::orch::MsgKind kind,
                      const String& text, const String& blobPath = "", const String& tags = "");

// Glass Box: reserve a row id WITHOUT writing a row ("m%08x", same counter as
// captureMessage). Used to mint a boot-stable turn id for turns that have no
// user row to key on (scheduled loops, synthesis/dream). Safe against the boot
// recovery path - any row written later in the turn takes a HIGHER id, so
// nextIdHint() still resumes past all persisted history.
String mintRowId();

// Device-event timeline row (Glass Box A3): a kind=Log row in the reserved
// "system" session, tagged "ev:<ev>" - boots, mode switches, OTA lifecycle, SD
// demote/promote, dream outcomes. Queryable by the model via memory.episodic
// (session "system", since_hours/before_hours) and rendered by the web timeline.
// ev: short slug ("boot","mode","ota","sd","dream","error").
void captureEvent(const char* ev, const String& text);
// Flush events held while the clock was unsynced (pre-SNTP boot rows). Cheap
// no-op when none pending; called from the main loop tick + on later captures.
void flushPendingEvents();

// Glass-box trace gate (A4): true when trace rows may write (SD append-log live
// + owner `otrace` knob ON). The hook wiring checks this per fire.
bool traceActive();

// ---- Glass Box P3: per-turn "dossier" files (/mem/trace/<turnId>.txt) --------
// The full turn anatomy (system prompt + per-turn input + raw model output +
// tool-loop transcript). Too big for a JSONL row, so it lives as a file keyed by
// the turn id and referenced from the ev:turnend row's blobPath.
// writeTraceFile keeps a bounded RING (oldest evicted) so trace can never grow
// without limit; both are no-ops unless traceActive(). Called on tg_poll only.
bool  writeTraceFile(const String& turnId, const char* buf, size_t len);
// Read one dossier into a PSRAM buffer (caller frees). Returns nullptr if absent.
char* readTraceFilePs(const String& turnId, size_t& outLen);
// Ring bounds - a dossier is capped by the caller; these bound the directory.
static const int    kTraceFilesMax = 16;
static const size_t kTraceBytesMax = 768 * 1024;

// Glass Box P4: park the FULL text of a clipped trace row in the existing
// content-addressed blob store (/mem/blobs), returning the path for the row's
// blobPath. Dedup + the 6 h retention sweep come free with that store. Returns
// "" with no SD (the row keeps its clip - degraded, never broken).
String persistTraceBlob(const String& text);

// True once SNTP has produced a real epoch (~2020+). Pre-sync, decay/prune/
// boost-on-use are guarded off (Release C3) - boot-relative hours vs stored
// real epochs would mis-age every entry.
bool clockSynced();

// Capture a media message with a DURABLE content-addressed sidecar under
// /mem/blobs (docs/orchestrator-storage.md §4): store `bytes` (voice note, TTS
// reply, artifact), then record an episodic row referencing it by blobPath. `ext`
// is the file extension without a dot ("ogg","mp3","wav"). SD-gated: a no-op with
// no card (media stays ephemeral in degraded mode), so the card-less path is intact.
void captureMedia(const char* sessionId, const char* role, nimbus::orch::MsgKind kind,
                  const String& text, const uint8_t* bytes, size_t len, const char* ext);

// Like captureMedia but STREAMS a file already on LittleFS (`srcPath`) into the
// content-addressed sidecar in a small buffer - never loading the whole file into RAM
// (for large media like the ~480 KB mic recording). Content-address dedup: an
// identical blob isn't re-copied. Returns true if a durable row was captured. SD-gated
// no-op (returns false with no card). The caller keeps owning/deleting `srcPath`.
bool captureMediaFile(const char* sessionId, const char* role, nimbus::orch::MsgKind kind,
                      const String& text, const char* srcPath, const char* ext);

// Retention maintenance (§4): on the SD append-log, prune day-streams older than
// `retentionDays` and reference-count-scan away unreferenced blob sidecars. Returns
// the number of messages pruned (0 with no SD append-log). Safe to call periodically.
int pruneRetention(int retentionDays = 30);

// Handle one MCP JSON-RPC request (the LAN endpoint + the web bridge both call
// this); auto-persists after a mutating tools/call. Returns the response string.
std::string handleMcp(const std::string& jsonRpcRequest,
                      const nimbus::orch::Principal& who);

// Dashboard stats.
struct Stats {
  int  vectorCount = 0;
  int  scratchItems = 0;
  int  episodicMsgs = 0;
  bool embedAvailable = false;   // configured provider has a key
  bool embedLocked = false;      // a vector has been embedded (config is frozen)
  bool sdPresent = false;        // bulk store on SD (/mem) vs degraded flash (/data)
  bool flashFull = false;        // degraded vector persist paused (LittleFS floor)
  int  maxVectors = 0;           // effective capacity cap (tier-aware)
  // v4.0.0 deep history: did the boot scan stop at its budget (older rows are on
  // the card but not indexed), and the oldest day it DID index. Surfaced so
  // "where did my old history go" has an answer instead of looking like loss.
  bool epiHydrateTruncated = false;
  int  epiIndexFloorDay = 0;
};
Stats stats();

// Wall-clock hours source for TTL stamping (NTP time if synced, else boot-
// relative). Exposed so the web layer stamps episodic rows consistently.
uint32_t nowHours();

#ifdef NIMBUS_TEST
// HIL max-memory seams (plan §1f): bulk-fill stores with synthetic tagged rows so
// cap boundaries are reachable in seconds with zero LLM spend. CHUNKED - each call
// does at most 200 rows (console/watchdog-safe); the harness loops the command.
// Episodic rows: session "hiltest-epi", kind=message, text "MARKER-<i> <pad>".
// Vector rows: deterministic pseudo-random int8 embeddings, importance ramps so
// cap-eviction order is assertable. Returns rows added this call.
int testFillEpisodic(int n, int bytesPerRow, const char* exactText = nullptr);
int testFillVectors(int n, const char* ns = "", float importance = 0.0f);
#endif

}  // namespace memory
}  // namespace agent
