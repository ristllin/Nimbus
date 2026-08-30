#include "memory_subsystem.h"


#include <LittleFS.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>                  // feed the WDT during a long durable-store wipe
#include <time.h>

#include "adapters/embeddings.h"
#include "adapters/tavily.h"
#include "health.h"                       // system.health tool (P5)
#include "skills.h"                        // skill.list/get/save/delete tools (P7 + v4)
#include "telegram.h"                      // owner alert on agent skill.save
#include "agent_config.h"
#include "../sys/agent_log.h"
#include "epi_fs_arduino.h"
#include "orchestrator.h"
#include "loops_subsystem.h"   // loop.create/list/cancel tools -> Local Loops
#include "nimbus/harness/dream.h"  // DREAMING: reserved-loop cancel refusal
#include "store.h"
#include "nimbus/docs_pack.h"             // docs.list/search/read - embedded device docs (W13)
#include "nimbus/fault.h"                 // resilience: simulated SD/memory faults
#include "nimbus/sd_health.h"             // SD demote/promote debounce (graceful degradation)
#include <solide/storage.h>               // re-probe the SD card on recovery
#include "nimbus/orch/blob_store.h"
#include "nimbus/orch/episodic_log.h"
#include "nimbus/orch/memory_tools.h"
#include "nimbus/orch/result_store.h"   // registerResultTools (results.get/list)
#include "nimbus/orch/session_tools.h"

namespace agent {
namespace memory {

static void registerTenantTools();   // v3.7.0 admin-by-conversation (defined below)

using namespace nimbus::orch;

namespace {
VectorMemory          g_vec;
// Cold store for TTL-expired memories (CUM-225). SD-only: attached as g_vec's prune
// sink and exposed to the model ONLY when a card is present, so a card-less device
// drops entries at TTL exactly as before. Lives in PSRAM like the live set.
nimbus::orch::VectorArchive g_archive;
Scratchpad            g_scratch;
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

// Bind the embedder: call the provider, and on the FIRST successful embedding
// freeze the embed config (set-once lock) so the web UI knows the VDB is now
// committed to this provider/model/dims. Signature matches the portable
// Embedder (std::string in); we bridge to the Arduino-String adapter call.
std::vector<int8_t> embedText(const std::string& text) {
  String err;
  std::vector<int8_t> v = embeddings::embed(String(text.c_str()), err);
  if (!v.empty() && !store::embedLocked()) store::setEmbedLocked(true);
  if (v.empty()) alogf("memory: embed failed: %s", err.c_str());
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
  g_scratch.clearAll();
  const char* root = g_haveSd ? "/mem" : "/data";
  bool ok = rmTree(*g_fs, root);
  g_fs->mkdir(root);   // recreate the empty parent so post-reboot persists work
  alogf("memory: durable store erased (%s) ok=%d", root, (int)ok);
  return true;   // wiped (caller reboots to an empty store); rmTree completeness logged
}

void lock()   { if (g_memMux) xSemaphoreTakeRecursive(g_memMux, portMAX_DELAY); }
void unlock() { if (g_memMux) xSemaphoreGiveRecursive(g_memMux); }

void begin() {
  if (g_begun) return;
  g_begun = true;
  g_memMux = xSemaphoreCreateRecursiveMutex();  // guard shared engines before any use
  LittleFS.begin(true);  // ALWAYS mount internal flash: it holds config and is the
                         // migration source if we just switched to the SD card. When
                         // g_fs is the SD card, solide::storage::begin() mounted it.
  // Resolve the storage tier: setDataFs(SD) (called before begin() when a card
  // mounted) makes g_fs the SD card. On SD, bulk blobs live under /mem; with no
  // card they stay on the legacy LittleFS /data paths (also the migration source).
  g_haveSd = (g_fs != &LittleFS);
  g_vecPath = g_haveSd ? kVecSdPath : kVecLfsPath;
  g_epiPath = g_haveSd ? kEpiSdPath : kEpiLfsPath;
  g_fs->mkdir(g_haveSd ? "/mem" : "/data");  // parent dir for the blobs; neither FS
                         // auto-creates it, so an absent dir made every persist fopen
                         // fail silently (state lost across reboot). Idempotent.

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
  // Load persisted scratchpad + retrieval config (both in the orchmem namespace).
  {
    Preferences p;
    if (p.begin(kScratchNs, true)) {
      // v4.1: the scratchpad moved to a BYTES blob ("scratchB") - a maxed-out
      // scratchpad (~4.2 KB under the caps) exceeds nvs_set_str's 4000-byte
      // value limit, which failed SILENTLY. Fall back to the legacy string key
      // so an upgrade keeps the old contents.
      std::string s;
      size_t blen = p.getBytesLength(kScratchBKey);
      if (blen > 0) {
        s.resize(blen);
        p.getBytes(kScratchBKey, &s[0], blen);
      } else {
        String legacy = p.getString(kScratchKey, "");
        s.assign(legacy.c_str(), legacy.length());
      }
      String c = p.getString(kMemCfgKey, "");
      p.end();
      if (s.length()) g_scratch.deserialize(s);
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
      alogf("memory: episodic boot scan hit its budget - older rows are on the card "
            "but not indexed (%d indexed)", loaded);
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

  // web.search (Tavily) - registered only when a key is set. Gives the orchestrator +
  // external MCP clients a live-web capability (the key is human-set, never model-
  // writable). The handler does one blocking HTTPS round-trip via the TLS arbiter.
  if (agent::tavily::available()) {
    g_reg.add("web.search",
              "Search the live web for up-to-date information. Returns an answer plus top results (title, url, snippet).",
              [](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) -> nimbus::orch::ToolResult {
                std::string q = a["query"].is<const char*>() ? std::string(a["query"].as<const char*>()) : std::string();
                if (q.empty()) return nimbus::orch::ToolResult::fail("missing 'query'");
                // Budget gate (owner: monthly Tavily call cap). Refuse BEFORE spending
                // a billable call when the month's ceiling is reached; the count itself
                // is recorded on a successful search below.
                if (agent::store::providerOverBudget("tavily"))
                  return nimbus::orch::ToolResult::fail("web search skipped: Tavily monthly call budget reached");
                int k = a["max_results"].is<int>() ? a["max_results"].as<int>() : 5;
                agent::websearch::Result r = agent::tavily::search(q, k);
                // Report the ACTUAL cause. This used to collapse six distinct
                // failures - and a genuinely empty result set - into one string,
                // "web search failed (network / no results)", which was wrong
                // about the cause in every case and left the model retrying a
                // search that could never work.
                if (!r.ok) return nimbus::orch::ToolResult::fail("web search failed: " + r.err);
                agent::store::recordProviderCall("tavily");
                return nimbus::orch::ToolResult::ok(r.digest);
              },
              R"({"type":"object","properties":{"query":{"type":"string"},"max_results":{"type":"integer"}},"required":["query"]})");
  }

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
              // flight) so an MCP client can't actuate the speaker + block the web
              // task with a multi-second TLS+I2S synth/playback (prism).
              if (!agent::orchestrator::turnInFlight())
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
              if (!agent::orchestrator::speakOnDevice(String(t.c_str())))
                // Honest failure (owner 2026-07-16: the model kept telling them the
                // SPEAKER was broken when synthesis was what failed): name both
                // possible stages, never blame the hardware.
                return nimbus::orch::ToolResult::fail(
                    "on-device speech failed at TTS synthesis or playback (NOT necessarily "
                    "the speaker hardware - that is usually fine); reply.telegram with "
                    "voice:true still delivers spoken audio");
              return nimbus::orch::ToolResult::ok("spoken on device speaker");
            },
            R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"]})");
  g_reg.add("reply.telegram",
            "Send a Telegram message. Omit chat_id to reply to the current conversation; set "
            "voice:true to deliver it as spoken audio instead of text.",
            [](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) -> nimbus::orch::ToolResult {
              if (!agent::orchestrator::turnInFlight())
                return nimbus::orch::ToolResult::fail("reply.telegram is only available during a turn");
              std::string t = a["text"].is<const char*>() ? std::string(a["text"].as<const char*>()) : std::string();
              if (t.empty()) return nimbus::orch::ToolResult::fail("missing 'text'");
              std::string cid = a["chat_id"].is<const char*>() ? std::string(a["chat_id"].as<const char*>()) : std::string();
              // Owner's "Voice replies" toggle (P2.5): OFF -> degrade voice to TEXT
              // (the content still arrives; it just isn't spoken - never dropped).
              bool voice = (a["voice"] | false) && agent::store::ttsEnabled();
              if (!agent::orchestrator::sendToChat(String(cid.c_str()), String(t.c_str()), voice))
                return nimbus::orch::ToolResult::fail("no allowed Telegram target (omit chat_id to reply to the current chat, or pass an allowlisted id)");
              return nimbus::orch::ToolResult::ok(voice ? "sent to Telegram" : "sent to Telegram as text");
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
            "Search your own device documentation (keyword AND-match over titles + bodies) "
            "- use it BEFORE saying what you can or cannot do, or how a feature of yours "
            "works. Returns section ids + a snippet; read the full section with docs.read.",
            [](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) -> nimbus::orch::ToolResult {
              std::string q = a["query"].is<const char*>() ? std::string(a["query"].as<const char*>()) : std::string();
              if (q.empty()) return nimbus::orch::ToolResult::fail("missing 'query'");
              const nimbus::docs::DocSection* hits[8];
              size_t n = nimbus::docs::search(q, hits, 8);
              if (n == 0)
                return nimbus::orch::ToolResult::ok(
                    "no sections match - try fewer or different keywords, or browse docs.list");
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
            "Report the device's live hardware + subsystem health (LED ring, display, mic, "
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

  alogf("memory: ready (%d vectors, dims=%d, store=%s, cap=%d, archive=%s, embed=%s, web=%s)",
        g_vec.size(), g_vec.dims(), g_haveSd ? "SD /mem" : "flash /data (no SD)",
        g_vec.maxEntries(),
        effHaveSd() ? (std::to_string(g_archive.size()) + "/" +
                       std::to_string(kArchiveMaxEntries)).c_str() : "off (no SD)",
        embeddings::available() ? "on" : "no-key",
        agent::tavily::available() ? "on" : "off");
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
bool flashFull() { return g_flashFull; }

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
  // bounded (unlike a TLS embed), so holding it here is safe.
  Lock g;
  // Attempt a real re-mount first (a physical pull leaves stale handles); then a
  // write probe confirms the card answers before we clear the latch. end() clears
  // the driver's idempotent "already mounted" latch so begin() actually re-probes
  // the bus - without it a re-seated card is never re-detected (begin() no-ops).
  if (sdLost()) {
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
  Lock g;
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

void tickSdHealth(uint32_t nowMs) {
  flushPendingEvents();   // pre-SNTP timeline rows flush once the clock syncs
  if (!g_haveSd) return;   // no card at boot: nothing to demote/promote
  // Probe cadence: slow when healthy (idle detection of a pull), fast when lost
  // (responsive recovery). Live writes also feed noteSdIoResult() in real time.
  const uint32_t interval = sdLost() ? 4000u : 20000u;   // benign single-bool read
  if (g_lastSdProbeMs != 0 && nowMs - g_lastSdProbeMs < interval) return;
  g_lastSdProbeMs = nowMs;
  Lock g;   // hold across the probe: serializes the SD bus vs promoteSd()'s end()/begin()
            // on the web task (prism) + the debounce RMW vs the web/Telegram tasks
  const bool ok = probeSdWrite();
  applySdEdge(g_sdHealth.note(ok));
}

// Write a blob to `path` atomically: to `<path>.tmp`, then rename over `path`, so a
// crash mid-write leaves the previous good blob intact (single-writer; FAT + LittleFS
// rename is atomic enough for our case). Returns false on any FS error.
static bool writeBlobAtomic(const std::string& path, const std::string& blob) {
  // Barrier: the durable store is being wiped - refuse every persist/sidecar so a
  // blocked-then-released persist can't rewrite a just-deleted blob from RAM.
  if (g_erasing) return false;
  // FAULT sd_io: emulate a mid-op SD write failure (see nimbus::fault::SD_IO) so
  // the vector/blob persist-failure + demote path is testable without a real pull.
  if (nimbus::fault::active(nimbus::fault::SD_IO)) return false;
  std::string tmp = path + ".tmp";
  File f = g_fs->open(tmp.c_str(), FILE_WRITE);
  if (!f) return false;
  size_t n = f.write((const uint8_t*)blob.data(), blob.size());
  f.close();
  if (n != blob.size()) { g_fs->remove(tmp.c_str()); return false; }
  g_fs->remove(path.c_str());               // rename won't overwrite on some FS impls
  return g_fs->rename(tmp.c_str(), path.c_str());
}

std::vector<std::string> recall(const String& queryText, int k,
                                const nimbus::orch::Principal& who) {
  std::vector<std::string> out;
  if (nimbus::fault::active(nimbus::fault::MEMORY)) return out;   // resilience: no-memory turn
  if (queryText.length() == 0 || !embeddings::available()) return out;
  { Lock g; if (g_vec.size() == 0) return out; }   // quick locked empty-check: skip the embed
  if (k <= 0) k = g_cfg.retrievalCount;
  String err;
  std::vector<int8_t> qv = embeddings::embed(queryText, err);  // TLS - NOT under the lock
  if (qv.empty()) { alogf("memory: recall embed failed (%s) - fail-open", err.c_str()); return out; }
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
  return out;
}

void persistVectors() {
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
      return;
    }
    g_flashFull = false;
  }
  bool w = writeBlobAtomic(g_vecPath, blob);
  if (effHaveSd()) noteSdIoResult(w);   // real SD write outcome feeds the demote watchdog
  if (!w) alog("memory: vector persist failed");
  persistArchive();   // the archive rides the same persist cadence (dirty-gated, SD-only)
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

void persistScratchpad() {
  Lock g;
  std::string blob = g_scratch.serialize();
  Preferences p;
  if (!p.begin(kScratchNs, false)) {
    alog("scratchpad: persist FAILED (NVS open) - contents survive in RAM only");
    return;
  }
  // Bytes blob, not putString: a maxed-out scratchpad (~4.2 KB under the caps)
  // exceeds nvs_set_str's 4000-byte limit and the old write failed SILENTLY -
  // the contract promises "survives reboot", so a failed persist must say so.
  size_t wrote = p.putBytes(kScratchBKey, blob.data(), blob.size());
  p.end();
  if (wrote != blob.size())
    alogf("scratchpad: persist FAILED (%u of %u B) - contents survive in RAM only",
          (unsigned)wrote, (unsigned)blob.size());
}

void persistMemConfig() {
  Lock g;
  std::string blob = g_cfg.serialize();
  Preferences p;
  if (!p.begin(kScratchNs, false)) return;
  p.putString(kMemCfgKey, blob.c_str());
  p.end();
}

void persistEpisodic() {
  // The SD append-log self-persists (every addMessage IS the durable write), so a
  // whole-blob rewrite only applies to the no-SD in-memory store.
  if (g_epiLog) return;
  Lock g;
  if (!writeBlobAtomic(g_epiPath, g_epi.serialize())) alog("memory: episodic persist failed");
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

String captureMessage(const char* sessionId, const char* role, nimbus::orch::MsgKind kind,
                      const String& text, const String& blobPath, const String& tags) {
  if (!g_begun) return String();
  Lock g;   // shared with the web/MCP task
  EpisodicMessage m;
  char id[16];
  snprintf(id, sizeof(id), "m%08x", (unsigned)(++g_epiSeq));
  m.id = id;
  m.sessionId = sessionId ? sessionId : "";
  m.tsHours = nowHours();
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
// jumps (~day 20 000 vs cutoff day-30). So unsynced-clock events are held in a
// small RAM list and flushed with real timestamps at first sync - the exact
// "did you lose power?" record the timeline exists for survives.
static std::vector<std::pair<std::string, std::string>> s_pendingEv;   // (ev, text)
bool clockSynced() { return time(nullptr) >= 1600000000; }             // ~2020-09

void flushPendingEvents() {
  if (!g_begun || !clockSynced()) return;
  // One-shot at first sync (Release C3): heal ANY vector stamped with the
  // boot-relative clock (a write/update/unpin/boost that ran in the WiFi-up-
  // but-SNTP-pending window) - real epoch-hours are ~495k, boot-relative are
  // tiny, so <400000 cleanly identifies them. Without this they all expire the
  // instant the clock becomes real.
  {
    static bool s_stampsHealed = false;
    if (!s_stampsHealed) {
      s_stampsHealed = true;
      Lock g;
      int n = g_vec.restampPreSync(400000, nowHours());
      if (n) {
        persistVectors();
        alogf("memory: re-stamped %d pre-sync vector(s) at first clock sync", n);
      }
    }
  }
  std::vector<std::pair<std::string, std::string>> pend;
  {
    Lock g;
    if (s_pendingEv.empty()) return;
    pend.swap(s_pendingEv);
  }
  captureSession("system", "device", "Device timeline");
  for (auto& e : pend)
    captureMessage("system", "system", nimbus::orch::MsgKind::Log,
                   String(e.second.c_str()) + " [pre-sync, stamped at first clock sync]",
                   "", String("ev:") + e.first.c_str());
}

void captureEvent(const char* ev, const String& text) {
  if (!g_begun) return;
  if (!clockSynced()) {
    Lock g;
    if (s_pendingEv.size() < 8)
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
                      const String& text, const char* srcPath, const char* ext) {
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
  captureMessage(sessionId, role, kind, text, String(path.c_str()));
  return true;
}

int pruneRetention(int retentionDays) {
  if (!g_epiLog || retentionDays <= 0) return 0;
  uint32_t nowDay = nowHours() / 24;
  if (nowDay < (uint32_t)retentionDays) return 0;  // clock not advanced past the window
  Lock g;   // prune scans + mutates the episodic store shared with the turn task
  int before = g_epiLog->messageCount();
  EpiPruneReport rep = g_epiLog->prune(nowDay - (uint32_t)retentionDays, kBlobDir);
  // Battery telemetry day-files (audit 2026-07-24: the ONE unbounded SD writer -
  // ~300 KB/day worst case, never deleted). Same day-numbered naming, same
  // retention window. Best-effort: a failed remove retries next 6 h tick.
  {
    const uint32_t cutoff = nowDay - (uint32_t)retentionDays;
    fs::FS& sd = dataFs();
    File dir = sd.open("/mem/battery");
    if (dir && dir.isDirectory()) {
      int removed = 0;
      for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        String name = f.name();   // "d<N>.jsonl"
        f.close();
        if (!name.startsWith("d")) continue;
        long day = name.substring(1).toInt();
        if (day > 0 && (uint32_t)day < cutoff &&
            sd.remove(String("/mem/battery/") + name))
          removed++;
      }
      if (removed) alogf("memory: retention pruned %d battery day-files", removed);
    }
    if (dir) dir.close();
  }
  // Glass Box P3 turn dossiers: the ring bounds SIZE on every write; this bounds
  // AGE, so a quiet device doesn't keep month-old turn anatomy after its episodic
  // rows are gone. Ids are opaque here, so age comes from the ring, not the name:
  // simply re-run the ring prune, then drop anything left if the whole directory
  // predates the window (cheap - the ring keeps this to <=16 files).
  traceRingPrune();
  alogf("memory: retention pruned %d day-files + %d blobs (kept %d msgs)",
        (int)rep.removedDayFiles.size(), (int)rep.removedBlobs.size(), rep.keptMessages);
  return before - rep.keptMessages;
}

// Which tool CALLS mutate durable state (so a read-only memory.search/view doesn't
// trigger a full-blob rewrite - the write-amplification the audit flagged). `action`
// is the call's action argument, needed for tools where only some actions write:
// memory.archive search/list are read-only, only restore mutates.
static bool isMutatingTool(const std::string& name, const std::string& action) {
  if (name == "memory.archive") return action == "restore";
  return name == "memory.write" || name == "memory.update" || name == "memory.pin" ||
         name == "memory.delete" || name == "memory.config" || name == "memory.scratchpad";
}

std::string handleMcp(const std::string& jsonRpcRequest,
                      const nimbus::orch::Principal& who) {
  // Resilience: with the memory subsystem faulted (FAULT memory), every MCP tool
  // call fails cleanly with a JSON-RPC error instead of touching the engines - the
  // model sees the error and carries on, proving a turn survives with no memory.
  if (nimbus::fault::active(nimbus::fault::MEMORY))
    return "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32001,"
           "\"message\":\"memory subsystem unavailable (fault-injected)\"}}";
  // A few tools own a LONG provider round-trip (image.generate blocks ~90-120 s on
  // the image-generation TLS) and must NOT hold the engine Lock for that whole wait:
  // the MAIN loop takes this Lock in tickSdHealth, so a 90 s hold stalls it past its
  // 8 s watchdog and reboots the device (proven live). Such tools do their SD work
  // OUTSIDE the long wait, under their OWN brief Lock (they decode to PSRAM first),
  // so they dispatch outside this one. See image_gen.cpp.
  JsonDocument d;
  const bool parsed = deserializeJson(d, jsonRpcRequest) == DeserializationError::Ok;
  const std::string method = parsed ? std::string(d["method"] | "") : std::string();
  const std::string name =
      (parsed && method == "tools/call") ? std::string(d["params"]["name"] | "") : std::string();
  // The call's `action` arg (for tools where only some actions write, e.g.
  // memory.archive restore vs search/list). MCP nests args under params.arguments.
  const std::string action =
      (parsed && method == "tools/call")
          ? std::string(d["params"]["arguments"]["action"] | "") : std::string();
  if (name == "image.generate") return g_reg.handleRpc(jsonRpcRequest, who);  // self-locks briefly
  // Serialize the whole dispatch with the turn task's engine access. Recursive, so the
  // nested applyConfig/persist* below re-enter safely. (A memory.write tool embeds via
  // TLS while holding this - a rare, brief stall for a concurrent turn, never a
  // deadlock; the poll task already tolerates multi-second long-poll waits.)
  Lock g;
  std::string resp = g_reg.handleRpc(jsonRpcRequest, who);
  // Persist ONLY after a MUTATING tools/call (dirty-flag discipline).
  if (method == "tools/call" && isMutatingTool(name, action)) {
    applyConfig();   // memory.config may have changed max_vectors
    persistVectors();
    persistScratchpad();
    if (name == "memory.config") persistMemConfig();   // survive reboot (was silently lost)
  }
  return resp;
}

Stats stats() {
  Lock g;
  Stats s;
  s.vectorCount = g_vec.size();
  s.scratchItems = (g_scratch.activeTask().empty() ? 0 : 1) +
                   g_scratch.count(Tier::Short) + g_scratch.count(Tier::Mid) +
                   g_scratch.count(Tier::Long);
  s.episodicMsgs = g_epiActive->messageCount();
  s.embedAvailable = embeddings::available();
  s.embedLocked = store::embedLocked();
  s.sdPresent = effHaveSd();
  s.flashFull = g_flashFull;
  s.maxVectors = g_vec.maxEntries();
  s.archivedCount = effHaveSd() ? g_archive.size() : 0;
  if (g_epiLog) {
    s.epiHydrateTruncated = g_epiLog->hydrateTruncated();
    s.epiIndexFloorDay = (int)g_epiLog->indexFloorDay();
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
