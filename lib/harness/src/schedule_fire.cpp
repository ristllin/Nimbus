// schedule_fire.cpp - the fires of engine 2's schedule entries (lane RB-U1b;
// nimbus/harness/schedule_fire.h): a reminder's fixed text through the routine delivery, a routine
// through runRoutine with its state file and its outside mark. Engine 2 only. No model text here:
// the lines it records are the tool's (orch_schedule_tool.cpp, schedule::words).
#include "nimbus/harness/schedule_fire.h"

#include <algorithm>

namespace nimbus {
namespace orch {
namespace schedule {

namespace {

using namespace store;
using namespace words;

// A fire that could not run now is tried again this soon (a deferred fire keeps its advanced slot
// otherwise: a reminder's is its whole delay).
constexpr uint32_t kRetrySec = kWakeupRetrySec;

// ---- fires --------------------------------------------------------------------------------------
bool readWhole(tx::TxFiles& f, const std::string& path, std::string& out) {
  out.clear();
  size_t n = 0;
  if (!f.size(path, n)) return false;
  out.resize(n);
  size_t got = 0;
  if (n && (!f.readAt(path, 0, &out[0], n, got) || got != n)) return false;
  return true;
}

bool hasOutside(const plain::FireReport& r) {
  const auto& ev = r.outcome.end.events;
  return std::find(ev.begin(), ev.end(), kOutsideEvent) != ev.end();
}

TokenUsage tokensOf(const plain::FireReport& r) {
  TokenUsage t;
  t.promptTokens = r.outcome.end.promptTokens;
  t.completionTokens = r.outcome.end.outputTokens;
  t.cacheReadTokens = r.outcome.end.cacheReadTokens;
  t.cacheWriteTokens = r.outcome.end.cacheWriteTokens;
  return t;
}

std::string statusText(const plain::Status& st) { return std::string(tx::errSlug(st.err)) + ' ' + st.where; }

// The entry's outside marks after a fire: this fire's output when the fire read outside content or
// opened tainted (its input held such an output), and only the marks whose output is still in the
// state. True when they changed.
bool markOutside(Extra& e, const std::vector<plain::RoutineOutput>& next, bool outside, const plain::RoutineOutput& out) {
  std::vector<uint64_t> marks = e.outsideAt;
  if (outside && !out.text.empty()) marks.push_back(out.at);
  std::vector<uint64_t> kept;
  for (uint64_t at : marks) {
    const bool inState = std::any_of(next.begin(), next.end(), [at](const plain::RoutineOutput& o) { return o.at == at; });
    if (inState && std::find(kept.begin(), kept.end(), at) == kept.end()) kept.push_back(at);
  }
  if (kept == e.outsideAt) return false;
  e.outsideAt = kept;
  return true;
}

enum class Found : uint8_t { Yes, NoRecord, Unreadable };

// The record and the sidecar as they are now (a copy). An unreadable sidecar is never guessed past:
// a reminder would read as a routine.
Found snapshot(Host& host, const std::string& id, LoopRecord& rec, std::vector<Extra>& extras) {
  Found f = Found::NoRecord;
  host.withTable([&](std::vector<LoopRecord>& t) {
    for (const LoopRecord& l : t)
      if (l.id == id) rec = l, f = Found::Yes;
    std::string side;
    if (f == Found::Yes && (!host.readSidecar(side) || !loadSidecar(side, extras))) f = Found::Unreadable;
  });
  return f;
}

// A fire that cannot run now: not counted, tried again soon (Skipped with its reason on the record
// when `mark` is set).
FireOutcome notNow(Host& host, const std::string& id, const char* why, bool mark) {
  if (mark)
    host.withTable([&](std::vector<LoopRecord>& t) {
      for (LoopRecord& l : t)
        if (l.id == id) l.lastResult = LastResult::Skipped, l.lastErr = why;
      (void)host.saveLoops(t);
    });
  FireOutcome o;
  o.deferred = true;
  o.retryAfterSec = kRetrySec;
  o.error = why;
  return o;
}

// The outside marks against the state the fire leaves; false when they could not be stored.
bool storeMarks(Host& host, const std::string& id, const std::vector<plain::RoutineOutput>& next, bool outside,
                const plain::RoutineOutput& out) {
  bool ok = false;
  host.withTable([&](std::vector<LoopRecord>& t) {
    std::string side;
    std::vector<Extra> extras;
    if (!host.readSidecar(side) || !loadSidecar(side, extras)) return;
    auto it = std::find_if(extras.begin(), extras.end(), [&](const Extra& e) { return e.id == id; });
    if (it == extras.end()) {
      Extra e;
      e.id = id;
      extras.push_back(e);
      it = extras.end() - 1;
    }
    ok = !markOutside(*it, next, outside, out) || host.writeSidecar(dumpSidecar(prunedSidecar(extras, t)));
  });
  return ok;
}

FireOutcome outcomeOf(const plain::Status& st, const plain::FireReport& rep, uint64_t now) {
  const bool held = rep.result == plain::FireResult::Held;   // in the outbox: it goes when it can
  FireOutcome o;
  o.ok = st.ok() && rep.result != plain::FireResult::Failed && (rep.status.ok() || held);
  o.answered = rep.result == plain::FireResult::Sent || held;
  // A silent fire is not "the same result again" (the repeat breaker hashes this).
  o.detail = rep.output.text.empty() ? std::to_string(now) : rep.output.text;
  o.tokens = tokensOf(rep);
  if (!o.ok) o.error = statusText(st.ok() ? rep.status : st);
  return o;
}

FireOutcome fireRoutine(Host& host, const LoopRecord& rec, const Extra* e, const FireBinding& b, uint64_t now) {
  std::vector<plain::RoutineOutput> last;
  if (!b.files || !b.run || !loadState(*b.files, rec.id, last)) return notNow(host, rec.id, kNoCard, true);
  const bool tainted = opensTainted(e, last);
  const plain::RoutineFire f = routineFire(rec, e, last, now, b.grants);
  plain::FireReport rep;
  const plain::Status st = b.run(f, tainted, rep);
  const FireOutcome o = outcomeOf(st, rep, now);
  if (!st.ok() || rep.result == plain::FireResult::Failed) return o;
  plain::nextRoutineState(last, rep.output);
  // The mark first: an output from outside content never sits in the state unmarked.
  const bool outside = tainted || hasOutside(rep);
  if (storeMarks(host, rec.id, last, outside, rep.output) || !outside) (void)saveState(*b.files, rec.id, last);
  return o;
}

}  // namespace

// ---- state --------------------------------------------------------------------------------------

bool loadState(tx::TxFiles& files, const std::string& id, std::vector<plain::RoutineOutput>& out) {
  out.clear();
  const std::string path = statePath(id), tmp = path + kTmpExt;
  size_t n = 0;
  std::string text;
  if (files.size(path, n)) {
    if (!readWhole(files, path, text)) return false;
  } else if (files.size(tmp, n)) {
    if (!readWhole(files, tmp, text)) return false;   // its rename was stopped: it is the newer, whole
  }
  out = plain::decodeRoutineState(text);
  return true;
}

bool saveState(tx::TxFiles& files, const std::string& id, const std::vector<plain::RoutineOutput>& last) {
  const std::string path = statePath(id), tmp = path + kTmpExt;
  if (!files.writeWhole(tmp, plain::encodeRoutineState(last))) return false;
  return files.rename(tmp, path);
}

// ---- fires --------------------------------------------------------------------------------------
plain::RoutineFire routineFire(const LoopRecord& l, const Extra* e, const std::vector<plain::RoutineOutput>& last,
                               uint64_t fireAt, bool grants) {
  plain::RoutineFire f;
  f.def.id = l.id;
  f.def.name = l.name;
  f.def.instruction = l.prompt;
  if (e) {
    f.def.tools = e->tools;
    f.def.paths = e->paths;
    f.def.holdS = e->holdS;
  }
  f.fireAt = fireAt;
  f.last = last;
  f.grants = grants;
  return f;
}

bool opensTainted(const Extra* e, const std::vector<plain::RoutineOutput>& last) {
  if (!e) return false;
  for (const plain::RoutineOutput& o : last)
    if (std::find(e->outsideAt.begin(), e->outsideAt.end(), o.at) != e->outsideAt.end()) return true;
  return false;
}

FireOutcome fireReminder(const FireBinding& b, const LoopRecord& l, const std::string& chatId, uint64_t now) {
  plain::HeldStore once(plain::HeldFile{[] { return std::string(); }, [](const std::string&) { return true; }});
  plain::HeldNote n;
  n.id = l.id + '-' + std::to_string(now);
  n.routine = l.id;
  n.name = l.name;
  n.chatId = chatId;
  n.at = now;
  n.expires = now + plain::kHeldNoteExpiryS;
  n.text = l.prompt;
  n.state = plain::HeldState::Ready;
  FireOutcome o;
  o.detail = l.prompt;
  plain::Status st = once.put(n);
  if (st.ok()) st = plain::deliverHeld(once, b.owner, n.id, now, b.tzMinutes, b.dayStartMin);
  const plain::HeldNote* left = once.find(n.id);
  o.ok = st.ok();
  o.answered = st.ok() || (left && left->state == plain::HeldState::Sent);
  if (!o.ok) o.error = statusText(st);
  return o;
}

bool entryKind(Host& host, const std::string& id, Kind& out) {
  LoopRecord rec;
  std::vector<Extra> extras;
  if (snapshot(host, id, rec, extras) != Found::Yes) return false;
  out = kindOf(rec, findExtra(extras, rec.id));
  return true;
}

FireOutcome fire(Host& host, const LoopFireRequest& req, const FireBinding& b, uint64_t now) {
  LoopRecord rec;
  std::vector<Extra> extras;
  const Found found = snapshot(host, req.id, rec, extras);
  if (found == Found::Unreadable) return notNow(host, req.id, kUnreadable, false);
  if (found == Found::NoRecord) {
    FireOutcome o;
    o.error = kNoEntry + req.id;
    return o;
  }
  const Extra* e = findExtra(extras, rec.id);
  if (kindOf(rec, e) == Kind::Reminder) return fireReminder(b, rec, req.chatId.empty() ? rec.chatId : req.chatId, now);
  return fireRoutine(host, rec, e, b, now);
}

}  // namespace schedule
}  // namespace orch
}  // namespace nimbus
