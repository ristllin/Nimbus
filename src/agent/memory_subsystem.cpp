#include "memory_subsystem.h"

#include <esp_littlefs.h>    // esp_littlefs_format_partition - the scrub's empty FS
#include <esp_partition.h>   // esp_partition_erase_range - the scrub erases every block

#include "nimbus/orch/reset_plan.h"   // scrubChunk: the erase plan (host-tested)


#include <LittleFS.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>                  // feed the WDT during a long durable-store wipe
#include <esp_heap_caps.h>                 // the recall and drain largest-block floor
#include <time.h>
#include <utility>

#include "adapters/embeddings.h"
#include "adapters/image_gen.h"   // CUM-508: imagegen::available() opens image.generate's gate
#include "nimbus/speaker_reply.h"   // speakerFailText - reply.speak's honest failure
#include "adapters/tavily.h"
#include "nimbus/harness/websearch.h"  // registerWebSearchTool (portable, host-tested)
#include "nimbus/orch/tool_gates.h"     // CUM-508: the provider-gated tool table
#include "memory_restore.h"               // CUM-406: portable restore parse/apply core
#include "../sys/ps_json.h"               // CUM-406: PSRAM JsonDocument for a large restore body
#include "health.h"                       // system.health tool (P5)
#include "skills.h"                        // skill.list/get/save/delete tools (P7 + v4)
#include "telegram.h"                      // owner alert on agent skill.save
#include "agent_config.h"
#include "../sys/agent_log.h"
#include "epi_fs_arduino.h"
#include "../sys/tls_arbiter.h"           // the index backfill yields to any open TLS session
#include "orchestrator.h"
#include "loops_subsystem.h"   // loop.create/list/cancel tools -> Local Loops
#include "nimbus/harness/dream.h"  // DREAMING: reserved-loop cancel refusal
#include "store.h"
#include "nimbus/docs_pack.h"             // docs.list/search/read - embedded device docs (W13)
#include "nimbus/fault.h"                 // resilience: simulated SD/memory faults
#include "nimbus/sd_health.h"             // SD demote/promote debounce (graceful degradation)
#include "storage_tier.h"                 // CUM-405: pure storage-tier decision (host-tested)
#include "fs_atomic.h"                    // writeFileAtomic - the scratchpad file
#include "nimbus/store/moved_store.h"     // the scratchpad's move out of NVS (load/persist/retire)
#include "../sys/config_nvs.h"            // notePersist + the NVS reserve (nvsAdmit)
#include "../sys/ota_update.h"            // imageValidAtBoot - when the legacy copy may go
#include <solide/storage.h>               // re-probe the SD card on recovery
#include "nimbus/orch/blob_store.h"
#include "nimbus/orch/episodic_log.h"
#include "nimbus/orch/memory_tools.h"
#include "nimbus/orch/billing_refusal.h"   // one owner notice per billing episode (shared with chat)
#include "nimbus/orch/pending_embed.h"     // memory writes that survive an embedding outage
#include "nimbus/orch/result_store.h"   // registerResultTools (results.get/list)
#include "nimbus/orch/session_tools.h"
#include "nimbus/orch/pending_events.h"   // pre-sync events survive a reset (crash forensics F3)
#include "nimbus/sys/loop_guard.h"        // the loop never waits for the card lock
#include "nimbus/sys/boot_record.h"       // fieldValue: a tool name is model text

// The Arduino core's loop task (setup() and loop() run on it); the core defines it in
// its main.cpp and no header declares it.
extern TaskHandle_t loopTaskHandle;

namespace agent {
namespace memory {

static void registerTenantTools();   // v3.7.0 admin-by-conversation (defined below)
static void loadPending();           // the pending-embed queue (defined below)

using namespace nimbus::orch;

namespace {
VectorMemory          g_vec;
MemOpCounters         g_memOps;         // memory.write/update outcomes since boot (lane M; RAM only)
// Cold store for TTL-expired memories (CUM-225). SD-only: attached as g_vec's prune
// sink and exposed to the model ONLY when a card is present, so a card-less device
// drops entries at TTL exactly as before. Lives in PSRAM like the live set.
nimbus::orch::VectorArchive g_archive;
Scratchpad            g_scratch;
// The bytes the scratchpad file last took (a persist of the same bytes writes nothing)
// and its failure line (once a minute, with the count).
nimbus::store::LandedFingerprint g_scratchLanded;
nimbus::store::FailureLog        g_scratchFailLog;
#ifdef NIMBUS_TEST
// FAULT scratch_fail: the scratchpad file write fails (flash full or failing), so the
// RAM-only path runs on demand whatever the NVS state. Test build only.
bool g_scratchFault = false;
#endif
MemConfig             g_cfg;
// Device cap: bound the ring so the whole-blob rewrite per turn (persistEpisodic)
// stays small on LittleFS (~500 msgs * ~150 B ~= 75 KB). The SD append-log store
// lifts this cap (this ring is only the no-card fallback).
InMemoryEpisodicStore g_epi{500};     // no-SD degraded store + SD-migration scratch
// SD-primary episodic backend (docs/orchestrator-storage.md §3): append-log
// day-streams under /mem/episodic, uncapped history. Constructed only when a card
// is mounted; on a card-less board these stay null and g_epi is the active store.
// ⚠ SD-gated -> compile-verified, not yet exercised on hardware (cardType=0 bench).
ArduinoEpiFs*          g_epiFs = nullptr;
AppendLogEpisodicStore* g_epiLog = nullptr;
EpisodicStore*         g_epiActive = &g_epi;  // the store the World + capture use
uint32_t              g_epiSeq = 0;   // monotonic message-id counter
const char* kEpiDir     = "/mem/episodic";
const char* kBlobDir    = "/mem/blobs";
ToolRegistry          g_reg;
bool                  g_begun = false;
fs::FS*               g_fs = &LittleFS;   // data store: SD when mounted, else LittleFS
SemaphoreHandle_t     g_memMux = nullptr; // recursive: guards g_vec/g_epi/g_scratch across tasks
bool                  g_haveSd = false;   // resolved in begin(): is the store the SD card?
bool                  g_tierResolved = false;  // begin() has resolved g_haveSd
// CUM-405: no card mounted this boot, yet evidence (a card seen last boot, or a
// non-empty /mem/vectors.bin still readable off the card) says memories are on the
// card. Resolved once in begin() and surfaced as the loud Memory-panel banner so a
// flaky/undetected card never reads as a silent empty store (looks like data loss).
bool                  g_sdMissing = false;
bool                  g_flashFull = false;// degraded LittleFS persist hit the free-space floor
// Set true just before wiping the durable store (Erase Storage). EVERY SD writer
// checks it and refuses, so the media lane and any raced persist can't write a
// blob into a store mid-wipe (or resurrect one after). Never cleared - a reboot
// follows immediately. Its two chokepoints are writeBlobAtomic (all persists +
// sidecars) and captureMediaFile (the one direct SD streamer).
volatile bool         g_erasing = false;

// Runtime SD graceful degradation: a card present at begin() can vanish mid-run
// (cold joint / pull). The tracker debounces IO outcomes (2 in a row each way)
// into demote/promote edges; effHaveSd() honours lost() so a demote instantly
// drops the memory system into no-card behaviour WITHOUT flipping g_haveSd (the
// begin()-resolved paths stay put, valid again on recovery). See sd_health.h +
// the HIL test spec.
nimbus::SdHealthTracker g_sdHealth(2, 2);
uint32_t              g_lastSdProbeMs = 0;

// Storage tiering (docs/orchestrator-storage.md). Bulk durable blobs live under
// /mem on the SD card; with no card they fall back to the legacy LittleFS /data
// paths (the migration source). Resolved once in begin() from g_haveSd.
const char* kVecSdPath  = "/mem/vectors.bin";
const char* kEpiSdPath  = "/mem/episodic.bin";
const char* kArcSdPath  = "/mem/archive.bin";    // TTL-expired cold store (SD-only, CUM-225)
const char* kVecLfsPath = "/data/orchvec.bin";   // pre-SD location (migration source)
const char* kEpiLfsPath = "/data/episodic.bin";
std::string g_vecPath   = kVecLfsPath;   // active vector-blob path (set in begin())
std::string g_epiPath   = kEpiLfsPath;   // active episodic-blob path (set in begin())
std::string g_arcPath   = kArcSdPath;    // active archive-blob path (SD-only, set in begin())
const char* kScratchNs = "orchmem";
const char* kScratchKey = "scratch";     // legacy string key (≤4000 B) - read-only fallback
const char* kScratchBKey = "scratchB";   // v4.1 bytes blob (no 4000 B string limit)
const char* kMemCfgKey  = "memcfg";   // persisted MemConfig blob (same NVS namespace)
const char* kSdSeenKey  = "sdSeen";   // CUM-405: was an SD card mounted the previous boot?
// The scratchpad lives in this LittleFS file since v4.5.14 (it was rewritten in NVS on
// every turn, and on a full partition every change lived in RAM only). The legacy
// NVS keys above are read once when the file is missing and erased only on a boot
// whose image was already valid (nimbus/store/moved_store.h).
const char* kScratchPath = "/data/scratchpad.bin";

// The scratchpad's moved-store binding: the file, and the legacy NVS bytes blob (or
// the older string key).
struct ScratchIo {
  bool readFile(std::string& out) {
    if (LittleFS.totalBytes() == 0 || !LittleFS.exists(kScratchPath)) return false;
    File f = LittleFS.open(kScratchPath, FILE_READ);
    if (!f) return false;
    out.resize(f.size());
    const size_t n = out.empty() ? 0 : f.read((uint8_t*)&out[0], out.size());
    f.close();
    return n == out.size();
  }
  bool writeFileAtomic(const std::string& v) {
#ifdef NIMBUS_TEST
    if (g_scratchFault) return false;
#endif
    if (LittleFS.totalBytes() == 0) return false;   // not mounted: nothing can be stored
    LittleFS.mkdir("/data");
    return agent::writeFileAtomic(kScratchPath, v, "scratchpad", false);
  }
  bool readNvs(std::string& out) {
    Preferences p;
    if (!p.begin(kScratchNs, true)) return false;
    bool have = false;
    const size_t blen = p.getBytesLength(kScratchBKey);
    if (blen > 0) {
      out.resize(blen);
      have = p.getBytes(kScratchBKey, &out[0], blen) == blen;
    } else if (p.isKey(kScratchKey)) {
      const String legacy = p.getString(kScratchKey, "");
      out.assign(legacy.c_str(), legacy.length());
      have = true;
    }
    p.end();
    return have;
  }
  bool eraseNvs() {   // erases only free entries: always lands on a full NVS
    Preferences p;
    if (!p.begin(kScratchNs, false)) return false;
    if (p.isKey(kScratchBKey)) nimbus::sys::notePersist(p.remove(kScratchBKey), kScratchBKey);
    if (p.isKey(kScratchKey)) nimbus::sys::notePersist(p.remove(kScratchKey), kScratchKey);
    const bool gone = !p.isKey(kScratchBKey) && !p.isKey(kScratchKey);
    p.end();
    return gone;
  }
};

// Degraded (no-SD) VDB cap: bound the durable LittleFS blob (~400 * ~297 B ~= 120 KB)
// so the working set + persist can't exhaust the few-MB internal flash. The full cap
// (g_cfg.maxVectors) applies only on the SD card. The working set itself lives in
// PSRAM (§2), so this cap is about flash-blob size, not RAM.
constexpr int    kDegradedMaxVectors = 400;
// Archive (cold store) FIFO cap. The card has room to spare; this bounds the blob a
// dream must rewrite when it archives, and the PSRAM the archive holds. Oldest-
// archived is evicted first when full (CUM-225).
constexpr int    kArchiveMaxEntries = 2000;
// Keep this many bytes free on LittleFS after a degraded vector persist; below it we
// refuse the write (the overflow guard the audit found missing).
constexpr size_t kFlashFreeFloor = 96 * 1024;

// ---- memory writes that survive an embedding outage (pending_embed.h) --------
// The queue of facts waiting for an embedding, its durable sidecar, and the RAM
// park memo. The sidecar lives on the card beside the vectors; with no card (or a
// card lost mid-run) it falls back to LittleFS, because an outage on a cardless
// device must not lose facts either. Never NVS.
nimbus::orch::PendingEmbeds g_pending;
nimbus::orch::EmbedPark     g_park;
MemoryContext               g_ctx;   // the context the tools were registered with (the drain reuses it)
const char* kPendSdPath  = "/mem/pending_embed.bin";
const char* kPendLfsPath = "/data/pendemb.bin";
bool        g_pendOnLfs  = false;   // the newest durable copy is the LittleFS fallback
uint32_t    g_lastDrainMs = 0;
// Every memory.write / memory.update outcome since boot (RAM only): the dream's
// end line reports the night's counts from the delta.
WriteOutcomes g_outcomes;
constexpr uint32_t kDrainEveryMs = 20000;   // at most one drain step per 20 s
constexpr uint32_t kDrainBootGraceMs = 60000;   // none in the first minute: let the boot settle
#ifdef NIMBUS_TEST
// FAULT embed_fail: only the embedding call fails, with Lumi's recorded refusal
// (13 Oct 2026), so the queue and its honest reply run on demand. Test build only.
// The flag is the fault registry's EMBED_FAIL bit, so FAULT clear ends it too.
const char* kEmbedFaultToken = "HTTP 429 credit_balance_exhausted";
#endif

// One embedding call for the memory engines, honoring the test fault.
std::vector<int8_t> embedRaw(const String& text, String& err) {
#ifdef NIMBUS_TEST
  if (nimbus::fault::active(nimbus::fault::EMBED_FAIL)) { err = kEmbedFaultToken; return {}; }
#endif
  return embeddings::embed(text, err);
}

// Bind the embedder: call the provider, and on the FIRST successful embedding
// freeze the embed config (set-once lock) so the web UI knows the VDB is now
// committed to this provider/model/dims. Signature matches the portable
// Embedder (std::string in); we bridge to the Arduino-String adapter call.
std::vector<int8_t> embedText(const std::string& text, std::string& err) {
  String e;
  std::vector<int8_t> v = embedRaw(String(text.c_str()), e);
  if (!v.empty() && !store::embedLocked()) store::setEmbedLocked(true);
  // Thread the real cause up to the memory tools (CUM-435) so they can report why
  // instead of a fixed guess; also keep the local log line.
  err = std::string(e.c_str());
  if (v.empty()) alogf("memory: embed failed: %s", e.c_str());
  return v;
}
}  // namespace

uint32_t nowHours() {
  time_t t = time(nullptr);
  // NTP-synced epoch (> 2001) -> real hours; else boot-relative (millis) so TTL
  // ordering still holds within a session.
  if (t > 1000000000) return (uint32_t)(t / 3600);
  return (uint32_t)(millis() / 3600000UL);
}

void setDataFs(fs::FS& fs) { g_fs = &fs; }
fs::FS& dataFs() { return *g_fs; }

static inline bool effHaveSd();   // defined below; eraseDurableStore() (here) sits above it

// Recursively delete everything under `path`. Returns true only if the whole
// subtree was removed. ⚠ File::name() returns the FULL path on SDFS but only the
// basename on LittleFS - normalize both by joining only when it isn't already
// absolute. Feeds the task WDT every few files: this runs on the main loop (the
// Erase Storage drain), whose 8 s watchdog a large /mem tree would otherwise trip
// mid-walk, leaving a half-erased store on reboot.
static bool rmTree(fs::FS& fs, const String& path) {
  File d = fs.open(path);
  if (!d) return false;
  if (!d.isDirectory()) { d.close(); return fs.remove(path); }
  bool ok = true;
  int fed = 0;
  for (File c = d.openNextFile(); c; c = d.openNextFile()) {
    String nm = c.name();
    String child = nm.startsWith("/") ? nm : (path + "/" + nm);
    bool dir = c.isDirectory();
    c.close();
    if (dir) ok = rmTree(fs, child) && ok;
    else     ok = fs.remove(child) && ok;
    if ((++fed & 0x0F) == 0) esp_task_wdt_reset();
  }
  d.close();
  ok = fs.rmdir(path) && ok;
  return ok;
}

bool eraseDurableStore() {
  Lock lk;   // block turn/web access to the engines during the wipe
  // Refuse honestly when the card that HELD the store is gone (demoted/pulled): a
  // hardcoded-success no-op would tell the owner their data is wiped while every
  // memory, file and photo survives on the reseatable card. Returning false lets
  // the caller surface the failure and NOT reboot into a "clean" device that isn't.
  if (g_haveSd && !effHaveSd()) { alog("memory: erase refused - storage not available"); return false; }
  // Arm the barrier BEFORE any delete so the Lock-free media lane and any raced
  // persist refuse for the rest of this boot (a reboot follows immediately).
  g_erasing = true;
  // Drop the in-RAM engines so a persist that was blocked on this Lock and slips
  // in after we release it would write EMPTY state, not resurrected data (the
  // barrier already refuses it; this is defense in depth + a consistent reload).
  g_vec.flushAll();
  g_archive.flushAll();   // drop the cold store too (the /mem tree wipe removes its blob)
  g_pending.flushAll();   // queued facts are memories too; the /mem wipe removes the SD copy
  g_park.clear();
  g_scratch.clearAll();
  LittleFS.remove(kPendLfsPath);   // the cardless fallback copy outlives a /mem wipe otherwise
  const char* root = g_haveSd ? "/mem" : "/data";
  const bool ok = !g_fs->exists(root) || rmTree(*g_fs, root);
  g_fs->mkdir(root);   // recreate the empty parent so post-reboot persists work
  alogf("memory: durable store erased (%s) ok=%d", root, (int)ok);
  // A partial wipe must not read as clean (2026-09-29 review): the caller
  // refuses the reset instead of rebooting a device that claims factory-fresh
  // while deletable data survived. An absent root counts as already-erased.
  return ok;
}

// Erase helpers that treat "was never there" as success: rmTree()/remove()
// return false for an absent path, and the factory sweep must only fail when
// data that EXISTS could not be deleted.
static bool rmTreeIfPresent(fs::FS& fs, const char* path) {
  return !fs.exists(path) || rmTree(fs, String(path));
}
static bool removeIfPresent(fs::FS& fs, const char* path) {
  return !fs.exists(path) || fs.remove(path);
}

bool factoryEraseWouldRefuse() {
  // Pure truth table in storage_tier.h (host-tested); this feeds it live state.
  return factoryEraseMustRefuse(g_sdMissing, g_haveSd, effHaveSd());
}

// The sidecar sweep, shared by eraseAllPrivateData and the pre-restart mop
// (finalSweepPass): LittleFS /data, loose audio, /audio, /log on both tiers,
// SD /music + settings mirror. Absent paths count as erased.
// The internal-flash half (LittleFS). Skipped once scrubInternalFs() has run: the
// partition is unmounted and blank by then, and every call would log "not mounted".
static bool sweepInternalSidecars() {
  bool ok = rmTreeIfPresent(LittleFS, "/data");
  LittleFS.mkdir("/data");
  // Loose private files outside any tree: the last push-to-talk recording, the
  // last spoken reply / TTS audio (both formats: the test-console SPKSAY writes
  // /tts.wav on WAV providers), and Telegram staging a crash can strand.
  static const char* kLoosePrivate[] = {"/voice.pcm", "/reply.wav", "/reply.mp3",
                                        "/tts.mp3", "/tts.wav"};
  for (const char* p : kLoosePrivate) ok = removeIfPresent(LittleFS, p) && ok;
  ok = rmTreeIfPresent(LittleFS, "/audio") && ok;   // tgvoice.ogg / tgattach.bin staging
  // Durable logs carry sender names, chat ids, and message/transcript snippets -
  // wipe the /log tree on BOTH tiers (errlog writes to SD when present, else
  // LittleFS; a device that changed tiers can have both).
  ok = rmTreeIfPresent(LittleFS, "/log") && ok;
  return ok;
}

// The SD card half.
static bool sweepCardSidecars() {
  if (!g_haveSd || !effHaveSd()) return true;
  bool ok = rmTreeIfPresent(*g_fs, "/log");
  ok = rmTreeIfPresent(*g_fs, "/music") && ok;    // owner-uploaded music
  ok = rmTreeIfPresent(*g_fs, "/memory") && ok;   // solide settings mirror (nimbus_cfg.bin)
  return ok;
}

static bool sweepSidecars() {
  bool ok = sweepInternalSidecars();
  return sweepCardSidecars() && ok;
}

// Set once scrubInternalFs() has unmounted LittleFS: from then on only the card is
// left for the final sweep to mop.
static bool g_internalFsScrubbed = false;

bool finalSweepPass() {
  // The last word before ESP.restart() (release-gate verifier, 2026-10-04): any
  // writer that slipped the quiesce timeout and the erasing() barriers (trace
  // dossiers, usage history, loops, TTS files, staging audio, the safety log)
  // wrote into trees this pass now erases AGAIN, milliseconds before the
  // restart. Best-effort by design - the primary guarantee is quiesce+barriers;
  // a false here is loudly Serial-logged by the caller but cannot refuse (the
  // NVS erase has already happened).
  esp_task_wdt_reset();
  // After the scrub the internal file area is unmounted and blank (and nothing can
  // write it again before the restart), so only the card half is left.
  bool ok = g_internalFsScrubbed || sweepInternalSidecars();
  ok = sweepCardSidecars() && ok;
  // Re-sweep the WHOLE store root, not just /mem/trace: the skills and files
  // tools recreate their own directories under /mem and nothing gates them
  // (verifier round 2). The store is already empty, so this is cheap, and it
  // catches any writer added later without this list needing to know.
  if (g_haveSd || !g_internalFsScrubbed) {
    const char* root = g_haveSd ? "/mem" : "/data";
    ok = rmTreeIfPresent(*g_fs, root) && ok;
    g_fs->mkdir(root);   // the next boot expects the empty parent
  }
  return ok;
}

// LittleFS's partition: memory::begin() mounts it with LittleFS.begin()'s default
// label, so the scrub erases the same partition the file system lives on.
static const char* kLfsPartitionLabel = "spiffs";

bool scrubInternalFs() {
  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kLfsPartitionLabel);
  if (!part) {
    alog("memory: no internal file area partition - nothing to erase");
    return true;
  }
  // Unmount under the engine lock: the web/MCP memory handlers persist through it,
  // so none of them is inside LittleFS at that instant, and once it is unmounted
  // nothing can reach the partition (nothing remounts it before the restart).
  lock();
  LittleFS.end();   // no-op when it was never mounted (Notifier mode)
  g_internalFsScrubbed = true;
  unlock();
  bool erased = true;
  uint32_t off = 0, len = 0;
  for (uint32_t i = 0; nimbus::orch::scrubChunk(part->size, i, off, len); ++i) {
    esp_err_t e = esp_partition_erase_range(part, off, len);
    if (e != ESP_OK) e = esp_partition_erase_range(part, off, len);   // one retry
    erased = (e == ESP_OK) && erased;
    esp_task_wdt_reset();   // one 64 KB block erase can take up to ~2 s
  }
  // An empty, valid file system for the next boot, so it mounts without a
  // mount-failure-then-format pass. Formats by partition: the label-based
  // LittleFS.format() has no label when Notifier mode never mounted it. A format
  // failure leaves only erased blocks (nothing private), and the next boot's
  // mount formats them, so it is logged but does not fail the scrub.
  const bool formatted = esp_littlefs_format_partition(part) == ESP_OK;
  alogf("memory: internal file area erased (%u bytes) erased=%d formatted=%d",
        (unsigned)part->size, (int)erased, (int)formatted);
  return erased;
}

bool eraseAllPrivateData() {
  // Adopt the live card routing before deciding anything: setDataFs() runs in
  // BOTH modes at boot, but g_haveSd is only resolved in begin(), which runs
  // Orchestrator-web-only. Without this a Notifier-mode factory reset erased
  // SD:/data (nothing) and left SD:/mem intact while reporting success
  // (2026-09-29 review, HIGH).
  if (!g_haveSd && g_fs != &LittleFS) g_haveSd = true;
  // Refuse while the memories could survive on a reseatable card this erase
  // cannot reach: no card mounted this boot but evidence a card holds data
  // (the CUM-405 banner predicate), or the store's card demoted/pulled since
  // boot. The caller aborts the whole reset and tells the owner to reseat.
  if (factoryEraseWouldRefuse()) {
    alog("memory: factory erase refused - memories on an unreachable card");
    return false;
  }
  // The store next: its erase can also be refused, and a refusal must abort
  // the whole reset before anything else is half-erased.
  if (!eraseDurableStore()) return false;
  // LittleFS /data survives eraseDurableStore whenever the store lives on SD. It
  // holds the RBAC table (tenants.txt - the old owner's Admin row), chat summaries
  // and per-chat memory (chatsum.txt/orchmem.txt), routines (loops.json), usage
  // history, and the legacy pre-SD memory blobs (orchvec.bin/episodic.bin) whose
  // survival makes the migration path re-import the "erased" memories into the
  // empty card on the next boot.
  esp_task_wdt_reset();   // the quiesce may have spent most of the loop budget
  bool ok = sweepSidecars();
  if (!ok) {
    // A delete failed on data that exists. Same contract as the refusals above:
    // never reboot a device that reads factory-fresh while private data survived.
    alog("memory: factory sweep INCOMPLETE - a delete failed; reset aborted");
    return false;
  }
  alog("memory: factory sweep done (data/audio/log cleared)");
  return true;
}

// ---- loop guard (nimbus/sys/loop_guard.h) ------------------------------------
namespace {
LoopGuardStats g_loopGuard;                     // counters, read by loopGuardStats()
portMUX_TYPE g_loopGuardMux = portMUX_INITIALIZER_UNLOCKED;
nimbus::sys::LockHoldMonitor g_holdMon;         // under the card lock (the holder updates it)
bool g_blockingTakeLogged = false;

// The loop task, while the task watchdog watches it (setup() and the flows that pause
// the watchdog, like hold-to-talk recording, may block as before).
bool onWatchedTask() {
  return loopTaskHandle && xTaskGetCurrentTaskHandle() == loopTaskHandle &&
         esp_task_wdt_status(nullptr) == ESP_OK;
}
bool ownsCardLock() {
  return g_memMux && xSemaphoreGetMutexHolder(g_memMux) == xTaskGetCurrentTaskHandle();
}
}  // namespace

// A blocking take on the watched loop: the bug class loop_guard.h describes. Counted,
// and the caller named once per boot; the take itself still happens (callers assume
// the lock is held), and the watchdog stays the backstop.
static void noteWatchedBlockingTake(uintptr_t caller) {
  portENTER_CRITICAL(&g_loopGuardMux);
  g_loopGuard.watchedBlockingTakes++;
  g_loopGuard.lastBlockingCaller = caller;
  portEXIT_CRITICAL(&g_loopGuardMux);
  if (g_blockingTakeLogged) return;
  g_blockingTakeLogged = true;
  alogf("memory: the main loop waited for the card lock (caller 0x%08x)", (unsigned)caller);
}

void lock() {
  if (!g_memMux) return;
  if (onWatchedTask() && !ownsCardLock()) noteWatchedBlockingTake((uintptr_t)__builtin_return_address(0));
  xSemaphoreTakeRecursive(g_memMux, portMAX_DELAY);
}
void unlock() { if (g_memMux) xSemaphoreGiveRecursive(g_memMux); }

LoopLock::LoopLock(uint32_t maxWaitMs) {
  const uint32_t waitMs = nimbus::sys::lockWaitMs(onWatchedTask(), ownsCardLock(), maxWaitMs);
  if (waitMs == UINT32_MAX) {
    lock();
    held_ = true;
    return;
  }
  held_ = tryLock(waitMs);
  if (held_) return;
  portENTER_CRITICAL(&g_loopGuardMux);
  g_loopGuard.loopBusySkips++;
  portEXIT_CRITICAL(&g_loopGuardMux);
}

LoopLock::~LoopLock() {
  if (held_) unlock();
}

LoopGuardStats loopGuardStats() {
  portENTER_CRITICAL(&g_loopGuardMux);
  const LoopGuardStats s = g_loopGuard;
  portEXIT_CRITICAL(&g_loopGuardMux);
  return s;
}

// The holder side: called by handleMcp while it still holds the card lock, so the line
// reaches the durable log (its writer takes the same lock and re-enters).
static void noteToolHold(const std::string& tool, uint32_t holdMs) {
  const bool logIt = g_holdMon.note(tool, holdMs, millis());
  if (holdMs < nimbus::sys::kLongHoldMs) return;
  // Built before the critical section: nothing may allocate with the spinlock held.
  const std::string what = nimbus::sys::fieldValue(g_holdMon.longestWhat(), 31);   // model text
  portENTER_CRITICAL(&g_loopGuardMux);
  g_loopGuard.longHolds = g_holdMon.longHolds();
  g_loopGuard.longestHoldMs = g_holdMon.longestMs();
  strlcpy(g_loopGuard.longestHoldTool, what.c_str(), sizeof g_loopGuard.longestHoldTool);
  portEXIT_CRITICAL(&g_loopGuardMux);
  if (logIt) alog(nimbus::sys::longHoldLine(tool, holdMs).c_str());
}

// Timeline events the main loop could not write because the card lock was busy (a
// turn's tool call held it). Fixed slots copied under a spinlock (no allocation in the
// critical section); the next tickSdHealth pass that gets the lock writes them. A full
// park drops the newest and counts it.
namespace {
struct ParkedEvent {
  char ev[16];
  char text[120];
};
constexpr size_t kParkedEvents = 4;
ParkedEvent g_parked[kParkedEvents];
size_t g_parkedCount = 0;
}  // namespace

static void parkEvent(const char* ev, const String& text) {
  portENTER_CRITICAL(&g_loopGuardMux);
  if (g_parkedCount < kParkedEvents) {
    ParkedEvent& p = g_parked[g_parkedCount++];
    strlcpy(p.ev, ev ? ev : "misc", sizeof p.ev);
    strlcpy(p.text, text.c_str(), sizeof p.text);
    g_loopGuard.parkedEvents++;
  } else {
    g_loopGuard.droppedEvents++;
  }
  portEXIT_CRITICAL(&g_loopGuardMux);
}

// Caller holds the card lock (tickSdHealth's LoopLock): captureEvent re-enters it.
static void drainParkedEvents() {
  for (;;) {
    ParkedEvent one;
    portENTER_CRITICAL(&g_loopGuardMux);
    const bool have = g_parkedCount > 0;
    if (have) {
      one = g_parked[0];
      for (size_t i = 1; i < g_parkedCount; ++i) g_parked[i - 1] = g_parked[i];
      --g_parkedCount;
    }
    portEXIT_CRITICAL(&g_loopGuardMux);
    if (!have) return;
    captureEvent(one.ev, String(one.text));
  }
}

#ifdef NIMBUS_TEST
namespace {
volatile uint32_t g_toolStallMs = 0;
}  // namespace
void testArmToolStall(uint32_t ms) { g_toolStallMs = ms; }
// Under handleMcp's card lock: hold it for the armed time, once, like a slow tool.
static void testToolStallPoint(const std::string& tool) {
  const uint32_t ms = g_toolStallMs;
  if (!ms) return;
  g_toolStallMs = 0;
  alogf("TOOLSTALL: %s holds the card lock for %u ms", tool.c_str(), (unsigned)ms);
  vTaskDelay(pdMS_TO_TICKS(ms));
}
#endif

// The card lock exists from the top of setup(), in BOTH modes (crash forensics F1):
// the durable log attaches to the SD card at its mount, long before begin(), and a
// Notifier unit never runs begin() at all. Without the mutex, lock()/tryLock()
// "succeed" as no-ops, so an early SD writer would open a two-writer window on the
// card. Idempotent; begin() calls it too. No task is created.
void lockInit() {
  if (!g_memMux) g_memMux = xSemaphoreCreateRecursiveMutex();
}

// True once begin() has resolved the storage tier (haveSd() means something).
bool storageResolved() { return g_tierResolved; }

// Timed, non-blocking acquire (CUM-401). lockInit() runs first thing in setup(), so the
// mutex normally exists for the very first log line; a call before that (static init)
// mirrors lock()'s null-handle contract and reports success, with unlock() a matching
// no-op. Otherwise wait at most timeoutMs.
bool tryLock(uint32_t timeoutMs) {
  if (!g_memMux) return true;
  return xSemaphoreTakeRecursive(g_memMux, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

// CUM-405 primary evidence: is a non-empty vector blob still readable off the card,
// even though it did not mount into our data FS this boot? This is the signal the
// banner leans on - it needs NO NVS write, so it fires on a full-NVS device (Lumi,
// CUM-389) where the sdSeen flag write silently drops.
//
// It routes through solide::storage, NEVER the raw Arduino SPI `SD` global: on an
// SDMMC board (Freenove/CYD, sdKind=Sdmmc) the card mounts via SD_MMC and `SD` is
// never begun, so SD.cardType() is ALWAYS CARD_NONE and the old SPI probe was dead
// in every reachable state (main.cpp:~3125 warns of exactly this). When the card is
// not currently mounted (the case whenever g_haveSd is false), do ONE non-destructive
// re-probe (end() then begin(), the same path /api/sdprobe uses) so a card that
// flaked on the first boot mount gets a second detection chance. Strictly READ-ONLY:
// we never format, write, or adopt the card (g_fs stays the resolved tier - the whole
// point of the banner). If we mounted it here, we end() again to leave storage in the
// exact card-less state main.cpp resolved, so nothing downstream sees a half-adopted
// card. Safe when the card is genuinely unreachable: begin() fails -> returns false.
static bool cardHoldsVectorData() {
  bool weMounted = false;
  if (!solide::storage::available()) {
    solide::storage::end();              // no-op if nothing is mounted; clears the latch
    weMounted = solide::storage::begin();// one re-probe; false again if truly no card
    if (!weMounted) return false;
  }
  bool has = false;
  if (solide::storage::cardType() != 0) {
    File f = solide::storage::activeFs().open(kVecSdPath, FILE_READ);
    if (f) { has = f.size() > 0; f.close(); }
  }
  if (weMounted) solide::storage::end();  // restore the card-less state we found
  return has;
}

// reply.speak's failure words. A synthesis failure is named as one, never as a broken
// speaker (owner 2026-07-16: the model kept blaming the speaker when synthesis failed);
// a speaker that would not start says so (22 Oct). CUM-523: point at reply.telegram
// only on a turn that was offered it.
static std::string speakFailure(const std::string& why) {
  const bool synth = why.rfind("text-to-speech failed", 0) == 0;
  const bool tooLong = why.rfind("the text is ", 0) == 0;
  std::string out = synth ? "on-device speech failed: " + why + " (the speaker itself is likely fine)"
                          : tooLong ? why : nimbus::sfx::speakerFailText(why);
  out += agent::orchestrator::telegramForTurn() ? "; reply.telegram with voice:true still delivers spoken audio"
                                                : "; reply in text instead";
  return out;
}

void begin() {
  if (g_begun) return;
  g_begun = true;
  lockInit();            // normally already made at the top of setup(); idempotent
  LittleFS.begin(true);  // ALWAYS mount internal flash: it holds config and is the
                         // migration source if we just switched to the SD card. When
                         // g_fs is the SD card, solide::storage::begin() mounted it.
  // Resolve the storage tier: setDataFs(SD) (called before begin() when a card
  // mounted) makes g_fs the SD card. On SD, bulk blobs live under /mem; with no
  // card they stay on the legacy LittleFS /data paths (also the migration source).
  g_haveSd = (g_fs != &LittleFS);
  g_tierResolved = true;
  // No card: the durable log's storage is LittleFS, ready now (with a card, main.cpp
  // attached it at the SD mount). Flushes the boot's early backlog.
  if (!g_haveSd) nimbus::errlog::attachStorage(LittleFS, false);
  g_vecPath = g_haveSd ? kVecSdPath : kVecLfsPath;
  g_epiPath = g_haveSd ? kEpiSdPath : kEpiLfsPath;
  LittleFS.mkdir("/data");   // routines, chat state, member table: LittleFS on every tier
  g_fs->mkdir(g_haveSd ? "/mem" : "/data");  // parent dir for the blobs; neither FS
                         // auto-creates it, so an absent dir made every persist fopen
                         // fail silently (state lost across reboot). Idempotent.

  // CUM-405: decide whether a card is "missing with data" this boot. The PRIMARY
  // signal is the read-only solide::storage probe (card present + non-empty
  // /mem/vectors.bin): it needs no NVS write, so it fires even on a full-NVS device
  // (Lumi, CUM-389) where the sdSeen flag below silently drops. The sdSeen flag is a
  // best-effort SECONDARY hint for the case where the card is momentarily unreachable
  // at probe time but was present the previous boot. Read the flag BEFORE overwriting
  // it, feed both into the pure decision (host-tested in test/test_storage_tier), then
  // persist the current mounted state (literal "previous boot" semantics) only when it
  // flipped - a write we do NOT depend on succeeding.
  {
    bool prevSdSeen = false;
    {
      Preferences p;
      if (p.begin(kScratchNs, true)) { prevSdSeen = p.getBool(kSdSeenKey, false); p.end(); }
    }
    TierInputs ti;
    ti.mountedSd = g_haveSd;
    ti.prevSdSeen = prevSdSeen;
    ti.cardHoldsData = g_haveSd ? false : cardHoldsVectorData();
    g_sdMissing = decideStorageTier(ti).sdMissingWithData;
    if (g_haveSd != prevSdSeen) {   // only touch flash when the state actually flipped
      Preferences p;
      if (p.begin(kScratchNs, false)) {
        const bool admitted = nimbus::sys::nvsAdmit(kScratchNs, kSdSeenKey, nimbus::store::NvsValKind::Int, 0);
        nimbus::sys::notePersist(admitted && p.putBool(kSdSeenKey, g_haveSd) == 1, kSdSeenKey);
        p.end();
      }
    }
    // Storage-tier decision at boot (the existing agent_log seam; lane L owns errlog).
    alogf("memory: storage tier=%s sd_mounted=%d prev_sd_seen=%d card_has_data=%d banner=%d",
          g_haveSd ? "SD /mem" : "flash /data (no SD)", (int)g_haveSd, (int)prevSdSeen,
          (int)ti.cardHoldsData, (int)g_sdMissing);
  }

  g_vec.configure(store::embedDims() > 0 ? store::embedDims() : EMBED_DEFAULT_DIMS);

  // Read a persisted blob from the active store's path, falling back to the LEGACY
  // LittleFS /data copy when the active store (SD) has none yet - a one-time
  // migration that carries the owner's existing memories onto the card (the next
  // persist writes them to /mem).
  auto loadBlob = [&](const char* sdPath, const char* lfsPath) -> String {
    File f = g_fs->open(g_haveSd ? sdPath : lfsPath, FILE_READ);
    if (!f && g_haveSd) f = LittleFS.open(lfsPath, FILE_READ);  // migrate from /data
    if (!f) return String();
    String b = f.readString();
    f.close();
    return b;
  };

  // Load persisted vectors.
  {
    String blob = loadBlob(kVecSdPath, kVecLfsPath);
    if (blob.length() && !g_vec.deserialize(std::string(blob.c_str(), blob.length())))
      alog("memory: vector blob partial/garbage - loaded what parsed");
  }
  // An EMPTY store takes the configured width (CUM-469): embedcfg reset=1 persists
  // the wiped store before the new width is applied, so its blob header carries the
  // OLD width (e.g. 256 while mistral-embed returns 1024). Host-tested rule.
  {
    const int was = g_vec.dims();
    if (g_vec.adoptWidthIfEmpty(store::embedDims() > 0 ? store::embedDims() : EMBED_DEFAULT_DIMS))
      alogf("memory: empty store width %d -> configured %d", was, g_vec.dims());
  }
  // Cold store for TTL-expired memories (CUM-225). SD-only: on a card-less device we
  // never attach it, so prune drops at TTL exactly as before. On the card, load the
  // archive blob and attach it as the prune sink so expiry MOVES entries here (the
  // embedding is preserved) instead of deleting them.
  g_arcPath = kArcSdPath;
  g_archive.configure(g_vec.dims());
  g_archive.setMaxEntries(kArchiveMaxEntries);
  if (effHaveSd()) {
    File f = g_fs->open(kArcSdPath, FILE_READ);
    if (f) {
      String blob = f.readString();
      f.close();
      if (blob.length() && !g_archive.deserialize(std::string(blob.c_str(), blob.length())))
        alog("memory: archive blob partial/garbage - loaded what parsed");
    }
    // Guard the width invariant: if the embed config changed since this archive was
    // written (its stored dims no longer match the live store), the archived vectors
    // are in a stale space - prune could no longer move same-width entries in, and a
    // search would reject the query. Drop the stale archive and re-configure.
    if (g_archive.dims() != g_vec.dims()) {
      g_archive.flushAll();
      g_archive.configure(g_vec.dims());
      alog("memory: archive dims stale (embed config changed) - archive reset");
    }
    g_archive.setMaxEntries(kArchiveMaxEntries);
    g_vec.setArchiveSink(&g_archive);
  }
  // Load the scratchpad (its file, else the legacy NVS copy: the moved-store rule)
  // and the retrieval config (NVS, orchmem namespace).
  {
    ScratchIo io;
    const nimbus::store::MovedLoad m = nimbus::store::loadMoved(io);
    if (m.value.length()) g_scratch.deserialize(m.value);
    // Loaded from the file and serializing back to the same bytes: nothing to write
    // until something changes (a legacy NVS load is written at the first persist).
    if (m.src == nimbus::store::MovedSource::File && g_scratch.serialize() == m.value)
      g_scratchLanded.noteLanded(m.value);
    // The legacy copy goes only on a boot whose image was already valid, and only
    // once the file reads back (a rollback from a trial boot still finds it).
    if (m.src == nimbus::store::MovedSource::File &&
        nimbus::store::retireLegacy(io, otaupd::imageValidAtBoot()))
      alog("scratchpad: legacy settings-storage copy removed (the file is in use)");
    Preferences p;
    if (p.begin(kScratchNs, true)) {
      String c = p.isKey(kMemCfgKey) ? p.getString(kMemCfgKey, "") : String();
      p.end();
      // Retrieval knobs used to silently reset to defaults every reboot; now they
      // persist (loaded here, rewritten on every config change).
      if (c.length()) g_cfg.deserialize(std::string(c.c_str(), c.length()));
    }
  }
  // Episodic history. On SD: the append-log day-streams are the system-of-record
  // (uncapped, O(1) appends). With no card: the legacy in-RAM 500-ring + whole-blob
  // (correct degraded behavior). See docs/orchestrator-storage.md §3.
  if (g_haveSd) {
    g_fs->mkdir(kEpiDir);
    g_fs->mkdir(kBlobDir);
    static ArduinoEpiFs epiFs(*g_fs);
    static AppendLogEpisodicStore epiLog(epiFs, kEpiDir, /*recentCap=*/512);
    // 512 (was 256): trace rows (A4) share the hot window with chat - the bump
    // (~300 KB PSRAM) keeps a day of chat zero-FS-read despite the extra rows.
    g_epiFs = &epiFs;
    g_epiLog = &epiLog;
    // Yield between day-files. NOT esp_task_wdt_reset() - setup() runs on a task
    // that is not subscribed to the TWDT, so that call returns "task not found"
    // and feeds nothing (it printed an error every boot while I was chasing this).
    // What actually matters is giving the scheduler a chance to run the IDLE task,
    // whose starvation is what trips the watchdog during a long blocking scan.
    int loaded = epiLog.hydrate(nimbus::orch::kHydrateMaxRows,
                                nimbus::orch::kHydrateMaxBytes,
                                [] { vTaskDelay(1); });
    if (epiLog.hydrateTruncated())
      alogf("memory: episodic boot scan hit its budget (%d indexed) - indexing the "
            "older rows on the card in the background", loaded);
    // One-time migration: if the day-streams are empty but a legacy whole-blob
    // exists (LittleFS /data or the pre-append-log /mem/episodic.bin), replay it
    // into the append-log. hydrate()==0 is the first-mount signal (once day-streams
    // exist it's >0), so no marker file is needed.
    if (loaded == 0) {
      String blob = loadBlob(kEpiSdPath, kEpiLfsPath);
      InMemoryEpisodicStore old;
      if (blob.length() && old.deserialize(std::string(blob.c_str(), blob.length()))) {
        for (const auto& s : old.sessions()) epiLog.addSession(s);
        MsgQuery all; all.limit = 1 << 20;
        auto rows = old.query(all);  // newest-first
        for (auto it = rows.rbegin(); it != rows.rend(); ++it) epiLog.addMessage(*it);
        alogf("memory: migrated %d episodic rows -> SD append-log", (int)rows.size());
      }
    }
    g_epiActive = &epiLog;
    g_epiSeq = epiLog.nextIdHint() ? epiLog.nextIdHint() - 1 : 0;  // ++ -> nextIdHint
  } else {
    String blob = loadBlob(kEpiSdPath, kEpiLfsPath);
    if (blob.length() && !g_epi.deserialize(std::string(blob.c_str(), blob.length())))
      alog("memory: episodic blob partial/garbage - loaded what parsed");
    g_epiActive = &g_epi;
    // Resume the id counter past the loaded history so new ids stay unique.
    g_epiSeq = (uint32_t)g_epi.messageCount();
  }

  MemoryContext ctx;
  ctx.vec = &g_vec;
  ctx.scratch = &g_scratch;
  ctx.cfg = &g_cfg;
  ctx.episodic = g_epiActive;
  // memory.archive is registered ONLY when a card is present at boot (ctx.archive
  // non-null). archiveAvailable is a LIVE check so a card pulled mid-run refuses the
  // tool cleanly. A card-less boot never registers it (adopting a later-inserted card
  // needs a restart, same as every other SD-resolved path here).
  if (effHaveSd()) {
    ctx.archive = &g_archive;
    ctx.archiveAvailable = [] { return effHaveSd(); };
  }
  ctx.embed = embedText;
  ctx.nowHours = [] { return nowHours(); };
  ctx.persistScratch = [] { return persistScratchpad(); };   // the tool reports a RAM-only save
  ctx.ops = &g_memOps;   // what writes did, for the nightly dream's honest outcome (lane M)
  // FAULT memory: writes are refused in the tool itself, so the refusal is counted
  // (the dream's row) on the MCP and the mem_write paths alike.
  ctx.writable = [] { return !nimbus::fault::active(nimbus::fault::MEMORY); };
  // Memory writes survive an embedding outage: the queue, its durable sidecar and
  // the park memo (lane P). The queue loads after the vectors so a crash replay
  // (stored AND still queued) is recognised by id on the first drain.
  loadPending();
  ctx.pending = &g_pending;
  ctx.persistPending = [] { return persistPending(); };
  ctx.park = &g_park;
  ctx.parkLock = [](const std::function<void()>& fn) { Lock g; fn(); };
  ctx.nowMs = [] { return (uint32_t)millis(); };
  ctx.clockSynced = [] { return clockSynced(); };
  ctx.embedProvider = [] { return std::string(store::embedProvider().c_str()); };
  ctx.onParked = [](nimbus::orch::EmbedFail kind, const std::string& raw) {
    alogf("memory: embeddings parked (%s)", nimbus::orch::embedFailWords(kind, "").c_str());
    if (kind != nimbus::orch::EmbedFail::OutOfCredit) return;
    // One plain notice per billing episode, shared with the chat head's notice so
    // the same provider refusing chat and embeddings makes ONE message. A
    // background turn (dream, routine) never pings; the next owner turn may.
    if (orchestrator::inScheduledTurn()) return;
    const String prov = store::embedProvider();
    if (!nimbus::orch::billingNoticeDue(prov.c_str())) return;
    const String owner = orchestrator::firstAllowedChat();
    if (!owner.length()) return;
    int status = 0;
    if (raw.rfind("HTTP ", 0) == 0) status = atoi(raw.c_str() + 5);
    const std::string words = nimbus::orch::billingNotice(prov.c_str(), status, "") +
        " Memories shared meanwhile are kept on the device and stored once it works again.";
    agent::telegram::send(owner, String(words.c_str()), /*block=*/false);
  };
  ctx.onWriteOutcome = [](const char* code) {
    if (!strcmp(code, nimbus::orch::kMemOutcomeStored)) g_outcomes.stored++;
    else if (!strcmp(code, nimbus::orch::kMemOutcomeQueued)) g_outcomes.queued++;
    else if (!strcmp(code, nimbus::orch::kMemOutcomeRefused)) g_outcomes.refused++;
  };
  g_ctx = ctx;
  registerMemoryTools(g_reg, ctx);
  applyConfig();   // capacity cap (score-based eviction on add); tier-aware (§degraded)

  // session.* tools for external MCP clients. list + terminate are live over the
  // journal/fabric; spawn is model-only (via the turn's session_ops[]/spawn[], which
  // assign an id at dispatch - the synchronous MCP contract can't return one), and
  // tell/poll stay null (report "not supported") because sub-agents are fire-and-
  // forget: the fabric's answer() only resumes a NeedsInput job, it can't inject a
  // follow-up user message into a live conversation.
  SessionHandlers sh;
  // The journal is single-writer on tg_poll; these handlers run on the AsyncTCP task
  // (external /mcp) - so read the cross-task snapshot, never the journal, and STAGE
  // the terminate for pollJobs to apply on tg_poll. (prism F24 - see orchestrator.h.)
  sh.list = [] { return orchestrator::sessionInfosSnapshot(); };
  sh.terminate = [](const std::string& id, std::string& err) -> bool {
    if (!orchestrator::sessionKnown(id)) {
      err = "no running session '" + id + "'";
      return false;
    }
    orchestrator::stageTerminate(id);   // async: drained on the journal's writer task
    return true;
  };
  registerSessionTools(g_reg, sh);

  // results.get / results.list - the recent-results ring (Context Fabric Stage 1:
  // a clipped tool result or overflowed sub-agent result is a VIEW; the full text
  // stays fetchable). Handlers route through orchestrator:: accessors, which take
  // the ring's own mutex (readers include this AsyncTCP /mcp task).
  {
    nimbus::orch::ResultHandlers rh;
    rh.get = [](const std::string& tag, size_t off, size_t maxB, std::string& out, size_t& total,
                const nimbus::orch::Principal& who) {
      return orchestrator::resultsGet(tag, off, maxB, out, total, who);
    };
    rh.list = [](const nimbus::orch::Principal& who) { return orchestrator::resultsList(who); };
    // The page must fit inside the turn's per-result clamp together with its
    // header (prism CRITICAL) - same derived value the loop clamps at.
    rh.viewCap = [] { return (size_t)agent::store::effectiveToolResultCap(); };
    registerResultTools(g_reg, rh);
  }

  // web.search (Tavily) - gives the orchestrator + external MCP clients a live-web
  // capability (the key is human-set, never model-writable). The handler does one
  // blocking HTTPS round-trip via the TLS arbiter. CUM-508: registered ALWAYS;
  // whether it is listed is the live key gate installed below, so a key saved
  // after boot works on the next turn (it used to need a restart).
  {
    agent::websearch::WebSearchToolDeps wd;
    wd.search = [](const std::string& q, int k) { return agent::tavily::search(q, k); };
    wd.overBudget = [] { return agent::store::providerOverBudget("tavily"); };
    wd.recordCall = [] { agent::store::recordProviderCall("tavily"); };
    agent::websearch::registerWebSearchTool(g_reg, wd);
  }
  // CUM-508: every provider-gated tool (nimbus/orch/tool_gates.h) is listed
  // exactly while its key is present - evaluated live on each listing and call,
  // so adding or removing a key never needs a restart. image.generate registers
  // later (files subsystem); gates are keyed by name, so order does not matter.
  nimbus::orch::applyToolGates(g_reg, [](nimbus::orch::ToolGate g) {
    switch (g) {
      case nimbus::orch::ToolGate::WebSearchKey: return agent::tavily::available();
      case nimbus::orch::ToolGate::ImageKey:     return agent::imagegen::available();
    }
    return false;
  });

  // Local Loops - schedule recurring/timed tasks that fire as future turns. A loop
  // the MODEL creates is stored PENDING until the owner approves it (prism: defuses
  // prompt-injection persistence). A scheduled turn can't create loops (fork-bomb guard).
  g_reg.add("wakeup.set",
            "Schedule ONE future wakeup for yourself - no owner approval needed. In "
            "`minutes` from now (2 min .. 7 days) you get a single automatic turn "
            "carrying your `note`, then the wakeup is gone. Use it to follow up on "
            "something you started ('check whether the sub-agent's report landed'), "
            "not for recurring work (that is loop.create, which the owner approves). "
            "You may set another when it fires; at most 4 armed at once, and wakeup "
            "turns spend the same daily budget as routines. Cancel with loop.cancel.",
            [](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) -> nimbus::orch::ToolResult {
              // ⚠ Enforce here, not just via setAdminOnly - the registry flag only
              // filters ADVERTISEMENT (W14); dispatch runs the handler regardless
              // (live-caught: the LAN /mcp User principal armed a wakeup). A wakeup
              // fires an unattended turn that replies to the owner's chat, so a
              // non-admin arming one is a prompt-injection persistence channel -
              // the exact hole the pending-approval rule closes for loop.create.
              if (!who.perms().manageTenants)
                return nimbus::orch::ToolResult::fail(
                    "only an admin's conversation can arm wakeups on this device");
              const long minutes = a["minutes"] | 0l;
              const char* note = a["note"] | "";
              if (!note[0])
                return nimbus::orch::ToolResult::fail(
                    "note is required - it is the context you get back on wakeup");
              JsonDocument sd;
              sd["kind"] = "once";
              sd["in_seconds"] = minutes * 60;
              auto r = agent::loops::createLoop("wakeup", note, "",
                                               sd.as<ArduinoJson::JsonObjectConst>(),
                                               /*byAgent=*/true,
                                               agent::orchestrator::inScheduledTurn());
              if (!r.ok) {
                // The tool speaks `minutes`; the core speaks `in_seconds` - translate
                // the bounds refusal so the model can actually correct its call.
                if (r.err.find("in_seconds") != std::string::npos)
                  return nimbus::orch::ToolResult::fail(
                      "minutes must be between 2 and 10080 (7 days)");
                return nimbus::orch::ToolResult::fail(r.err);
              }
              if (!r.approved)
                // Owner turned on "Wake-ups: ask me first": it is staged, not live,
                // until they approve the single card. Don't promise a firing time.
                return nimbus::orch::ToolResult::ok(
                    "wakeup staged (id " + r.id + "), pending the owner's approval. "
                    "It will NOT fire until they approve it; tell the owner you have "
                    "requested a follow-up in ~" + std::to_string(minutes) +
                    " min and are waiting on their approval.");
              return nimbus::orch::ToolResult::ok(
                  "wakeup armed (id " + r.id + "): in ~" + std::to_string(minutes) +
                  " min you get one automatic turn with your note. It fires once, "
                  "then it's gone - do not tell the owner you are 'monitoring' "
                  "continuously; say you'll check back at that time.");
            },
            R"({"type":"object","properties":{"minutes":{"type":"integer"},"note":{"type":"string"}},"required":["minutes","note"]})");

  g_reg.add("loop.create",
            "Schedule a recurring or timed task ('local loop') that fires as a FUTURE orchestrator turn. "
            "Use for RECURRING work: reminders, digests, nightly upkeep. For a one-time "
            "follow-up for yourself use wakeup.set instead (no approval needed). schedule is either "
            "{kind:'interval', every_seconds:N} (>=300) or {kind:'daily'|'weekly', at:'HH:MM' 24h, "
            "days:['mon','wed',...] for weekly}. chat_id (optional) must be an allow-listed chat. A loop "
            "YOU create is saved but stays PENDING until the owner approves it, then it fires on schedule.",
            [](ArduinoJson::JsonObjectConst a,
               const nimbus::orch::Principal& who) -> nimbus::orch::ToolResult {
              // Routines run unattended and fire into a chat of their choosing.
              // A member proposing them fills the owner's approval queue at best,
              // and at worst gets one approved in passing. Admin-only, matching
              // loop.list/loop.cancel.
              if (!who.perms().manageTenants)
                return nimbus::orch::ToolResult::fail(
                    "only an admin can set up this device's routines");
              // The scheduled-turn refusal now lives in the registry's ToolPolicy
              // resolver (installed below) - enforcement at the dispatch seam, so
              // EVERY route (turn loop, LAN /mcp) hits the same gate. The core
              // createLoop still receives the flag as defense in depth.
              std::string name = a["name"] | "";
              auto r = loops::createLoop(String((const char*)(a["name"] | "")),
                                         String((const char*)(a["prompt"] | "")),
                                         String((const char*)(a["chat_id"] | "")),
                                         a["schedule"].as<ArduinoJson::JsonObjectConst>(),
                                         /*byAgent=*/true, orchestrator::inScheduledTurn());
              if (!r.ok) return nimbus::orch::ToolResult::fail(r.err);
              return nimbus::orch::ToolResult::ok("loop '" + name + "' created (id " + r.id +
                        ") - PENDING the owner's approval before it can fire.");
            },
            R"({"type":"object","properties":{"name":{"type":"string"},"prompt":{"type":"string"},"chat_id":{"type":"string"},"schedule":{"type":"object","properties":{"kind":{"type":"string","enum":["interval","daily","weekly"]},"every_seconds":{"type":"integer"},"at":{"type":"string"},"days":{"type":"array","items":{"type":"string"}}}}},"required":["name","prompt","schedule"]})");
  g_reg.add("loop.list",
            "List all scheduled loops with status: schedule, next run, last result, enabled, and whether it's approved.",
            [](ArduinoJson::JsonObjectConst,
               const nimbus::orch::Principal& who) -> nimbus::orch::ToolResult {
              // The listing carries every routine's PROMPT and the chat ids they
              // fire into - free text the owner wrote, often the most sensitive
              // strings on the device ("every morning check <account> and tell
              // me..."), plus who else talks to it. Routines are an admin
              // surface until they are per-principal.
              if (!who.perms().manageTenants)
                return nimbus::orch::ToolResult::fail(
                    "only an admin can see this device's routines");
              return nimbus::orch::ToolResult::ok(std::string(loops::loopsJson().c_str()));
            },
            R"({"type":"object","properties":{}})");
  g_reg.add("loop.cancel",
            "Cancel (delete) a scheduled loop by its id.",
            [](ArduinoJson::JsonObjectConst a,
               const nimbus::orch::Principal& who) -> nimbus::orch::ToolResult {
              if (!who.perms().manageTenants)
                return nimbus::orch::ToolResult::fail(
                    "only an admin can change this device's routines");
              std::string id = a["id"] | "";
              if (id.empty()) return nimbus::orch::ToolResult::fail("missing 'id'");
              // DREAMING: the reserved system loop is pause-only - refuse with
              // the reason (a bare "no loop with that id" would be a lie).
              std::string refusal = agent::dream::cancelRefusal(id);
              if (!refusal.empty()) return nimbus::orch::ToolResult::fail(refusal);
              return loops::cancelLoop(String(id.c_str()))
                       ? nimbus::orch::ToolResult::ok("loop cancelled")
                       : nimbus::orch::ToolResult::fail("no loop with that id");
            },
            R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})");

  // Output-channel tools (P6) - the MODEL decides how to reach the user, replacing
  // the old global "speak replies aloud" checkbox. Both are loop tools (callable
  // mid-turn); the orch_turn `reply` still goes to the originating channel by default.
  g_reg.add("reply.speak",
            "Read text aloud on the device's built-in speaker (text-to-speech). Use when a "
            "spoken response is helpful. Returns whether playback succeeded.",
            [](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) -> nimbus::orch::ToolResult {
              // Turn-output tool only: refuse from the LAN /mcp path (no turn in
              // flight, or a turn running on the poll task while the call came in on
              // the web task) so an MCP client can't actuate the speaker + block the
              // web task with a multi-second TLS+I2S synth and the playback wait (prism).
              if (!agent::orchestrator::turnInFlight() || !agent::telegram::onTurnTask())
                return nimbus::orch::ToolResult::fail("reply.speak is only available during a turn");
              // Owner's "Voice replies" toggle (P2.5): OFF -> no audio, tell the
              // model to use text so the content still reaches the owner.
              if (!agent::store::ttsEnabled())
                return nimbus::orch::ToolResult::fail(
                    "voice replies are switched OFF - reply with text; if the owner just "
                    "ASKED for spoken replies, enable them NOW with the device.control tool "
                    "({\"action\":{\"type\":\"config\",\"ttsOn\":true}}) and call reply.speak "
                    "again in THIS same turn");
              // Volume 0 = the driver multiplies every sample to silence, but
              // playback still "succeeds" - the model would truthfully report
              // "spoken" while the owner hears nothing. Refuse honestly instead.
              if (agent::store::sfxVolume() == 0)
                return nimbus::orch::ToolResult::fail(
                    "the speaker volume is 0, so speech would play as silence - "
                    "raise it NOW with the device.control tool "
                    "({\"action\":{\"type\":\"config\",\"sfxVol\":25}}), call "
                    "reply.speak again in this same turn, and restore the volume "
                    "after if the owner wanted silence");
              std::string t = a["text"].is<const char*>() ? std::string(a["text"].as<const char*>()) : std::string();
              if (t.empty()) return nimbus::orch::ToolResult::fail("missing 'text'");
              // "spoken" only once the speaker opened for the clip (22 Oct: a driver
              // that could not start was swallowed and the clip counted as played).
              String why;
              if (!agent::orchestrator::speakOnDevice(String(t.c_str()), true, &why))
                return nimbus::orch::ToolResult::fail(speakFailure(std::string(why.c_str())));
              return nimbus::orch::ToolResult::ok("spoken on device speaker");
            },
            R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"]})");
  g_reg.add("reply.telegram",
            "Send a Telegram message. Omit chat_id to reply to the current conversation; set "
            "voice:true to deliver it as spoken audio instead of text (up to 2,400 characters, "
            "sent as voice messages of up to 600 characters each). A voice result names "
            "Telegram's message_id for every voice message that arrived; without one, nothing "
            "arrived.",
            [](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) -> nimbus::orch::ToolResult {
              if (!agent::orchestrator::turnInFlight())
                return nimbus::orch::ToolResult::fail("reply.telegram is only available during a turn");
              std::string t = a["text"].is<const char*>() ? std::string(a["text"].as<const char*>()) : std::string();
              if (t.empty()) return nimbus::orch::ToolResult::fail("missing 'text'");
              std::string cid = a["chat_id"].is<const char*>() ? std::string(a["chat_id"].as<const char*>()) : std::string();
              // Owner's "Voice replies" toggle (P2.5): OFF -> degrade voice to TEXT
              // (the content still arrives; it just isn't spoken - never dropped).
              const bool asked = a["voice"] | false;
              const bool voice = asked && agent::store::ttsEnabled();
              // Voice: the receipt (Telegram's message ids) or why nothing arrived, so
              // the model can fall back to text and say so (22 Oct: "sent to Telegram"
              // came back for a voice message that never left the device).
              std::string result;
              if (!agent::orchestrator::sendToChat(String(cid.c_str()), String(t.c_str()), voice, &result))
                return nimbus::orch::ToolResult::fail(
                    result.empty() ? "no allowed Telegram target (omit chat_id to reply to the current "
                                     "chat, or pass an allowlisted id)"
                                   : result);
              if (voice) return nimbus::orch::ToolResult::ok(result);
              return nimbus::orch::ToolResult::ok(
                  asked ? "sent to Telegram as text (voice replies are switched off, so it was not spoken)"
                        : "sent to Telegram as text");
            },
            R"({"type":"object","properties":{"text":{"type":"string"},"chat_id":{"type":"string"},"voice":{"type":"boolean"}},"required":["text"]})");

  // skills (P7) - on-device documentation capsules the model pulls on demand
  // (architecture / hardware / tools-guide), keeping the per-turn prompt lean.
  g_reg.add("skill.list",
            "List the on-device playbooks/knowledge capsules you can read (id + title + "
            "description): task recipes, how you're wired, your hardware, your tools.",
            [](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal&) -> nimbus::orch::ToolResult {
              JsonDocument d;
              JsonArray arr = d.to<JsonArray>();
              for (const auto& c : agent::skills::list()) {
                JsonObject o = arr.add<JsonObject>();
                o["id"] = c.id; o["title"] = c.title;
                if (!c.desc.empty()) o["desc"] = c.desc;
              }
              std::string s; serializeJson(d, s);
              return nimbus::orch::ToolResult::ok(s);
            },
            R"({"type":"object","properties":{}})");
  g_reg.add("skill.get",
            "Read a knowledge capsule by id (from skill.list) - the long-form detail about "
            "yourself, so it needn't bloat every prompt.",
            [](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) -> nimbus::orch::ToolResult {
              std::string id = a["id"].is<const char*>() ? std::string(a["id"].as<const char*>()) : std::string();
              std::string body = agent::skills::get(id);
              if (body.empty()) return nimbus::orch::ToolResult::fail("unknown skill id (call skill.list)");
              return nimbus::orch::ToolResult::ok(body);
            },
            R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})");
  // docs.* (W13) - the device's OWN documentation, embedded in the firmware
  // image (tools/gen_docs_pack.py -> nimbus/docs_pack_data.h), so an OTA update
  // ships matching docs and they can never version-skew. Read-only, no admin
  // gate: any principal may read the docs. Bodies are flash rodata scanned in
  // place - never copied through an internal-heap intermediate buffer.
  g_reg.add("docs.list",
            "Browse your own device documentation - search it before saying what you can "
            "or cannot do. No arguments: the doc files (slug, title, section count). With "
            "file: one file's sections (id + title) for docs.read.",
            [](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) -> nimbus::orch::ToolResult {
              std::string want = a["file"].is<const char*>() ? std::string(a["file"].as<const char*>()) : std::string();
              JsonDocument d;
              JsonArray arr = d.to<JsonArray>();
              if (want.empty()) {
                for (size_t i = 0; i < nimbus::docs::fileCount(); i++) {
                  const nimbus::docs::DocFile& f = nimbus::docs::file(i);
                  JsonObject o = arr.add<JsonObject>();
                  o["file"] = f.slug; o["title"] = f.title; o["sections"] = f.count;
                }
              } else {
                const nimbus::docs::DocFile* f = nimbus::docs::findFile(want);
                if (!f)
                  return nimbus::orch::ToolResult::fail(
                      "unknown doc file '" + want + "' (call docs.list with no arguments for the file index)");
                for (size_t i = f->first; i < (size_t)(f->first + f->count); i++) {
                  const nimbus::docs::DocSection& s = nimbus::docs::section(i);
                  JsonObject o = arr.add<JsonObject>();
                  o["id"] = s.id; o["title"] = s.title;
                }
              }
              std::string s; serializeJson(d, s);
              return nimbus::orch::ToolResult::ok(s);
            },
            R"({"type":"object","properties":{"file":{"type":"string"}}})");
  g_reg.add("docs.search",
            "Search your own device documentation (ranked keyword match over titles + "
            "bodies: any of the words, sections holding more of them first) - use it "
            "BEFORE saying what you can or cannot do, or how a feature of yours works. "
            "Returns section ids + a snippet; read the full section with docs.read.",
            [](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) -> nimbus::orch::ToolResult {
              std::string q = a["query"].is<const char*>() ? std::string(a["query"].as<const char*>()) : std::string();
              if (q.empty()) return nimbus::orch::ToolResult::fail("missing 'query'");
              const nimbus::docs::DocSection* hits[8];
              size_t n = nimbus::docs::search(q, hits, 8);
              if (n == 0)
                return nimbus::orch::ToolResult::ok(
                    "no sections match - try different keywords, or browse docs.list");
              JsonDocument d;
              JsonArray arr = d.to<JsonArray>();
              for (size_t i = 0; i < n; i++) {
                JsonObject o = arr.add<JsonObject>();
                o["id"] = hits[i]->id; o["title"] = hits[i]->title;
                o["snippet"] = nimbus::docs::snippet(*hits[i], q);
              }
              std::string s; serializeJson(d, s);
              return nimbus::orch::ToolResult::ok(s);
            },
            R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query"]})");
  g_reg.add("docs.read",
            "Read ONE section of your own device documentation by id (from docs.list or "
            "docs.search) - the authoritative answer to what you can do and how it works.",
            [](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) -> nimbus::orch::ToolResult {
              std::string id = a["id"].is<const char*>() ? std::string(a["id"].as<const char*>()) : std::string();
              if (id.empty()) return nimbus::orch::ToolResult::fail("missing 'id'");
              const nimbus::docs::DocSection* s = nimbus::docs::find(id);
              if (!s) {
                std::string near = nimbus::docs::nearMisses(id, 5);
                if (near.empty())
                  return nimbus::orch::ToolResult::fail(
                      "unknown doc id '" + id + "' (call docs.list or docs.search)");
                return nimbus::orch::ToolResult::fail(
                    "unknown doc id '" + id + "' - did you mean: " + near + "?");
              }
              std::string out = "## ";
              out += s->title;
              out += "\n\n";
              out += s->body;   // rodata appended straight into the result string
              return nimbus::orch::ToolResult::ok(out);
            },
            R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})");
  // v4.0.0 skills authoring: the model can WRITE its own capsules - with rails.
  // Server-stamped created_by: agent + approved: false (INERT for spawn
  // injection until the owner approves); reserved built-in ids refused; capped
  // pending queue; denied in scheduled/unattended turns (PolicyResolver below).
  g_reg.add("skill.save",
            "Save (or update) a reusable skill capsule you authored: a markdown playbook "
            "future runs can inject into sub-agents. id: a-z0-9-_ max 23 chars. md: the "
            "SKILL.md text (optional front matter title:/inject:). ⚠ Approval is "
            "ASYNCHRONOUS - a skill you save is INACTIVE until the owner approves it "
            "later (web UI or /skill approve), so save skills as an investment for FUTURE "
            "tasks, never as a step of the CURRENT one.",
            [](ArduinoJson::JsonObjectConst a,
               const nimbus::orch::Principal& who) -> nimbus::orch::ToolResult {
              // Same rationale as loop.create: a persistent instruction blob
              // proposed from a member chat fills the owner's approval queue at
              // best. Admin conversations only.
              if (!who.perms().manageTenants)
                return nimbus::orch::ToolResult::fail(
                    "only an admin's conversation can author skills");
              if (agent::skills::pendingAgentCount() >= 4)
                return nimbus::orch::ToolResult::fail(
                    "4 skills are already awaiting the owner's approval - ask them to "
                    "approve or delete some first");
              std::string id = a["id"].is<const char*>() ? a["id"].as<const char*>() : "";
              std::string md = a["md"].is<const char*>() ? a["md"].as<const char*>() : "";
              std::string err;
              if (!agent::skills::save(id, md, err, /*byAgent=*/true))
                return nimbus::orch::ToolResult::fail(err);
              // Owner alert (the loops pattern): direct, never left to the
              // model's narration.
              String owner = orchestrator::firstAllowedChat();
              if (owner.length())
                agent::telegram::send(owner,
                    String("The assistant saved a new skill '") + id.c_str() +
                    "' - it stays inactive until you approve it. Reply /skill approve " +
                    id.c_str() + " to activate, or /skill deny " + id.c_str() + ".",
                    /*block=*/false);
              return nimbus::orch::ToolResult::ok(
                  "skill '" + id + "' saved - PENDING the owner's approval (it cannot be "
                  "used in the current task; once approved it works in future runs)");
            },
            R"({"type":"object","properties":{"id":{"type":"string"},"md":{"type":"string"}},"required":["id","md"]})");
  g_reg.add("skill.delete",
            "Delete a skill capsule YOU authored (created_by: agent). Owner-created "
            "skills can only be deleted by the owner in the web UI.",
            [](ArduinoJson::JsonObjectConst a,
               const nimbus::orch::Principal& who) -> nimbus::orch::ToolResult {
              if (!who.perms().manageTenants)
                return nimbus::orch::ToolResult::fail(
                    "only an admin's conversation can manage skills");
              std::string id = a["id"].is<const char*>() ? a["id"].as<const char*>() : "";
              std::string err;
              if (!agent::skills::remove(id, err, /*byAgent=*/true))
                return nimbus::orch::ToolResult::fail(err);
              return nimbus::orch::ToolResult::ok("skill '" + id + "' deleted");
            },
            R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})");

  // system.health (P5) - the model can query live hardware/subsystem status
  // (a passive snapshot; never actuates hardware, so it is safe on the turn task).
  // BLE/battery are Notifier-only / absent in the Orchestrator context this runs in.
  g_reg.add("system.health",
            "Report the device's live hardware + subsystem health (LED ring or status LED, display, mic, "
            "speaker, SD card, memory, PSRAM, Wi-Fi, Telegram). Use it to answer questions "
            "about what hardware you have or whether a component is working.",
            [](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal&) -> nimbus::orch::ToolResult {
              agent::health::Env env;
              env.wifiKnown = true; env.wifiUp = true;   // a turn only runs with WiFi up
              return nimbus::orch::ToolResult::ok(agent::health::reportJson(env));
            },
            R"({"type":"object","properties":{}})");

  // ToolPolicy resolver - dispatch-level enforcement (defuses the fork-bomb the
  // in-handler check used to catch, but at the seam every caller shares). A
  // scheduled/unattended turn (Local Loops fire, auto-synthesis) may not create
  // loops; SAME refusal string the handler carried. Default-allow otherwise, so
  // behavior is identical outside scheduled turns.
  registerTenantTools();   // fwd-declared above

  g_reg.setPolicyResolver([](const std::string& name) -> ToolRegistry::Verdict {
    if (name == "loop.create" && orchestrator::inScheduledTurn())
      return ToolRegistry::Verdict::deny("a scheduled loop cannot create loops");
    // v4.0.0: skill authoring is a CONTROL action - an unattended turn (a
    // routine firing, a fan-out synthesis chewing on untrusted sub-agent
    // output) must never persist instruction blobs. ⚠ synthesis turns ARE
    // scheduled turns, so this also keeps a poisoned sub-result from writing a
    // capsule; files.read/artifact.save stay ALLOWED there by design.
    if ((name == "skill.save" || name == "skill.delete") && orchestrator::inScheduledTurn())
      return ToolRegistry::Verdict::deny("a scheduled turn cannot modify skills");
    // v3.7.0: tenant management is a CONTROL action. An unattended turn (a
    // routine firing, a sub-agent synthesis) must never be able to promote a
    // tenant or lift a quota - the same rail reboot and loop.create ride.
    if (name.rfind("tenant.", 0) == 0 && orchestrator::inScheduledTurn())
      return ToolRegistry::Verdict::deny("a scheduled turn cannot manage people");
    return ToolRegistry::Verdict::allow();
  });

  alogf("memory: ready (%d vectors, dims=%d, store=%s, cap=%d, archive=%s, embed=%s, web=%s, pending=%d)",
        g_vec.size(), g_vec.dims(), g_haveSd ? "SD /mem" : "flash /data (no SD)",
        g_vec.maxEntries(),
        effHaveSd() ? (std::to_string(g_archive.size()) + "/" +
                       std::to_string(kArchiveMaxEntries)).c_str() : "off (no SD)",
        embeddings::available() ? "on" : "no-key",
        agent::tavily::available() ? "on" : "off", (int)g_pending.size());
}

// ---- v3.7.0 admin-by-conversation -------------------------------------------
// The owner asked to manage people by talking to the device ("approve X",
// "make Y a guest"). These are ADMIN-only at DISPATCH: hiding a tool from the
// advertisement is not a boundary, so each handler re-checks the caller's role
// and the policy resolver additionally refuses them in scheduled turns.
static void registerTenantTools() {
  using nimbus::orch::Role;
  auto adminOnly = [](const nimbus::orch::Principal& who) -> bool {
    return who.perms().manageTenants;
  };

  g_reg.add("tenant.list",
            "List the people who can talk to this device: chat id, role "
            "(admin/user/guest/unknown), and their USAGE vs quota (memories "
            "stored/allowed, pins used/allowed, file limits).",
            [adminOnly](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal& who)
                -> ToolResult {
              if (!adminOnly(who)) return ToolResult::fail("only an admin can see the people list");
              std::string out;
              Lock g;   // countIn/pinsIn read the shared VDB
              for (const auto& t : orchestrator::tenantSnapshot()) {
                const auto q = nimbus::orch::effectiveQuota(t.role, t.quota);
                // W11: usage BESIDE the ceiling - the list used to show only the
                // quota, so "is anyone near their limit?" was unanswerable.
                const std::string ns =
                    nimbus::orch::nsForChat(t.chatId, t.role == nimbus::orch::Role::Admin);
                out += t.chatId + " - " + nimbus::orch::roleName(t.role) +
                       " (memories " + std::to_string(g_vec.countIn(ns)) + "/" +
                       std::to_string(q.maxVectors) +
                       ", pins " + std::to_string(g_vec.pinsIn(ns)) + "/" +
                       std::to_string(q.maxPins) +
                       ", files " + std::to_string(q.maxBytes / 1024) + "KB" +
                       ", ttl " + std::to_string(q.maxTtlHours) + "h)\n";
              }
              return ToolResult::ok(out.empty() ? "No one is registered yet." : out);
            },
            R"({"type":"object","properties":{}})");

  g_reg.add("tenant.set_role",
            "Approve someone or change what they may do: role is admin, user, "
            "guest, or unknown (revokes access). The last admin cannot be demoted.",
            [adminOnly](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who)
                -> ToolResult {
              if (!adminOnly(who)) return ToolResult::fail("only an admin can change roles");
              const char* chat = a["chat"] | "";
              const char* role = a["role"] | "";
              if (!chat[0] || !role[0]) return ToolResult::fail("need 'chat' and 'role'");
              Role r;
              if (!nimbus::orch::roleFromName(role, r))
                return ToolResult::fail("role must be admin, user, guest or unknown");
              std::string err;
              if (!orchestrator::tenantSetRole(chat, r, err))
                return ToolResult::fail(err);
              return ToolResult::ok(std::string(chat) + " is now " + nimbus::orch::roleName(r));
            },
            R"({"type":"object","properties":{"chat":{"type":"string"},"role":{"type":"string","enum":["admin","user","guest","unknown"]}},"required":["chat","role"]})");

  g_reg.add("tenant.set_quota",
            "Set how much one person may store: vectors (memories), bytes "
            "(files), ttl_hours (how long their memories live), pins. 0 restores "
            "the default for their role.",
            [adminOnly](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who)
                -> ToolResult {
              if (!adminOnly(who)) return ToolResult::fail("only an admin can change quotas");
              const char* chat = a["chat"] | "";
              if (!chat[0]) return ToolResult::fail("need 'chat'");
              nimbus::orch::Quota q;
              if (!orchestrator::tenantQuotaOf(chat, q))
                return ToolResult::fail("no such person - approve them first");
              if (a["vectors"].is<unsigned>())  q.maxVectors  = a["vectors"];
              if (a["bytes"].is<unsigned>())    q.maxBytes    = a["bytes"];
              if (a["ttl_hours"].is<unsigned>())q.maxTtlHours = a["ttl_hours"];
              if (a["pins"].is<unsigned>())     q.maxPins     = (uint16_t)(unsigned)a["pins"];
              std::string err;
              if (!orchestrator::tenantSetQuota(chat, q, err)) return ToolResult::fail(err);
              return ToolResult::ok(std::string("updated quotas for ") + chat);
            },
            R"({"type":"object","properties":{"chat":{"type":"string"},"vectors":{"type":"integer"},"bytes":{"type":"integer"},"ttl_hours":{"type":"integer"},"pins":{"type":"integer"}},"required":["chat"]})");

  // W14 - ADVERTISEMENT scope, not the boundary. Each handler above already
  // refuses a non-admin; this keeps the tool out of a member/guest turn's
  // prompt so the assistant never offers a capability it will then be denied
  // (and so a guest is not shown the owner's admin surface). Grouped here, at
  // the end of registration, so the list is auditable in one place.
  for (const char* n : {"loop.create", "loop.list", "loop.cancel", "wakeup.set",
                        "skill.save", "skill.delete",
                        "tenant.list", "tenant.set_role", "tenant.set_quota"})
    g_reg.setAdminOnly(n);
}

// Effective SD presence: the real mount AND not simulated-lost by a resilience
// fault (FAULT sd). Every DEGRADED-path decision (capacity cap, the LittleFS
// overflow guard, the reported tier) routes through this, so `FAULT sd on` drops
// the running device into the exact no-card behavior with no reboot and no
// physical unplug. The begin-time PATH resolution (g_vecPath/g_epiPath) stays put
// - blobs keep landing where they were mounted; only the safety limits tighten.
static inline bool effHaveSd() {
  return g_haveSd && !nimbus::fault::active(nimbus::fault::SD) && !g_sdHealth.lost();
}

bool haveSd()    { return effHaveSd(); }
bool erasing()   { return g_erasing; }
bool flashFull() { return g_flashFull; }
bool sdMissingWithData() { return g_sdMissing; }

ToolRegistry&  registry()   { return g_reg; }
VectorMemory&  vectors()    { return g_vec; }
nimbus::orch::VectorArchive& archive() { return g_archive; }
Scratchpad&    scratchpad() { return g_scratch; }
MemConfig&     config()     { return g_cfg; }

void applyConfig() {
  Lock g;
  // Tier-aware capacity cap: the full configured cap only on the SD card; with no
  // card, clamp to a flash-safe cap so the durable LittleFS blob can't exhaust the
  // few-MB internal partition (score-based eviction drops the lowest-value entries).
  int cap = g_cfg.maxVectors;
  if (!effHaveSd() && (cap == 0 || cap > kDegradedMaxVectors)) cap = kDegradedMaxVectors;
  g_vec.setMaxEntries(cap);
}
EpisodicStore& episodic()   { return *g_epiActive; }

// ---- SD graceful degradation: demote/promote (the HIL test spec) --------

// Tiny liveness probe: write+read+delete a temp file on the active store, honouring
// the resilience faults so a simulated loss (FAULT sd / sd_io) makes the probe fail
// (and thus can't promote). Returns false with no card / on any FS error.
static bool probeSdWrite() {
  if (!g_haveSd) return false;
  if (nimbus::fault::active(nimbus::fault::SD_IO) || nimbus::fault::active(nimbus::fault::SD))
    return false;
  const char* p = "/mem/.sdhealth";
  Lock g;
  File f = g_fs->open(p, FILE_WRITE);
  if (!f) return false;
  size_t n = f.write((const uint8_t*)"ok", 2);
  f.close();
  if (n != 2) { g_fs->remove(p); return false; }
  File rf = g_fs->open(p, FILE_READ);
  bool okRead = rf && rf.size() == 2;
  if (rf) rf.close();
  g_fs->remove(p);
  return okRead;
}

// Apply a demote/promote edge: re-cap the VDB for the new tier + log. The main
// loop raises the user-facing CTA + sfx by watching the sdLost() edge.
static void applySdEdge(nimbus::SdHealthTracker::Event e) {
  if (e == nimbus::SdHealthTracker::Event::Demote) {
    applyConfig();   // tighten the vector cap to the flash-safe degraded limit
    // Detach the archive sink: with no card, prune must DROP at TTL as before (a
    // card-less device never archives). memory.archive also refuses live via
    // archiveAvailable(). The archive's RAM contents are kept for a later promote.
    g_vec.setArchiveSink(nullptr);
    alog("memory: SD lost mid-run - demoted to no-card tier (appends -> RAM ring)");
  } else if (e == nimbus::SdHealthTracker::Event::Promote) {
    applyConfig();   // restore the full SD-tier cap
    // Re-attach the archive sink only if the archive existed at boot (a card-less
    // boot never built it; adopting a freshly-inserted card needs a restart).
    if (g_haveSd) g_vec.setArchiveSink(&g_archive);
    alog("memory: SD recovered - promoted back to the SD tier");
  }
}

bool sdLost() { return g_haveSd && g_sdHealth.lost(); }

// g_sdHealth's streak counters are a non-atomic read-modify-write shared between
// the main loop (tickSdHealth/forceSdProbe) and the AsyncTCP web/MCP + Telegram
// tasks (noteSdIoResult via persist/capture, promoteSd via /api/sdprobe). The
// recursive memory Lock is the ONLY thing serializing them - every mutation site
// below takes it (prism 2026-07-12: unlocked main-loop access raced the locked
// task access, losing/duplicating demote edges).
void noteSdIoResult(bool ok) {
  // Only fed on the effHaveSd() write path, so a no-card device never demotes.
  // Promote is driven by the probe (SD writes are skipped while lost).
  if (!g_haveSd) return;
  Lock g;
  applySdEdge(g_sdHealth.note(ok));
}

void demoteSd() { Lock g; applySdEdge(g_sdHealth.forceDemote()); }

bool promoteSd() {
  // Hold the memory Lock across the ENTIRE bus re-init + probe: promoteSd() runs on the
  // AsyncTCP web task (POST /api/sdprobe) and the menu drain, while the main loop's
  // tickSdHealth() also probes the SD/SPI bus - tearing the bus down (end()/begin()) or
  // probing it from two tasks at once corrupts the transfer (prism HIGH). The SD driver
  // is not re-entrant; Lock (recursive) is the one mutex serializing SD access. SD I/O is
  // bounded (unlike a TLS embed), so holding it here is safe. From the menu (the main
  // loop) a busy lock answers "not promoted" instead of waiting (loop_guard.h).
  LoopLock g;
  if (!g) return false;
  // Attempt a real re-mount first (a physical pull leaves stale handles); then a
  // write probe confirms the card answers before we clear the latch. end() clears
  // the driver's idempotent "already mounted" latch so begin() actually re-probes
  // the bus - without it a re-seated card is never re-detected (begin() no-ops).
  if (sdLost()) {
    nimbus::errlog::releaseHandle();   // never keep the log file open across a remount
    solide::storage::end();
    solide::storage::begin();
  }
  if (!probeSdWrite()) return false;
  applySdEdge(g_sdHealth.forcePromote());
  return !g_sdHealth.lost();
}

void appendBatteryHistory(uint32_t hours, uint16_t mv, uint8_t pct, const char* state) {
  // Durable discharge log on the SD card (docs/orchestrator-storage.md): one JSONL
  // row per telemetry tick, day-partitioned. SD-gated via effHaveSd(), so it skips
  // cleanly with no card / while demoted (composes with WS-B). The caller only logs
  // DISCHARGING ticks, which both bounds growth and captures exactly the data the
  // BatteryModel learns time-to-empty/health from - the owner's "track estimations
  // over time to improve prediction".
  if (!g_begun || !effHaveSd()) return;
  LoopLock g;   // runs on the main loop: a busy card lock skips this one row
  if (!g) return;
  g_fs->mkdir("/mem/battery");
  char path[40];
  snprintf(path, sizeof path, "/mem/battery/d%lu.jsonl", (unsigned long)(hours / 24));
  File f = g_fs->open(path, FILE_APPEND);
  if (!f) { noteSdIoResult(false); return; }   // a failed append is also a demote signal
  char row[104];
  int n = snprintf(row, sizeof row, "{\"h\":%lu,\"mv\":%u,\"pct\":%u,\"st\":\"%s\"}\n",
                   (unsigned long)hours, (unsigned)mv, (unsigned)pct, state ? state : "");
  size_t w = f.write((const uint8_t*)row, n);
  f.close();
  noteSdIoResult(w == (size_t)n);
}

bool forceSdProbe() {
  // Test/console hook: run one probe+note immediately, ignoring the cadence gate,
  // so the demote/promote state machine can be driven deterministically (two failing
  // probes -> demote; two passing -> promote). Returns the probe result.
  if (!g_haveSd) return false;
  const bool ok = probeSdWrite();
  Lock g;
  applySdEdge(g_sdHealth.note(ok));
  return ok;
}

static constexpr uint32_t kTickBusyEscalate = 50;   // busy passes before the tick waits

void tickSdHealth(uint32_t nowMs) {
  // Runs on the main loop every pass. A turn can hold the card lock across a whole
  // tool dispatch (a provider round trip, a card scan), so the loop only tries for
  // it: busy means this pass skips, and the next pass retries (loop_guard.h). The
  // guard also serializes the probe with promoteSd()'s end()/begin() on the web task
  // and the debounce read-modify-write with the web and Telegram tasks (prism).
  // Every pass retries, so it never waits at first; after a run of busy passes it waits
  // the full short try, so back-to-back tool calls cannot starve the probe forever.
  static uint32_t s_busyRun = 0;
  LoopLock g(s_busyRun >= kTickBusyEscalate ? nimbus::sys::kLoopLockWaitMs : 0);
  if (!g) {
    ++s_busyRun;
    return;
  }
  s_busyRun = 0;
  drainParkedEvents();    // timeline events the loop parked while the lock was busy
  flushPendingEvents();   // pre-SNTP timeline rows flush once the clock syncs
  if (!g_haveSd) return;   // no card at boot: nothing to demote/promote
  // Probe cadence: slow when healthy (idle detection of a pull), fast when lost
  // (responsive recovery). Live writes also feed noteSdIoResult() in real time.
  const uint32_t interval = sdLost() ? 4000u : 20000u;   // benign single-bool read
  if (g_lastSdProbeMs != 0 && nowMs - g_lastSdProbeMs < interval) return;
  g_lastSdProbeMs = nowMs;
  const bool ok = probeSdWrite();
  applySdEdge(g_sdHealth.note(ok));
}

// Write a blob to `path` atomically: to `<path>.tmp`, then rename over `path`, so a
// crash mid-write leaves the previous good blob intact (single-writer; FAT + LittleFS
// rename is atomic enough for our case). Returns false on any FS error.
static bool writeBlobAtomicOn(fs::FS& fsys, const std::string& path, const std::string& blob) {
  // Barrier: the durable store is being wiped - refuse every persist/sidecar so a
  // blocked-then-released persist can't rewrite a just-deleted blob from RAM.
  if (g_erasing) return false;
  std::string tmp = path + ".tmp";
  File f = fsys.open(tmp.c_str(), FILE_WRITE);
  if (!f) return false;
  size_t n = f.write((const uint8_t*)blob.data(), blob.size());
  f.close();
  if (n != blob.size()) { fsys.remove(tmp.c_str()); return false; }
  fsys.remove(path.c_str());               // rename won't overwrite on some FS impls
  return fsys.rename(tmp.c_str(), path.c_str());
}
static bool writeBlobAtomic(const std::string& path, const std::string& blob) {
  // FAULT sd_io: emulate a mid-op SD write failure (see nimbus::fault::SD_IO) so
  // the vector/blob persist-failure + demote path is testable without a real pull.
  // It applies to the data store only; the pending queue's explicit LittleFS
  // fallback (persistPending) is internal flash and keeps working.
  if (nimbus::fault::active(nimbus::fault::SD_IO)) return false;
  return writeBlobAtomicOn(*g_fs, path, blob);
}

// The recall feeds the same park memo as writes and the drain: a failure parks,
// a success clears the park (and so lets the drain run).
static void noteRecallEmbed(bool ok, const String& err) {
  std::string detail;
  nimbus::orch::EmbedFail kind = nimbus::orch::EmbedFail::None;
  if (!ok) {
    kind = nimbus::orch::classifyEmbedFail(std::string(err.c_str()), detail);
    if (kind == nimbus::orch::EmbedFail::None) kind = nimbus::orch::EmbedFail::Unknown;
  }
  bool started = false;
  {
    Lock g;
    if (ok && g_park.reason() == nimbus::orch::EmbedFail::OutOfCredit)
      nimbus::orch::billingEpisodeEnd(store::embedProvider().c_str());
    started = g_park.onResult(kind, millis(), detail);
  }
  if (started && g_ctx.onParked) g_ctx.onParked(kind, std::string(err.c_str()));
}

// The memory floors of the embed path: total free internal heap and the largest
// contiguous internal block (one TLS handshake's worth). The pending drain waits
// below them, and the keyword recall skips its scan below them: it runs while
// embeddings are down, often on a device already short of memory.
static bool recallHeapOk() {
  if (ESP.getFreeHeap() < ORCH_RECALL_MIN_HEAP) return false;
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >=
         ORCH_TURN_MIN_LARGEST_BLOCK;
}

// Recall when embeddings cannot serve it: the keyword scan over the live store and
// the pending queue, inside the same read boundary, labelled with the cause so the
// prompt says recall is keyword-only (never a silent empty section). Below the
// recall floors the scan is skipped and the label says so.
static nimbus::orch::RecallOut keywordRecall(const String& queryText, int k,
                                             const nimbus::orch::Principal& who,
                                             const std::string& cause) {
  nimbus::orch::RecallScope scope;
  scope.nsAllow = {who.valid() ? who.ns : std::string("\x01none")};
  scope.nowHours = nowHours();
  scope.k = k;
  scope.lowMemory = !recallHeapOk();
  if (scope.lowMemory) alogi("memory: keyword recall skipped - low memory");   // RAM ring only
  Lock g;
  return nimbus::orch::lexicalRecall(g_vec, &g_pending, std::string(queryText.c_str()), scope, cause);
}

std::vector<std::string> recall(const String& queryText, int k,
                                const nimbus::orch::Principal& who) {
  return recallFor(queryText, k, who).hits;
}

nimbus::orch::RecallOut recallFor(const String& queryText, int k,
                                  const nimbus::orch::Principal& who) {
  nimbus::orch::RecallOut res;
  std::vector<std::string>& out = res.hits;
  if (nimbus::fault::active(nimbus::fault::MEMORY)) return res;   // resilience: no-memory turn
  if (queryText.length() == 0) return res;
  // Quick locked empty-check: skip the embed. Facts queued during an outage count:
  // a fresh device's first facts may all be waiting.
  { Lock g; if (g_vec.size() == 0 && g_pending.empty()) return res; }
  if (k <= 0) k = g_cfg.retrievalCount;
  if (!embeddings::available())
    return keywordRecall(queryText, k, who,
                         nimbus::orch::embedFailWords(nimbus::orch::EmbedFail::NoKey, ""));
  // Embeddings parked (out of credit, key rejected, offline): no doomed TLS embed
  // on every turn (about 1.8 s and a 10 KB heap dip each, 13-14 Oct).
  bool parked;
  { Lock g; parked = g_park.blocked(millis()); }
  if (parked) {
    const String why = embedParkedWhy();
    alogf("memory: recall skipped - embeddings parked (%s)", why.c_str());
    return keywordRecall(queryText, k, who, std::string(why.c_str()));
  }
  String err;
  std::vector<int8_t> qv = embedRaw(queryText, err);  // TLS - NOT under the lock
  noteRecallEmbed(!qv.empty(), err);
  if (qv.empty()) {
    alogf("memory: recall embed failed (%s) - keyword recall", err.c_str());
    std::string detail;
    const auto kind = nimbus::orch::classifyEmbedFail(std::string(err.c_str()), detail);
    return keywordRecall(queryText, k, who, nimbus::orch::embedFailWords(kind, detail));
  }
  // Composite-ranked recall: relevance x recency x importance, query-time expiry
  // filtering, near-dup collapse, MMR-on-ties - all tuned by the persisted MemConfig.
  nimbus::orch::RecallParams rp;
  rp.k = k;
  rp.relevanceThreshold   = g_cfg.relevanceThreshold;
  rp.recencyHalfLifeHours = (float)g_cfg.recencyHalfLifeHours;
  rp.mmrLambda            = g_cfg.mmrLambda;
  // v3.7.0 read boundary: STRICTLY the caller's own namespace - there is no
  // shared vector namespace, because one would let any tenant write facts that
  // get recalled into the ADMIN's prompt (persistent context poisoning). An
  // unattributed caller recalls NOTHING rather than falling back to a wildcard.
  if (who.valid()) rp.nsAllow.push_back(who.ns);
  else rp.nsAllow.push_back("\x01none");   // matches no entry - fail closed
  const uint32_t now = nowHours();
  std::vector<std::string> boostIds;
  Lock g;   // lock for the in-RAM recall + boost (the other task may be mutating g_vec)
  for (const auto& h : g_vec.recall(qv, rp, now)) {
    char pct[12];
    snprintf(pct, sizeof(pct), "[%d%%] ", (int)(h.importance * 100));
    out.push_back(std::string(pct) + h.content);
    boostIds.push_back(h.id);
  }
  // Reinforce recalled memories: bump importance + reset the TTL clock so what the
  // owner actually uses resists decay + eviction. In RAM only (no per-turn full-VDB
  // rewrite); the boosted values persist on the next mutating op (mem_write).
  // ⚠ PRE-SYNC GUARD (Release C3): boostAccessed resets createdAtHours to `now` -
  // with an unsynced clock (now≈0, boot-relative) that re-stamps a real-epoch
  // entry to "hour 0", and the moment SNTP syncs its age becomes ~half a century:
  // instantly expired. Skip the boost until the clock is real; recall itself is
  // unaffected (expiry clamps SAFE pre-sync - nothing expires).
  if (!boostIds.empty() && clockSynced()) g_vec.boostAccessed(boostIds, 0.05f, now);
  return res;
}

nimbus::orch::MemOpCounters opCounters() {
  Lock g;
  return g_memOps;
}

bool persistVectors() {
  Lock g;
  std::string blob = g_vec.serialize();
  // LittleFS overflow guard (degraded mode): never fill the internal partition. If
  // writing the blob would drop below the free floor, refuse + flag it (the model
  // keeps the vector in RAM this session but it won't persist) rather than exhaust
  // flash. The SD card has room to spare, so this only gates the no-card path.
  if (!effHaveSd()) {
    size_t freeB = LittleFS.totalBytes() - LittleFS.usedBytes();
    if (freeB < blob.size() + kFlashFreeFloor) {
      if (!g_flashFull)
        alogf("memory: vectors flash full (free=%uKB need=%uKB) - persist paused",
              (unsigned)(freeB / 1024), (unsigned)(blob.size() / 1024));
      g_flashFull = true;
      return false;
    }
    g_flashFull = false;
  }
  bool w = writeBlobAtomic(g_vecPath, blob);
  if (effHaveSd()) noteSdIoResult(w);   // real SD write outcome feeds the demote watchdog
  if (!w) alog("memory: vector persist failed");
  persistArchive();   // the archive rides the same persist cadence (dirty-gated, SD-only)
  return w;
}

// Persist the TTL-expired cold store (CUM-225). SD-only and dirty-gated: a dream that
// archived nothing, or a card-less device, never rewrites the blob. Called from
// persistVectors so every mutating path that touches the live store (dream prune,
// restore) also flushes any archive change in the same step.
void persistArchive() {
  Lock g;
  if (!effHaveSd() || !g_archive.dirty()) return;
  if (writeBlobAtomic(g_arcPath, g_archive.serialize())) g_archive.markClean();
  else alog("memory: archive persist failed");
}

// ---- the pending-embed queue (lane P) -----------------------------------------
// Make the queue durable. On the card while it answers; with no card, or a card
// lost mid-run, on LittleFS behind the same free-space floor as the degraded
// vector persist. false = nothing durable was written, and the write seam then
// refuses honestly instead of claiming "queued". An empty queue removes its file.
bool persistPending() {
  Lock g;
  if (g_erasing) return false;
  const std::string blob = g_pending.serialize();
  if (effHaveSd()) {
    const bool ok = g_pending.empty() ? (!g_fs->exists(kPendSdPath) || g_fs->remove(kPendSdPath))
                                      : writeBlobAtomic(kPendSdPath, blob);
    noteSdIoResult(ok);
    if (ok) {
      if (g_pendOnLfs) { LittleFS.remove(kPendLfsPath); g_pendOnLfs = false; }
      g_pending.markClean();
      return true;
    }
    // The card refused the write: keep a durable copy on LittleFS instead.
  }
  if (g_pending.empty()) {
    LittleFS.remove(kPendLfsPath);
    g_pendOnLfs = false;
    g_pending.markClean();
    return true;
  }
  const size_t freeB = LittleFS.totalBytes() - LittleFS.usedBytes();
  if (freeB < blob.size() + kFlashFreeFloor) {
    alogf("memory: pending queue not persisted (flash free %uKB)", (unsigned)(freeB / 1024));
    return false;
  }
  if (!writeBlobAtomicOn(LittleFS, kPendLfsPath, blob)) return false;
  g_pendOnLfs = g_haveSd;   // a card device writing here means the card is away
  g_pending.markClean();
  return true;
}

// Boot: the card copy, then any LittleFS fallback copy (written while the card
// was away, or on a cardless boot) merged in, deduplicated by id.
static void loadPending() {
  auto readAll = [](fs::FS& fsys, const char* path) -> std::string {
    File f = fsys.open(path, FILE_READ);
    if (!f) return std::string();
    String b = f.readString();
    f.close();
    return std::string(b.c_str(), b.length());
  };
  if (g_haveSd) {
    const std::string sd = readAll(*g_fs, kPendSdPath);
    if (!sd.empty() && !g_pending.deserialize(sd))
      alog("memory: pending queue blob partial - kept what parsed");
  }
  const std::string lfs = readAll(LittleFS, kPendLfsPath);
  if (!lfs.empty()) {
    nimbus::orch::PendingEmbeds extra;
    if (!extra.deserialize(lfs)) alog("memory: pending fallback blob partial - kept what parsed");
    for (const auto& e : extra.getAll()) g_pending.enqueue(e);
    g_pendOnLfs = true;
    if (g_haveSd && persistPending()) alog("memory: pending fallback merged back onto the card");
  }
  g_pending.markClean();
}

int pendingCount() { Lock g; return (int)g_pending.size(); }

WriteOutcomes writeOutcomes() { Lock g; return g_outcomes; }

std::string pendingNotes(const std::string& ns) { Lock g; return g_pending.takeNotes(ns); }

String embedParkedWhy() {
  Lock g;   // the detail string is rewritten by other tasks
  if (!g_park.parked()) return String();
  return String(nimbus::orch::embedFailWords(g_park.reason(), g_park.detail()).c_str());
}

void noteEmbedConfigChanged() { Lock g; g_park.clear(); }

// RAM-only predicate polled by the Telegram task's staged-work slot (cheap: no
// I/O). Due when facts wait, the park allows a try, the device has the memory a
// TLS embed needs (the recall admission gate plus a largest-block floor; below it
// the drain simply waits, the queue is durable), and the last step was 20 s ago.
bool embedDrainDue() {
  if (!g_begun || g_erasing || g_pending.empty()) return false;
  const uint32_t now = millis();
  if (now < kDrainBootGraceMs || now - g_lastDrainMs < kDrainEveryMs) return false;
  // Lock-free on purpose: a few word-sized reads polled often on the Telegram
  // task; drainOne re-checks the park under the lock before any network call.
  if (g_park.blocked(now) || !embeddings::available()) return false;
  return recallHeapOk();
}

// The principal that writes into `ns` today: the owner's namespace is the
// admin's; a member's is "chat:<id>" under that chat's current role and quota.
static bool principalForNs(const std::string& ns, nimbus::orch::Principal& who) {
  using nimbus::orch::Role;
  if (ns.empty() || ns == nimbus::orch::kOwnerNs) {
    who = nimbus::orch::principalForRole("web", Role::Admin);
    return true;
  }
  if (ns.rfind("chat:", 0) != 0) return false;   // mcp / unknown: no writer to resolve
  const std::string chat = ns.substr(5);
  nimbus::orch::Quota q;
  orchestrator::tenantQuotaOf(chat, q);
  who = nimbus::orch::principalForRole(chat, orchestrator::roleOfChat(String(chat.c_str())), q);
  return true;
}

// One drain step, on the Telegram task between its own work (never inside a
// turn). The stack high-water mark is logged so the bench can check it keeps
// 4 KB free on tg_poll.
void drainPendingStep() {
  g_lastDrainMs = millis();
  if (g_pending.empty()) return;
  nimbus::orch::EmbedDrainDeps d;
  d.principalForNs = principalForNs;
  d.withLock = [](const std::function<void()>& fn) { Lock g; fn(); };
  d.persistVectors = [] { return persistVectors(); };
  const nimbus::orch::EmbedDrainResult r = nimbus::orch::drainOne(g_ctx, d);
  alogf("memory: pending drain %s %s (left=%d, stack free %u B)", nimbus::orch::embedDrainStepName(r.step),
        r.id.c_str(), pendingCount(), (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}

// Dream phase 1: up to `maxEntries` drain steps within `budgetMs`, so the night's
// reflection runs over a store that holds the facts queued during the day.
int drainPendingBurst(int maxEntries, uint32_t budgetMs) {
  const uint32_t t0 = millis();
  int stored = 0;
  for (int i = 0; i < maxEntries && !g_pending.empty() && millis() - t0 < budgetMs; i++) {
    {
      Lock g;
      if (g_park.blocked(millis())) break;
    }
    nimbus::orch::EmbedDrainDeps d;
    d.principalForNs = principalForNs;
    d.withLock = [](const std::function<void()>& fn) { Lock g; fn(); };
    d.persistVectors = [] { return persistVectors(); };
    const nimbus::orch::EmbedDrainResult r = nimbus::orch::drainOne(g_ctx, d);
    if (r.step == nimbus::orch::EmbedDrainStep::Stored) stored++;
    if (r.step == nimbus::orch::EmbedDrainStep::Failed || r.step == nimbus::orch::EmbedDrainStep::Parked ||
        r.step == nimbus::orch::EmbedDrainStep::NotDurable)
      break;
  }
  g_lastDrainMs = millis();
  return stored;
}

#ifdef NIMBUS_TEST
String setEmbedFault(bool on) {
  nimbus::fault::set(nimbus::fault::EMBED_FAIL, on);
  if (!on) { Lock g; g_park.clear(); }   // the outage is over: the next attempt goes straight out
  return String("FAULT embed_fail=") + (on ? "1" : "0") + " pending=" + pendingCount();
}

// Off retries the save at once, so notes changed during the fault land in the file.
String setScratchFault(bool on) {
  Lock g;
  g_scratchFault = on;
  if (on) return String("FAULT scratch_fail=1");
  return String("FAULT scratch_fail=0 saved=") + (persistScratchpad() ? "1" : "0");
}
#endif

// The scratchpad file: written atomically and read back (the moved-store rule), and
// only when its bytes differ from the ones that last landed. A save that did not land
// is reported to the persist ledger ("lfs_scratch"), so the scratchpad route answers
// 507 and /api/state names it; the notes stay in RAM and the next persist tries again.
// The result also reaches the scratchpad's own failing flag, so the tool result, the
// turn's effect ledger and the prompt header all say when the notes are RAM only.
bool persistScratchpad() {
  Lock g;
  const std::string blob = g_scratch.serialize();
  ScratchIo io;
  const auto out = nimbus::store::persistIfChanged(io, g_scratchLanded, blob);
  const bool ok = out != nimbus::store::PersistOutcome::Failed;
  if (!ok) {
    if (const uint32_t n = g_scratchFailLog.noteFailure(millis()))
      alogf("scratchpad: persist FAILED x%u (%u B) - contents survive in RAM only",
            (unsigned)n, (unsigned)blob.size());
  }
  g_scratch.setSaveFailing(!ok);
  return nimbus::sys::notePersist(ok, "lfs_scratch");
}

// The retrieval knobs stay in NVS (owner config, about 100 B): asked of the reserve
// and reported, so the memory-config route answers 507 when they did not store.
bool persistMemConfig() {
  Lock g;
  std::string blob = g_cfg.serialize();
  Preferences p;
  if (!p.begin(kScratchNs, false)) return nimbus::sys::notePersist(false, kMemCfgKey);
  const bool admitted =
      nimbus::sys::nvsAdmit(kScratchNs, kMemCfgKey, nimbus::store::NvsValKind::Str, blob.size());
  const bool ok =
      nimbus::sys::notePersist(admitted && p.putString(kMemCfgKey, blob.c_str()) == blob.size(), kMemCfgKey);
  p.end();
  return ok;
}

void persistEpisodic() {
  // The SD append-log self-persists (every addMessage IS the durable write), so a
  // whole-blob rewrite only applies to the no-SD in-memory store.
  if (g_epiLog) return;
  Lock g;
  if (!writeBlobAtomic(g_epiPath, g_epi.serialize())) alog("memory: episodic persist failed");
}

// ---- CUM-406: memory restore write path --------------------------------------
// restoreImport() parses ONE backup artifact's JSON (the exact shapes
// backup_device.py writes), applies it under the shared memory Lock, persists, and
// returns the JSON reply POST /api/mem/import sends back. dryRun validates + counts
// and writes nothing. The parse/apply core is portable (memory_restore.h) and shares
// its code path with the native round-trip test; the device seam here adds only the
// Lock, the persist, and (for vectors) the tier/embed bookkeeping.
// SECRETS: never touches NVS keys/tokens - only the in-RAM memory engines.
// AUTHORITATIVE: nothing on the device re-clobbers a restore (there is no sync); the
// backed-up ids/timestamps are preserved so recall/decay/TTL behave as on the source.
// Idempotent: vectors REPLACE-BY-ID (safe to re-run/page); episodic APPENDS
// (id-deduped within the call - restore_device.py guards a non-empty store);
// scratchpad REPLACES.

static std::string importErr(const char* msg) {
  JsonDocument d;
  d["ok"] = false; d["error"] = msg;
  std::string out; serializeJson(d, out); return out;
}

static std::string importVectors(JsonArrayConst entries, bool dryRun) {
  Lock g;
  // Vectors must match the store's embedding width. On the common path the width is
  // the 256-dim default; for a device configured with a wider model the operator sets
  // the SAME embed config as the source BEFORE restore (which sets the width) - see the
  // runbook. A row whose width still doesn't match is reported (widthErrors), never a
  // silent drop, so the mismatch is visible rather than looking like an empty restore.
  const int before = g_vec.size();
  restore::VecReport rep = restore::applyVectors(g_vec, entries, g_vec.dims(), dryRun);
  const int after = g_vec.size();
  if (!dryRun && (rep.added || rep.replaced)) {
    persistVectors();
    // Restored vectors freeze the embed config: a later change must go through the
    // explicit reset-and-wipe path rather than silently orphan them.
    if (!store::embedLocked()) store::setEmbedLocked(true);
  }
  // Over-cap honesty: add() evicts silently at the cap, so reconcile added vs actually
  // stored. A restore that exceeds the device vector cap is NOT lossless; report how
  // many rows this call displaced rather than claiming they all landed.
  const int evicted = dryRun ? 0 : restore::evictedCount(before, rep.added, after);
  JsonDocument d;
  d["ok"] = true; d["kind"] = "vectors"; d["dryRun"] = dryRun;
  d["added"] = rep.added; d["replaced"] = rep.replaced;
  d["skippedNoVec"] = rep.skippedNoVec; d["widthErrors"] = rep.widthErrors;
  d["badRows"] = rep.badRows; d["stored"] = g_vec.size(); d["evicted"] = evicted;
  d["dims"] = g_vec.dims();
  std::string out; serializeJson(d, out); return out;
}

// Defense in depth: reject an oversized episodic array outright (no partial apply) so
// one giant body cannot monopolize the recursive memory mutex regardless of dedup cost.
// The restore tool pages episodic in 64-row batches; this cap sits comfortably above
// that while bounding the per-call work done under the held Lock.
static constexpr int kMaxEpisodicRowsPerImport = 512;

static std::string importEpisodic(JsonArrayConst msgs, bool dryRun) {
  // Cap check happens before the Lock so an oversized body is rejected without ever
  // taking the mutex, and never half-applies.
  if ((int)msgs.size() > kMaxEpisodicRowsPerImport)
    return importErr("episodic batch too large (max 512 rows per import)");
  Lock g;
  // Truncation honesty: the no-SD store is a bounded ring (oldest evicted), so count
  // how many messages actually landed, not just how many we tried to append.
  const int before = g_epiActive->messageCount();
  restore::EpiReport rep = restore::applyEpisodic(*g_epiActive, msgs, dryRun);
  const int after = g_epiActive->messageCount();
  // The SD append-log self-persists on each addMessage; the no-SD in-memory store
  // needs an explicit whole-blob flush.
  if (!dryRun && rep.added && !g_epiLog) persistEpisodic();
  const int stored = dryRun ? 0 : (after - before);
  // Genuine restore truncation is when the ring could not hold all the rows we
  // restored, i.e. added exceeds the FINAL size. Pre-existing rows displaced by a
  // --force append to a near-cap store are not restore truncation, so compare added
  // against `after` (not `stored`). Cases: fresh before=0/added=2500/after=2000 -> 500;
  // force before=1990/added=20/after=2000 -> 0; force before=1990/added=2500/after=2000 -> 500.
  const int truncated = (!dryRun && rep.added > after) ? rep.added - after : 0;
  JsonDocument d;
  d["ok"] = true; d["kind"] = "episodic"; d["dryRun"] = dryRun;
  d["added"] = rep.added; d["dupSkipped"] = rep.dupSkipped; d["badRows"] = rep.badRows;
  d["stored"] = stored; d["truncated"] = truncated; d["total"] = after;
  std::string out; serializeJson(d, out); return out;
}

static std::string importScratchpad(JsonObjectConst obj, bool dryRun) {
  Lock g;
  restore::ScratchReport rep = restore::applyScratchpad(g_scratch, obj, dryRun);
  if (!dryRun) persistScratchpad();
  JsonDocument d;
  d["ok"] = true; d["kind"] = "scratchpad"; d["dryRun"] = dryRun;
  d["active"] = rep.active; d["short"] = rep.shortN; d["mid"] = rep.midN; d["long"] = rep.longN;
  std::string out; serializeJson(d, out); return out;
}

std::string restoreImport(const char* body, size_t len) {
  // Parse straight from the (PSRAM) body buffer - no internal-heap copy of a large blob.
  JsonDocument doc(&agent::PsramJsonAllocator::instance());
  if (deserializeJson(doc, body, len)) return importErr("invalid JSON body");
  JsonObjectConst root = doc.as<JsonObjectConst>();
  const char* kind = root["kind"] | "";
  const bool dryRun = root["dryRun"] | false;
  // Validate the top-level shape BEFORE any write, so a malformed or truncated body is
  // rejected outright and can never half-apply (or, for scratchpad, wipe a live tier).
  if (!strcmp(kind, "vectors")) {
    if (const char* e = restore::arrayEnvelopeError(root, "entries")) return importErr(e);
    return importVectors(root["entries"].as<JsonArrayConst>(), dryRun);
  }
  if (!strcmp(kind, "episodic")) {
    if (const char* e = restore::arrayEnvelopeError(root, "messages")) return importErr(e);
    return importEpisodic(root["messages"].as<JsonArrayConst>(), dryRun);
  }
  if (!strcmp(kind, "scratchpad")) {
    if (!restore::scratchpadEnvelopeValid(root))
      return importErr("scratchpad must be an object with active/short/mid/long");
    return importScratchpad(root["scratchpad"].as<JsonObjectConst>(), dryRun);
  }
  return importErr("kind must be vectors|episodic|scratchpad");
}

// Durable media sidecar (docs/orchestrator-storage.md §4). Content-address `bytes`
// to /mem/blobs/<hash>.<ext> (identical bytes dedup to one file) and return the
// path for an episodic row's blobPath. SD-gated: returns "" with no card (degraded
// mode keeps media ephemeral), so the active card-less path is unchanged.
static String persistBlobSidecar(const std::string& bytes, const char* ext) {
  if (!effHaveSd() || bytes.empty()) return String();
  std::string path = nimbus::orch::blobPath(kBlobDir, nimbus::orch::blobHash(bytes),
                                            ext ? ext : "");
  if (!g_fs->exists(path.c_str())) {              // dedup: skip if already stored
    if (!writeBlobAtomic(path, bytes)) { alog("memory: blob sidecar write failed"); return String(); }
  }
  return String(path.c_str());
}

// Glass Box P4: the full text behind a clipped trace row. Rides the SAME
// content-addressed store as media sidecars - dedup (a repeated tool result
// stores once) and retention pruning are already implemented there.
String persistTraceBlob(const String& text) {
  if (!traceActive() || !text.length()) return String();
  return persistBlobSidecar(std::string(text.c_str()), "txt");
}

void captureSession(const char* sessionId, const char* provider, const String& title) {
  if (!g_begun || !sessionId) return;
  Lock g;   // shared with the web/MCP task
  // Preserve the first record for this id: addSession upserts (would reset the
  // start time + title every turn), so skip if it already exists.
  for (const auto& e : g_epiActive->sessions())
    if (e.id == sessionId) return;
  EpisodicSession s;
  s.id = sessionId;
  s.startedHours = nowHours();
  s.provider = provider ? provider : "";
  s.title = std::string(title.c_str());
  s.status = "active";
  g_epiActive->addSession(s);
  // session rows persist with the next message capture (cheap to defer).
}

// A row's references and stamp: the blob, the tags, and `tsHours` when the caller knows
// when it happened (an inbound Telegram item's send time; 0 = now).
struct RowRefs {
  const String& blobPath;
  const String& tags;
  uint32_t      tsHours;
};

static String captureMessageAt(const char* sessionId, const char* role, nimbus::orch::MsgKind kind,
                               const String& text, const RowRefs& refs) {
  const String& blobPath = refs.blobPath;
  const String& tags = refs.tags;
  const uint32_t tsHours = refs.tsHours;
  if (!g_begun) return String();
  Lock g;   // shared with the web/MCP task
  EpisodicMessage m;
  char id[16];
  snprintf(id, sizeof(id), "m%08x", (unsigned)(++g_epiSeq));
  m.id = id;
  m.sessionId = sessionId ? sessionId : "";
  m.tsHours = tsHours ? tsHours : nowHours();
  m.role = role ? role : "";
  m.kind = kind;
  m.text = std::string(text.c_str());
  m.blobPath = std::string(blobPath.c_str());
  m.tags = std::string(tags.c_str());   // e.g. "from:<sender>" (unified chat label)
  // Feed the SD demote watchdog: on the append-log, a failed day-stream write bumps
  // unpersistedCount() (the row falls back to the RAM ring) - the sudden-loss signal.
  const bool watch = effHaveSd() && g_epiLog;
  const size_t unpBefore = watch ? g_epiLog->unpersistedCount() : 0;
  g_epiActive->addMessage(m);
  if (watch) noteSdIoResult(g_epiLog->unpersistedCount() == unpBefore);
  persistEpisodic();   // no-op for the SD append-log (addMessage already durable)
  return String(m.id.c_str());
}

String captureMessage(const char* sessionId, const char* role, nimbus::orch::MsgKind kind,
                      const String& text, const String& blobPath, const String& tags) {
  return captureMessageAt(sessionId, role, kind, text, RowRefs{blobPath, tags, 0});
}

// ---- Glass Box P3: per-turn dossier files ----------------------------------
// One file per turn under /mem/trace, named by the turn id. A bounded RING (both
// file count AND total bytes) so the richest-but-biggest artifact can never run
// away with the card; the 6 h retention sweep drops stragglers by age too.
static const char* kTraceDir = "/mem/trace";

// Only ever build a path from a validated id - never from caller/query text.
static bool validTurnId(const String& id) {
  if (id.length() < 2 || id.length() > 12 || id[0] != 'm') return false;
  for (size_t i = 1; i < id.length(); i++)
    if (!isxdigit((unsigned char)id[i])) return false;
  return true;
}

static String traceFilePath(const String& turnId) {
  return String(kTraceDir) + "/" + turnId + ".txt";
}

// Evict oldest-by-name until the ring fits. Ids are monotonic, so lexicographic
// order IS chronological order - no stat() per file needed.
static void traceRingPrune() {
  fs::FS& fsd = dataFs();
  std::vector<String> names;
  size_t total = 0;
  File dir = fsd.open(kTraceDir);
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String n = f.name();
    size_t sz = f.size();
    f.close();
    if (!n.endsWith(".txt")) continue;
    int slash = n.lastIndexOf('/');
    if (slash >= 0) n = n.substring(slash + 1);
    names.push_back(n);
    total += sz;
  }
  dir.close();
  std::sort(names.begin(), names.end(),
            [](const String& a, const String& b) { return strcmp(a.c_str(), b.c_str()) < 0; });
  size_t i = 0;
  while (i < names.size() &&
         ((int)(names.size() - i) > kTraceFilesMax || total > kTraceBytesMax)) {
    String p = String(kTraceDir) + "/" + names[i];
    File f = fsd.open(p, FILE_READ);
    size_t sz = f ? f.size() : 0;
    if (f) f.close();
    if (fsd.remove(p)) total -= sz;
    i++;
  }
}

bool writeTraceFile(const String& turnId, const char* buf, size_t len) {
  // Factory-sweep barrier: the dossier writer RECREATES /mem/trace itself, so a
  // turn finishing after the quiesce timeout would hand the next owner its last
  // conversation trace (release-gate verifier, 2026-10-04).
  if (g_erasing) return false;
  if (!traceActive() || !buf || !len || !validTurnId(turnId)) return false;
  fs::FS& fsd = dataFs();
  fsd.mkdir(kTraceDir);
  File f = fsd.open(traceFilePath(turnId), FILE_WRITE);
  if (!f) return false;
  const size_t wrote = f.write((const uint8_t*)buf, len);
  f.close();
  if (wrote != len) { fsd.remove(traceFilePath(turnId)); return false; }
  traceRingPrune();
  return true;
}

char* readTraceFilePs(const String& turnId, size_t& outLen) {
  outLen = 0;
  if (!validTurnId(turnId)) return nullptr;
  File f = dataFs().open(traceFilePath(turnId), FILE_READ);
  if (!f) return nullptr;
  const size_t n = f.size();
  if (!n) { f.close(); return nullptr; }
  char* b = (char*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  if (!b) { f.close(); return nullptr; }
  const size_t got = f.read((uint8_t*)b, n);
  f.close();
  if (got != n) { free(b); return nullptr; }
  outLen = n;
  return b;
}

// Reserve an id without writing a row (Glass Box turn identity). Same counter
// and format as captureMessage, so a minted turn id sorts with the rows it tags.
String mintRowId() {
  if (!g_begun) return String();
  Lock g;
  char id[16];
  snprintf(id, sizeof(id), "m%08x", (unsigned)(++g_epiSeq));
  return String(id);
}

// Glass-box trace gate (A4): rows write only when the SD append-log is live
// (degraded mode rewrites the whole LittleFS blob per row - actively harmful)
// AND the owner knob is on. Cheap enough to call per hook fire.
bool traceActive() {
  return g_begun && g_epiLog && !sdLost() && agent::store::orchTrace();
}

// Device-event timeline (Glass Box A3): one Log row in the reserved "system"
// session. captureSession keeps sessions.jsonl at exactly ONE extra row ever
// (upsert dedups by id). Events are ~10/day - negligible against the 30-day
// prune - and, unlike trace rows, they capture in degraded mode too (the
// timeline is most valuable exactly when the device is having a bad day).
//
// ⚠ PRE-SNTP HOLD (prism Release-A finding, medium): a row captured before the
// clock syncs (the BOOT row on a power-on/brownout - RTC time doesn't survive
// those) would stamp tsHours≈0, land in day-file d0, be invisible to every
// since_hours window, and be DELETED by the first retention prune once the clock
// jumps (~day 20 000 vs cutoff day-30). So unsynced-clock events are held and
// flushed with real timestamps at first sync - the exact "did you lose power?"
// record the timeline exists for survives.
//
// Crash forensics F3: the hold is a small file on the data store (pending_events.h:
// /mem/pending_ev.txt on SD, /data/pending_ev.txt on flash, 16 rows / 4 KB, never
// NVS), so it survives a reset before sync - offline boots and early crash loops used
// to lose their boot rows with the RAM list. The first sync on any later boot stamps
// the leftovers (labelled with the boot that captured them) and then this boot's rows.
// The RAM list below is only the fallback when the file cannot be written.
static std::vector<std::pair<std::string, std::string>> s_pendingEv;   // (ev, text)
bool clockSynced() { return time(nullptr) >= 1600000000; }             // ~2020-09

static const char* pendingEvPath() {
  return g_haveSd ? "/mem/pending_ev.txt" : "/data/pending_ev.txt";
}
// Whether the file may hold rows: unknown until the first flush looks (rows from an
// earlier boot), then tracked here so a synced device does not probe the card for a
// file that is not there on every event.
static bool s_pendingFileMaybe = true;
// One flush owns the file at a time: captureEvent runs on the loop and on tg_poll, and
// at first sync both can flush. The owner claims it under the Lock, writes the rows
// without it, then deletes the file; anyone else skips the file meanwhile, so no row
// is written twice. (Delete-after-write keeps a reset mid-flush from losing rows.)
static bool s_pendingFlushOwned = false;
// Rows THIS boot appended to the file (CUM-714). Rows are only ever appended at the
// end, so the flush labels exactly the last s_heldThisBoot rows as this boot's own and
// every other row as an earlier boot's, whatever boot numbers they carry.
static size_t s_heldThisBoot = 0;

// Caller holds the Lock. One file's bytes (bounded), "" when it is not there.
static std::string readSmallFile(const std::string& path) {
  File f = g_fs->open(path.c_str(), FILE_READ);
  if (!f) return {};
  std::string out;
  uint8_t buf[256];
  while (out.size() <= nimbus::orch::kPendingEvMaxBytes) {
    const int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    out.append((const char*)buf, (size_t)n);
  }
  f.close();
  return out;
}

// Caller holds the Lock. The file is rewritten through a .tmp sibling and a rename
// (writeBlobAtomicOn), so a reset can leave the .tmp beside the file (after the .tmp
// was written) or alone (between the remove and the rename): the copy that holds more
// rows wins (choosePendingContent), and no row is lost either way.
static std::string pendingEvTmpPath() { return std::string(pendingEvPath()) + ".tmp"; }
static std::string readPendingEvFile() {
  return nimbus::orch::choosePendingContent(readSmallFile(pendingEvPath()), readSmallFile(pendingEvTmpPath()));
}

// Append one held event to the file. Caller holds the Lock. False when the store
// is not writable (the caller then keeps the event in RAM).
static bool holdPendingEvent(const char* ev, const String& text) {
  if (g_erasing || (g_haveSd && !effHaveSd())) return false;
  nimbus::orch::PendingEvent e;
  e.bootNo = nimbus::errlog::bootNumber();
  e.upMs = millis();
  e.ev = ev ? ev : "misc";
  e.text = std::string(text.c_str());
  const std::string body = nimbus::orch::appendPendingEvent(readPendingEvFile(), e);
  // Whole-file rewrite through a .tmp + rename: a reset mid-write keeps the previous
  // rows, and a false return means the file did not change, so the caller's RAM
  // fallback never writes the same row twice (CUM-714).
  if (!writeBlobAtomicOn(*g_fs, pendingEvPath(), body)) return false;
  s_pendingFileMaybe = true;
  ++s_heldThisBoot;
  return true;
}

// One-shot at first sync (Release C3): heal ANY vector stamped with the
// boot-relative clock (a write/update/unpin/boost that ran in the WiFi-up-
// but-SNTP-pending window) - real epoch-hours are ~495k, boot-relative are
// tiny, so <400000 cleanly identifies them. Without this they all expire the
// instant the clock becomes real. Caller holds the Lock.
static void healPreSyncStampsOnce() {
  static bool s_stampsHealed = false;
  if (s_stampsHealed) return;
  s_stampsHealed = true;
  int n = g_vec.restampPreSync(400000, nowHours());
  if (n) {
    persistVectors();
    alogf("memory: re-stamped %d pre-sync vector(s) at first clock sync", n);
  }
}

void flushPendingEvents() {
  if (!g_begun || !clockSynced()) return;
  LoopLock loopGuard;   // the power-off path calls this from the main loop
  if (!loopGuard) return;   // busy: the rows stay held for the next flush
  healPreSyncStampsOnce();
  std::vector<std::pair<std::string, std::string>> pend;
  nimbus::orch::PendingFile held;
  bool own = false;
  {
    Lock g;
    if (s_pendingFileMaybe && !s_pendingFlushOwned) {
      held = nimbus::orch::parsePendingFile(readPendingEvFile());
      own = !held.rows.empty() || held.dropped;
      s_pendingFlushOwned = own;
      if (!own) s_pendingFileMaybe = false;
    }
    pend.swap(s_pendingEv);
  }
  if (held.rows.empty() && !held.dropped && pend.empty()) return;
  captureSession("system", "device", "Device timeline");
  for (const auto& r : nimbus::orch::planPendingFlush(held, s_heldThisBoot))
    captureMessage("system", "system", nimbus::orch::MsgKind::Log, String(r.text.c_str()), "",
                   String("ev:") + r.ev.c_str());
  for (auto& e : pend)
    captureMessage("system", "system", nimbus::orch::MsgKind::Log,
                   String(e.second.c_str()) + " [pre-sync, stamped at first clock sync]",
                   "", String("ev:") + e.first.c_str());
  // Truncate only after the rows were written: a reset mid-flush re-flushes them
  // (a duplicate row) rather than losing them.
  if (own) {
    Lock g;
    g_fs->remove(pendingEvPath());
    g_fs->remove(pendingEvTmpPath().c_str());
    s_pendingFileMaybe = false;
    s_pendingFlushOwned = false;
    s_heldThisBoot = 0;
  }
}

void captureEvent(const char* ev, const String& text) {
  if (!g_begun) return;
  // The main loop captures some events itself (an SD edge, a mode switch): when the
  // card lock is busy there, the event is parked and written by a later pass.
  LoopLock loopGuard;
  if (!loopGuard) {
    parkEvent(ev, text);
    return;
  }
  if (!clockSynced()) {
    Lock g;
    if (!holdPendingEvent(ev, text) && s_pendingEv.size() < 8)
      s_pendingEv.emplace_back(ev ? ev : "misc", std::string(text.c_str()));
    return;
  }
  flushPendingEvents();   // older held rows first, preserving order
  captureSession("system", "device", "Device timeline");
  captureMessage("system", "system", nimbus::orch::MsgKind::Log, text, "",
                 String("ev:") + (ev ? ev : "misc"));
}

// Capture a media message with a DURABLE sidecar (§4): store the bytes content-
// addressed under /mem/blobs and record an episodic row referencing it. SD-gated -
// with no card persistBlobSidecar returns "" and (matching degraded mode: "media
// ephemeral, no durable audit") we capture nothing, leaving the active path intact.
void captureMedia(const char* sessionId, const char* role, nimbus::orch::MsgKind kind,
                  const String& text, const uint8_t* bytes, size_t len, const char* ext) {
  if (!g_begun || !effHaveSd() || !bytes || !len) return;
  String path = persistBlobSidecar(std::string((const char*)bytes, len), ext);
  if (path.length()) captureMessage(sessionId, role, kind, text, path);
}

bool captureMediaFile(const char* sessionId, const char* role, nimbus::orch::MsgKind kind,
                      const String& text, const char* srcPath, const char* ext, int64_t sentEpoch,
                      std::string* refOut) {
  if (!g_begun || !effHaveSd() || !srcPath) return false;
  if (g_erasing) return false;   // durable store is being wiped - don't write a new blob into it
  uint8_t buf[512];
  // Pass 1: stream-hash the source file (on LittleFS) in a small buffer - no full load.
  File f = LittleFS.open(srcPath, FILE_READ);
  if (!f) return false;
  nimbus::orch::BlobHasher hasher;
  size_t total = 0, n;
  while ((n = f.read(buf, sizeof(buf))) > 0) { hasher.update(buf, n); total += n; }
  f.close();
  if (total == 0) return false;
  std::string path = nimbus::orch::blobPath(kBlobDir, hasher.hex(), ext ? ext : "");
  // Pass 2: stream-copy to the SD sidecar via a .part temp + rename (crash-atomic).
  // Content-addressed, so an identical blob already present is reused, not re-copied.
  if (!g_fs->exists(path.c_str())) {
    File src = LittleFS.open(srcPath, FILE_READ);
    std::string tmp = path + ".part";
    File dst = g_fs->open(tmp.c_str(), FILE_WRITE);
    if (!src || !dst) { if (src) src.close(); if (dst) dst.close(); alog("memory: media blob open failed"); return false; }
    bool ok = true;
    while ((n = src.read(buf, sizeof(buf))) > 0) { if (dst.write(buf, n) != n) { ok = false; break; } }
    src.close(); dst.close();
    if (!ok) { g_fs->remove(tmp.c_str()); alog("memory: media blob copy failed"); return false; }
    g_fs->remove(path.c_str());
    if (!g_fs->rename(tmp.c_str(), path.c_str())) { alog("memory: media blob rename failed"); return false; }
  }
  // L19-51: an inbound item is stamped with its original send time, and the exact time
  // rides a "sent:<epoch>" tag, so a late or redelivered item never reads as new.
  const bool sent = sentEpoch >= 1577836800LL;   // a plausible Telegram date
  char tag[32] = "";
  if (sent) snprintf(tag, sizeof(tag), "sent:%lld", (long long)sentEpoch);
  const String blob(path.c_str()), tags(tag);
  captureMessageAt(sessionId, role, kind, text, RowRefs{blob, tags, sent ? (uint32_t)(sentEpoch / 3600) : 0});
  if (refOut) *refOut = hasher.hex();
  return true;
}

// Battery telemetry day-files (audit 2026-07-24: the ONE unbounded SD writer -
// ~300 KB/day worst case, never deleted). Same day-numbered naming, same
// retention window. Best-effort: a failed remove retries next pass. Names are
// read with the directory walk (no file is opened per entry).
static void pruneBatteryDays(uint32_t cutoffDay) {
  fs::FS& sd = dataFs();
  File dir = sd.open("/mem/battery");
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }
  int removed = 0;
  for (;;) {
    bool isDir = false;
    const String path = dir.getNextFileName(&isDir);
    if (path.length() == 0) break;
    if (isDir) continue;
    const int slash = path.lastIndexOf('/');
    const String name = slash >= 0 ? path.substring(slash + 1) : path;   // "d<N>.jsonl"
    if (!name.startsWith("d")) continue;
    const long day = name.substring(1).toInt();
    const String full = String("/mem/battery/") + name;
    if (day > 0 && (uint32_t)day < cutoffDay &&
        !nimbus::orch::isRetentionExempt(full.c_str()) && sd.remove(full))
      removed++;
  }
  dir.close();
  if (removed) alogf("memory: retention pruned %d battery day-files", removed);
}

nimbus::orch::RetentionStatus pruneRetention(int retentionDays, nimbus::orch::EpiPruneReport* report) {
  using nimbus::orch::RetentionStatus;
  // Cheap gate before the lock: a turn in flight owns the card and the heap.
  if (orchestrator::turnInFlight()) return RetentionStatus::DeferredTurn;
  const uint32_t nowDay = nowHours() / 24;
  // prune scans + mutates the episodic store shared with the turn and web tasks.
  // Only TRIED: this runs on the watchdog-fed loop task, and the lock can be held
  // across a memory tool's embedding round-trip; a busy lock defers the pass to
  // the next loop pass instead of stalling the loop behind it.
  if (!tryLock(50)) return RetentionStatus::DeferredBusy;
  // Checked again under the lock (a turn can start while we waited for it); the
  // portable gate also refuses with no store or a clock short of the window.
  nimbus::orch::RetentionResult res = nimbus::orch::runEpisodicRetention(
      g_epiLog, nowDay, retentionDays, kBlobDir, orchestrator::turnInFlight());
  if (res.status != RetentionStatus::Ran) {
    unlock();
    return res.status;
  }
  const nimbus::orch::EpiPruneReport& rep = res.report;
  pruneBatteryDays(res.cutoffDay);
  // Glass Box P3 turn dossiers: the ring bounds SIZE on every write; this bounds
  // AGE, so a quiet device doesn't keep month-old turn anatomy after its episodic
  // rows are gone. Ids are opaque here, so age comes from the ring, not the name:
  // simply re-run the ring prune, then drop anything left if the whole directory
  // predates the window (cheap - the ring keeps this to <=16 files).
  traceRingPrune();
  unlock();
  if (rep.blobGcSkipped) {
    alogf("memory: retention pruned %d day-files (kept %d msgs); blob GC deferred: index incomplete",
          (int)rep.removedDayFiles.size(), rep.keptMessages);
  } else {
    alogf("memory: retention pruned %d day-files + %d of %d blobs (kept %d msgs)",
          (int)rep.removedDayFiles.size(), (int)rep.removedBlobs.size(), rep.blobsPresent,
          rep.keptMessages);
  }
  if (report) *report = std::move(res.report);
  return RetentionStatus::Ran;
}

// ---- episodic index backfill (docs/orchestrator-storage.md §3a) -------------
// After a boot scan that stopped at its budget, the main loop indexes the rest of
// the card in small slices. No task of its own (the loop task already runs
// retention), no slice beside a turn or an open TLS session, and the card lock is
// only tried, never waited on, so the loop never stalls behind a web query.
namespace {
constexpr uint32_t kBackfillPeriodMs = 250;   // one slice per period at most
constexpr uint32_t kBackfillRetryMs = 5UL * 60UL * 1000UL;   // pause after card read errors
uint32_t g_bfLastMs = 0;
uint32_t g_bfRetryAtMs = 0;
bool     g_bfPaused = false;     // stopped on card read errors; retried at g_bfRetryAtMs
bool     g_bfPauseLogged = false;
uint32_t g_bfFirstMs = 0;
uint32_t g_bfWorkUs = 0;
uint32_t g_bfMaxSliceUs = 0;
int      g_bfRuns = 0;
bool     g_bfDone = false;   // outcome logged; nothing more to do this boot
}  // namespace

// What one slice reported, captured under the lock.
struct BackfillSlice {
  bool more = true;         // call again later
  bool readErrors = false;  // stopped because a file kept failing to read
  int  rows = 0;            // index rows after the slice
  nimbus::orch::EpiBackfillStats st;
};

// May a slice run now? Paced, card healthy, no turn, and the TLS work slot free.
// On true the caller holds the work slot and must release it.
static bool backfillSliceAllowed(uint32_t nowMs) {
  if (g_bfDone || !g_epiLog) return false;
  if (g_bfPaused && (int32_t)(nowMs - g_bfRetryAtMs) < 0) return false;
  if ((uint32_t)(nowMs - g_bfLastMs) < kBackfillPeriodMs) return false;
  g_bfLastMs = nowMs;
  // The card must be present and taking writes: a demoted or failing card is not
  // read in the background.
  if (!effHaveSd() || g_epiLog->unpersistedCount() > 0) return false;
  if (orchestrator::turnInFlight()) return false;
  // Zero wait: if any TLS session holds the work slot, its handshake may need the
  // internal heap now, so the slice waits for a later pass.
  return arbiter::acquireWork(0);
}

// One slice under the card lock (tried, never waited on), with its timing.
static BackfillSlice runBackfillSlice(uint32_t nowMs) {
  BackfillSlice out;
  if (!tryLock(20)) return out;   // busy: try again next period
  if (g_bfPaused) {   // the pause is over: retry from where the reads failed
    g_epiLog->resumeAfterReadErrors();
    g_bfPaused = false;
  }
  const uint32_t t0 = micros();
  out.more = g_epiLog->backfillStep();
  const uint32_t us = micros() - t0;
  out.rows = g_epiLog->messageCount();
  out.st = g_epiLog->backfillStats();
  out.readErrors = g_epiLog->stalledOnReadErrors();
  unlock();
  if (g_bfRuns++ == 0) g_bfFirstMs = nowMs;
  g_bfWorkUs += us;
  if (us > g_bfMaxSliceUs) g_bfMaxSliceUs = us;
  return out;
}

// The backfill stopped: log how, once.
static void reportBackfillOutcome(const BackfillSlice& sl, uint32_t nowMs) {
  if (sl.st.slices == 0) return;   // the boot scan had already indexed everything
  const unsigned workMs = (unsigned)(g_bfWorkUs / 1000);
  const unsigned maxMs = (unsigned)((g_bfMaxSliceUs + 999) / 1000);
  const unsigned wallMs = (unsigned)(nowMs - g_bfFirstMs);
  if (sl.st.complete) {
    alogf("memory: episodic index complete (%d rows, %d added in %d slices, %u ms work, "
          "longest slice %u ms, %u ms elapsed)", sl.rows, sl.st.rows, sl.st.slices, workMs, maxMs,
          wallMs);
  } else if (sl.st.capped) {
    alogf("memory: episodic index stopped at its cap (%d rows) - older rows stay on the card, "
          "searched on demand; old attachments are kept", sl.rows);
  } else {
    alogf("memory: episodic index incomplete (%d rows; a row too long to index) - older rows "
          "stay on the card, searched on demand; old attachments are kept", sl.rows);
  }
}

void episodicBackfillTick(uint32_t nowMs) {
  if (!backfillSliceAllowed(nowMs)) return;
  const BackfillSlice sl = runBackfillSlice(nowMs);
  arbiter::releaseWork();
  if (sl.more) return;
  if (sl.readErrors) {
    // A card that misread for a moment may read again: pause, then retry from
    // where it stopped. The index stays incomplete (and the blob sweep
    // deferred) meanwhile.
    g_bfPaused = true;
    g_bfRetryAtMs = nowMs + kBackfillRetryMs;
    if (!g_bfPauseLogged) {
      g_bfPauseLogged = true;
      alogf("memory: episodic index paused after card read errors (%d rows) - retrying "
            "every 5 minutes; old attachments are kept", sl.rows);
    }
    return;
  }
  g_bfDone = true;
  reportBackfillOutcome(sl, nowMs);
}

bool episodicIndexComplete() { return !g_epiLog || g_epiLog->indexComplete(); }

// A retention pass asked for out of schedule (test builds, bench B2).
// The loop runs it through the same schedule and turn gate as the 6-hourly pass.
namespace {
volatile bool g_retentionRequested = false;
}  // namespace
void requestRetentionPass() { g_retentionRequested = true; }
bool consumeRetentionRequest() {
  if (!g_retentionRequested) return false;
  g_retentionRequested = false;
  return true;
}

#ifdef NIMBUS_TEST
// Bench seam (B1/B2: the field-shaped corpus). Writes `n` rows dated `dayNum`
// straight into that day's stream, each about `bytesPerRow` long; every
// `blobEvery`-th row (0 = none) is a tool output that references a small sidecar
// written to /mem/blobs. Ids continue the device counter. At most 200 rows per
// call, like testFillEpisodic.
int testSeedEpisodicDay(uint32_t dayNum, int n, int bytesPerRow, int blobEvery) {
  if (!g_epiLog || !effHaveSd() || dayNum == 0) return 0;
  if (n > 200) n = 200;
  if (n <= 0) return 0;
  if (bytesPerRow < 64) bytesPerRow = 64;
  if (bytesPerRow > 4096) bytesPerRow = 4096;
  Lock g;
  int added = 0;
  for (int i = 0; i < n; i++) {
    char id[16];
    snprintf(id, sizeof(id), "m%08x", (unsigned)(++g_epiSeq));
    nimbus::orch::EpisodicMessage m;
    m.id = id;
    m.sessionId = "hiltest-epi";
    m.role = "user";
    m.kind = nimbus::orch::MsgKind::Message;
    m.tsHours = dayNum * 24 + (uint32_t)(i * 24 / n);
    if (blobEvery > 0 && i % blobEvery == 0) {
      char path[64];
      snprintf(path, sizeof(path), "%s/5eed%04x%08x.txt", kBlobDir, (unsigned)(dayNum & 0xFFFF),
               (unsigned)g_epiSeq);
      File f = g_fs->open(path, FILE_WRITE);
      if (!f) continue;
      f.print("seeded sidecar for ");
      f.print(id);
      f.close();
      m.kind = nimbus::orch::MsgKind::ToolOutput;
      m.blobPath = path;
    }
    std::string text = std::string("SEED-") + id + " day " + std::to_string(dayNum) + " ";
    while ((int)text.size() < bytesPerRow) text += "hiltest-pad ";
    m.text = text;
    g_epiLog->addMessage(m);
    added++;
  }
  return added;
}
#endif

// Which tool CALLS mutate durable state (so a read-only memory.search/view doesn't
// trigger a full-blob rewrite - the write-amplification the audit flagged). `action`
// is the call's action argument, needed for tools where only some actions write:
// memory.archive search/list are read-only, only restore mutates.
static bool isMutatingTool(const std::string& name, const std::string& action) {
  if (name == "memory.archive") return action == "restore";
  return name == "memory.write" || name == "memory.update" || name == "memory.pin" ||
         name == "memory.delete" || name == "memory.config" || name == "memory.scratchpad";
}

// Tools that own a long network or speaker wait and take the card lock themselves,
// only around their own card work, so handleMcp runs them outside its whole-call Lock:
// image.generate (a 90-120 s image download, see image_gen.cpp) and the output-channel
// tools (a text-to-speech download, a Telegram upload, a wait for playback: 25 s under
// the Lock on 22 Oct), whose one card touch is the history row captureMessage locks.
static bool locksItself(const std::string& name) {
  return name == "image.generate" || name == "reply.telegram" || name == "reply.speak";
}

// The write tools reach their handler even under FAULT memory, which refuses and
// counts them (MemoryContext::writable).
static bool isMemoryWriteTool(const std::string& name) {
  return name == "memory.write" || name == "memory.update";
}

std::string handleMcp(const std::string& jsonRpcRequest,
                      const nimbus::orch::Principal& who) {
  JsonDocument d;
  const bool parsed = deserializeJson(d, jsonRpcRequest) == DeserializationError::Ok;
  const std::string method = parsed ? std::string(d["method"] | "") : std::string();
  const bool isToolCall = method == "tools/call";   // never true for an unparsed request
  const std::string name = isToolCall ? std::string(d["params"]["name"] | "") : std::string();
  // The call's `action` arg (for tools where only some actions write, e.g.
  // memory.archive restore vs search/list). MCP nests args under params.arguments.
  const std::string action =
      isToolCall ? std::string(d["params"]["arguments"]["action"] | "") : std::string();
  // Resilience: with the memory subsystem faulted (FAULT memory), every MCP tool
  // call fails cleanly with a JSON-RPC error instead of touching the engines - the
  // model sees the error and carries on, proving a turn survives with no memory.
  // The two write tools go on to their handler, which refuses them as "memory
  // engine unavailable" (ctx.writable) and counts the refusal: refused here, a
  // write was invisible to the night's outcome, which then read ok (unit L, 22 Oct).
  const bool faulted = nimbus::fault::active(nimbus::fault::MEMORY);
  if (faulted && !isMemoryWriteTool(name))
    return "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32001,"
           "\"message\":\"memory subsystem unavailable (fault-injected)\"}}";
  // A few tools own a LONG provider round-trip (image.generate blocks ~90-120 s on
  // the image-generation TLS) and must NOT hold the engine Lock for that whole wait:
  // the MAIN loop takes this Lock in tickSdHealth, so a 90 s hold stalls it past its
  // 8 s watchdog and reboots the device (proven live). Such tools do their SD work
  // OUTSIDE the long wait, under their OWN brief Lock (they decode to PSRAM first),
  // so they dispatch outside this one. See image_gen.cpp.
  // The reply tools refuse outside a turn, and a turn never runs on the main loop, so
  // their history row needs no LoopLock.
  if (locksItself(name)) return g_reg.handleRpc(jsonRpcRequest, who);
  // Serialize the whole dispatch with the turn task's engine access. Recursive, so the
  // nested applyConfig/persist* below re-enter safely. (A memory.write tool embeds via
  // TLS while holding this - a rare, brief stall for a concurrent turn, never a
  // deadlock; the poll task already tolerates multi-second long-poll waits.)
  // The main loop never waits for this lock (LoopLock), so a slow tool here no longer
  // stalls it; a hold of kLongHoldMs or more is still named in the log (noteToolHold).
  Lock g;
  const uint32_t heldFrom = millis();
#ifdef NIMBUS_TEST
  if (isToolCall) testToolStallPoint(name);
#endif
  std::string resp = g_reg.handleRpc(jsonRpcRequest, who);
  // Persist ONLY after a MUTATING tools/call (dirty-flag discipline); a faulted
  // write was refused and changed nothing.
  if (isToolCall && isMutatingTool(name, action) && !faulted) {
    applyConfig();   // memory.config may have changed max_vectors
    persistVectors();
    // memory.scratchpad saves itself (its result reports a failed save); no
    // other memory tool changes the scratchpad.
    if (name != "memory.scratchpad") persistScratchpad();
    if (name == "memory.config") persistMemConfig();   // survive reboot (was silently lost)
  }
  if (isToolCall) noteToolHold(name, millis() - heldFrom);
  return resp;
}

Stats stats() {
  Stats s;
  s.sdPresent = effHaveSd();
  s.sdMissingWithData = g_sdMissing;
  s.flashFull = g_flashFull;
  // The test console's STATUS runs on the main loop: while a tool holds the card lock
  // it reports the engine counts as -1 (unknown) instead of waiting (loop_guard.h).
  LoopLock g;
  if (!g) {
    s.vectorCount = s.scratchItems = s.episodicMsgs = s.maxVectors = -1;
    return s;
  }
  s.vectorCount = g_vec.size();
  s.scratchItems = (g_scratch.activeTask().empty() ? 0 : 1) +
                   g_scratch.count(Tier::Short) + g_scratch.count(Tier::Mid) +
                   g_scratch.count(Tier::Long);
  s.episodicMsgs = g_epiActive->messageCount();
  s.embedAvailable = embeddings::available();
  s.embedLocked = store::embedLocked();
  s.maxVectors = g_vec.maxEntries();
  s.archivedCount = effHaveSd() ? g_archive.size() : 0;
  s.pending = (int)g_pending.size();
  s.embedParked = embedParkedWhy();
  if (g_epiLog) {
    s.epiHydrateTruncated = g_epiLog->hydrateTruncated();
    s.epiIndexFloorDay = (int)g_epiLog->indexFloorDay();
    s.epiBootIndexed = g_epiLog->bootIndexed();
    s.epiBackfillRows = g_epiLog->backfillStats().rows;
    s.epiIndexComplete = g_epiLog->indexComplete();
  }
  return s;
}

#ifdef NIMBUS_TEST
// HIL max-memory seams (plan §1f). Chunked at 200 rows/call - console-safe.
int testFillEpisodic(int n, int bytesPerRow, const char* exactText) {
  if (n > 200) n = 200;
  if (bytesPerRow < 16) bytesPerRow = 16;
  if (bytesPerRow > 2048) bytesPerRow = 2048;
  int added = 0;
  for (int i = 0; i < n; i++) {
    static uint32_t s_seq = 0;
    // `exactText` plants a row VERBATIM (deep-history tests need one unique
    // needle; the MARKER-<seq> pattern substring-matches its own siblings, so a
    // test built on it can pass without the needle ever being found).
    if (exactText && *exactText) {
      captureMessage("hiltest-epi", "user", nimbus::orch::MsgKind::Message,
                     String(exactText), "", "hiltest");
      added++;
      continue;
    }
    String text = String("MARKER-") + String((unsigned long)s_seq++) + " ";
    while ((int)text.length() < bytesPerRow) text += "hiltest-pad ";
    captureMessage("hiltest-epi", "user", nimbus::orch::MsgKind::Message, text, "",
                   "hiltest");
    added++;
  }
  return added;
}

int testFillVectors(int n, const char* ns, float importance) {
  if (n > 200) n = 200;
  const int dims = store::embedDims() > 0 ? store::embedDims() : EMBED_DEFAULT_DIMS;
  int added = 0;
  Lock lk;
  for (int i = 0; i < n; i++) {
    static uint32_t s_seq = 0;
    nimbus::orch::VecEntry e;
    e.id = "hilvec-" + std::to_string(s_seq);
    e.content = "MARKER-VEC-" + std::to_string(s_seq);
    // Importance ramps 0.10..0.90 cyclically so cap-eviction order is assertable
    // (lowest importance*ttl-left evicts first at the cap).
    // importance>0 pins every row at one value (quota/eviction-order tests);
    // 0 keeps the ramp so cap-eviction order stays assertable.
    e.importance = importance > 0.0f ? importance : (0.10f + 0.05f * (float)(s_seq % 17));
    e.ttlHours = 720;
    e.createdAtHours = nowHours();
    e.source = "hiltest";
    if (ns && ns[0]) e.ns = ns;   // v3.7.0 namespace (privacy suites)
    e.vec.resize((size_t)dims);
    uint32_t x = 0x9E3779B9u ^ (s_seq * 2654435761u);   // deterministic per row
    for (int d = 0; d < dims; d++) {
      x ^= x << 13; x ^= x >> 17; x ^= x << 5;          // xorshift32
      e.vec[(size_t)d] = (int8_t)((x & 0xFF) - 128);
    }
    s_seq++;
    if (g_vec.add(e, /*dedup=*/false)) added++;
  }
  persistVectors();
  return added;
}
#endif

}  // namespace memory
}  // namespace agent\n
