#include "loops_subsystem.h"

#include <LittleFS.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <vector>

#include "../sys/agent_log.h"
#include "agent_config.h"         // ORCH_AUTO_TURN_MIN_HEAP
#include "store.h"
#include "../sys/config_nvs.h"    // (device name - unused directly, kept for parity)
#include "net/wifi_portal.h"      // net::staConnected

namespace agent {
namespace loops {

namespace orch = nimbus::orch;
using orch::LoopRecord;
using orch::CivilTime;

// ---- state (single-writer on tg_poll) -------------------------------------
static std::vector<LoopRecord> g_loops;
static orch::DeviceCounters    g_dev;
static orch::LoopCaps          g_caps;   // TODO: honour owner NVS overrides (clamped)
static FireHook                g_fire;
static ChatAllowedHook         g_chatAllowed;
static AlertHook               g_alert;
static GateHook                g_gate;          // pre-fire idle gate (dream)
static std::vector<std::string> g_reservedIds;  // never-deletable loop ids
static bool                    g_loaded = false;

// g_loops is written on tg_poll (checkDue/drainWebMutations) but ALSO read on the
// AsyncTCP task (loopsJson/loopsText for GET /api/loops) and written there (the MCP
// loop.create/cancel tools run synchronously in the /mcp handler). On the dual-core
// S3 those are true concurrent accesses -> a push_back realloc mid-iteration is a
// use-after-free. Guard every g_loops touch with a recursive mutex. The lock is held
// only for SHORT sections; checkDue RELEASES it across the (5-30 s) fire so a turn
// never blocks the web reader.
static SemaphoreHandle_t       g_mux = nullptr;
struct Lock {
  Lock()  { if (g_mux) xSemaphoreTakeRecursive(g_mux, portMAX_DELAY); }
  ~Lock() { if (g_mux) xSemaphoreGiveRecursive(g_mux); }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
};

static const char* kLoopPath  = "/data/loops.json";
static const uint32_t LOOP_TICK_MS = 20000;   // 20 s sweep (cf. tickSdHealth / attn sweep)

// ---- persistence -----------------------------------------------------------
static std::string loadRaw() {
  File f = LittleFS.open(kLoopPath, FILE_READ);
  if (!f) return "";
  String s = f.readString();
  f.close();
  return std::string(s.c_str(), s.length());
}
static void persist() {
  std::string json = orch::dumpLoops(g_loops);
  File f = LittleFS.open(kLoopPath, FILE_WRITE);
  if (!f) { alog("loops: LittleFS write failed"); return; }
  f.write((const uint8_t*)json.data(), json.size());
  f.close();
}

// ---- time helpers (the ONLY tz-aware code; libc, on tg_poll) --------------
static bool clockValid() { time_t t = time(nullptr); return t > 1000000000; }

static CivilTime nowCivilLocal(uint64_t epoch) {
  time_t t = (time_t)epoch;
  struct tm lt;
  localtime_r(&t, &lt);
  CivilTime c;
  c.year = lt.tm_year + 1900; c.month = lt.tm_mon + 1; c.day = lt.tm_mday;
  c.hour = lt.tm_hour; c.min = lt.tm_min; c.sec = lt.tm_sec; c.wday = lt.tm_wday;
  return c;
}
static uint64_t civilToEpoch(const CivilTime& c) {
  struct tm lt = {};
  lt.tm_year = c.year - 1900; lt.tm_mon = c.month - 1; lt.tm_mday = c.day;
  lt.tm_hour = c.hour; lt.tm_min = c.min; lt.tm_sec = c.sec; lt.tm_isdst = -1;
  return (uint64_t)mktime(&lt);
}
static uint32_t curLocalDay(uint64_t epoch) {
  CivilTime c = nowCivilLocal(epoch);
  return orch::daysFromCivil(c.year, (unsigned)c.month, (unsigned)c.day);
}

// Next-fire: Interval off epoch (boot-relative pre-sync = monotonic uptime);
// Daily/Weekly off local civil (needs a valid clock).
static void advanceNextRun(LoopRecord& l, uint64_t nowEpoch, bool clk) {
  if (l.sched.kind == orch::SchedKind::Interval ||
      l.sched.kind == orch::SchedKind::Once) {   // Once: armed for now+N; the
                                                 // post-fire path retires it
    l.nextRun = orch::nextIntervalEpoch(l.sched, nowEpoch);
  } else if (clk) {
    l.nextRun = civilToEpoch(orch::nextWallClockLocal(l.sched, nowCivilLocal(nowEpoch)));
  } else {
    l.nextRun = 0;   // recomputed on clock sync
  }
}

// ---- id-gen ----------------------------------------------------------------
static std::string genId() {
  char b[10];
  snprintf(b, sizeof b, "lp%06x", (unsigned)(esp_random() & 0xFFFFFF));
  return std::string(b);
}

static LoopRecord* findById(const std::string& id) {
  for (auto& l : g_loops) if (l.id == id) return &l;
  return nullptr;
}

// The chat a fire targets ("" => owner default) + whether it's still allowed.
static bool fireChatAllowed(const LoopRecord& l) {
  if (l.chatId.empty()) return true;                 // owner default channel
  return g_chatAllowed ? g_chatAllowed(l.chatId) : true;
}

// ---- SNTP-landed rebase (prism correctness fix: no 0->epoch burst) ---------
static bool s_wasClockValid = false;
static uint32_t s_critAlertDay = 0;   // "device ceiling" alert de-dupe (once/day)

static void onClockSynced(uint64_t realNow) {
  const uint32_t day = curLocalDay(realNow);
  for (auto& l : g_loops) {
    if (l.sched.kind == orch::SchedKind::Interval)
      l.nextRun = realNow + l.sched.intervalSec;      // restart, no retro burst
    else
      advanceNextRun(l, realNow, true);               // compute the real next occurrence
    if (l.lastSyncedDay == 0) l.lastSyncedDay = day;   // adopt, don't wipe
  }
  g_dev.dayNumber = day;
  g_dev.windowStart = realNow;
  alogf("loops: clock synced (day=%u) - %u loop(s) rebased", (unsigned)day, (unsigned)g_loops.size());
  persist();
}

// ---- live TIMEZONE change (web Settings -> Timezone; drained on tg_poll) ----
// Deliberately NOT onClockSynced: that path restarts Interval loops (now+interval
// - wrong here, intervals are epoch-based and tz-independent) and only adopts the
// day frame when lastSyncedDay==0 (exactly the westward-stall hazard). A tz edit
// recomputes ONLY wall-clock schedules and adopts the day budget-neutrally.
static void onTzChanged() {
  onNetworkUp();   // re-reads devTz -> setenv/tzset via configTzTime + re-kicks
                   // SNTP (harmless offline: TZ still applies to localtime_r)
  if (!clockValid()) return;   // nextRun stays 0; onClockSynced rebases at sync
  const uint64_t now = (uint64_t)time(nullptr);
  const uint32_t day = curLocalDay(now);   // day under the NEW tz
  Lock lk;
  for (auto& l : g_loops) {
    // Leave a DUE-but-unfired wall-clock loop alone: its pending fire happens
    // this tick and its own post-fire advanceNextRun recomputes under the new
    // tz. Rebasing it here would strictly-after-now it and skip the fire (prism).
    if (l.sched.kind != orch::SchedKind::Interval &&
        !(l.nextRun != 0 && l.nextRun <= now))
      advanceNextRun(l, now, true);        // next occurrence on the NEW local clock
    orch::adoptLocalDay(l, day);           // counters untouched, both directions
  }
  g_dev.dayNumber = day;                   // same adoption for the device window
  alogf("loops: tz changed - wall-clock loops rebased (day=%u)", (unsigned)day);
  persist();
}

// ---- lifecycle -------------------------------------------------------------
void begin(FireHook fire, ChatAllowedHook chatAllowed, AlertHook alert) {
  if (!g_mux) g_mux = xSemaphoreCreateRecursiveMutex();   // before any g_loops access
  Lock lk;
  g_fire = fire; g_chatAllowed = chatAllowed; g_alert = alert;
  orch::loadLoops(loadRaw(), g_loops);
  g_loaded = true;
  const bool clk = clockValid();
  const uint64_t now = (uint64_t)time(nullptr);
  s_wasClockValid = clk;
  // Recompute nextRun at boot: raw millis-based nextRun never survives reboot.
  for (auto& l : g_loops) advanceNextRun(l, now, clk);
  if (clk) { g_dev.dayNumber = curLocalDay(now); }
  g_dev.windowStart = now;
  alogf("loops: begin - %u loop(s), clock=%d", (unsigned)g_loops.size(), (int)clk);
}

// ---- reserved system loops (DREAMING) --------------------------------------
static bool isReservedStd(const std::string& id) {
  for (const auto& r : g_reservedIds)
    if (r == id) return true;
  return false;
}

void ensureLoop(const orch::LoopRecord& record, bool reserved) {
  Lock lk;
  if (reserved && !isReservedStd(record.id)) g_reservedIds.push_back(record.id);
  if (findById(record.id)) return;   // persisted owner state (incl. paused) wins
  LoopRecord l = record;             // insert fresh: compute the first nextRun
  l.name.resize(std::min(l.name.size(), (size_t)orch::kLoopNameMax));      // cap on ensure too
  l.prompt.resize(std::min(l.prompt.size(), (size_t)orch::kLoopPromptMax));
  advanceNextRun(l, (uint64_t)time(nullptr), clockValid());
  g_loops.push_back(l);
  persist();
  alogf("loops: ensured %sloop '%s' (%s)", reserved ? "reserved " : "",
        l.name.c_str(), l.id.c_str());
}

bool isReservedId(const String& id) {
  Lock lk;
  return isReservedStd(std::string(id.c_str()));
}

void setFireGate(GateHook gate) {
  Lock lk;
  g_gate = std::move(gate);
}

// ---- the tick --------------------------------------------------------------
void checkDue(uint64_t nowEpoch, bool turnInFlight, uint32_t heap) {
  if (!g_loaded) return;
  // Apply web-staged mutations (create/approve/pause/delete) on EVERY tick, before
  // the fire-cadence gate - a loop created in the web UI must appear promptly, not
  // wait out the 20 s fire interval. (drainWebMutations locks g_loops itself.)
  drainWebMutations();

  static uint32_t s_last = 0;
  if ((uint32_t)(millis() - s_last) < LOOP_TICK_MS) return;
  s_last = millis();

  const bool clk = clockValid();
  { Lock lk;   // rebase on the 0->epoch SNTP jump (touches g_loops)
    if (clk && !s_wasClockValid) onClockSynced(nowEpoch);
    s_wasClockValid = clk;
  }

  if (turnInFlight) return;
  if (heap < (uint32_t)ORCH_AUTO_TURN_MIN_HEAP) return;
  if (!nimbus::net::staConnected()) return;   // no network => can't fire or deliver

  const uint32_t day = clk ? curLocalDay(nowEpoch) : 0;

  // --- SELECT + advance + persist, all under the lock (fast); release before firing.
  LoopFireRequest req;
  bool haveFire = false;
  {
    Lock lk;
    if (g_loops.empty()) return;
    for (auto& l : g_loops) orch::rollDayIfNeeded(l, day, clk);
    orch::rollDeviceDayIfNeeded(g_dev, g_caps, day, nowEpoch, clk);

    // Pick AT MOST ONE due loop (smallest nextRun); act on blocked ones inline.
    LoopRecord* due = nullptr;
    bool devCeiling = false;
    for (auto& l : g_loops) {
      orch::FireDecision d = orch::evaluate(l, g_dev, g_caps, nowEpoch, clk, fireChatAllowed(l));
      switch (d) {
        case orch::FireDecision::Fire:
          if (!due || l.nextRun < due->nextRun) due = &l;
          break;
        case orch::FireDecision::BlockedConsecFails:
          if (l.enabled) { l.enabled = false; l.lastResult = orch::LastResult::Paused;
            if (g_alert) g_alert(AlertLevel::Warn, l.id, "loop '" + l.name + "' disabled after repeated failures");
            persist(); }
          break;
        case orch::FireDecision::BlockedLoopTokens:
          if (l.enabled) { l.enabled = false; l.lastResult = orch::LastResult::Paused;
            if (g_alert) g_alert(AlertLevel::Warn, l.id, "loop '" + l.name + "' paused: hit its daily token limit");
            persist(); }
          break;
        case orch::FireDecision::BlockedChatRevoked:
          if (l.enabled) { l.enabled = false; l.lastResult = orch::LastResult::Paused;
            if (g_alert) g_alert(AlertLevel::Warn, l.id, "loop '" + l.name + "' paused: its target chat is no longer allow-listed");
            persist(); }
          break;
        case orch::FireDecision::BlockedDeviceTokens:
          devCeiling = true;
          break;
        default:
          break;   // SkipNotDue / BlockedDisabled / BlockedUnapproved / BlockedRateWindow / BlockedNoClock - silent
      }
    }
    if (devCeiling) {   // device-wide daily token ceiling -> alert once/day, don't fire
      if (day != s_critAlertDay) { s_critAlertDay = day;
        if (g_alert) g_alert(AlertLevel::Critical, "", "device daily token ceiling reached - scheduled loops paused until tomorrow"); }
      return;
    }
    if (!due) return;

    // --- pre-fire idle gate (DREAMING): a busy device defers the loop instead
    // of firing - nextRun slips, NO fire/ceiling counter moves (deferLoop).
    if (g_gate) {
      const uint32_t deferSec = g_gate(*due);
      if (deferSec) {
        orch::deferLoop(*due, nowEpoch, deferSec);
        persist();
        alogf("loops: '%s' deferred %us (gate: device not idle)",
              due->name.c_str(), (unsigned)deferSec);
        return;
      }
    }

    // --- fire: advance + persist BEFORE (at-most-once, no double-fire) ---
    req.id = due->id; req.name = due->name; req.chatId = due->chatId;
    // W20: a one-shot wakeup's turn opens with WHY it exists - the model set
    // it for itself and needs the context restored ("attach a message for
    // itself on wakeup").
    req.prompt = due->prompt;
    req.once   = (due->sched.kind == orch::SchedKind::Once);   // executor renders [WAKEUP]
    // An owner-set one-shot (/remind) is a REMINDER, not a self-wakeup - the
    // OWNER scheduled it, so the framing must not say "you scheduled this for
    // yourself". createdBy distinguishes the two (both are Once loops).
    req.ownerReminder = (req.once && due->createdBy == orch::CreatedBy::Owner);
    req.scheduledFor = nowEpoch;
    due->lastRun = nowEpoch;
    advanceNextRun(*due, nowEpoch, clk);
    due->firesToday++;
    g_dev.firesInWindow++;
    persist();
    haveFire = true;
  }   // <-- lock RELEASED here: the (5-30 s) fire must not block the web reader.
  if (!haveFire) return;

  const std::string id = req.id;
  FireOutcome o = g_fire ? g_fire(req) : FireOutcome{};

  // Re-acquire: the turn (and any concurrent MCP/web mutation) may have changed
  // g_loops, so re-find by id - the earlier pointer is not safe to reuse.
  Lock lk;
  LoopRecord* l = findById(id);
  if (l) {
    // NOTE (prism finding #2, partial): o.tokens is the scheduled turn's REAL
    // spend incl. all its tool-loop rounds (Phase 0). Sub-agents the turn spawns
    // dispatch on later ticks and their synthesis turn runs outside this path, so
    // their tokens are NOT yet attributed here - a follow-up (tag the JobRecord
    // with loopId, roll usage on completion). Until then the FIRE-COUNT caps
    // (kLoopMaxFiresPerDay + the device rate window) bound the fan-out blast radius.
    orch::onFireResult(*l, o.ok, o.tokens, orch::fnv64(o.detail), nowEpoch);
    g_dev.tokensToday += o.tokens.total();
    // W20: a one-shot wakeup fires ONCE then retires. Exception: if the TURN
    // failed (provider outage), one short retry - a silently lost wakeup would
    // break the model's follow-up promise; a second failure retires it and the
    // owner hears about it once.
    switch (orch::onceAfterFire(*l, o.ok)) {
      case orch::OnceAfterFire::RetryShort:
        l->nextRun = nowEpoch + orch::kWakeupRetrySec;
        persist();
        return;
      case orch::OnceAfterFire::Retire: {
        const std::string wid = l->id;
        const bool failed = !o.ok;
        for (auto it = g_loops.begin(); it != g_loops.end(); ++it)
          if (it->id == wid) { g_loops.erase(it); break; }
        persist();
        if (failed && g_alert)
          g_alert(AlertLevel::Warn, wid,
                  "a wakeup the assistant set for itself failed twice and was dropped");
        return;
      }
      case orch::OnceAfterFire::NotOnce: break;
    }
    if (orch::isSemanticRepeat(*l, g_caps.maxRepeats)) {
      l->enabled = false; l->lastResult = orch::LastResult::Paused;
      if (g_alert) g_alert(AlertLevel::Warn, l->id, "loop '" + l->name + "' paused: it kept returning the same result");
    } else if (!o.ok && l->consecFails >= (uint8_t)g_caps.maxConsecFails) {
      l->enabled = false; l->lastResult = orch::LastResult::Paused;
      if (g_alert) g_alert(AlertLevel::Warn, l->id, "loop '" + l->name + "' disabled after repeated failures");
    }
    persist();
    alogf("loops: fired '%s' ok=%d tokens=%u next=%llu", l->name.c_str(), (int)o.ok,
          (unsigned)o.tokens.total(), (unsigned long long)l->nextRun);
  }
}

void onNetworkUp() {
  // Kick SNTP (non-blocking). TZ from AKEY_TZ (POSIX, default UTC). localtime_r/
  // mktime then get tz + DST right. The 0->epoch jump is handled by onClockSynced.
  String tz = store::deviceTz();   // "" => UTC0
  if (tz.length() == 0) tz = "UTC0";
  configTzTime(tz.c_str(), "pool.ntp.org", "time.nist.gov", "time.google.com");
  alogf("loops: SNTP kicked (tz=%s)", tz.c_str());
}

// ---- mutations -------------------------------------------------------------
CreateResult createLoop(const String& name, const String& prompt, const String& chatId,
                        ArduinoJson::JsonObjectConst schedule, bool byAgent,
                        bool inScheduledTurn) {
  Lock lk;
  CreateResult r;
  // Parse FIRST so the scheduled-turn rail can exempt one-shot wakeups: a
  // wakeup chaining another wakeup ("not done yet - check again in 30") is the
  // owner-designed follow-up pattern, bounded by the same daily governor;
  // creating a RECURRING loop from a scheduled turn stays refused (persistence).
  orch::SchedSpec probe;
  {
    std::string perr;
    if (!orch::parseSpec(schedule, g_caps, probe, perr)) { r.err = perr; return r; }
  }
  if (byAgent && inScheduledTurn && probe.kind != orch::SchedKind::Once) {
    r.err = "a scheduled loop cannot create loops"; return r;
  }
  if (probe.kind == orch::SchedKind::Once) {
    int armed = 0;
    for (const auto& e : g_loops)
      if (e.sched.kind == orch::SchedKind::Once && e.enabled) armed++;
    if (armed >= orch::kWakeupMaxPending) {
      r.err = "you already have " + std::to_string(orch::kWakeupMaxPending) +
              " wakeups armed - cancel one (loop.cancel) or let one fire first";
      return r;
    }
  }
  if ((int)g_loops.size() >= g_caps.maxCount) { r.err = "too many loops (max " + std::to_string(g_caps.maxCount) + ")"; return r; }
  if (prompt.length() == 0) { r.err = "prompt is required"; return r; }
  orch::SchedSpec spec = probe;   // parsed once above

  LoopRecord l;
  l.id = genId();
  l.name = std::string(name.c_str()).substr(0, orch::kLoopNameMax);
  l.prompt = std::string(prompt.c_str()).substr(0, orch::kLoopPromptMax);
  l.chatId = std::string(chatId.c_str());
  l.sched = spec;
  l.createdBy = byAgent ? orch::CreatedBy::Agent : orch::CreatedBy::Owner;
  l.enabled = true;
  l.approved = orch::autoApproved(l.sched.kind, byAgent);   // Once wakeups are
                            // auto-approved even byAgent (see loops.h rationale);
                            // recurring agent loops still await the owner
  const uint64_t now = (uint64_t)time(nullptr);
  advanceNextRun(l, now, clockValid());
  g_loops.push_back(l);
  persist();
  if (byAgent && g_alert)
    g_alert(AlertLevel::Warn, l.id, "the assistant wants to schedule '" + l.name +
            "' - reply /loop approve " + l.id + " to allow it (or /loop deny " + l.id + ")");
  r.ok = true; r.id = l.id;
  return r;
}

bool approveLoop(const String& id) {
  Lock lk;
  LoopRecord* l = findById(std::string(id.c_str()));
  if (!l) return false;
  l->approved = true;
  persist();
  return true;
}
bool setEnabled(const String& id, bool on) {
  Lock lk;
  LoopRecord* l = findById(std::string(id.c_str()));
  if (!l) return false;
  l->enabled = on;
  if (on) l->consecFails = 0;   // resume clears the breaker
  persist();
  return true;
}
bool cancelLoop(const String& id) {
  Lock lk;
  const std::string sid(id.c_str());
  if (isReservedStd(sid)) return false;   // reserved (dream): pause-only, never deletable
  for (auto it = g_loops.begin(); it != g_loops.end(); ++it)
    if (it->id == sid) { g_loops.erase(it); persist(); return true; }
  return false;
}

int count() { Lock lk; return (int)g_loops.size(); }

String loopsJson() {
  Lock lk;
  ArduinoJson::JsonDocument d;
  ArduinoJson::JsonArray arr = d.to<ArduinoJson::JsonArray>();
  static const char* kSched[] = {"interval", "daily", "weekly", "once"};
  static const char* kRes[]   = {"none", "ok", "fail", "skipped", "paused"};
  for (const LoopRecord& l : g_loops) {
    ArduinoJson::JsonObject o = arr.add<ArduinoJson::JsonObject>();
    o["id"] = l.id; o["name"] = l.name; o["prompt"] = l.prompt;
    o["chatId"] = l.chatId;
    o["kind"] = kSched[(int)l.sched.kind % 4];
    if (l.sched.kind == orch::SchedKind::Interval ||
        l.sched.kind == orch::SchedKind::Once) o["everySec"] = l.sched.intervalSec;
    else { char at[6]; snprintf(at, sizeof at, "%02u:%02u", l.sched.minuteOfDay / 60, l.sched.minuteOfDay % 60); o["at"] = at; o["weekMask"] = l.sched.weekMask; }
    o["byAgent"]   = (l.createdBy == orch::CreatedBy::Agent);
    o["reserved"]  = isReservedStd(l.id);   // system loop: web UI hides delete
    o["enabled"]   = l.enabled;
    o["approved"]  = l.approved;
    o["nextRun"]   = l.nextRun;
    o["lastRun"]   = l.lastRun;
    o["lastResult"] = kRes[(int)l.lastResult % 5];
    o["firesToday"]  = l.firesToday;
    o["tokensToday"] = l.tokensToday;
    o["consecFails"] = l.consecFails;
  }
  String out; ArduinoJson::serializeJson(d, out);
  return out;
}

String loopsText() {
  Lock lk;
  if (g_loops.empty()) return "No scheduled loops. Ask me to set one up, e.g. \"every morning at 8, summarize my overnight sessions\".";
  String s = "Scheduled loops:\n";
  for (const LoopRecord& l : g_loops) {
    s += "\xE2\x80\xA2 " + String(l.name.c_str()) + "  [" + String(l.id.c_str()) + "]  ";
    if (l.sched.kind == orch::SchedKind::Once) {
      s += "one-time wakeup, in ~" +
           String((uint32_t)((l.nextRun > (uint64_t)time(nullptr))
                                 ? (l.nextRun - (uint64_t)time(nullptr)) / 60 : 0)) + " min";
    } else if (l.sched.kind == orch::SchedKind::Interval) {
      s += "every " + String(l.sched.intervalSec / 60) + " min";
    } else {
      char at[6]; snprintf(at, sizeof at, "%02u:%02u", l.sched.minuteOfDay / 60, l.sched.minuteOfDay % 60);
      s += String(l.sched.kind == orch::SchedKind::Daily ? "daily " : "weekly ") + at;
    }
    if (l.createdBy == orch::CreatedBy::Agent && !l.approved)
      s += "  \xE2\x9A\xA0 PENDING approval \xE2\x86\x92 /loop approve " + String(l.id.c_str());
    else if (!l.enabled) s += "  (paused \xE2\x86\x92 /loop on " + String(l.id.c_str()) + ")";
    s += "\n";
  }
  return s;
}

// ---- web staging (AsyncTCP task -> drained on tg_poll) --------------------
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static std::vector<std::string> s_pending;   // guarded by s_mux

void stageWebMutation(const String& json) {
  portENTER_CRITICAL(&s_mux);
  s_pending.push_back(std::string(json.c_str()));
  portEXIT_CRITICAL(&s_mux);
}

void drainWebMutations() {
  std::vector<std::string> work;
  portENTER_CRITICAL(&s_mux);
  work.swap(s_pending);
  portEXIT_CRITICAL(&s_mux);
  if (work.empty()) return;
  Lock lk;   // apply the batch atomically vs AsyncTCP readers (recursive: the
             // create/approve/… sub-calls re-take it harmlessly)
  for (const std::string& j : work) {
    ArduinoJson::JsonDocument d;
    if (ArduinoJson::deserializeJson(d, j)) continue;
    const char* action = d["action"] | "";
    const char* id = d["id"] | "";
    if (!strcmp(action, "create")) {
      createLoop(d["name"] | "", d["prompt"] | "", d["chatId"] | "",
                 d["schedule"].as<ArduinoJson::JsonObjectConst>(), false, false);
    } else if (!strcmp(action, "approve")) { approveLoop(id);
    } else if (!strcmp(action, "pause"))   { setEnabled(id, false);
    } else if (!strcmp(action, "resume"))  { setEnabled(id, true);
    } else if (!strcmp(action, "delete"))  { cancelLoop(id);
    } else if (!strcmp(action, "tzapply")) {
      // devTz was already persisted on the AsyncTCP task (NVS is mutexed);
      // the tz APPLY must run here - g_loops is single-writer on tg_poll, and
      // setenv/tzset must not race this task's own localtime_r/mktime calls.
      onTzChanged();
    } else if (!strcmp(action, "sntp")) {
      // "Sync now": re-kick SNTP. Rate-limited so a retry-clicker can't churn
      // the SNTP service; the badge flips via the normal /api/state poll.
      static uint32_t s_lastSntpKick = 0;
      const uint32_t nowMs = millis();
      if (s_lastSntpKick == 0 || nowMs - s_lastSntpKick >= 10000) {
        s_lastSntpKick = nowMs;
        onNetworkUp();
      }
    }
  }
}

}  // namespace loops
}  // namespace agent
