#include "loops_subsystem.h"

#include <LittleFS.h>
#include <esp_sntp.h>              // esp_sntp_* - restart the client without touching TZ
#include <time.h>
#include <esp_heap_caps.h>       // heap_caps_get_largest_free_block (CUM-404 fragmentation gate)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <vector>

#include "../sys/agent_log.h"
#include "agent_config.h"         // ORCH_AUTO_TURN_MIN_HEAP
#include "memory_subsystem.h"     // memory::clockSynced - the strict "clock is real" test
#include "nimbus/harness/dream.h"           // applyInterruptedToLoop - the dream's boot reconcile
#include "nimbus/orch/loop_delivery.h"   // resolveDelivery - where a fire delivers (CUM-505)
#include "nimbus/wifi/net_up.h"   // SntpKick - the pure SNTP kick schedule (CUM-504)
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
static orch::LoopCaps          g_caps;   // caps.h defaults, tightened by owner NVS overrides (reloadCaps)
static FireHook                g_fire;
static ChatAllowedHook         g_chatAllowed;
static AlertHook               g_alert;
static GateHook                g_gate;          // pre-fire idle gate (dream)
static OriginHook              g_origin;        // the creating turn's chat (CUM-505)
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

// "Wake-ups: ask me first" policy (CUM-27). Off by default = an agent-armed wakeup
// is auto-approved (shipped behavior); on = it lands pending and the owner gets one
// approval card. Persisted in its own tiny file so the loops.json array shape (and
// its roundtrip test) is untouched.
static const char* kWakeupModePath = "/data/wakeup_mode";
static bool g_wakeupsAskFirst = false;
static void loadWakeupMode() {
  File f = LittleFS.open(kWakeupModePath, FILE_READ);
  if (!f) { g_wakeupsAskFirst = false; return; }
  String s = f.readString();
  f.close();
  g_wakeupsAskFirst = (s.length() > 0 && s[0] == '1');
}
static void persistWakeupMode() {
  File f = LittleFS.open(kWakeupModePath, FILE_WRITE);
  if (!f) { alog("loops: wakeup-mode write failed"); return; }
  f.write((const uint8_t*)(g_wakeupsAskFirst ? "1" : "0"), 1);
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

// Where a fire delivers (CUM-505): the loop's remembered channel, the owner default,
// or the web chat for the owner's own Telegram loop while the bot is off. The pure rule
// is orch::resolveDelivery; this seam only reads the Telegram facts (once per tick,
// `world`) and asks the allow hook (allowlist + tenant role) about a Telegram chat. A
// Revoked verdict is the "chat revoked" pause rail; no fire is ever aimed at "".
static orch::DeliveryWorld readDeliveryWorld() {
  return orch::deliveryWorld(std::string(store::telegramToken().c_str()),
                             std::string(store::telegramAllowlist().c_str()),
                             std::string(store::telegramOwners().c_str()));
}
static orch::Delivery deliveryFor(const LoopRecord& l, const orch::DeliveryWorld& world) {
  const bool telegramChat = orch::deliveryChannelOf(l.chatId) == orch::DeliveryChannel::Telegram;
  const bool allowed = !telegramChat || !g_chatAllowed || g_chatAllowed(l.chatId);
  return orch::resolveDelivery(l.chatId, world, allowed);
}

// ---- SNTP-landed rebase (prism correctness fix: no 0->epoch burst) ---------
static bool s_wasClockValid = false;
static uint64_t s_prevTickNow = 0;    // last pre-sync now() - the boot-relative base for the rebase
static uint32_t s_critAlertDay = 0;   // "device ceiling" alert de-dupe (once/day)
// CUM-404/403: the last tick deferred all fires for memory pressure (low total-free
// OR a low largest contiguous internal block). Transient, not persisted: it reflects
// the most recent tick's heap so the Routines pane can honestly say a due routine is
// "waiting for working memory" instead of a bare next-run countdown. Cleared the tick
// the device clears both floors.
static bool s_memDefer = false;
// A loop's scheduled turn is running (CUM-505). Set and cleared around g_fire on
// tg_poll, read on tg_poll by the turn-completion wiring (orchestrator.cpp).
static bool s_firing = false;

static void onClockSynced(uint64_t realNow, uint64_t oldNow) {
  const uint32_t day = curLocalDay(realNow);
  for (auto& l : g_loops) {
    if (l.sched.kind == orch::SchedKind::Interval)
      l.nextRun = realNow + l.sched.intervalSec;      // restart, no retro burst
    else if (l.sched.kind == orch::SchedKind::Once)
      l.nextRun = orch::onceRebaseNextRun(l, realNow, oldNow);   // keep the remaining/retry delay
    else
      advanceNextRun(l, realNow, true);               // compute the real next occurrence
    if (l.lastSyncedDay == 0) l.lastSyncedDay = day;   // adopt, don't wipe
  }
  g_dev.dayNumber = day;
  g_dev.windowStart = realNow;
  alogf("loops: clock synced (day=%u) - %u loop(s) rebased", (unsigned)day, (unsigned)g_loops.size());
  persist();
}

// The owner's POSIX timezone from NVS ("" => UTC0).
static String deviceTzOrUtc() {
  String tz = store::deviceTz();
  return tz.length() ? tz : String("UTC0");
}

// Apply the timezone (setenv + tzset). Only in setup() (begin) and on tg_poll (a tz
// edit), the task that runs localtime_r/mktime here: never from the main task, whose
// SNTP restarts deliberately leave TZ alone (CUM-504).
static String applyTz() {
  const String tz = deviceTzOrUtc();
  setenv("TZ", tz.c_str(), 1);
  tzset();
  return tz;
}

// ---- live TIMEZONE change (web Settings -> Timezone; drained on tg_poll) ----
// Deliberately NOT onClockSynced: that path restarts Interval loops (now+interval
// - wrong here, intervals are epoch-based and tz-independent) and only adopts the
// day frame when lastSyncedDay==0 (exactly the westward-stall hazard). A tz edit
// recomputes ONLY wall-clock schedules and adopts the day budget-neutrally.
static void onTzChanged() {
  alogf("loops: timezone applied (tz=%s)", applyTz().c_str());   // local: no network needed
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
// Fold the owner's NVS overrides onto the caps.h defaults (tighten-only; the
// clamp lives in the pure core). Called at begin() and whenever the web config
// path writes a governor key, so a tightened cap takes effect without a reboot.
void reloadCaps() {
  orch::LoopCapOverrides ov;
  ov.maxCount        = store::loopCapMaxCount();
  ov.minIntervalSec  = (uint32_t)store::loopCapMinIntervalS();
  ov.maxFiresPerDay  = store::loopCapFiresPerDay();
  ov.maxTokensPerDay = (uint32_t)store::loopCapTokensPerDay();
  ov.devTokensPerDay = (uint32_t)store::loopCapDevTokensPerDay();
  ov.devFiresWindow  = store::loopCapDevFiresWindow();
  g_caps = orch::clampLoopCaps(orch::LoopCaps{}, ov);
}

void begin(FireHook fire, ChatAllowedHook chatAllowed, AlertHook alert) {
  if (!g_mux) g_mux = xSemaphoreCreateRecursiveMutex();   // before any g_loops access
  Lock lk;
  reloadCaps();   // honour owner NVS cap overrides (clamped, tighten-only)
  g_fire = fire; g_chatAllowed = chatAllowed; g_alert = alert;
  orch::loadLoops(loadRaw(), g_loops);
  loadWakeupMode();
  g_loaded = true;
  // The timezone is a local setting: apply it before any wall-clock math below, not on
  // the first SNTP kick (CUM-504: that lands on the first loop tick, after setup). A
  // soft restart keeps the RTC clock valid, so daily/weekly next runs are computed right
  // here and must use local time. Runs in setup(), before tg_poll exists.
  applyTz();
  // CUM-505 repair: the old fire-time check paused loops that remembered the web chat or
  // the device as "chat revoked". Resume the recurring ones (a stale reminder stays).
  int lifted = 0;
  for (auto& l : g_loops) lifted += orch::liftDeviceChannelRevoke(l) ? 1 : 0;
  if (lifted) {
    alogf("loops: resumed %d routine(s) wrongly paused as chat-revoked", lifted);
    persist();
  }
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
  if (LoopRecord* have = findById(record.id)) {   // persisted owner state (incl. paused) wins
    // ... except a breaker pause of a reserved loop, which is firmware's own decision,
    // not the owner's: the dream is never left off by repeated failures (lane M).
    if (reserved && orch::reviveReservedLoop(*have, g_caps.maxConsecFails)) {
      persist();
      alogf("loops: resumed reserved loop '%s' (paused by failures, not by the owner)",
            have->name.c_str());
    }
    return;
  }
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

void setOriginHook(OriginHook origin) {
  Lock lk;
  g_origin = std::move(origin);
}

bool firing() { return s_firing; }

bool applyInterruptedOutcome(const String& id, uint32_t retryAfterSec, uint64_t nowEpoch) {
  Lock lk;
  LoopRecord* l = findById(std::string(id.c_str()));
  if (!l) return false;
  // The decision lives in the host-tested core; this seam only holds the lock.
  agent::dream::applyInterruptedToLoop(*l, nowEpoch, retryAfterSec);
  persist();
  return true;
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
    if (clk && !s_wasClockValid) onClockSynced(nowEpoch, s_prevTickNow);
    s_wasClockValid = clk;
  }
  s_prevTickNow = nowEpoch;   // remember this tick's clock as the next rebase's oldNow

  if (turnInFlight) return;
  // CUM-404/403: defer BEFORE selecting/advancing (round 0) on BOTH floors, so a
  // memory defer never advances nextRun, bumps firesToday, or burns the breaker - it
  // simply retries next tick. Total-free is the conservative backstop; the largest
  // contiguous internal block is the real gate (fragmentation, not total free, is
  // what fails the scheduled turn's mbedTLS handshake on a fragmented CYD).
  const uint32_t largest =
      (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (heap < (uint32_t)ORCH_AUTO_TURN_MIN_HEAP ||
      largest < (uint32_t)ORCH_TURN_MIN_LARGEST_BLOCK) {
    if (!s_memDefer)
      alogf("loops: fires deferred (low memory: heap %u/%u largest %u/%u)",
            (unsigned)heap, (unsigned)ORCH_AUTO_TURN_MIN_HEAP,
            (unsigned)largest, (unsigned)ORCH_TURN_MIN_LARGEST_BLOCK);
    s_memDefer = true;
    return;
  }
  s_memDefer = false;   // both floors clear this tick
  if (!nimbus::net::staConnected()) return;   // no network => can't fire or deliver

  const uint32_t day = clk ? curLocalDay(nowEpoch) : 0;

  // --- SELECT + advance + persist, all under the lock (fast); release before firing.
  LoopFireRequest req;
  bool haveFire = false;
  uint64_t prevLastRun = 0;   // restored if the fire turns out to be a memory defer
  {
    Lock lk;
    if (g_loops.empty()) return;
    const orch::DeliveryWorld world = readDeliveryWorld();   // Telegram facts, once per tick
    for (auto& l : g_loops) orch::rollDayIfNeeded(l, day, clk);
    orch::rollDeviceDayIfNeeded(g_dev, g_caps, day, nowEpoch, clk);

    // Pick AT MOST ONE due loop (smallest nextRun); act on blocked ones inline.
    LoopRecord* due = nullptr;
    bool devCeiling = false;
    for (auto& l : g_loops) {
      const bool deliverable = deliveryFor(l, world).verdict != orch::DeliveryVerdict::Revoked;
      orch::FireDecision d = orch::evaluate(l, g_dev, g_caps, nowEpoch, clk, deliverable);
      switch (d) {
        case orch::FireDecision::Fire:
          if (!due || l.nextRun < due->nextRun) due = &l;
          break;
        case orch::FireDecision::BlockedConsecFails:
          if (!orch::breakerTrips(l, g_caps.maxConsecFails, isReservedStd(l.id))) { persist(); break; }
          if (l.enabled) { l.enabled = false; l.lastResult = orch::LastResult::Paused;
            l.pauseReason = orch::PauseReason::ConsecFails;
            if (g_alert) g_alert(AlertLevel::Warn, l.id, "loop '" + l.name + "' disabled after repeated failures");
            persist(); }
          break;
        case orch::FireDecision::BlockedLoopTokens:
          if (l.enabled) { l.enabled = false; l.lastResult = orch::LastResult::Paused;
            l.pauseReason = orch::PauseReason::LoopTokens;
            if (g_alert) g_alert(AlertLevel::Warn, l.id, "loop '" + l.name + "' paused: hit its daily token limit");
            persist(); }
          break;
        case orch::FireDecision::BlockedChatRevoked:
          if (l.enabled) { l.enabled = false; l.lastResult = orch::LastResult::Paused;
            l.pauseReason = orch::PauseReason::ChatRevoked;
            // CUM-505: also a revoked tenant, an emptied allowlist, or a member's chat
            // while the bot is off - not only an allowlist removal.
            if (g_alert) g_alert(AlertLevel::Warn, l.id, "routine '" + l.name + "' paused: its chat can no longer receive it");
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
    // The request carries the RESOLVED target (never "": the executor would aim that at
    // the first Telegram allowlist entry, which is "" on a web-only device), the
    // [WAKEUP]/[REMINDER] framing flags, and on a fallback the instruction to say so.
    const orch::Delivery dl = deliveryFor(*due, world);
    req = orch::buildFireRequest(*due, dl, nowEpoch);
    if (dl.verdict == orch::DeliveryVerdict::FellBack)
      alogf("loops: '%s' delivers to %s (%s is not available)", due->name.c_str(),
            orch::deliveryChannelLabel(dl.to), orch::deliveryChannelLabel(dl.from));
    prevLastRun = due->lastRun;   // so a memory defer can un-claim this "run"
    due->lastRun = nowEpoch;
    advanceNextRun(*due, nowEpoch, clk);
    due->firesToday++;
    g_dev.firesInWindow++;
    persist();
    haveFire = true;
  }   // <-- lock RELEASED here: the (5-30 s) fire must not block the web reader.
  if (!haveFire) return;

  const std::string id = req.id;
  s_firing = true;    // tg_poll only: read by the turn-completion wiring (firing())
  FireOutcome o = g_fire ? g_fire(req) : FireOutcome{};
  s_firing = false;

  // Re-acquire: the turn (and any concurrent MCP/web mutation) may have changed
  // g_loops, so re-find by id - the earlier pointer is not safe to reuse.
  Lock lk;
  LoopRecord* l = findById(id);
  if (l) {
    // CUM-404/403: the executor refused this fire for memory pressure (the largest
    // block dropped between the pre-fire gate above and the fire - e.g. the dream
    // loop's phase-1 maintenance itself consumed the heap before its phase-2 turn).
    // A defer is NOT a fire: undo the pre-fire counter bump and un-claim the run, so
    // it never rolls the consec-fail breaker or burns a daily fire.
    // Deliberately leave nextRun at its advanced slot rather than pulling it back to a
    // short retry: a fire's pre-work is not always idempotent (the dream loop decays,
    // prunes, dedups + persists BEFORE its turn), so a 20 s re-fire would re-run that
    // maintenance every tick on a fragmented board. Prompt, pre-work-free retry is the
    // PRIMARY tick gate's job (it defers before selecting any loop while memory is low).
    if (!orch::fireWasAttempted(o)) {
      if (l->firesToday > 0) l->firesToday--;
      if (g_dev.firesInWindow > 0) g_dev.firesInWindow--;
      l->lastRun = prevLastRun;   // the fire did not happen; do not claim it did
      s_memDefer = true;
      // A fire whose pre-work is idempotent (the dream runs maintenance once per local
      // day) asks for a same-night retry instead of waiting for its next slot.
      if (o.retryAfterSec) l->nextRun = nowEpoch + o.retryAfterSec;
      persist();
      alogf("loops: '%s' fire deferred (low memory) - not counted, retries %s",
            l->name.c_str(), o.retryAfterSec ? "tonight" : "next slot");
      return;
    }
    // NOTE (prism finding #2, partial): o.tokens is the scheduled turn's REAL
    // spend incl. all its tool-loop rounds (Phase 0). Sub-agents the turn spawns
    // dispatch on later ticks and their synthesis turn runs outside this path, so
    // their tokens are NOT yet attributed here - a follow-up (tag the JobRecord
    // with loopId, roll usage on completion). Until then the FIRE-COUNT caps
    // (kLoopMaxFiresPerDay + the device rate window) bound the fan-out blast radius.
    orch::onFireResult(*l, o.ok, o.tokens, orch::fnv64(o.detail), nowEpoch);
    orch::noteFireError(*l, o.ok, o.error);   // why it failed, on the record (lane T)
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
    // A transiently failed fire with idempotent pre-work retries soon (lane M).
    if (!o.ok && o.retryAfterSec) l->nextRun = nowEpoch + o.retryAfterSec;
    if (orch::isSemanticRepeat(*l, g_caps.maxRepeats)) {
      l->enabled = false; l->lastResult = orch::LastResult::Paused;
      l->pauseReason = orch::PauseReason::Repeated;
      if (g_alert) g_alert(AlertLevel::Warn, l->id, "loop '" + l->name + "' paused: it kept returning the same result");
    } else if (!o.ok && orch::breakerTrips(*l, g_caps.maxConsecFails, isReservedStd(l->id))) {
      l->enabled = false; l->lastResult = orch::LastResult::Paused;
      l->pauseReason = orch::PauseReason::ConsecFails;
      if (g_alert) g_alert(AlertLevel::Warn, l->id, "loop '" + l->name + "' disabled after repeated failures");
    }
    persist();
    alogf("loops: fired '%s' ok=%d tokens=%u next=%llu", l->name.c_str(), (int)o.ok,
          (unsigned)o.tokens.total(), (unsigned long long)l->nextRun);
  }
}

// The SNTP kick schedule. MAIN TASK ONLY (the net::link hook), and the ONLY thing that
// restarts the SNTP client: two producers interleaving stop/mode/init on the lwIP
// thread can trip lwIP's "mode set while running" assert. "Sync now" on tg_poll only
// raises s_sntpRequested; sntpTick consumes it.
static nimbus::wifi::SntpKick s_sntpKick;
static std::atomic<bool>      s_sntpRequested{false};

// Restart the SNTP client WITHOUT touching TZ (see applyTz). esp_sntp_* queue onto the
// lwIP thread in order.
static void restartSntpClient() {
  if (esp_sntp_enabled()) esp_sntp_stop();
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_setservername(1, "time.nist.gov");
  esp_sntp_setservername(2, "time.google.com");
  esp_sntp_init();
}

void sntpTick(uint32_t nowMs, bool netUp, bool upEdge) {
  using nimbus::wifi::SntpKick;
  if (s_sntpRequested.exchange(false)) s_sntpKick.request();
  const bool synced = memory::clockSynced();
  const SntpKick::Why w = s_sntpKick.tick(nowMs, netUp, upEdge, synced);
  if (w == SntpKick::Why::None) return;
  restartSntpClient();
  // Durable only for the evidence a bench reads: a link-up kick while the clock is still
  // unsynced (the boot join, or a reconnect before the first sync) and the owner's Sync
  // now. Backstop retries and re-syncs of a synced clock can repeat for hours on a
  // flapping or NTP-blocking network, so they stay RAM + serial only (flash wear).
  const bool durable = w == SntpKick::Why::Requested || (w == SntpKick::Why::LinkUp && !synced);
  const char* why = nimbus::wifi::sntpWhyName(w);
  const String tz = deviceTzOrUtc();
  if (durable) alogf("loops: SNTP kicked (tz=%s, %s)", tz.c_str(), why);
  else         alogif("loops: SNTP kicked (tz=%s, %s)", tz.c_str(), why);
}

// ---- mutations -------------------------------------------------------------
// The chat of the turn creating an agent loop ("" for an owner surface or no turn).
static std::string creatingTurnChat(bool byAgent) {
  if (!byAgent || !g_origin) return std::string();
  return g_origin();
}

// True (and sets `err`) if arming another one-shot wakeup would exceed the cap.
// Call with the loops lock held (createLoop does).
static bool wakeupArmedCapReached(std::string& err) {
  int armed = 0;
  for (const auto& e : g_loops)
    if (e.sched.kind == orch::SchedKind::Once && e.enabled) armed++;
  if (armed < orch::kWakeupMaxPending) return false;
  err = "you already have " + std::to_string(orch::kWakeupMaxPending) +
        " wakeups armed - cancel one (loop.cancel) or let one fire first";
  return true;
}

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
  if (probe.kind == orch::SchedKind::Once && wakeupArmedCapReached(r.err)) return r;
  if ((int)g_loops.size() >= g_caps.maxCount) { r.err = "too many loops (max " + std::to_string(g_caps.maxCount) + ")"; return r; }
  if (prompt.length() == 0) { r.err = "prompt is required"; return r; }
  orch::SchedSpec spec = probe;   // parsed once above

  LoopRecord l;
  l.id = genId();
  l.name = std::string(name.c_str()).substr(0, orch::kLoopNameMax);
  l.prompt = std::string(prompt.c_str()).substr(0, orch::kLoopPromptMax);
  // Remember the channel it was created from (CUM-505): an explicit chat wins, else
  // an agent-created loop takes the creating turn's chat (web chat, Telegram chat,
  // hold-to-talk), so it delivers where it was asked for.
  l.chatId = orch::originChat(std::string(chatId.c_str()), creatingTurnChat(byAgent));
  l.sched = spec;
  l.createdBy = byAgent ? orch::CreatedBy::Agent : orch::CreatedBy::Owner;
  l.enabled = true;
  l.approved = orch::autoApproved(l.sched.kind, byAgent, g_wakeupsAskFirst);
                            // Once wakeups are auto-approved even byAgent UNLESS
                            // the owner turned on "Wake-ups: ask me first" (CUM-27);
                            // recurring agent loops always await the owner.
  const uint64_t now = (uint64_t)time(nullptr);
  advanceNextRun(l, now, clockValid());
  g_loops.push_back(l);
  persist();
  // Raise the SINGLE approval card only when the loop actually lands pending - an
  // auto-approved wakeup must not tell the owner to approve something already
  // live, and evaluate() blocks a pending loop each tick without re-asking, so
  // there is no re-ask loop.
  if (byAgent && !l.approved && g_alert)
    g_alert(AlertLevel::Warn, l.id, "the assistant wants to schedule '" + l.name +
            "' - reply /loop approve " + l.id + " to allow it (or /loop deny " + l.id + ")");
  r.ok = true; r.id = l.id; r.approved = l.approved;
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
  if (on) { l->consecFails = 0;                         // resume clears the breaker,
            l->tokensToday = 0;                          // ... the daily token budget (so a
                                                         // token-capped resume actually runs, not
                                                         // re-pause+re-alert - the "Resume to run it
                                                         // again" copy is now true),
            l->pauseReason = orch::PauseReason::None; }  // ... and the paused-reason badge
  else      l->pauseReason = orch::PauseReason::Owner;   // a deliberate owner pause (no fault)
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

// Authoritative "why is this routine (not) firing" for the read surfaces
// (loopsJson + loopsText). An enabled+approved loop is evaluated live against the
// current clock + governor; a paused loop reports its persisted PauseReason so the
// pane can state WHY, not a bare "paused". chatAllowed is passed true here: a
// revoked chat auto-pauses the loop within a tick, so it surfaces via PauseReason
// rather than a cross-task allow-list call on the web reader. Call with the lock held.
static void loopReason(const LoopRecord& l, uint64_t now, bool clk,
                       const char*& slug, const char*& text) {
  if (!l.enabled) {
    slug = orch::pauseReasonSlug(l.pauseReason);
    text = orch::pauseReasonText(l.pauseReason);
    return;
  }
  orch::FireDecision d = orch::evaluate(l, g_dev, g_caps, now, clk, /*chatAllowed=*/true);
  // CUM-404/403: a routine that WOULD fire but is held back this tick by device
  // memory pressure (a fragmented heap) is not "scheduled" and not "failing" - say so
  // honestly. Transient (s_memDefer reflects the last tick), so it clears on its own
  // when memory frees. Only overrides the would-fire case; a not-due loop keeps its
  // countdown. (Copy: US English, no em dash, no space-hyphen-space.)
  if (d == orch::FireDecision::Fire && s_memDefer) {
    slug = "low-memory";
    text = "Waiting for working memory. It runs when memory frees up.";
    return;
  }
  slug = orch::fireReasonSlug(d);
  text = orch::fireReasonText(d);
}

String loopsJson() {
  Lock lk;
  const uint64_t now = (uint64_t)time(nullptr);
  const bool clk = clockValid();
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
    if (!l.lastErr.empty()) o["lastErr"] = l.lastErr;   // why the last fire failed
    // Why it is / isn't firing right now (empty text = firing or simply scheduled;
    // the pane shows the next-run countdown for those). whyCode is the stable slug.
    const char* rslug; const char* rtext;
    loopReason(l, now, clk, rslug, rtext);
    o["why"]     = rtext;
    o["whyCode"] = rslug;
  }
  String out; ArduinoJson::serializeJson(d, out);
  return out;
}

String wakeupsJson() {
  Lock lk;
  return String(orch::dumpWakeupsApi(g_loops, g_wakeupsAskFirst,
                                     (uint64_t)time(nullptr)).c_str());
}

bool wakeupsAskFirst() { Lock lk; return g_wakeupsAskFirst; }

void setWakeupsAskFirst(bool on) {
  Lock lk;
  if (g_wakeupsAskFirst == on) return;
  g_wakeupsAskFirst = on;
  persistWakeupMode();
}

String loopsText() {
  Lock lk;
  if (g_loops.empty()) return "No scheduled loops. Ask me to set one up, e.g. \"every morning at 8, summarize my overnight sessions\".";
  const uint64_t now = (uint64_t)time(nullptr);
  const bool clk = clockValid();
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
    if (l.createdBy == orch::CreatedBy::Agent && !l.approved) {
      s += "  \xE2\x9A\xA0 PENDING approval \xE2\x86\x92 /loop approve " + String(l.id.c_str());
    } else if (!l.enabled) {
      s += "  " + String(orch::pauseReasonText(l.pauseReason)) +
           " (/loop on " + String(l.id.c_str()) + ")";
    } else {
      // Enabled + approved: surface a live block reason (no clock, rate window,
      // caps) so a routine that is not firing never looks simply idle.
      const char* rslug; const char* rtext;
      loopReason(l, now, clk, rslug, rtext);
      if (rtext[0]) s += "  " + String(rtext);
    }
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
    } else if (!strcmp(action, "wakeup_mode")) {
      // "Wake-ups: ask me first" toggle from the web surface (CUM-27).
      const char* v = d["value"] | "";
      setWakeupsAskFirst(!strcmp(v, "ask"));
    } else if (!strcmp(action, "tzapply")) {
      // devTz was already persisted on the AsyncTCP task (NVS is mutexed);
      // the tz APPLY must run here - g_loops is single-writer on tg_poll, and
      // setenv/tzset must not race this task's own localtime_r/mktime calls.
      onTzChanged();
    } else if (!strcmp(action, "sntp")) {
      // "Sync now": ask the main-task kick schedule for a restart (it is the one
      // SNTP producer, and it rate-limits to one kick per 10 s). The badge flips via
      // the normal /api/state poll.
      s_sntpRequested.store(true);
    }
  }
}

}  // namespace loops
}  // namespace agent
