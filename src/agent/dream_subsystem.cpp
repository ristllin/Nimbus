#include "dream_subsystem.h"

#include <LittleFS.h>
#include <time.h>

#include <memory>   // unique_ptr: the night state off the poll task stack (lane STACK)
#include <string>

#include "../sys/agent_log.h"
#include "agent_config.h"          // ORCH_AUTO_TURN_MIN_HEAP (gate heap floor)
#include "fs_atomic.h"             // writeFileAtomic - /data/dream.json
#include "memory_subsystem.h"      // engines + Lock + nowHours + the pending drain
#include "orchestrator.h"          // activeJobCount - the gate

namespace agent {
namespace dream {

namespace orch = nimbus::orch;

static agent::Hooks g_hooks;             // only onDreamStart/onDreamEnd used
static DreamGate    g_gateCfg;
static volatile uint32_t g_lastTurnEndMs = 0;   // 0 = no turn since boot
// Busy defers taken this local night (RAM; reset at a fire and at the day roll).
static uint8_t  g_defersTonight = 0;
static uint32_t g_deferDay = 0;
// A starved night's row, decided in the gate. The gate runs under the loops lock and
// the timeline write takes the memory lock - the opposite order to loop.create (memory
// lock, then loops lock) - so the row is written later, from tg_poll with no loops lock
// held: at the next turn end or the next fire, whichever comes first.
static bool        g_starvedPending = false;
static NightReport g_starved;

// The night's one ev:dream row (main.cpp's hook writes it to the episodic store).
static void emitRow(const agent::DreamEndEv& ev) {
  if (g_hooks.onDreamEnd) g_hooks.onDreamEnd(ev);
}

static void emitStarvedRow() {
  if (!g_starvedPending) return;
  g_starvedPending = false;
  emitRow(dreamEndEvent(g_starved));
}

// ---- dream state: /data/dream.json (LittleFS), never NVS -------------------------
static const char* kStatePath = "/data/dream.json";
// This boot's working copy (tg_poll only): read from the file once, then RAM is the
// authority and every save is durability only (DreamMemo in dream.h).
static DreamMemo g_memo;

static bool loadState(DreamState& st) {
  File f = flashMounted() ? LittleFS.open(kStatePath, FILE_READ) : File();
  if (!f) return false;   // first night on this device: maintenance runs tonight
  // A torn or foreign file larger than the state can be is never read into RAM.
  String s;
  if (f.size() <= kStateFileMax) s = f.readString();
  f.close();
  if (loadDreamState(std::string(s.c_str(), s.length()), st)) return true;
  alog("dream: state file unreadable - starting fresh");
  st = DreamState{};
  return false;
}

static bool saveState(const DreamState& st) {
  return writeFileAtomic(kStatePath, dumpDreamState(st), "dream");
}

// ---- the local clock (tz-aware; tg_poll, like the loops scheduler) ---------------
static NightClock nightClock() {
  NightClock c;
  c.epoch = (uint64_t)time(nullptr);
  c.synced = memory::clockSynced();
  time_t t = (time_t)c.epoch;
  struct tm lt;
  localtime_r(&t, &lt);
  c.localDay = orch::daysFromCivil(lt.tm_year + 1900, (unsigned)(lt.tm_mon + 1),
                                   (unsigned)lt.tm_mday);
  c.minuteOfDay = (uint16_t)(lt.tm_hour * 60 + lt.tm_min);
  return c;
}

void begin(const agent::Hooks& hooks) {
  g_hooks = hooks;
  g_gateCfg.minHeap = ORCH_AUTO_TURN_MIN_HEAP;  // device-authoritative floor
}

void noteTurnEnd(uint32_t nowMs) {
  g_lastTurnEndMs = nowMs ? nowMs : 1;
  emitStarvedRow();
}

uint32_t gateDefer(const orch::LoopRecord& l) {
  if (!isReserved(l.id)) return 0;   // gate applies only to the dream loop
  const NightClock c = nightClock();
  if (c.localDay != g_deferDay) { g_deferDay = c.localDay; g_defersTonight = 0; }
  GateInputs in;
  in.nowMs = millis();
  in.lastTurnEndMs = g_lastTurnEndMs;
  in.activeJobs = orchestrator::activeJobCount();
  in.freeHeap = ESP.getFreeHeap();
  in.defersTonight = g_defersTonight;
  GateResult r = evaluateGate(g_gateCfg, in);
  if (!r.deferSec) return 0;
  // The night window closed with the device still not idle: no upkeep tonight, said
  // once in the timeline; the loop waits for tomorrow's slot.
  if (c.synced && c.minuteOfDay >= windowEndFor(l.sched.minuteOfDay)) {
    g_starved = NightReport{};
    g_starved.outcome = DreamOutcome::Starved;
    g_starved.cause = r.why ? r.why : "busy";
    g_starved.defers = g_defersTonight;
    g_starvedPending = true;   // written outside this lock (emitStarvedRow)
    alogf("dream: skipped tonight (%s after %u defers)", g_starved.cause.c_str(),
          (unsigned)g_defersTonight);
    g_defersTonight = 0;
    return secondsToNextSlot(c, l.sched.minuteOfDay);
  }
  if (g_defersTonight < 255) g_defersTonight++;
  alogf("dream: not idle (%s) - deferring %us (%u tonight)", r.why ? r.why : "?",
        (unsigned)r.deferSec, (unsigned)g_defersTonight);
  return r.deferSec;
}

// ---- the last night, for /api/state and health (read from any task) --------------
static portMUX_TYPE g_statusMux = portMUX_INITIALIZER_UNLOCKED;
static Status g_status;

static void publishStatus(const NightReport& rep) {
  Status s;
  s.known = true;
  s.last = outcomeSlug(rep.outcome);
  strlcpy(s.cause, rep.cause.c_str(), sizeof s.cause);
  s.stateSaved = rep.stateSaved;
  strlcpy(s.stateWhy, rep.stateWhy.c_str(), sizeof s.stateWhy);
  portENTER_CRITICAL(&g_statusMux);
  g_status = s;
  portEXIT_CRITICAL(&g_statusMux);
}

Status status() {
  portENTER_CRITICAL(&g_statusMux);
  const Status s = g_status;
  portEXIT_CRITICAL(&g_statusMux);
  return s;
}

static nimbus::orch::FireOutcome deferredLowMemory(const char* what) {
  alogf("dream: fire deferred (low memory) - the night's %s could not be allocated", what);
  nimbus::orch::FireOutcome o;
  o.deferred = true;   // not a failure: it must not roll the breaker (lane M)
  o.detail = std::string("deferred: low memory (") + what + ")";
  return o;
}

nimbus::orch::FireOutcome fire(const orch::LoopFireRequest&) {
  const NightClock clock = nightClock();
  emitStarvedRow();   // a previous night's starved row, if still unwritten
  if (g_hooks.onDreamStart) g_hooks.onDreamStart(agent::DreamStartEv{clock.epoch});
  g_defersTonight = 0;

  // The night's deps and result live on the heap (lane STACK): the night runs on the
  // Telegram poll task. nothrow: fire() runs on tg_poll with no bad_alloc handler up the
  // chain, so a failed box reports a memory defer instead of aborting the device.
  const std::unique_ptr<NightDeps> deps(new (std::nothrow) NightDeps());
  if (!deps) return deferredLowMemory("deps");
  NightDeps& d = *deps;
  d.loadState = loadState;
  d.saveState = saveState;
  d.storeReady = flashMounted;
  d.memo = &g_memo;
  // Under the memory Lock (recursive; shared with the web/MCP task). Release C2:
  // windowed dedup - the full O(n^2 dims) scan at the 5000-entry cap blocks turns and
  // the web under the lock.
  d.maintain = [] {
    memory::Lock lk;
    MaintResult m = maintainVectors(memory::vectors(), memory::config().decayFactor,
                                    memory::nowHours(), 1500, [] { return memory::persistVectors(); });
    alogf("dream: maintenance - decayed, pruned=%d deduped=%d (%d vectors)%s", m.pruned,
          m.deduped, m.vectors, m.persisted ? "" : " NOT saved");
    return m;
  };
  d.emitRow = emitRow;

  // Memory writes the day's turns queued during an embedding outage: store up to 10
  // (30 s budget) before the maintenance. Outside the memory Lock (each step embeds over
  // TLS); a parked embedder skips it.
  if (memory::pendingCount() > 0) {
    const int drained = memory::drainPendingBurst(10, 30000);
    alogf("dream: pending memories stored=%d left=%d", drained, memory::pendingCount());
  }

  std::unique_ptr<NightResult> result(new (std::nothrow) NightResult(runNight(d, clock)));
  if (!result) return deferredLowMemory("result");
  const NightReport& rep = result->report;
  alogf("dream: %s pruned=%d deduped=%d", outcomeSlug(rep.outcome), rep.maint.pruned,
        rep.maint.deduped);
  if (!rep.stateSaved)
    alogf("dream: state not saved (%s) - this boot keeps the day in RAM", rep.stateWhy.c_str());
  publishStatus(rep);
  return result->fire;
}

// ---- the console DREAM drill: queued here, run on tg_poll -------------------------
static portMUX_TYPE g_drillMux = portMUX_INITIALIZER_UNLOCKED;
static bool g_drillPending = false;

bool requestDrill() {
  portENTER_CRITICAL(&g_drillMux);
  const bool fresh = !g_drillPending;
  g_drillPending = true;
  portEXIT_CRITICAL(&g_drillMux);
  return fresh;
}

void pumpDrill() {
  portENTER_CRITICAL(&g_drillMux);
  const bool pending = g_drillPending;
  g_drillPending = false;
  portEXIT_CRITICAL(&g_drillMux);
  if (!pending) return;
  // The request and outcome live on the heap (lane STACK). nothrow: pumpDrill runs on
  // tg_poll with no bad_alloc handler up the chain.
  const std::unique_ptr<orch::LoopFireRequest> req(new (std::nothrow) orch::LoopFireRequest());
  if (!req) { alogf("DREAM drill skipped (low memory) - the request could not be allocated"); return; }
  req->id = kLoopId;
  const std::unique_ptr<const orch::FireOutcome> fired(new (std::nothrow) orch::FireOutcome(fire(*req)));
  if (!fired) { alogf("DREAM drill skipped (low memory) - the outcome could not be allocated"); return; }
  alogf("DREAM fired ok=%d detail=%s", fired->ok ? 1 : 0, fired->detail.c_str());
}

}  // namespace dream
}  // namespace agent
