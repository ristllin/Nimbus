// orch_schedule_tool.cpp - engine 2's `schedule` tool, the loops2 sidecar codec and the fires
// (lane RB-U1b; schedule_tool.h). Engine 2 only (tools/engine/engine2_only.txt). Every string
// literal in this file is model text (gen_model_text_catalog.py E2_EXTRA_SOURCES, tool_results):
// keys, values and result lines alike, so formatting uses char literals and digits by hand.
#include "nimbus/orch/schedule_tool.h"

#include <algorithm>
#include <cstring>
#include <new>

#include "nimbus/utf8.h"

namespace nimbus {
namespace orch {
namespace schedule {

// The lines the tool and the fires (nimbus/harness/schedule_fire.h) share.
namespace words {
const char kNoEntry[] = "no entry ";
const char kUnreadable[] = "not stored: loops2.json is unreadable";
const char kNoCard[] = "no card";
}  // namespace words

namespace {

// ---- the words the model reads and writes ---------------------------------------------------
constexpr char kOp[] = "op", kKind[] = "kind", kId[] = "id", kWhen[] = "when", kText[] = "text", kEvery[] = "every";
constexpr char kCreate[] = "create", kUpdate[] = "update", kDelete[] = "delete", kList[] = "list";
constexpr char kReminder[] = "reminder", kRoutine[] = "routine";
constexpr char kDays[] = "sunmontuewedthufrisat";   // three letters a day, bit 0 = Sunday
constexpr char kDaily[] = "daily", kWeekly[] = "weekly", kMin[] = "min", kHour[] = "h", kSec[] = "s";
using namespace store;   // the sidecar keys and file names (schedule_tool.h; no model reads them)
using namespace words;   // the lines the fires share (defined below)

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
constexpr char kBadEveryA[] = "every must read daily HH:MM, weekly mon,thu HH:MM or every N min|h (",
               kBadEveryB[] = " min or more)";
constexpr char kWrongKey[] = "a reminder takes when, a routine takes every";
constexpr char kFull[] = "full: ", kFullB[] = " entries";
constexpr char kNoClock[] = "the clock is not set";
constexpr char kNotHere[] = "a turn no person started may list or set reminders only";
constexpr char kNotStored[] = "not stored: the write was refused";
constexpr char kNoId[] = "id is empty";
constexpr char kNothing[] = "nothing to change";
constexpr char kKindDiffers[] = "kind differs from the entry";
constexpr char kNoMemory[] = "out of memory; nothing changed";

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

// The first `n` bytes of `text` with each run of spaces, tabs and line breaks as one space, none first.
std::string flatHead(const std::string& text, size_t n) {
  std::string head;
  for (char ch : text) {
    if (head.size() >= n) break;
    const bool space = ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t';
    if (space && (head.empty() || head.back() == ' ')) continue;
    head += space ? ' ' : ch;
  }
  return head;
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
    if (!parseEvery(every, host.minIntervalSec(), s))
      return kBadEveryA + std::to_string((host.minIntervalSec() + 59) / 60) + kBadEveryB;
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
    if (!a[reminder ? kEvery : kWhen].isNull()) return void(out = ToolResult::fail(kWrongKey));
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
    extras.erase(std::remove_if(extras.begin(), extras.end(), [&](const Extra& x) { return x.id == e.id; }), extras.end());
    extras.push_back(e);
    std::vector<LoopRecord> next = *live;
    next.push_back(l);
    if (commit(next)) out = ToolResult::ok(kStored + line(l));
  }

  // The fields an update names, applied to `l`; "" when it may be stored.
  std::string applyUpdate(LoopRecord& l, Kind k) {
    const bool when = !a[kWhen].isNull(), every = !a[kEvery].isNull(), text = !a[kText].isNull();
    if (!when && !every && !text) return kNothing;
    if ((k == Kind::Reminder && every) || (k == Kind::Routine && when)) return kWrongKey;
    if (text) {
      const std::string refusal = textRefusal(str(a, kText));
      if (!refusal.empty()) return refusal;
      setText(l, str(a, kText));
    }
    const std::string bad = when ? setWhen(l, str(a, kWhen)) : every ? setEvery(l, str(a, kEvery)) : std::string();
    if (!bad.empty()) return bad;
    if (when) l.consecFails = 0;                 // a new moment starts with its one short retry
    if (k == Kind::Routine) l.approved = false;   // a changed routine waits for the owner again
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
    if (named < 0) return void(out = ToolResult::fail(kBadKind));
    if (named > 0 && (named == 1) != (k == Kind::Reminder)) return void(out = ToolResult::fail(kKindDiffers));
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
    const bool readable = host.readSidecar(side) && loadSidecar(side, extras);
    const Op op = opOf(a);
    // An unreadable sidecar is never rewritten from nothing: its kinds, tools and marks would go.
    if (!readable && op != Op::List) return void(out = ToolResult::fail(kUnreadable));
    if (!readable) extras.clear();
    switch (op) {
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
    const uint32_t unit = s.intervalSec % 3600 == 0 ? 3600 : s.intervalSec % 60 == 0 ? 60 : 1;
    const char* word = unit == 3600 ? kHour : unit == 60 ? kMin : kSec;   // an engine-1 record may hold seconds
    return std::string(kEvery) + ' ' + std::to_string(s.intervalSec / unit) + ' ' + word;
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
  std::string head = flatHead(text, cap + 1);
  if (head.size() <= cap) {
    while (!head.empty() && head.back() == ' ') head.pop_back();
    return head;
  }
  const size_t sp = head.rfind(' ', cap);
  if (sp != std::string::npos && sp > 0) return head.substr(0, sp);
  head.resize(cap);
  while (!head.empty() && !utf8Valid(head)) head.pop_back();
  return head;
}

// ---- the call -----------------------------------------------------------------------------------
ToolResult call(Host& host, JsonObjectConst args, const CallCtx& ctx) {
  if (!mayRun(args, ctx)) return ToolResult::fail(kNotHere);
  try {
    Call c{host, args, ctx};
    host.withTable([&c](std::vector<LoopRecord>& t) { c.run(t); });
    return c.out;
  } catch (const std::bad_alloc&) {   // ruling 30: a refused allocation is a result (the table is
    return ToolResult::fail(kNoMemory);   // assigned only after both writes landed)
  }
}

std::string statePath(const std::string& id) { return std::string(kStateDir) + '/' + id + kStateExt; }

}  // namespace schedule
}  // namespace orch
}  // namespace nimbus
