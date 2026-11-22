// orch_schedule_tool.cpp - engine 2's `schedule` tool, the loops2 sidecar codec and the fires
// (lane RB-U1b; schedule_tool.h). Engine 2 only (tools/engine/engine2_only.txt). Every string
// literal in this file is model text (gen_model_text_catalog.py E2_EXTRA_SOURCES, tool_results):
// keys, values and result lines alike, so formatting uses char literals and digits by hand.
#include "nimbus/orch/schedule_tool.h"

#include <algorithm>
#include <cstring>

#include "nimbus/utf8.h"

namespace nimbus {
namespace orch {
namespace schedule {

namespace {

// ---- the words the model reads and writes ---------------------------------------------------
constexpr char kOp[] = "op", kKind[] = "kind", kId[] = "id", kWhen[] = "when", kText[] = "text", kEvery[] = "every";
constexpr char kCreate[] = "create", kUpdate[] = "update", kDelete[] = "delete", kList[] = "list";
constexpr char kReminder[] = "reminder", kRoutine[] = "routine";
constexpr char kDays[] = "sunmontuewedthufrisat";   // three letters a day, bit 0 = Sunday
constexpr char kDaily[] = "daily", kWeekly[] = "weekly", kMin[] = "min", kHour[] = "h";
using namespace store;   // the sidecar keys and file names (schedule_tool.h; no model reads them)

// results
constexpr char kStored[] = "stored ", kUpdated[] = "updated ", kDeleted[] = "deleted ", kNoEntries[] = "no entries";
constexpr char kWaiting[] = " (waiting for approval)", kPaused[] = " (paused)";
// errors
constexpr char kBadOp[] = "op must be create, update, delete or list";
constexpr char kBadKind[] = "kind must be reminder or routine";
constexpr char kNoText[] = "text is empty";
constexpr char kOverA[] = "text is over the ", kOverB[] = " B limit";
constexpr char kBadWhen[] = "when must read YYYY-MM-DD HH:MM";
constexpr char kWhenRange[] = "when must be 2 min to 7 days ahead";
constexpr char kBadEvery[] = "every must read daily HH:MM, weekly mon,thu HH:MM or every N min|h (5 min or more)";
constexpr char kWrongKey[] = "a reminder takes when, a routine takes every";
constexpr char kNoEntry[] = "no entry ";
constexpr char kFull[] = "full: ", kFullB[] = " entries";
constexpr char kNoClock[] = "the clock is not set";
constexpr char kNotHere[] = "a turn no person started may list or set reminders only";
constexpr char kNotStored[] = "not stored: the write was refused";
constexpr char kNoId[] = "id is empty";
constexpr char kNothing[] = "nothing to change";
constexpr char kKindDiffers[] = "kind differs from the entry";
constexpr char kNoCard[] = "no card";

const char* kindSlug(Kind k) { return k == Kind::Reminder ? kReminder : kRoutine; }

std::string str(JsonObjectConst a, const char* key) {
  const char* v = a[key] | "";
  return v;
}

void two(std::string& s, int n) {
  s += (char)('0' + n / 10 % 10);
  s += (char)('0' + n % 10);
}

// "HH:MM" from a minute of the day.
std::string hhmm(uint16_t minute) {
  std::string s;
  two(s, minute / 60);
  s += ':';
  two(s, minute % 60);
  return s;
}

// Reads exactly `n` digits at `p` (advanced past them); false otherwise.
bool digits(const std::string& s, size_t& p, size_t n, int& out) {
  out = 0;
  for (size_t i = 0; i < n; ++i, ++p) {
    if (p >= s.size() || s[p] < '0' || s[p] > '9') return false;
    out = out * 10 + (s[p] - '0');
  }
  return true;
}

// "HH:MM" from `p` to the end of `s`.
bool parseHhmm(const std::string& s, size_t p, uint16_t& minute) {
  int h = 0, m = 0;
  if (!digits(s, p, 2, h) || p >= s.size() || s[p++] != ':' || !digits(s, p, 2, m) || p != s.size()) return false;
  if (h > 23 || m > 59) return false;
  minute = (uint16_t)(h * 60 + m);
  return true;
}

bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
int monthDays(int y, int m) {
  static const uint8_t k[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && leap(y) ? 29 : k[m - 1];
}

// "mon,thu" -> the week mask; 0 on any other word.
uint8_t dayMask(const std::string& list) {
  uint8_t mask = 0;
  size_t p = 0;
  while (p <= list.size()) {
    const size_t comma = std::min(list.find(',', p), list.size());
    const std::string day = list.substr(p, comma - p);
    int bit = -1;
    for (int d = 0; d < 7 && day.size() == 3; ++d)
      if (day.compare(0, 3, kDays + d * 3, 3) == 0) bit = d;
    if (bit < 0) return 0;
    mask |= (uint8_t)(1u << bit);
    p = comma + 1;
  }
  return mask;
}

bool parseInterval(const std::string& rest, uint32_t minIntervalSec, SchedSpec& out) {
  size_t p = 0;
  uint32_t n = 0;
  while (p < rest.size() && rest[p] >= '0' && rest[p] <= '9' && n < 100000) n = n * 10 + (uint32_t)(rest[p++] - '0');
  if (p == 0 || p >= rest.size() || rest[p] != ' ') return false;
  const std::string unit = rest.substr(p + 1);
  const uint32_t mult = unit == kMin ? 60u : unit == kHour ? 3600u : 0u;
  if (!mult || n == 0 || n * mult < minIntervalSec) return false;
  out.kind = SchedKind::Interval;
  out.intervalSec = n * mult;
  return true;
}

// ---- the sidecar --------------------------------------------------------------------------------
void putStrings(JsonObject o, const char* key, const std::vector<std::string>& v) {
  if (v.empty()) return;
  JsonArray a = o[key].to<JsonArray>();
  for (const std::string& s : v) a.add(s);
}

std::vector<std::string> getStrings(JsonObjectConst o, const char* key) {
  std::vector<std::string> v;
  for (JsonVariantConst x : o[key].as<JsonArrayConst>())
    if (x.is<const char*>()) v.push_back(x.as<const char*>());
  return v;
}

bool readExtra(JsonObjectConst o, Extra& e) {
  e.id = str(o, kId);
  if (e.id.empty()) return false;
  e.kind = str(o, kKind) == kReminder ? Kind::Reminder : Kind::Routine;
  e.tools = getStrings(o, kTools);
  e.paths = getStrings(o, kPaths);
  e.holdS = o[kHoldS] | 0u;
  for (JsonVariantConst x : o[kOutsideAt].as<JsonArrayConst>()) e.outsideAt.push_back(x | 0ull);
  return true;
}

// ---- one call -----------------------------------------------------------------------------------
enum class Op : uint8_t { Create, Update, Delete, List, Bad };

Op opOf(JsonObjectConst a) {
  if (a[kOp].isNull()) return Op::Create;   // as the rails read it
  const std::string op = str(a, kOp);
  if (op == kCreate) return Op::Create;
  if (op == kUpdate) return Op::Update;
  if (op == kDelete) return Op::Delete;
  if (op == kList) return Op::List;
  return Op::Bad;
}

// The kind the call names: 0 none, 1 reminder, 2 routine, -1 a word that is neither.
int kindArg(JsonObjectConst a) {
  if (a[kKind].isNull()) return 0;
  const std::string k = str(a, kKind);
  return k == kReminder ? 1 : k == kRoutine ? 2 : -1;
}

std::string overLimit() { return std::string(kOverA) + std::to_string(kLoopPromptMax) + kOverB; }

// A text the owner sent, whole or refused ("" = it may be stored).
std::string textRefusal(const std::string& t) {
  if (t.empty()) return kNoText;
  return t.size() > (size_t)kLoopPromptMax ? overLimit() : std::string();
}

struct Call {
  Host& host;
  JsonObjectConst a;
  const CallCtx& ctx;
  std::vector<LoopRecord>* live = nullptr;
  std::vector<Extra> extras;
  ToolResult out;

  std::string whenText(const LoopRecord& l) {
    const CivilTime c = host.epochToLocal(l.nextRun);
    std::string s = std::to_string(c.year);
    s += '-';
    two(s, c.month);
    s += '-';
    two(s, c.day);
    s += ' ';
    s += hhmm((uint16_t)(c.hour * 60 + c.min));
    return s;
  }

  std::string line(const LoopRecord& l) {
    const Kind k = kindOf(l, findExtra(extras, l.id));
    std::string s = l.id + ' ' + kindSlug(k) + ' ' + (k == Kind::Reminder ? whenText(l) : everyText(l.sched));
    if (!l.approved) s += kWaiting;
    if (!l.enabled) s += kPaused;
    return s + ':' + ' ' + l.prompt;
  }

  // The reminder's moment, as a record's schedule; "" when it is set.
  std::string setWhen(LoopRecord& l, const std::string& when) {
    const Clock c = host.clock();
    if (!c.valid) return kNoClock;
    CivilTime t;
    uint64_t at = 0;
    if (!parseWhen(when, t) || !host.localToEpoch(t, at)) return kBadWhen;
    if (at < c.now + kWakeupMinSec || at > c.now + kWakeupMaxSec) return kWhenRange;
    l.sched.kind = SchedKind::Once;
    l.sched.intervalSec = (uint32_t)(at - c.now);
    l.nextRun = at;
    return std::string();
  }

  std::string setEvery(LoopRecord& l, const std::string& every) {
    SchedSpec s;
    if (!parseEvery(every, host.minIntervalSec(), s)) return kBadEvery;
    l.sched = s;
    l.nextRun = host.nextRun(l, host.clock().now);
    return std::string();
  }

  void setText(LoopRecord& l, const std::string& t) {
    l.prompt = t;
    l.name = nameFor(t);
  }

  // Sidecar first (a create or an update), then loops.json; the live table only when both landed.
  bool commit(std::vector<LoopRecord>& next) {
    if (!host.writeSidecar(dumpSidecar(prunedSidecar(extras, next))) || !host.saveLoops(next)) {
      out = ToolResult::fail(kNotStored);
      return false;
    }
    *live = next;
    return true;
  }

  LoopRecord* find(std::vector<LoopRecord>& v, const std::string& id) {
    for (LoopRecord& l : v)
      if (l.id == id && !host.reserved(id)) return &l;
    return nullptr;
  }

  void create() {
    const int k = kindArg(a);
    if (k <= 0) return void(out = ToolResult::fail(kBadKind));
    const bool reminder = k == 1;
    const std::string text = str(a, kText), refusal = textRefusal(text);
    if (!refusal.empty()) return void(out = ToolResult::fail(refusal));
    if (a[reminder ? kEvery : kWhen].is<const char*>()) return void(out = ToolResult::fail(kWrongKey));
    if ((int)live->size() >= host.maxCount())
      return void(out = ToolResult::fail(kFull + std::to_string(host.maxCount()) + kFullB));
    LoopRecord l;
    l.id = host.newId(*live);
    setText(l, text);
    l.chatId = ctx.chatId;
    l.createdBy = CreatedBy::Agent;
    const std::string bad = reminder ? setWhen(l, str(a, kWhen)) : setEvery(l, str(a, kEvery));
    if (!bad.empty()) return void(out = ToolResult::fail(bad));
    l.approved = autoApproved(l.sched.kind, true, host.askFirst());
    Extra e;
    e.id = l.id;
    e.kind = reminder ? Kind::Reminder : Kind::Routine;
    extras.push_back(e);
    std::vector<LoopRecord> next = *live;
    next.push_back(l);
    if (commit(next)) out = ToolResult::ok(kStored + line(l));
  }

  // The fields an update names, applied to `l`; "" when it may be stored.
  std::string applyUpdate(LoopRecord& l, Kind k) {
    const bool when = a[kWhen].is<const char*>(), every = a[kEvery].is<const char*>(), text = a[kText].is<const char*>();
    if (!when && !every && !text) return kNothing;
    if ((k == Kind::Reminder && every) || (k == Kind::Routine && when)) return kWrongKey;
    if (text) {
      const std::string refusal = textRefusal(str(a, kText));
      if (!refusal.empty()) return refusal;
      setText(l, str(a, kText));
    }
    const std::string bad = when ? setWhen(l, str(a, kWhen)) : every ? setEvery(l, str(a, kEvery)) : std::string();
    if (bad.empty() && k == Kind::Routine) l.approved = false;   // a changed routine waits for the owner again
    return bad;
  }

  void update() {
    const std::string id = str(a, kId);
    if (id.empty()) return void(out = ToolResult::fail(kNoId));
    std::vector<LoopRecord> next = *live;
    LoopRecord* l = find(next, id);
    if (!l) return void(out = ToolResult::fail(kNoEntry + id));
    const Kind k = kindOf(*l, findExtra(extras, id));
    const int named = kindArg(a);
    if (named < 0 || (named > 0 && (named == 1) != (k == Kind::Reminder))) return void(out = ToolResult::fail(kKindDiffers));
    const std::string bad = applyUpdate(*l, k);
    if (!bad.empty()) return void(out = ToolResult::fail(bad));
    const LoopRecord done = *l;
    if (commit(next)) out = ToolResult::ok(kUpdated + line(done));
  }

  // loops.json first; the sidecar after it (refused: an orphan entry, dropped by the next write).
  void remove() {
    const std::string id = str(a, kId);
    if (id.empty()) return void(out = ToolResult::fail(kNoId));
    std::vector<LoopRecord> next = *live;
    LoopRecord* l = find(next, id);
    if (!l) return void(out = ToolResult::fail(kNoEntry + id));
    next.erase(next.begin() + (l - next.data()));
    if (!host.saveLoops(next)) return void(out = ToolResult::fail(kNotStored));
    *live = next;
    (void)host.writeSidecar(dumpSidecar(prunedSidecar(extras, next)));
    host.forgetState(id);
    out = ToolResult::ok(kDeleted + id);
  }

  void list() {
    std::string s;
    for (const LoopRecord& l : *live) {
      if (host.reserved(l.id)) continue;
      if (!s.empty()) s += '\n';
      s += line(l);
    }
    out = ToolResult::ok(s.empty() ? std::string(kNoEntries) : s);
  }

  void run(std::vector<LoopRecord>& table) {
    live = &table;
    std::string side;
    if (!host.readSidecar(side) || !loadSidecar(side, extras)) extras.clear();
    switch (opOf(a)) {
      case Op::Create: return create();
      case Op::Update: return update();
      case Op::Delete: return remove();
      case Op::List: return list();
      case Op::Bad: break;
    }
    out = ToolResult::fail(kBadOp);
  }
};

// A turn no person started may list and set a reminder, nothing else.
bool mayRun(JsonObjectConst a, const CallCtx& ctx) {
  if (ctx.origin == notes::Origin::Attended) return true;
  const Op op = opOf(a);
  return op == Op::List || (op == Op::Create && kindArg(a) == 1);
}

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

// The entry's outside marks after a fire: this fire's output when it read outside content, and only
// the marks whose output is still in the state. True when they changed.
bool markOutside(Extra& e, const std::vector<plain::RoutineOutput>& last, const plain::FireReport& r) {
  std::vector<uint64_t> marks = e.outsideAt;
  if (hasOutside(r) && !r.output.text.empty()) marks.push_back(r.output.at);
  std::vector<uint64_t> kept;
  for (uint64_t at : marks) {
    const bool inState = std::any_of(last.begin(), last.end(), [at](const plain::RoutineOutput& o) { return o.at == at; });
    if (inState && std::find(kept.begin(), kept.end(), at) == kept.end()) kept.push_back(at);
  }
  if (kept == e.outsideAt) return false;
  e.outsideAt = kept;
  return true;
}

// The record and the sidecar as they are now (a copy).
bool snapshot(Host& host, const std::string& id, LoopRecord& rec, std::vector<Extra>& extras) {
  bool found = false;
  host.withTable([&](std::vector<LoopRecord>& t) {
    for (const LoopRecord& l : t)
      if (l.id == id) rec = l, found = true;
    std::string side;
    if (!host.readSidecar(side) || !loadSidecar(side, extras)) extras.clear();
  });
  return found;
}

// A routine that cannot run now: Skipped with its reason, the fire not counted.
FireOutcome skipped(Host& host, const std::string& id) {
  host.withTable([&](std::vector<LoopRecord>& t) {
    for (LoopRecord& l : t)
      if (l.id == id) l.lastResult = LastResult::Skipped, l.lastErr = kNoCard;
    (void)host.saveLoops(t);
  });
  FireOutcome o;
  o.deferred = true;
  o.error = kNoCard;
  return o;
}

void storeMarks(Host& host, const std::string& id, const std::vector<plain::RoutineOutput>& last,
                const plain::FireReport& rep) {
  host.withTable([&](std::vector<LoopRecord>& t) {
    std::string side;
    std::vector<Extra> extras;
    if (!host.readSidecar(side) || !loadSidecar(side, extras)) extras.clear();
    auto it = std::find_if(extras.begin(), extras.end(), [&](const Extra& e) { return e.id == id; });
    if (it == extras.end()) {
      Extra e;
      e.id = id;
      extras.push_back(e);
      it = extras.end() - 1;
    }
    if (markOutside(*it, last, rep)) (void)host.writeSidecar(dumpSidecar(prunedSidecar(extras, t)));
  });
}

FireOutcome fireRoutine(Host& host, const LoopRecord& rec, const Extra* e, const FireBinding& b, uint64_t now) {
  std::vector<plain::RoutineOutput> last;
  if (!b.files || !b.run || !loadState(*b.files, rec.id, last)) return skipped(host, rec.id);
  const plain::RoutineFire f = routineFire(rec, e, last, now, b.grants);
  plain::FireReport rep;
  const plain::Status st = b.run(f, opensTainted(e, last), rep);
  FireOutcome o;
  o.ok = st.ok() && rep.status.ok() && rep.result != plain::FireResult::Failed;
  o.answered = rep.result == plain::FireResult::Sent || rep.result == plain::FireResult::Held;
  o.detail = rep.output.text;
  o.tokens = tokensOf(rep);
  if (!o.ok) o.error = statusText(st.ok() ? rep.status : st);
  if (!st.ok() || rep.result == plain::FireResult::Failed) return o;
  plain::nextRoutineState(last, rep.output);
  (void)saveState(*b.files, rec.id, last);
  storeMarks(host, rec.id, last, rep);
  return o;
}

}  // namespace

// ---- sidecar ------------------------------------------------------------------------------------
std::string dumpSidecar(const std::vector<Extra>& extras) {
  JsonDocument d;
  d[kV] = 1;
  JsonArray arr = d[kLoops].to<JsonArray>();
  for (const Extra& e : extras) {
    JsonObject o = arr.add<JsonObject>();
    o[kId] = e.id;
    o[kKind] = kindSlug(e.kind);
    putStrings(o, kTools, e.tools);
    putStrings(o, kPaths, e.paths);
    if (e.holdS) o[kHoldS] = e.holdS;
    if (!e.outsideAt.empty()) {
      JsonArray m = o[kOutsideAt].to<JsonArray>();
      for (uint64_t at : e.outsideAt) m.add(at);
    }
  }
  std::string out;
  serializeJson(d, out);
  return out;
}

bool loadSidecar(const std::string& json, std::vector<Extra>& out) {
  out.clear();
  if (json.empty()) return true;
  JsonDocument d;
  if (deserializeJson(d, json) || !d.is<JsonObject>() || (d[kV] | 0) != 1) return false;
  for (JsonObjectConst o : d[kLoops].as<JsonArrayConst>()) {
    Extra e;
    if (readExtra(o, e)) out.push_back(e);
  }
  return true;
}

const Extra* findExtra(const std::vector<Extra>& extras, const std::string& id) {
  for (const Extra& e : extras)
    if (e.id == id) return &e;
  return nullptr;
}

std::vector<Extra> prunedSidecar(const std::vector<Extra>& extras, const std::vector<LoopRecord>& loops) {
  std::vector<Extra> out;
  for (const Extra& e : extras) {
    const bool live = std::any_of(loops.begin(), loops.end(), [&](const LoopRecord& l) { return l.id == e.id; });
    if (live && !findExtra(out, e.id)) out.push_back(e);
  }
  return out;
}

Kind kindOf(const LoopRecord& l, const Extra* e) {
  if (e) return e->kind;
  return l.sched.kind == SchedKind::Once && l.createdBy == CreatedBy::Owner ? Kind::Reminder : Kind::Routine;
}

// ---- arguments ----------------------------------------------------------------------------------
bool parseWhen(const std::string& s, CivilTime& out) {
  size_t p = 0;
  int y = 0, mo = 0, d = 0;
  if (!digits(s, p, 4, y) || p >= s.size() || s[p++] != '-' || !digits(s, p, 2, mo) || p >= s.size() ||
      s[p++] != '-' || !digits(s, p, 2, d) || p >= s.size() || s[p++] != ' ')
    return false;
  uint16_t minute = 0;
  if (mo < 1 || mo > 12 || d < 1 || d > monthDays(y, mo) || !parseHhmm(s, p, minute)) return false;
  out = CivilTime{};
  out.year = y;
  out.month = mo;
  out.day = d;
  out.hour = minute / 60;
  out.min = minute % 60;
  return true;
}

bool parseEvery(const std::string& s, uint32_t minIntervalSec, SchedSpec& out) {
  const size_t sp = s.find(' ');
  if (sp == std::string::npos) return false;
  const std::string word = s.substr(0, sp), rest = s.substr(sp + 1);
  SchedSpec r;
  if (word == kEvery) {
    if (!parseInterval(rest, minIntervalSec, r)) return false;
  } else if (word == kDaily) {
    r.kind = SchedKind::Daily;
    r.weekMask = 0x7F;
    if (!parseHhmm(rest, 0, r.minuteOfDay)) return false;
  } else if (word == kWeekly) {
    const size_t sp2 = rest.find(' ');
    if (sp2 == std::string::npos) return false;
    r.kind = SchedKind::Weekly;
    r.weekMask = dayMask(rest.substr(0, sp2));
    if (!r.weekMask || !parseHhmm(rest, sp2 + 1, r.minuteOfDay)) return false;
  } else {
    return false;
  }
  out = r;
  return true;
}

std::string everyText(const SchedSpec& s) {
  if (s.kind == SchedKind::Interval || s.kind == SchedKind::Once) {
    const bool hours = s.intervalSec % 3600 == 0;
    return std::string(kEvery) + ' ' + std::to_string(s.intervalSec / (hours ? 3600 : 60)) + ' ' + (hours ? kHour : kMin);
  }
  if (s.kind == SchedKind::Daily) return std::string(kDaily) + ' ' + hhmm(s.minuteOfDay);
  std::string days;
  for (int d = 0; d < 7; ++d) {
    const int bit = (d + 1) % 7;   // Monday first, Sunday last
    if (!(s.weekMask & (1u << bit))) continue;
    if (!days.empty()) days += ',';
    days.append(kDays + bit * 3, 3);
  }
  return std::string(kWeekly) + ' ' + days + ' ' + hhmm(s.minuteOfDay);
}

std::string nameFor(const std::string& text) {
  const size_t cap = (size_t)kLoopNameMax;
  if (text.size() <= cap && text.find('\n') == std::string::npos) return text;
  std::string head = text.substr(0, std::min(text.find('\n'), cap + 1));
  if (head.size() <= cap) return head;
  const size_t sp = head.rfind(' ', cap);
  if (sp != std::string::npos && sp > 0) return head.substr(0, sp);
  head.resize(cap);
  while (!head.empty() && !utf8Valid(head)) head.pop_back();
  return head;
}

// ---- the call -----------------------------------------------------------------------------------
ToolResult call(Host& host, JsonObjectConst args, const CallCtx& ctx) {
  if (!mayRun(args, ctx)) return ToolResult::fail(kNotHere);
  Call c{host, args, ctx};
  host.withTable([&c](std::vector<LoopRecord>& t) { c.run(t); });
  return c.out;
}

// ---- state --------------------------------------------------------------------------------------
std::string statePath(const std::string& id) { return std::string(kStateDir) + '/' + id + kStateExt; }

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

FireOutcome fire(Host& host, const LoopFireRequest& req, const FireBinding& b, uint64_t now) {
  LoopRecord rec;
  std::vector<Extra> extras;
  if (!snapshot(host, req.id, rec, extras)) {
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
