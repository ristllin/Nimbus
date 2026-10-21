#include "dream_subsystem.h"

#include <LittleFS.h>
#include <time.h>

#include <string>

#include "../sys/agent_log.h"
#include "adapters/embeddings.h"   // available() - can memory writes land tonight?
#include "agent_config.h"          // ORCH_AUTO_TURN_MIN_HEAP (gate heap floor)
#include "fs_atomic.h"             // writeFileAtomic - /data/dream.json
#include "loops_subsystem.h"       // applyInterruptedOutcome - the loop side of the reconcile
#include "memory_subsystem.h"      // engines + Lock + stats + nowHours + opCounters
#include "orchestrator.h"
#include "store.h"                 // the legacy NVS quiet-night baseline (read only)

namespace agent {
namespace dream {

namespace orch = nimbus::orch;

// The sessions that belong to the device's owner: the token/physically-
// authenticated local surfaces plus every Telegram chat with the owner role,
// plus "system" (device events - nobody's words).
static std::vector<std::string> ownerSessionIds() {
  std::vector<std::string> ids{"web", "serial", "voice", "system"};
  String owners = store::telegramOwners();
  int start = 0;
  while (start < (int)owners.length()) {
    int end = owners.indexOf(',', start);
    if (end < 0) end = owners.length();
    String one = owners.substring(start, end);
    one.trim();
    if (one.length()) ids.push_back(std::string(one.c_str()));
    start = end + 1;
  }
  // An empty owner list means the FIRST allow-list entry is the owner (the
  // single-account default the rest of the firmware uses).
  if (ids.size() == 4) {
    String first = orchestrator::firstAllowedChat();
    if (first.length()) ids.push_back(std::string(first.c_str()));
  }
  return ids;
}

static agent::Hooks g_hooks;             // only onDreamStart/onDreamEnd used
static AlertSink    g_alert;             // the owner message for a failing streak
static DreamGate    g_gateCfg;
static volatile uint32_t g_lastTurnEndMs = 0;   // 0 = no turn since boot
// Busy defers taken this local night (RAM; reset at a fire and at the day roll).
static uint8_t  g_defersTonight = 0;
static uint32_t g_deferDay = 0;
static bool     g_firing = false;        // the dream's own turn is running
static std::string g_pendingAlert;       // sent at the next owner turn, never at night
// A starved night's row, decided in the gate. The gate runs under the loops lock and
// the timeline write takes the memory lock - the opposite order to loop.create (memory
// lock, then loops lock) - so the row is written later, from tg_poll with no loops lock
// held: at the next turn end or the next fire, whichever comes first.
static bool        g_starvedPending = false;
static NightReport g_starved;

static void emitStarvedRow() {
  if (!g_starvedPending) return;
  g_starvedPending = false;
  if (!g_hooks.onDreamEnd) return;
  agent::DreamEndEv ev;
  ev.cause = g_starved.cause;
  ev.report = &g_starved;
  g_hooks.onDreamEnd(ev);
}

// ---- dream state: /data/dream.json (LittleFS), never NVS -------------------------
static const char* kStatePath = "/data/dream.json";
// This boot's working copy (tg_poll only): read from the file once, then RAM is the
// authority and every save is durability only (DreamMemo in dream.h).
static DreamMemo g_memo;

static bool loadState(DreamState& st) {
  File f = flashMounted() ? LittleFS.open(kStatePath, FILE_READ) : File();
  if (f) {
    String s = f.readString();
    f.close();
    if (loadDreamState(std::string(s.c_str(), s.length()), st)) return true;
    alog("dream: state file unreadable - starting fresh");
    st = DreamState{};
    return false;
  }
  // First night on this firmware: migrate the quiet-night baseline from the legacy
  // NVS key (read once; it is never written again - NVS is full on real devices).
  st = stateFromLegacyHash(std::string(store::dreamScratchHash().c_str()));
  if (st.scrHash) alog("dream: quiet-night baseline migrated from NVS");
  return true;
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

void begin(const agent::Hooks& hooks, AlertSink alert) {
  g_hooks = hooks;
  g_alert = std::move(alert);
  g_gateCfg.minHeap = ORCH_AUTO_TURN_MIN_HEAP;  // device-authoritative floor
}

void noteTurnEnd(uint32_t nowMs) {
  g_lastTurnEndMs = nowMs ? nowMs : 1;
  if (!g_firing) emitStarvedRow();
  // A failing streak is told when the owner is next around (any turn of theirs
  // ends), never by a 04:00 message.
  if (!g_firing && !g_pendingAlert.empty() && g_alert) {
    const std::string msg = std::move(g_pendingAlert);
    g_pendingAlert.clear();
    g_alert(msg);
  }
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
  // The night window closed with the device still not idle: no reflection tonight,
  // said once in the timeline; the loop waits for tomorrow's slot.
  if (c.synced && c.minuteOfDay >= windowEndFor(l.sched.minuteOfDay)) {
    g_starved = NightReport{};
    g_starved.outcome = DreamOutcome::Starved;
    g_starved.cause = r.why ? r.why : "busy";
    g_starved.attempt = g_defersTonight;
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
  strlcpy(s.err, rep.error.c_str(), sizeof s.err);
  s.stateSaved = rep.stateSaved;
  strlcpy(s.stateWhy, rep.stateWhy.c_str(), sizeof s.stateWhy);
  s.attempts = rep.attempt;
  s.streak = rep.streak;
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

nimbus::orch::FireOutcome fire(const orch::LoopFireRequest& r, bool force) {
  // The reserved record's slot is fixed (03:30; loops are paused or resumed, never
  // rescheduled), so NightClock's default slot is the one the window counts from.
  const NightClock clock = nightClock();
  emitStarvedRow();   // a previous night's starved row, if still unwritten
  if (g_hooks.onDreamStart) g_hooks.onDreamStart(agent::DreamStartEv{clock.epoch});
  g_defersTonight = 0;

  NightDeps d;
  d.loadState = loadState;
  d.saveState = saveState;
  d.storeReady = flashMounted;
  d.memo = &g_memo;
  // Phase 1 under the memory Lock (recursive; shared with the web/MCP task), never
  // held across the network turn. Release C2: windowed dedup - the full O(n^2 dims)
  // scan at the 5000-entry cap blocks turns and the web under the lock.
  d.maintain = [] {
    memory::Lock lk;
    MaintResult m = maintainVectors(memory::vectors(), memory::config().decayFactor,
                                    memory::nowHours(), 1500, [] { return memory::persistVectors(); });
    alogf("dream: maintenance - decayed, pruned=%d deduped=%d (%d vectors)%s", m.pruned,
          m.deduped, m.vectors, m.persisted ? "" : " NOT saved");
    return m;
  };
  // The owner's last 24 h of conversation, minus captured job-engine output and
  // /clear markers. v3.7.0 (prism R2, "namespace laundering"): limited to the owner's
  // OWN surfaces plus the device timeline - a guest's private conversation must
  // never be absorbed into the owner's memories overnight.
  d.yesterday = [] {
    memory::Lock lk;
    return readYesterday(memory::episodic(), memory::nowHours(), ownerSessionIds());
  };
  d.scratchBlock = [] {
    memory::Lock lk;
    std::string out;
    memory::scratchpad().appendPromptBlock(out);
    return out;
  };
  d.scratchHash = [] {
    memory::Lock lk;
    return orch::fnv64(memory::scratchpad().serialize());
  };
  d.stats = [] {
    memory::Stats s = memory::stats();
    MemStats m;
    m.vectors = s.vectorCount;
    m.scratchItems = s.scratchItems;
    m.episodicMsgs = s.episodicMsgs;
    return m;
  };
  d.embeddingsUp = [] { return embeddings::available(); };
  d.memOps = [] { return memory::opCounters(); };
  // An ordinary scheduled turn: the same rails (reboot/loop.create refused, LoopCaps
  // metered as "loop:dream"), memory tools only, and quiet - an empty reply and every
  // engine notice stay off the owner's phone at night.
  d.injectTurn = [&r](const std::string& inputs) {
    return orchestrator::injectScheduledTurn(String(r.chatId.c_str()), String(inputs.c_str()),
                                             String(r.name.c_str()), String(r.id.c_str()),
                                             /*quietOk=*/true);
  };

  g_firing = true;
  NightResult night = runNight(d, clock, force);
  g_firing = false;
  const NightReport& rep = night.report;
  alogf("dream: %s%s%s tokens=%u attempt=%u retry=%us streak=%u", outcomeSlug(rep.outcome),
        rep.cause.empty() ? "" : " cause=", rep.cause.c_str(), (unsigned)rep.tokens,
        (unsigned)rep.attempt, (unsigned)rep.retryAfterSec, (unsigned)rep.streak);
  if (!rep.stateSaved)
    alogf("dream: state not saved (%s) - this boot keeps the count in RAM", rep.stateWhy.c_str());
  if (rep.alert) g_pendingAlert = streakAlertText(rep);
  publishStatus(rep);
  if (g_hooks.onDreamEnd) {
    agent::DreamEndEv ev;
    ev.turnOk = night.fire.ok;
    ev.prunedVectors = rep.maint.pruned;
    ev.dedupedVectors = rep.maint.deduped;
    ev.tokens = rep.tokens;
    ev.cause = rep.error;
    ev.report = &rep;
    g_hooks.onDreamEnd(ev);
  }
  return night.fire;
}

// ---- the console DREAM drill: queued here, run on tg_poll -------------------------
static portMUX_TYPE g_drillMux = portMUX_INITIALIZER_UNLOCKED;
static bool g_drillPending = false;
static char g_drillChat[48] = {};

bool requestDrill(const std::string& chatId) {
  portENTER_CRITICAL(&g_drillMux);
  const bool fresh = !g_drillPending;
  if (fresh) {
    strlcpy(g_drillChat, chatId.c_str(), sizeof g_drillChat);
    g_drillPending = true;
  }
  portEXIT_CRITICAL(&g_drillMux);
  return fresh;
}

void pumpDrill() {
  char chat[sizeof g_drillChat];
  portENTER_CRITICAL(&g_drillMux);
  const bool pending = g_drillPending;
  g_drillPending = false;
  memcpy(chat, g_drillChat, sizeof chat);
  portEXIT_CRITICAL(&g_drillMux);
  if (!pending) return;
  const orch::LoopRecord rec = reservedLoopRecord();
  orch::LoopFireRequest req;
  req.id = rec.id;
  req.name = rec.name;
  req.prompt = rec.prompt;
  req.chatId = chat;
  const orch::FireOutcome o = fire(req, /*force=*/true);
  alogf("DREAM fired ok=%d detail=%s", o.ok ? 1 : 0, o.detail.c_str());
}

// ---- the boot reconcile: a night the restart cut short (L19-31) ------------------
static bool g_bootChecked = false;    // one reconcile per boot
static uint8_t g_bootTries = 0;       // state saves attempted for it (flood bound)
constexpr uint8_t kBootSaveTries = 10;

void pumpBootReconcile() {
  if (g_bootChecked) return;
  if (!flashMounted()) {   // nothing readable: no marker can exist on this boot
    g_bootChecked = true;
    return;
  }
  if (!memory::clockSynced()) return;   // the night and its window need the local clock
  DreamState st;
  loadState(st);
  const NightClock c = nightClock();
  const InterruptedNight v = reconcileBoot(st, c);
  if (!v.interrupted) {
    g_bootChecked = true;
    g_memo.loaded = true;   // this boot's working copy (RAM is the authority now)
    g_memo.st = st;
    return;
  }
  // Save FIRST, then count: the marker clears durably before any counter moves,
  // so a boot whose save fails (tried again below) can never move the streak or
  // consecFails twice for one interruption. A store that keeps refusing is said
  // ONCE and never looped on (the unit L bench class): the marker stays for a
  // boot with a working store, and this boot moves no counter.
  if (!saveState(st)) {
    if (++g_bootTries >= kBootSaveTries) {
      g_bootChecked = true;
      alog("dream: interrupted night NOT reconciled this boot (the state save keeps "
           "failing); the marker stays on disk");
    }
    return;
  }
  g_bootChecked = true;
  g_memo.loaded = true;
  g_memo.st = st;
  const NightReport& rep = v.report;
  alogf("dream: interrupted night reconciled - attempt %u of %u, the device restarted "
        "during the reflection%s",
        (unsigned)rep.attempt, (unsigned)kMaxAttempts,
        rep.retryAfterSec ? ", retry scheduled" : ", no retry inside the window");
  loops::applyInterruptedOutcome(kLoopId, rep.retryAfterSec, c.epoch);
  publishStatus(rep);
  if (rep.alert) g_pendingAlert = streakAlertText(rep);
  if (g_hooks.onDreamEnd) {
    agent::DreamEndEv ev;
    ev.turnOk = false;
    ev.cause = rep.error;
    ev.report = &rep;
    g_hooks.onDreamEnd(ev);
  }
}

}  // namespace dream
}  // namespace agent
