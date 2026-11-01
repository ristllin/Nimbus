#pragma once
#include <ArduinoJson.h>

#include <cstdio>
#include <ctime>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "nimbus/harness/skill_store.h"   // skillTurnTainted: this turn read outside content
#include "nimbus/orch/loop_delivery.h"
#include "nimbus/orch/loops.h"
#include "nimbus/orch/spend_ceiling.h"   // isSpendFrozenError
#include "nimbus/orch/tool_registry.h"
#include "hosted_settings.h"             // tzMutex: every local-time read here takes it
#include "posix_fs.h"

// hosted_loops - routines and wake-ups on a hosted instance, over the same pure core
// the device runs (nimbus/orch/loops.h, loop_delivery.h: schedule math, the fire
// decision, the breakers, the persistence format, the /api/wakeups contract). What
// is here is the seam the device keeps in src/agent/loops_subsystem.cpp: the file, the
// clock, one due fire per tick. There is no dream here and no memory floor: a hosted
// instance has no card to groom and a container's heap is not the device's.
//
// Single-context like the engine: every method runs on the engine thread (the web
// routes dispatch there, and a tool call runs inside a turn).
namespace nimbusd {

struct HostedLoopHooks {
  std::function<nimbus::orch::FireOutcome(const nimbus::orch::LoopFireRequest&)> fire;
  std::function<bool(const std::string& chat)> chatAllowed;   // fire-time re-check
  std::function<nimbus::orch::DeliveryWorld()> world;         // the Telegram facts
  std::function<void(const std::string& text)> alert;         // a message to the owner
  std::function<std::string()> originChat;                    // the creating turn's chat
  std::function<bool()> inScheduledTurn;
  std::function<void(const std::string& line)> log;
};

class HostedLoops {
 public:
  struct Created {
    bool ok = false;
    std::string id, err;
    bool approved = false;
  };

  void begin(const std::string& dir, HostedLoopHooks hooks) {
    path_ = dir + "/loops.json";
    modePath_ = dir + "/wakeup_mode";
    h_ = std::move(hooks);
    std::string blob;
    if (fsutil::readFile(path_, blob)) nimbus::orch::loadLoops(blob, loops_);
    std::string mode;
    askFirst_ = fsutil::readFile(modePath_, mode) && !mode.empty() && mode[0] == '1';
    // A deadline in the past fires on the next tick only for a one-shot (its moment
    // has come); a recurring routine takes its next slot, with no burst after a restart.
    const uint64_t now = epoch();
    for (auto& l : loops_) {
      const bool oneShotKept = l.sched.kind == nimbus::orch::SchedKind::Once && l.nextRun != 0;
      if (!oneShotKept) l.nextRun = nextRunFor(l, now);
    }
    dev_.dayNumber = localDay(now);
    dev_.windowStart = now;
  }

  // At most one due fire per sweep. `busy` marks the engine thread around a fire.
  void tick(bool turnInFlight, const std::function<void(bool)>& busy) {
    const uint64_t now = epoch();
    if (lastTick_ && now - lastTick_ < tickSec_) return;
    lastTick_ = now;
    if (turnInFlight || loops_.empty()) return;
    nimbus::orch::LoopFireRequest req;
    uint64_t prevLastRun = 0;
    if (!selectAndClaim(now, req, prevLastRun)) return;
    if (busy) busy(true);
    const nimbus::orch::FireOutcome o = h_.fire ? h_.fire(req) : nimbus::orch::FireOutcome{};
    if (busy) busy(false);
    settle(req.id, o, now, prevLastRun);
  }
  // The owner moved the time zone (device onTzChanged): wall-clock schedules take the
  // next occurrence on the new clock, a due fire keeps its slot, and the daily counters
  // adopt the new day with no change in either direction.
  void onTzChanged() {
    const uint64_t now = epoch();
    const uint32_t day = localDay(now);
    for (auto& l : loops_) {
      if (l.sched.kind != nimbus::orch::SchedKind::Interval && l.sched.kind != nimbus::orch::SchedKind::Once &&
          !(l.nextRun != 0 && l.nextRun <= now))
        l.nextRun = nextRunFor(l, now);
      nimbus::orch::adoptLocalDay(l, day);
    }
    dev_.dayNumber = day;
    persist();
  }

  Created create(const std::string& name, const std::string& prompt, const std::string& chatId,
                 ArduinoJson::JsonObjectConst schedule, bool byAgent, bool inScheduled) {
    Created r;
    nimbus::orch::SchedSpec spec;
    std::string perr;
    if (!nimbus::orch::parseSpec(schedule, caps_, spec, perr)) { r.err = perr; return r; }
    r.err = createRefusal(spec, prompt, byAgent && inScheduled);
    if (!r.err.empty()) return r;
    nimbus::orch::LoopRecord l;
    l.id = genId();
    l.name = name.substr(0, nimbus::orch::kLoopNameMax);
    l.prompt = prompt.substr(0, nimbus::orch::kLoopPromptMax);
    const std::string origin = byAgent && h_.originChat ? h_.originChat() : std::string();
    l.chatId = nimbus::orch::originChat(chatId, origin);
    l.sched = spec;
    l.createdBy = byAgent ? nimbus::orch::CreatedBy::Agent : nimbus::orch::CreatedBy::Owner;
    l.enabled = true;
    // A turn that read outside content (a web page, a transcribed photo) does not arm
    // anything by itself: its wake-up waits for the owner, as "ask me first" does.
    l.approved = nimbus::orch::autoApproved(spec.kind, byAgent, askFirst_ || agent::skillTurnTainted());
    l.nextRun = nextRunFor(l, epoch());
    loops_.push_back(l);
    persist();
    if (byAgent && !l.approved && h_.alert)
      h_.alert("The assistant wants to schedule '" + l.name + "'. Approve it under Assistant > Routines, or " +
               "ignore this and it never runs.");
    r.ok = true;
    r.id = l.id;
    r.approved = l.approved;
    return r;
  }
  bool approve(const std::string& id) {
    nimbus::orch::LoopRecord* l = find(id);
    if (!l) return false;
    l->approved = true;
    persist();
    return true;
  }
  bool setEnabled(const std::string& id, bool on) {
    nimbus::orch::LoopRecord* l = find(id);
    if (!l) return false;
    l->enabled = on;
    if (on) {
      l->consecFails = 0;
      l->tokensToday = 0;
      l->pauseReason = nimbus::orch::PauseReason::None;
    } else {
      l->pauseReason = nimbus::orch::PauseReason::Owner;
    }
    persist();
    return true;
  }
  bool cancel(const std::string& id) {
    for (auto it = loops_.begin(); it != loops_.end(); ++it)
      if (it->id == id) {
        loops_.erase(it);
        persist();
        return true;
      }
    return false;
  }
  void setAskFirst(bool on) {
    askFirst_ = on;
    fsutil::writeFileAtomic(modePath_, on ? "1" : "0");
  }
  size_t count() const { return loops_.size(); }
  std::string wakeupsJson() const { return nimbus::orch::dumpWakeupsApi(loops_, askFirst_, epoch()); }

  // GET /api/loops (device loopsJson, field for field).
  std::string json() const {
    const uint64_t now = epoch();
    JsonDocument d;
    JsonArray arr = d.to<JsonArray>();
    static const char* kSched[] = {"interval", "daily", "weekly", "once"};
    static const char* kRes[] = {"none", "ok", "fail", "skipped", "paused"};
    for (const auto& l : loops_) {
      JsonObject o = arr.add<JsonObject>();
      o["id"] = l.id;
      o["name"] = l.name;
      o["prompt"] = l.prompt;
      o["chatId"] = l.chatId;
      o["kind"] = kSched[(int)l.sched.kind % 4];
      fillSchedule(o, l);
      o["byAgent"] = l.createdBy == nimbus::orch::CreatedBy::Agent;
      o["reserved"] = false;
      o["enabled"] = l.enabled;
      o["approved"] = l.approved;
      o["nextRun"] = l.nextRun;
      o["lastRun"] = l.lastRun;
      o["lastResult"] = kRes[(int)l.lastResult % 5];
      o["firesToday"] = l.firesToday;
      o["tokensToday"] = l.tokensToday;
      o["consecFails"] = l.consecFails;
      if (!l.lastErr.empty()) o["lastErr"] = l.lastErr;
      const nimbus::orch::FireDecision why = nimbus::orch::evaluate(l, dev_, caps_, now, true, true);
      o["why"] = l.enabled ? nimbus::orch::fireReasonText(why) : nimbus::orch::pauseReasonText(l.pauseReason);
      o["whyCode"] = l.enabled ? nimbus::orch::fireReasonSlug(why) : nimbus::orch::pauseReasonSlug(l.pauseReason);
    }
    std::string out;
    serializeJson(d, out);
    return out;
  }

  // wakeup.set and loop.* : the device's four tools, in its words (memory_subsystem.cpp).
  void registerTools(nimbus::orch::ToolRegistry& reg);

 private:
  // Why this routine may not be created ("" = it may).
  std::string createRefusal(const nimbus::orch::SchedSpec& spec, const std::string& prompt, bool agentInScheduled) const {
    const bool once = spec.kind == nimbus::orch::SchedKind::Once;
    if (agentInScheduled && !once) return "a scheduled loop cannot create loops";
    if (once && armedWakeups() >= nimbus::orch::kWakeupMaxPending)
      return "you already have " + std::to_string(nimbus::orch::kWakeupMaxPending) +
             " wakeups armed - cancel one (loop.cancel) or let one fire first";
    if ((int)loops_.size() >= caps_.maxCount) return "too many loops (max " + std::to_string(caps_.maxCount) + ")";
    return prompt.empty() ? std::string("prompt is required") : std::string();
  }
  static uint64_t epoch() { return (uint64_t)time(nullptr); }
  static nimbus::orch::CivilTime civilLocal(uint64_t e) {
    time_t t = (time_t)e;
    struct tm lt{};
    {
      std::lock_guard<std::mutex> lk(hosted::tzMutex());
      localtime_r(&t, &lt);
    }
    nimbus::orch::CivilTime c;
    c.year = lt.tm_year + 1900;
    c.month = lt.tm_mon + 1;
    c.day = lt.tm_mday;
    c.hour = lt.tm_hour;
    c.min = lt.tm_min;
    c.sec = lt.tm_sec;
    c.wday = lt.tm_wday;
    return c;
  }
  static uint64_t civilEpoch(const nimbus::orch::CivilTime& c) {
    struct tm lt{};
    lt.tm_year = c.year - 1900;
    lt.tm_mon = c.month - 1;
    lt.tm_mday = c.day;
    lt.tm_hour = c.hour;
    lt.tm_min = c.min;
    lt.tm_sec = c.sec;
    lt.tm_isdst = -1;
    std::lock_guard<std::mutex> lk(hosted::tzMutex());
    return (uint64_t)mktime(&lt);
  }
  static uint32_t localDay(uint64_t e) {
    const nimbus::orch::CivilTime c = civilLocal(e);
    return nimbus::orch::daysFromCivil(c.year, (unsigned)c.month, (unsigned)c.day);
  }
  static uint64_t nextRunFor(const nimbus::orch::LoopRecord& l, uint64_t now) {
    if (l.sched.kind == nimbus::orch::SchedKind::Interval || l.sched.kind == nimbus::orch::SchedKind::Once)
      return nimbus::orch::nextIntervalEpoch(l.sched, now);
    return civilEpoch(nimbus::orch::nextWallClockLocal(l.sched, civilLocal(now)));
  }
  static void fillSchedule(JsonObject o, const nimbus::orch::LoopRecord& l) {
    if (l.sched.kind == nimbus::orch::SchedKind::Interval || l.sched.kind == nimbus::orch::SchedKind::Once) {
      o["everySec"] = l.sched.intervalSec;
      return;
    }
    char at[6];
    snprintf(at, sizeof at, "%02u:%02u", (unsigned)(l.sched.minuteOfDay / 60) % 24, (unsigned)l.sched.minuteOfDay % 60);
    o["at"] = at;
    o["weekMask"] = l.sched.weekMask;
  }
  std::string genId() {
    static std::mt19937 rng{std::random_device{}()};
    for (;;) {
      char b[10];
      snprintf(b, sizeof b, "lp%06x", (unsigned)(rng() & 0xFFFFFF));
      if (!find(b)) return b;
    }
  }
  int armedWakeups() const {
    int n = 0;
    for (const auto& l : loops_)
      if (l.sched.kind == nimbus::orch::SchedKind::Once && l.enabled) n++;
    return n;
  }
  nimbus::orch::LoopRecord* find(const std::string& id) {
    for (auto& l : loops_)
      if (l.id == id) return &l;
    return nullptr;
  }
  bool persist() {
    if (fsutil::writeFileAtomic(path_, nimbus::orch::dumpLoops(loops_))) return true;
    say("loops: routines not saved - keeping the previous copy");
    return false;
  }
  void say(const std::string& line) const {
    if (h_.log) h_.log(line);
  }
  nimbus::orch::Delivery deliveryFor(const nimbus::orch::LoopRecord& l, const nimbus::orch::DeliveryWorld& w) const {
    const bool tg = nimbus::orch::deliveryChannelOf(l.chatId) == nimbus::orch::DeliveryChannel::Telegram;
    const bool allowed = !tg || !h_.chatAllowed || h_.chatAllowed(l.chatId);
    return nimbus::orch::resolveDelivery(l.chatId, w, allowed);
  }
  void pause(nimbus::orch::LoopRecord& l, nimbus::orch::PauseReason why, const std::string& alertText) {
    if (!l.enabled) return;
    l.enabled = false;
    l.lastResult = nimbus::orch::LastResult::Paused;
    l.pauseReason = why;
    if (h_.alert) h_.alert(alertText);
    persist();
  }
  // The decision for one loop this sweep: true when it may fire now.
  bool considerDue(nimbus::orch::LoopRecord& l, const nimbus::orch::DeliveryWorld& w, uint64_t now, bool& ceiling) {
    using nimbus::orch::FireDecision;
    const bool deliverable = deliveryFor(l, w).verdict != nimbus::orch::DeliveryVerdict::Revoked;
    switch (nimbus::orch::evaluate(l, dev_, caps_, now, true, deliverable)) {
      case FireDecision::Fire:
        return true;
      case FireDecision::BlockedConsecFails:
        if (nimbus::orch::breakerTrips(l, caps_.maxConsecFails, false))
          pause(l, nimbus::orch::PauseReason::ConsecFails, "Routine '" + l.name + "' is paused after repeated failures.");
        break;
      case FireDecision::BlockedLoopTokens:
        pause(l, nimbus::orch::PauseReason::LoopTokens, "Routine '" + l.name + "' is paused: it hit its daily token limit.");
        break;
      case FireDecision::BlockedChatRevoked:
        pause(l, nimbus::orch::PauseReason::ChatRevoked,
              "Routine '" + l.name + "' is paused: its chat can no longer receive it.");
        break;
      case FireDecision::BlockedDeviceTokens:
        ceiling = true;
        break;
      default:
        break;
    }
    return false;
  }
  // Pick at most one due loop, advance it and store that BEFORE the fire: a fire whose
  // advance did not land never runs, so a restart can never fire one occurrence twice.
  bool selectAndClaim(uint64_t now, nimbus::orch::LoopFireRequest& req, uint64_t& prevLastRun) {
    const uint32_t day = localDay(now);
    const nimbus::orch::DeliveryWorld world = h_.world ? h_.world() : nimbus::orch::DeliveryWorld{};
    for (auto& l : loops_) nimbus::orch::rollDayIfNeeded(l, day, true);
    nimbus::orch::rollDeviceDayIfNeeded(dev_, caps_, day, now, true);
    nimbus::orch::LoopRecord* due = nullptr;
    bool ceiling = false;
    for (auto& l : loops_)
      if (considerDue(l, world, now, ceiling) && (!due || l.nextRun < due->nextRun)) due = &l;
    if (ceiling) {
      if (day != ceilingAlertDay_ && h_.alert) {
        ceilingAlertDay_ = day;
        h_.alert("Routines are paused until tomorrow: the daily token ceiling for routines is reached.");
      }
      return false;
    }
    if (!due) return false;
    const nimbus::orch::Delivery dl = deliveryFor(*due, world);
    req = nimbus::orch::buildFireRequest(*due, dl, now);
    // The shared rule names the device's web chat; a hosted page chats as "owner".
    if (nimbus::orch::deliveryChannelOf(req.chatId) == nimbus::orch::DeliveryChannel::Web)
      req.chatId = nimbus::orch::kHostedWebChatId;
    prevLastRun = due->lastRun;
    const nimbus::orch::FireClaim before = nimbus::orch::claimSnapshot(*due, dev_);
    nimbus::orch::claimFire(*due, dev_, now, nextRunFor(*due, now));
    if (persist()) return true;
    nimbus::orch::undoClaim(*due, dev_, before);
    say("loops: '" + due->name + "' not fired - its schedule could not be saved, retrying next tick");
    return false;
  }
  void settleDeferred(nimbus::orch::LoopRecord& l, const nimbus::orch::FireOutcome& o, uint64_t now, uint64_t prevLastRun) {
    if (l.firesToday > 0) l.firesToday--;
    if (dev_.firesInWindow > 0) dev_.firesInWindow--;
    l.lastRun = prevLastRun;
    if (nimbus::orch::isSpendFrozenError(o.error)) {
      if (nimbus::orch::onCeilingSkip(l, now)) say("loops: '" + l.name + "' skipped - paid calls are paused");
    } else if (o.retryAfterSec) {
      l.nextRun = now + o.retryAfterSec;
    }
    persist();
  }
  // A one-shot that fired: gone, or one short retry. True when the record was handled.
  bool settleOnce(nimbus::orch::LoopRecord& l, const nimbus::orch::FireOutcome& o, uint64_t now) {
    switch (nimbus::orch::onceAfterFire(l, o)) {
      case nimbus::orch::OnceAfterFire::RetryShort:
        l.nextRun = now + nimbus::orch::kWakeupRetrySec;
        persist();
        return true;
      case nimbus::orch::OnceAfterFire::Retire: {
        const std::string answered = nimbus::orch::answeredFailureAlert(l);
        const std::string id = l.id;
        cancel(id);
        if (!o.ok && h_.alert)
          h_.alert(o.answered ? answered : std::string("A wakeup the assistant set for itself failed twice and was dropped."));
        return true;
      }
      case nimbus::orch::OnceAfterFire::NotOnce:
        return false;
    }
    return false;
  }
  void settle(const std::string& id, const nimbus::orch::FireOutcome& o, uint64_t now, uint64_t prevLastRun) {
    nimbus::orch::LoopRecord* l = find(id);   // the turn may have changed the table
    if (!l) return;
    if (!nimbus::orch::fireWasAttempted(o)) {
      settleDeferred(*l, o, now, prevLastRun);
      return;
    }
    nimbus::orch::onFireResult(*l, o.ok, o.tokens, nimbus::orch::fnv64(o.detail), now);
    nimbus::orch::noteFireError(*l, o.ok, o.error);
    dev_.tokensToday += o.tokens.total();
    say("loops: fired '" + l->name + "' ok=" + (o.ok ? "1" : "0") + " tokens=" + std::to_string(o.tokens.total()) +
        " next=" + std::to_string(l->nextRun));
    if (settleOnce(*l, o, now)) return;
    if (nimbus::orch::retriesSoon(o)) l->nextRun = now + o.retryAfterSec;
    if (nimbus::orch::isSemanticRepeat(*l, caps_.maxRepeats))
      pause(*l, nimbus::orch::PauseReason::Repeated, "Routine '" + l->name + "' is paused: it kept returning the same result.");
    else if (!o.ok && nimbus::orch::breakerTrips(*l, caps_.maxConsecFails, false))
      pause(*l, nimbus::orch::PauseReason::ConsecFails, "Routine '" + l->name + "' is paused after repeated failures.");
    persist();
  }

  std::vector<nimbus::orch::LoopRecord> loops_;
  nimbus::orch::DeviceCounters dev_;
  nimbus::orch::LoopCaps caps_;
  HostedLoopHooks h_;
  std::string path_, modePath_;
  bool askFirst_ = false;
  uint32_t ceilingAlertDay_ = 0;
  uint64_t lastTick_ = 0;
  static constexpr uint32_t tickSec_ = 20;   // the device's sweep
};

inline void HostedLoops::registerTools(nimbus::orch::ToolRegistry& reg) {
  using nimbus::orch::Principal;
  using nimbus::orch::ToolResult;
  reg.add("wakeup.set",
          "Schedule ONE future wakeup for yourself - no owner approval needed. In `minutes` from now "
          "(2 min .. 7 days) you get a single automatic turn carrying your `note`, then the wakeup is gone. "
          "Use it to follow up on something you started ('check whether the sub-agent's report landed'), not "
          "for recurring work (that is loop.create, which the owner approves). You may set another when it "
          "fires; at most 4 armed at once, and wakeup turns spend the same daily budget as routines. Cancel "
          "with loop.cancel.",
          [this](ArduinoJson::JsonObjectConst a, const Principal& who) -> ToolResult {
            if (!who.perms().manageTenants) return ToolResult::fail("only an admin's conversation can arm wakeups");
            const long minutes = a["minutes"] | 0l;
            const char* note = a["note"] | "";
            if (!note[0]) return ToolResult::fail("note is required - it is the context you get back on wakeup");
            JsonDocument sd;
            sd["kind"] = "once";
            sd["in_seconds"] = minutes * 60;
            const Created r = create("wakeup", note, "", sd.as<ArduinoJson::JsonObjectConst>(), true, h_.inScheduledTurn && h_.inScheduledTurn());
            if (!r.ok) {
              if (r.err.find("in_seconds") != std::string::npos)
                return ToolResult::fail("minutes must be between 2 and 10080 (7 days)");
              return ToolResult::fail(r.err);
            }
            if (!r.approved)
              return ToolResult::ok("wakeup staged (id " + r.id + "), pending the owner's approval. It will NOT fire "
                                    "until they approve it; tell the owner you have requested a follow-up in ~" +
                                    std::to_string(minutes) + " min and are waiting on their approval.");
            return ToolResult::ok("wakeup armed (id " + r.id + "): in ~" + std::to_string(minutes) +
                                  " min you get one automatic turn with your note. It fires once, then it's gone - "
                                  "do not tell the owner you are 'monitoring' continuously; say you'll check back at "
                                  "that time.");
          },
          R"({"type":"object","properties":{"minutes":{"type":"integer"},"note":{"type":"string"}},"required":["minutes","note"]})");
  reg.add("loop.create",
          "Schedule a recurring or timed task ('local loop') that fires as a FUTURE orchestrator turn. Use for "
          "RECURRING work: reminders, digests, nightly upkeep. For a one-time follow-up for yourself use "
          "wakeup.set instead (no approval needed). schedule is either {kind:'interval', every_seconds:N} (>=300) "
          "or {kind:'daily'|'weekly', at:'HH:MM' 24h, days:['mon','wed',...] for weekly}. chat_id (optional) must "
          "be an allow-listed chat. A loop YOU create is saved but stays PENDING until the owner approves it, then "
          "it fires on schedule.",
          [this](ArduinoJson::JsonObjectConst a, const Principal& who) -> ToolResult {
            if (!who.perms().manageTenants) return ToolResult::fail("only an admin can set up this instance's routines");
            const std::string name = a["name"] | "";
            const Created r = create(name, a["prompt"] | "", a["chat_id"] | "", a["schedule"].as<ArduinoJson::JsonObjectConst>(), true,
                                     h_.inScheduledTurn && h_.inScheduledTurn());
            if (!r.ok) return ToolResult::fail(r.err);
            return ToolResult::ok("loop '" + name + "' created (id " + r.id +
                                  ") - PENDING the owner's approval before it can fire.");
          },
          R"({"type":"object","properties":{"name":{"type":"string"},"prompt":{"type":"string"},"chat_id":{"type":"string"},"schedule":{"type":"object","properties":{"kind":{"type":"string","enum":["interval","daily","weekly"]},"every_seconds":{"type":"integer"},"at":{"type":"string"},"days":{"type":"array","items":{"type":"string"}}}}},"required":["name","prompt","schedule"]})");
  reg.add("loop.list",
          "List all scheduled loops with status: schedule, next run, last result, enabled, and whether it's approved.",
          [this](ArduinoJson::JsonObjectConst, const Principal& who) -> ToolResult {
            if (!who.perms().manageTenants) return ToolResult::fail("only an admin can see this instance's routines");
            return ToolResult::ok(json());
          },
          R"({"type":"object","properties":{}})");
  reg.add("loop.cancel", "Cancel (delete) a scheduled loop by its id.",
          [this](ArduinoJson::JsonObjectConst a, const Principal& who) -> ToolResult {
            if (!who.perms().manageTenants) return ToolResult::fail("only an admin can change this instance's routines");
            const std::string id = a["id"] | "";
            if (id.empty()) return ToolResult::fail("missing 'id'");
            return cancel(id) ? ToolResult::ok("loop cancelled") : ToolResult::fail("no loop with that id");
          },
          R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})");
}

}  // namespace nimbusd
