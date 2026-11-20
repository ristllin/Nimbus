// The compaction driver of engine 2 (plain/compact.h, lane RB-C2; PRD section 3).
#include "nimbus/harness/plain/compact.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <cstdlib>
#include <memory>
#include <new>

#include "nimbus/harness/http.h"
#include "nimbus/harness/log.h"
#include "nimbus/harness/plain/exchange.h"
#include "nimbus/harness/plain/texts.h"
#include "nimbus/harness/platform.h"
#include "nimbus/harness/turn_memory.h"
#include "nimbus/orch/day_start.h"   // RB-REC5: the one day rule
#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/log.h"
#include "nimbus/orch/tx/rules.h"

namespace agent {
namespace plain {

namespace tx = nimbus::orch::tx;
namespace day = nimbus::orch::day;
namespace st = texts::summarizer;

// ---- the control record -------------------------------------------------------------------

// One line per fact, space-separated tokens: "compact v1", "day <day> <paid>", "chat <key> <fails>
// <next>". Built from words, never from formats, so no literal here reads as text to a model.
namespace {
const char kMagic[] = "compact";
const char kVersion[] = "v1";
const char kDayKey[] = "day";
const char kChatKey[] = "chat";

void putLine(std::string& out, const char* key, std::initializer_list<std::string> values) {
  out += key;
  for (const std::string& v : values) {
    out += ' ';
    out += v;
  }
  out += '\n';
}

std::vector<std::string> tokens(const std::string& line) {
  std::vector<std::string> out;
  size_t at = 0;
  while (at < line.size() && out.size() < 8) {
    const size_t sp = line.find(' ', at);
    out.push_back(line.substr(at, sp == std::string::npos ? std::string::npos : sp - at));
    if (sp == std::string::npos) break;
    at = sp + 1;
  }
  return out;
}

bool number(const std::string& s, uint64_t& out, uint64_t max = UINT64_MAX) {
  if (s.empty() || s.size() > 20) return false;
  out = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    out = out * 10 + (uint64_t)(c - '0');
  }
  return out <= max;
}

// One fact line of the record; anything it cannot read is skipped.
void readFact(const std::vector<std::string>& t, CompactCtl& c) {
  uint64_t a = 0, b = 0;
  if (t.size() == 3 && t[0] == kDayKey && number(t[1], a, UINT32_MAX) && number(t[2], b, UINT32_MAX)) {
    c.day = (uint32_t)a;
    c.paid = (uint32_t)b;
    return;
  }
  const bool chat = t.size() == 4 && t[0] == kChatKey && tx::chatKeyOk(t[1]);
  if (chat && number(t[2], a, UINT32_MAX) && number(t[3], b) && c.chats.size() < kCompactCtlChats)
    c.chats.push_back(CompactCtl::Chat{t[1], (uint32_t)a, b});
}
}  // namespace

std::string CompactCtl::encode() const {
  std::string out;
  putLine(out, kMagic, {kVersion});
  putLine(out, kDayKey, {std::to_string(day), std::to_string(paid)});
  for (const Chat& c : chats) putLine(out, kChatKey, {c.key, std::to_string(c.fails), std::to_string(c.next)});
  return out;
}

CompactCtl CompactCtl::decode(const std::string& text) {
  CompactCtl c;
  size_t at = 0;
  bool first = true;
  while (at < text.size()) {
    const size_t nl = text.find('\n', at);
    if (nl == std::string::npos) break;   // a torn last line is dropped
    const std::vector<std::string> t = tokens(text.substr(at, nl - at));
    at = nl + 1;
    if (!first) {
      readFact(t, c);
      continue;
    }
    if (t.size() != 2 || t[0] != kMagic || t[1] != kVersion) return c;
    first = false;
  }
  return c;
}

CompactCtl::Chat& CompactCtl::chat(const std::string& key) {
  for (Chat& c : chats)
    if (c.key == key) return c;
  if (chats.size() >= kCompactCtlChats) chats.erase(chats.begin());   // the oldest entry
  chats.push_back(Chat{key, 0, 0});
  return chats.back();
}

uint32_t compactWaitS(uint32_t fails) {
  if (fails < kCompactHeadTries) return 0;
  const size_t n = sizeof(kCompactWaitS) / sizeof(kCompactWaitS[0]);
  const size_t i = fails - kCompactHeadTries;
  return kCompactWaitS[i < n ? i : n - 1];
}

size_t rungFor(uint32_t fails, size_t rungs) {
  if (rungs == 0 || fails < kCompactHeadTries) return 0;
  return (1 + (fails - kCompactHeadTries)) % rungs;
}

const char* compactStepSlug(CompactStep s) {
  switch (s) {
    case CompactStep::Idle: return "idle";
    case CompactStep::Waiting: return "waiting";
    case CompactStep::Nothing: return "nothing";
    case CompactStep::Failed: return "failed";
    case CompactStep::Stored: return "stored";
    case CompactStep::Refused: return "refused";
    case CompactStep::ToolsFolded: return "tools";
  }
  return "idle";
}

// ---- days -------------------------------------------------------------------------------------

namespace {

// RB-REC5: the device's one day rule (orch/day_start.h), the boundary the message stamp names.
int64_t dayNumber(uint64_t epoch, int16_t tz, uint16_t dayStartMin) { return day::runningDay(epoch, tz, dayStartMin); }

}  // namespace

std::string localDay(uint64_t epoch, int16_t tzMinutes, uint16_t dayStartMin) {
  char buf[16];
  day::formatDate(dayNumber(epoch, tzMinutes, dayStartMin), buf, sizeof buf);
  return buf;
}

// ---- the slice ----------------------------------------------------------------------------------

namespace {

bool rendered(tx::ItemKind k) {
  return k == tx::ItemKind::User || k == tx::ItemKind::Assistant || k == tx::ItemKind::Call ||
         k == tx::ItemKind::Result || k == tx::ItemKind::DeviceNote;
}

// ---- what a request renders (ruling 91 table/F1, F6) ---------------------------------------------
// The marks H and L are request bytes (tx::budgetFor: the head's due and keep in tokens on its wire,
// less the fixed part and the digest block), so the transcript is measured as a request renders it,
// not as it is stored: a stored result line also holds its stub (up to 400 B) and a bulk result
// stores a handle where the request shows the whole. Each rendered item counts its text as shown
// plus kItemFramingBytes of wire framing (role, type, ids, quotes: about what a stored line's keys
// take, so a text-only chat measures as it did); a photo counts its stub line (its pixels reach the
// hot trigger in real tokens); items no request renders (t, i, k of a closed turn, x, d, e, s)
// count nothing.
constexpr size_t kItemFramingBytes = 64;
constexpr size_t kStampBytes = 24;           // "[2026-11-17 Tue 13:02] "
constexpr size_t kPhotoShownBytes = 96;      // '[photo p<ref8> <when> not shown: view id=p<ref8>]'

// The stub rule's facts over the resident items: the pointer (the newest x item's stub) and every x
// item's superseded list, sorted.
struct ShowState {
  uint64_t pointer = 0;
  std::vector<uint64_t> superseded;
};
Status showState(const tx::TxArena& arena, ShowState& out) {
  out = ShowState{};
  tx::Item it;
  for (size_t i = 0; i < arena.count(); ++i) {
    tx::ArenaEntry e;
    Status s = arena.header(i, e);
    if (!s.ok()) return s;
    if (e.kind != tx::ItemKind::PointerMove) continue;
    if (!(s = arena.get(i, it)).ok()) return s;
    out.pointer = std::max(out.pointer, it.stub);
    out.superseded.insert(out.superseded.end(), it.sup.begin(), it.sup.end());
  }
  std::sort(out.superseded.begin(), out.superseded.end());
  out.superseded.erase(std::unique(out.superseded.begin(), out.superseded.end()), out.superseded.end());
  return Status::success();
}
bool shownWhole(const tx::Item& r, const ShowState& s) {
  tx::ShowFacts f;
  f.pointer = s.pointer;
  f.superseded = &s.superseded;
  tx::ShowAs as = tx::ShowAs::Stub;
  return tx::showResult(r, f, as).ok() && as == tx::ShowAs::Whole;
}
// What one decoded item adds to a request between turns (no turn open).
size_t shownBytes(const tx::Item& it, const ShowState& s) {
  size_t n = kItemFramingBytes;
  switch (it.kind) {
    case tx::ItemKind::User:
      n += it.x.size() + kStampBytes;
      for (const tx::Part& p : it.parts) n += (p.kind == tx::PartKind::Image ? kPhotoShownBytes : p.ref.size() + kStampBytes) + p.note.size();
      return n;
    case tx::ItemKind::Assistant:
    case tx::ItemKind::DeviceNote: return n + it.x.size();
    case tx::ItemKind::Call: return n + it.id.size() + it.n.size() + tx::shownArgs(it, 0, 0).size();   // between turns
    case tx::ItemKind::Result:
      if (!shownWhole(it, s)) return n + it.id.size() + it.st.size();
      return n + it.id.size() + (it.cls == tx::ResultClass::Receipt || it.handle.empty() ? it.x.size() : it.bytes);
    default: return 0;
  }
}

// One turn's run of items, in q order.
struct Unit {
  size_t first = 0, last = 0;   // arena indexes
  uint32_t turn = 0;
  uint64_t lastQ = 0;
  uint64_t at = 0;              // epoch of its first clocked item
  size_t bytes = 0;             // what a request renders of it (shownBytes)
  size_t stored = 0;            // what it holds of the arena (lines and headers)
  size_t text = 0;              // owner and assistant text (v5.1.2's unit: what its window counted)
  bool owner = false;           // has a u item
  bool content = false;         // has a u or an a item
  bool closed = false;          // has its e item (or is turn 0: device items between turns)
  bool openIntent = false;
  bool tainted = false;
  std::string day;
  std::vector<std::string> intents;   // intent ids with no result yet
};

// What one decoded item says about its turn.
void noteItem(const tx::Item& it, Unit& u) {
  switch (it.kind) {
    case tx::ItemKind::User:
      u.owner = u.content = true;
      u.text += it.x.size();
      for (const tx::Part& p : it.parts) u.tainted |= p.kind == tx::PartKind::File;
      break;
    case tx::ItemKind::Assistant:
      u.content = true;
      u.text += it.x.size();
      break;
    case tx::ItemKind::Intent: u.intents.push_back(it.id); break;
    case tx::ItemKind::Result:
      u.tainted |= it.cls == tx::ResultClass::Outside;
      u.intents.erase(std::remove(u.intents.begin(), u.intents.end(), it.id), u.intents.end());
      break;
    case tx::ItemKind::DeviceNote: u.tainted |= it.note == tx::NoteClass::Agent; break;
    default: break;
  }
}

bool decoded(tx::ItemKind k) { return rendered(k) || k == tx::ItemKind::Intent; }

// A turn's items are one unit even when something stored between turns (a digest, turn 0)
// landed inside it: the units from its first part to here become one, so a turn is never split.
void joinTurn(std::vector<Unit>& units) {
  const uint32_t turn = units.back().turn;
  size_t k = units.size() - 1;
  while (k > 0 && units[k - 1].turn != turn) --k;
  if (k == 0 || turn == 0) return;
  Unit& head = units[k - 1];
  for (size_t i = k; i < units.size(); ++i) {
    const Unit& u = units[i];
    head.bytes += u.bytes;
    head.stored += u.stored;
    head.text += u.text;
    head.owner |= u.owner;
    head.content |= u.content;
    head.tainted |= u.tainted;
    head.closed = u.turn == turn ? u.closed || head.closed : head.closed;
    head.intents.insert(head.intents.end(), u.intents.begin(), u.intents.end());
    head.last = u.last;
    head.lastQ = u.lastQ;
  }
  units.resize(k);
}

Status scanUnits(const tx::TxArena& arena, uint64_t thru, std::vector<Unit>& units) {
  ShowState show;
  Status ss = showState(arena, show);
  if (!ss.ok()) return ss;
  tx::Item it;
  for (size_t i = 0; i < arena.count(); ++i) {
    tx::ArenaEntry e;
    Status s = arena.header(i, e);
    if (!s.ok()) return s;
    if (e.q <= thru) continue;
    if (units.empty() || units.back().turn != e.turn) {
      Unit u;
      u.first = i;
      u.turn = e.turn;
      u.closed = e.turn == 0;
      units.push_back(u);
      joinTurn(units);
    }
    Unit& u = units.back();
    u.last = i;
    u.lastQ = e.q;
    if (!u.at) u.at = e.t;
    u.stored += tx::recordBytes(e.lineBytes);
    u.closed |= e.kind == tx::ItemKind::TurnEnd;
    if (!decoded(e.kind)) continue;
    if (!(s = arena.get(i, it)).ok()) return s;
    u.bytes += shownBytes(it, show);
    noteItem(it, u);
  }
  for (Unit& u : units) u.openIntent = !u.intents.empty();
  return Status::success();
}

// The first turn that is open, waits on an intent's result or on a running agent job.
size_t heldFrom(const std::vector<Unit>& units, const SliceRules& r) {
  for (size_t i = 0; i < units.size(); ++i) {
    const Unit& u = units[i];
    if (!u.closed || u.openIntent || (r.jobRunning && u.turn && r.jobRunning(u.turn))) return i;
  }
  return units.size();
}

// The oldest of the newest kNewestOwnerTurnsKept owner turns.
size_t newestOwnersFrom(const std::vector<Unit>& units) {
  uint32_t owners = 0;
  for (size_t i = units.size(); i-- > 0;)
    if (units[i].owner && ++owners == tx::kNewestOwnerTurnsKept) return i;
  return units.size();
}

// The first turn of the owner's current day; when that day alone is above H, the first turn
// past its oldest half.
size_t todayFrom(const std::vector<Unit>& units, const SliceRules& r, const std::string& today) {
  size_t from = units.size(), bytes = 0;
  for (size_t i = 0; i < units.size(); ++i) {
    if (units[i].day.empty() || units[i].day < today) continue;
    if (from == units.size()) from = i;
    bytes += units[i].bytes;
  }
  if (from == units.size() || bytes <= r.high) return from;
  size_t half = 0;
  while (from < units.size() && half + units[from].bytes <= bytes / 2) half += units[from++].bytes;
  return from;
}

// The first unit a slice may not reach.
size_t protectedFrom(const std::vector<Unit>& units, const SliceRules& r, const std::string& today) {
  if (r.readBack) return heldFrom(units, r);
  return std::min(heldFrom(units, r), std::min(newestOwnersFrom(units), todayFrom(units, r, today)));
}

// A tool name as the summarizer sees it: letters and '_' only, so no figure rides in it.
std::string toolWord(const std::string& n) {
  std::string out;
  for (char c : n)
    if (((c | 32) >= 'a' && (c | 32) <= 'z') || c == '_') out += c;
  return out.substr(0, 32).empty() ? std::string("tool") : out.substr(0, 32);
}

bool fileChange(const std::string& n) {
  static const char* const kChange[] = {"write_file", "append_file", "edit_file", "delete_file",
                                        "move_file",  "rename_file", "pdf_write"};
  for (const char* c : kChange)
    if (n == c) return true;
  return false;
}
bool fileWork(const std::string& n) { return n.find("file") != std::string::npos || n.find("pdf") != std::string::npos; }

// Text inside the frame cannot open or close it: '<' before "data" or "/data" becomes '('.
std::string inert(const std::string& s) {
  std::string out = s;
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] != '<') continue;
    size_t j = i + 1;
    while (j < out.size() && (out[j] == '/' || out[j] == ' ')) ++j;
    if (j + 4 <= out.size()) {
      std::string w = out.substr(j, 4);
      for (char& c : w) c = (char)(c | 32);
      if (w == "data") out[i] = '(';
    }
  }
  return out;
}

void renderItem(const tx::Item& it, bool tainted, int16_t tz, std::string& out) {
  std::string hhmm;
  (void)tx::stubTime(it.t, tz, hhmm);
  switch (it.kind) {
    case tx::ItemKind::User:
      out += "[" + hhmm + "]" + st::kOwner + it.x;
      for (const tx::Part& p : it.parts)
        if (p.kind == tx::PartKind::Image) out += st::kPhoto + (p.note.empty() ? std::string() : " " + p.note);
      out += "\n";
      break;
    case tx::ItemKind::Assistant:
      if (it.x.empty()) break;
      out += "[" + hhmm + "]" + st::kAssistant;
      out += tainted ? std::string(st::kFrameOpen) + inert(it.x) + st::kFrameClose : it.x;
      out += "\n";
      break;
    case tx::ItemKind::Result:
      if (it.cls == tx::ResultClass::Outside) out += st::kOutsideRead;
      else if (fileChange(it.n)) out += st::kFileChange;
      else if (fileWork(it.n)) out += st::kFileWork;
      else out += "[" + toolWord(it.n) + (it.ok ? st::kToolDone : st::kToolFailed);
      out += "\n";
      break;
    case tx::ItemKind::DeviceNote:
      if (it.note == tx::NoteClass::Agent) out += std::string(st::kOutsideRead) + "\n";
      break;
    default: break;
  }
}

// Cut `s` to at most `cap` bytes on a character boundary.
void clipUtf8(std::string& s, size_t cap) {
  if (s.size() <= cap) return;
  size_t n = cap;
  while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) --n;
  s.resize(n);
}

Status renderUnit(const tx::TxArena& arena, const Unit& u, int16_t tz, std::string& out, uint64_t& lastRow) {
  tx::Item it;
  for (size_t i = u.first; i <= u.last; ++i) {
    Status s = arena.get(i, it);
    if (!s.ok()) return s;
    if (it.kind == tx::ItemKind::User && it.m > lastRow) lastRow = it.m;
    renderItem(it, u.tainted, tz, out);
  }
  return Status::success();
}

}  // namespace

size_t residentBytes(const tx::TxArena& arena, uint64_t thru) {
  ShowState show;
  if (!showState(arena, show).ok()) return 0;
  size_t n = 0;
  tx::Item it;
  for (size_t i = 0; i < arena.count(); ++i) {
    tx::ArenaEntry e;
    if (arena.header(i, e).ok() && e.q > thru && rendered(e.kind) && arena.get(i, it).ok()) n += shownBytes(it, show);
  }
  return n;
}

namespace {

constexpr uint64_t kFutureSlackS = 300;

// Each turn's local day; a turn stored before the clock was set takes its neighbor's (the one
// before, else the one after).
void assignDays(std::vector<Unit>& units, const SliceRules& r) {
  for (Unit& u : units)   // a time ahead of the clock was stored by a clock that ran ahead: unclocked
    if (u.at && u.at <= r.now + kFutureSlackS) u.day = localDay(u.at, r.tzMinutes, r.dayStartMin);
  for (size_t i = 1; i < units.size(); ++i)
    if (units[i].day.empty()) units[i].day = units[i - 1].day;
  for (size_t i = units.size(); i-- > 1;)
    if (units[i - 1].day.empty()) units[i - 1].day = units[i].day;
}

// The arena is short of its room and this slice has not yet freed enough for twice it.
bool pressed(const SliceRules& r, const SlicePlan& plan) {
  return r.arenaRoom && r.arenaFree < r.arenaRoom && r.arenaFree + plan.freed < 2 * r.arenaRoom;
}

// May a turn with text join the slice: its day, and what stays after it at least L, with at least
// the head's v5.1.2 keep of owner and assistant text (owner ruling 81). The arena's room is the one
// real limit that may take a slice under them (ruling 91 table/F1): never silently, the plan says so.
bool joins(const Unit& u, const SliceRules& r, SlicePlan& plan) {
  if (!u.content) return true;
  if (u.day.empty() || (!plan.day.empty() && u.day != plan.day)) return false;
  if (r.manual) return true;
  const bool keepsL = plan.rest >= u.bytes && plan.rest - u.bytes >= r.low;
  if (keepsL && plan.restText >= u.text + r.keepText) return true;
  if (pressed(r, plan)) return plan.belowKeep = true;
  if (!plan.turns && keepsL) plan.textHeld = true;
  return false;
}

void take(const Unit& u, std::string& text, uint64_t row, SlicePlan& out) {
  if (text.size() > kCompactSliceMaxBytes) {   // one turn over the input cap: summarized from its start
    clipUtf8(text, kCompactSliceMaxBytes - sizeof(st::kCut));
    text += st::kCut;
    text += '\n';
  }
  out.input += text;
  out.lastRow = row;
  out.freed += u.stored;
  out.rest -= u.bytes;
  out.restText -= u.text;
  out.thru = u.lastQ;
  out.tainted |= u.tainted;
  if (!u.content) return;
  out.turns += 1;
  out.day = u.day;
}

}  // namespace

Status planSlice(const tx::TxArena& arena, uint64_t thru, const SliceRules& r, SlicePlan& out) {
  out = SlicePlan{};
  if (!r.now || !r.high || r.low > r.high) return Status::fail(Err::InvalidArgument, "plain::planSlice");
  std::vector<Unit> units;
  Status s = scanUnits(arena, thru, units);
  if (!s.ok()) return s;
  assignDays(units, r);
  for (const Unit& u : units) {
    out.resident += u.bytes;
    out.restText += u.text;
  }
  out.rest = out.resident;
  const size_t stop = protectedFrom(units, r, localDay(r.now, r.tzMinutes, r.dayStartMin));
  std::string text;
  for (size_t i = 0; i < stop && joins(units[i], r, out); ++i) {
    text.clear();
    uint64_t row = out.lastRow;
    if (!(s = renderUnit(arena, units[i], r.tzMinutes, text, row)).ok()) return s;
    if (units[i].content && out.turns && out.input.size() + text.size() > kCompactSliceMaxBytes) break;
    take(units[i], text, row, out);
  }
  if (!out.turns) {   // device items alone are never a slice: they go with the next turn
    const size_t resident = out.resident, text = out.restText;
    const bool held = out.textHeld;
    out = SlicePlan{};
    out.resident = out.rest = resident;
    out.restText = text;
    out.textHeld = held;
  } else if (r.omittedThru > thru) {   // stored items the arena could not hold: named, never silent
    out.input.insert(0, std::string(st::kNotLoaded) + "\n");
  }
  return Status::success();
}

namespace {

// One closed unit's results that render whole, as the tool fold takes them (oldest first) while
// the transcript is still over `target`; `left` is what it still renders.
struct FoldScan {
  const tx::TxArena& arena;
  uint64_t thru;
  const ShowState& show;
  size_t target;
};
Status foldUnit(const FoldScan& f, const Unit& u, size_t& left, std::vector<uint64_t>& sup) {
  const tx::TxArena& arena = f.arena;
  const uint64_t thru = f.thru;
  const ShowState& show = f.show;
  tx::Item it;
  for (size_t k = u.first; k <= u.last && left > f.target; ++k) {
    tx::ArenaEntry e;
    Status s = arena.header(k, e);
    if (!s.ok()) return s;
    if (e.q <= thru || e.kind != tx::ItemKind::Result) continue;
    if (!(s = arena.get(k, it)).ok()) return s;
    if (!shownWhole(it, show) || it.st.empty()) continue;   // already a stub, or none to show
    const size_t whole = shownBytes(it, show), stub = kItemFramingBytes + it.id.size() + it.st.size();
    if (whole <= stub) continue;
    left -= whole - stub;
    sup.push_back(it.q);
  }
  return Status::success();
}

}  // namespace

Status planToolFold(const tx::TxArena& arena, uint64_t thru, const SliceRules& r, std::vector<uint64_t>& sup,
                    size_t& saved) {
  sup.clear();
  saved = 0;
  if (!r.now || !r.high || r.low > r.high) return Status::fail(Err::InvalidArgument, "plain::planToolFold");
  std::vector<Unit> units;
  Status s = scanUnits(arena, thru, units);
  if (!s.ok()) return s;
  assignDays(units, r);
  size_t rest = 0;
  for (const Unit& u : units) rest += u.bytes;
  ShowState show;
  if (!(s = showState(arena, show)).ok()) return s;
  const size_t stop = protectedFrom(units, r, localDay(r.now, r.tzMinutes, r.dayStartMin));
  size_t left = rest;
  for (size_t i = 0; i < stop && left > r.low && units[i].closed; ++i)   // thru never jumps a turn; nor a fold
    if (!(s = foldUnit(FoldScan{arena, thru, show, r.low}, units[i], left, sup)).ok()) return s;
  saved = rest - left;
  return Status::success();
}

// ---- the driver ---------------------------------------------------------------------------------

namespace {

// Ruling 89 item 3: with no head named nothing could compact and the overflow ladder would end up
// hiding whole turns with no digest. That is never silent: a Status the caller sees and one log line.
bool headNamed(const CompactDeps& d) { return !d.headKey.host.empty() && !d.headKey.model.empty(); }
Status noHead(const CompactDeps& d, const char* where) {
  hlog::logf("compact(%s): no head named (CompactDeps::headKey): compaction refused", d.chatKey.c_str());
  return Status::fail(Err::InvalidArgument, where);
}

uint32_t calendarDay(uint64_t now, int16_t tz) { return (uint32_t)dayNumber(now, tz, 0); }

// The head that answered the newest resident turn (its t item's "host/model"), read at every step
// (ruling 91 blockers/F5): the marks follow a head switched in the chat without a new driver. The
// host is read as the engine reads it (providerSlugOf: a slug, or an endpoint host such as
// api.anthropic.com); the installed head stands when no t item is resident or the t item names its
// model (its routing is known only there).
tx::HeadKey answeringHead(const tx::TxArena& arena, const tx::HeadKey& installed) {
  tx::Item it;
  for (size_t i = arena.count(); i-- > 0;) {
    tx::ArenaEntry e;
    if (!arena.header(i, e).ok() || e.kind != tx::ItemKind::TurnOpen || !arena.get(i, it).ok()) continue;
    const size_t slash = it.head.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= it.head.size()) return installed;
    TurnCtx t;
    t.host = it.head.substr(0, slash);
    HeadEndpoint ep;
    ep.host = t.host;
    tx::HeadKey k{providerSlugOf(t, ep), it.head.substr(slash + 1), false};
    if (k.model == installed.model && (k.host == installed.host || k.host == "custom")) return installed;
    k.routed = k.host == "cumulo";
    return k;
  }
  return installed;
}

// The whole prompt of the newest turn's last request (its e item's lastPromptTokens), 0 when none
// is resident. Never the turn's summed promptTokens: a two-request turn on an OpenAI wire would read
// twice its size, and a cached Anthropic request would read only its uncached part (T4-ADV-01).
uint32_t lastPromptTokens(const tx::TxArena& arena) {
  for (size_t i = arena.count(); i-- > 0;) {
    tx::ArenaEntry e;
    if (!arena.header(i, e).ok() || e.kind != tx::ItemKind::TurnEnd) continue;
    tx::Item it;
    return arena.get(i, it).ok() ? it.end.lastPromptTokens : 0;
  }
  return 0;
}

// The rung's wire with the summarizer's output cap: the exchange picks a cap per request and
// this wire holds it at `cap` (kSummarizerMaxTokens, then kSummarizerRetryMaxTokens).
class CappedWire : public WireCodec {
 public:
  CappedWire(const WireCodec& inner, uint16_t cap) : inner_(inner), cap_(cap) {}
  Dialect dialect() const override { return inner_.dialect(); }
  Status render(const RenderInput& in, HttpRequest& out) const override {
    RenderInput capped = in;
    capped.maxTokens = cap_;
    return inner_.render(capped, out);
  }
  // A cut answer is read as a plain stop and remembered: the exchange would ask again at its own
  // retry cap, which this wire holds at the same cap, so the retry could only be cut again; the
  // driver asks once more itself, with more room.
  Status parse(const HttpResponse& in, ParsedStep& out) const override {
    const Status s = inner_.parse(in, out);
    if (s.ok() && out.stop == StopReason::MaxTokens) {
      cut = true;
      out.stop = StopReason::Other;
    }
    return s;
  }
  mutable bool cut = false;

 private:
  const WireCodec& inner_;
  uint16_t cap_;
};

// The digest body: the summary, framed when its slice read outside content, made to fit
// kDigestMaxBytes with its day header so the frame is never cut.
Status digestFor(const SlicePlan& plan, const std::string& summary, std::string& text) {
  Status s = tx::digestText(plan.day, inert(summary), text);
  if (!s.ok() || !plan.tainted) return s;
  std::string header;
  if (!(s = tx::dayHeader(plan.day, header)).ok()) return s;
  const size_t lead = header.size() + sizeof(tx::digest_text::kDaySep) - 1;
  std::string body = text.substr(lead);
  const size_t frame = sizeof(st::kFrameOpen) - 1 + sizeof(st::kFrameClose) - 1;
  const size_t room = tx::kDigestMaxBytes - lead - frame;
  if (body.size() > room) {
    size_t cut = body.rfind(". ", room);
    if (cut == std::string::npos || cut == 0) cut = body.rfind(' ', room);
    if (cut == std::string::npos || cut == 0) cut = room;
    else if (body[cut] == '.') ++cut;
    body.resize(cut);
    clipUtf8(body, room);
  }
  return tx::digestText(plan.day, std::string(st::kFrameOpen) + body + st::kFrameClose, text);
}

}  // namespace

// RAM is the authority; the file adds what other chats' drivers (and earlier boots) counted. A
// refused save therefore never lifts the cap or the wait for this driver.
void CompactDriver::loadCtl() {
  if (!d_.ctl.load) return;
  const CompactCtl disk = CompactCtl::decode(d_.ctl.load());
  if (disk.day == ctl_.day) ctl_.paid = std::max(ctl_.paid, disk.paid);
  else if (disk.day > ctl_.day) {
    ctl_.day = disk.day;
    ctl_.paid = disk.paid;
  }
  for (const CompactCtl::Chat& c : disk.chats) {
    CompactCtl::Chat& mine = ctl_.chat(c.key);
    mine.fails = std::max(mine.fails, c.fails);
    mine.next = std::max(mine.next, c.next);
  }
}
void CompactDriver::saveCtl() {
  if (d_.ctl.save && !d_.ctl.save(ctl_.encode())) hlog::log("compact: the control record was not saved");
}

bool CompactDriver::requestManual() {
  if (manual_) return false;
  manual_ = true;
  manualAsks_ = manualStored_ = 0;
  return true;
}

void CompactDriver::endManual(const char* text) {
  manual_ = false;
  manualAsks_ = manualStored_ = 0;
  if (d_.tell) d_.tell(d_.chatId, text);
}

Status CompactDriver::step(const IdleCtx& ctx, CompactOutcome& out) {
  out = CompactOutcome{};
  if (!d_.platform || !d_.transport || !d_.log || !d_.arena || !d_.head || !d_.digests || !d_.head() ||
      !d_.digests())
    return Status::fail(Err::InvalidArgument, "plain::CompactDriver::step");
  if (ctx.ownerWaiting) return Status::success();   // the owner's message goes first
  PsramFirstScope scope;
  try {
    return run(ctx, out);
  } catch (const std::bad_alloc&) {
    hlog::log("compact: an allocation was refused");
  }
  if (manual_) {   // a manual run is answered even now (the answer is one short line)
    try {
      endManual(texts::device_notes::kCompactFailed);
    } catch (const std::bad_alloc&) {
      manual_ = false;
    }
  }
  return Status::fail(Err::TooLarge, "plain::CompactDriver::step");
}

Status CompactDriver::run(const IdleCtx& ctx, CompactOutcome& out) {
  out.thru = d_.head()->thru;
  if (!ctx.clockSet || !ctx.now) {   // the day rule cannot be applied without a clock
    if (manual_) endManual(texts::device_notes::kCompactNoClock);
    return Status::success();
  }
  if (!manual_ && !automaticMayAsk(ctx)) return Status::success();
  SlicePlan plan;
  bool due = false;
  Status s = planIfDue(ctx, plan, due);
  if (!s.ok() && manual_) endManual(texts::device_notes::kCompactFailed);   // answered, never left waiting
  if (!s.ok() || !due) return s;
  if (!plan.thru) return noSlice(plan, out);
  if (manual_ && manualAsks_ >= kCompactManualRequests) {
    endManual(texts::device_notes::kCompactDone);
    return Status::success();
  }
  return attempt(ctx, plan, out);
}

// Due, and no slice may go: the tools fold when only the text keep held it (RB-H81), else nothing.
Status CompactDriver::noSlice(const SlicePlan& plan, CompactOutcome& out) {
  if (plan.textHeld && !manual_) return foldTools(rules_, out);
  out.step = CompactStep::Nothing;
  if (manual_) endManual(manualStored_ ? texts::device_notes::kCompactDone : texts::device_notes::kCompactNothing);
  return Status::success();
}

// An automatic step needs quiet and a request left in this idle window. A turn since the last
// look, or an hour, opens a new window.
bool CompactDriver::automaticMayAsk(const IdleCtx& ctx) {
  if (d_.arena->lastQ() != windowQ_ || ctx.nowMs - windowStartMs_ >= kCompactWindowS * 1000u) {
    windowQ_ = d_.arena->lastQ();
    windowStartMs_ = ctx.nowMs;
    windowAsks_ = 0;
  }
  return ctx.quietForS >= kCompactQuietS && windowAsks_ < kCompactRequestsPerWindow;
}

// The slice rules of a step: the head's marks, the text keep at full, the arena's room.
SliceRules CompactDriver::rulesFor(const IdleCtx& ctx, const tx::HeadBudget& b) const {
  SliceRules rules;
  rules.now = ctx.now;
  rules.tzMinutes = ctx.tzMinutes;
  rules.dayStartMin = d_.dayStartMin;
  rules.high = b.marks.high;
  rules.low = b.marks.low;
  rules.keepText = b.profile == tx::Profile::Full ? b.releasedKeep : 0;   // an owner's smaller profile is his choice
  rules.manual = manual_;
  rules.jobRunning = d_.jobRunning;
  rules.omittedThru = d_.omittedThru ? d_.omittedThru() : 0;
  rules.arenaFree = d_.arena->freeBytes();
  rules.arenaRoom = std::min(kCompactArenaRoomBytes, d_.arena->capacity() / 4);
  return rules;
}

Status CompactDriver::planIfDue(const IdleCtx& ctx, SlicePlan& plan, bool& due) {
  // The head's marks at its profile (tx::budgetFor). No head named: nothing is compacted, never a
  // fixed smaller profile in its place (ruling 83 item 4).
  if (!headNamed(d_)) return noHead(d_, "plain::CompactDriver.headKey");
  tx::HeadBudget b;
  Status s = tx::budgetFor(answeringHead(*d_.arena, d_.headKey), tx::profileOfHead(d_.head()->profile), b);
  if (!s.ok()) return s;
  rules_ = rulesFor(ctx, b);
  if (!(s = planSlice(*d_.arena, d_.head()->thru, rules_, plan)).ok()) return s;
  if (rules_.omittedThru > d_.head()->thru) {   // what left RAM is folded first, from the card
    SlicePlan back;
    bool read = false;
    if (!(s = planDropped(ctx, rules_.omittedThru, back, read)).ok() || !read) return s.ok() ? dueBy(b, plan, due) : s;
    back.resident = plan.resident;
    plan = std::move(back);
    due = true;
    return Status::success();
  }
  return dueBy(b, plan, due);
}

// Due by the marks (H), the hot trigger in real tokens (the last request reached the aim less the
// next request's growth) or the arena's room (ruling 91 table/F1: before the engine would drop
// whole turns from RAM), never by an hour (owner ruling 81, ruling 83 item 8: no night fold). Once
// due it runs on down to L (ruling 82 item 4).
Status CompactDriver::dueBy(const tx::HeadBudget& b, const SlicePlan& plan, bool& due) {
  const bool hot = lastPromptTokens(*d_.arena) >= b.tokens.due;
  const bool room = rules_.arenaFree < rules_.arenaRoom;
  if (plan.resident > b.marks.high || hot || room) cycle_ = true;
  due = manual_ || cycle_;
  if (cycle_ && !plan.thru && (!plan.textHeld || plan.resident <= b.marks.low)) cycle_ = false;   // L reached, or nothing may go
  return Status::success();
}

namespace {

// The stored lines with thru < q <= upTo, read back from the segments from thru's offset into
// `arena` (at most its capacity; the oldest first): the turns a full arena or the restart fill
// gave up, so their digest summarizes them (ruling 91 table/F1: never "not loaded" when the card
// still holds them). False when the card did not answer or nothing was read.
// The segment numbers from `from` on, oldest first; false when the card did not answer.
bool segmentsFrom(tx::TxFiles& files, const std::string& chatKey, uint32_t from, std::vector<uint32_t>& segs) {
  std::string dir;
  std::vector<std::string> names;
  if (!tx::txDir(chatKey, dir).ok() || !files.list(dir, names)) return false;
  for (const std::string& n : names) {
    uint32_t k = 0;
    const bool seg = n.size() == 16 && n.compare(0, 4, "seg-") == 0 && n.compare(10, 6, ".jsonl") == 0;
    if (seg && std::sscanf(n.c_str() + 4, "%6u", &k) == 1 && k >= from) segs.push_back(k);
  }
  std::sort(segs.begin(), segs.end());
  return true;
}

// The whole lines of `buf` from its start: thru < q <= upTo go to the arena. Returns where the
// unread rest starts; `done` when a q past upTo was seen or the arena is full.
size_t takeLines(const std::string& buf, uint64_t thru, uint64_t upTo, tx::TxArena& arena, bool& done) {
  size_t at = 0;
  for (size_t nl; !done && (nl = buf.find('\n', at)) != std::string::npos; at = nl + 1) {
    tx::LineHeader h;
    if (!tx::checkLine(buf.data() + at, nl + 1 - at, h).ok() || h.q <= thru) continue;
    done = h.q > upTo || !arena.appendLine(buf.data() + at, nl + 1 - at).ok();
  }
  return at;
}

bool readBack(tx::TxFiles& files, const std::string& chatKey, const tx::HeadState& head, uint64_t upTo,
              tx::TxArena& arena) {
  std::vector<uint32_t> segs;
  if (!segmentsFrom(files, chatKey, head.thruSeg, segs)) return false;
  std::string buf, chunk(tx::kFillSliceBytes, '\0'), path;
  bool done = false;
  for (size_t i = 0; i < segs.size() && !done; ++i) {
    if (!tx::segmentPath(chatKey, segs[i], path).ok()) return false;
    size_t off = segs[i] == head.thruSeg ? head.thruOffset : 0, got = 0;
    buf.clear();
    while (!done && files.readAt(path, off, &chunk[0], chunk.size(), got) && got) {
      off += got;
      buf.append(chunk.data(), got);
      buf.erase(0, takeLines(buf, head.thru, upTo, arena, done));
    }
  }
  return arena.count() > 0;
}
}  // namespace

// Turns stored but no longer resident (a full arena's drop, the restart fill's give-up) come first:
// they are read back from the card and sliced as a manual run would (they are out of every request
// already, so neither L nor the text keep holds them), one day and one input at a time.
Status CompactDriver::planDropped(const IdleCtx& ctx, uint64_t omitted, SlicePlan& plan, bool& read) {
  read = false;
  if (!d_.files || omitted <= d_.head()->thru) return Status::success();
  auto alloc = d_.platform->allocLarge ? d_.platform->allocLarge : [](size_t n) { return std::malloc(n); };
  auto release = d_.platform->freeLarge ? d_.platform->freeLarge : [](void* q) { std::free(q); };
  tx::TxArena back;   // the plan keeps only the input text: freed when this returns
  if (!back.init(kCompactReadBackBytes, alloc, release).ok()) return Status::fail(Err::TooLarge, "plain::planDropped");
  if (!readBack(*d_.files, d_.chatKey, *d_.head(), omitted, back)) return Status::success();
  SliceRules r = rules_;
  r.now = ctx.now;
  r.manual = true;
  r.readBack = true;
  r.omittedThru = 0;
  r.arenaRoom = 0;
  const Status s = planSlice(back, d_.head()->thru, r, plan);
  read = s.ok() && plan.thru;
  return s;
}

// The tool fold, stored as one x item per kMaxListItems results (the pointer unchanged): code only,
// no request, no daily count. TooLarge only for a refused allocation.
Status CompactDriver::foldTools(const SliceRules& rules, CompactOutcome& out) {
  tx::HeadState& head = *d_.head();
  std::vector<uint64_t> sup;
  size_t saved = 0;
  Status s = planToolFold(*d_.arena, head.thru, rules, sup, saved);
  if (!s.ok() || sup.empty()) {
    if (s.ok()) out.step = CompactStep::Nothing;
    return s;
  }
  for (size_t at = 0; at < sup.size(); at += tx::kMaxListItems) {
    tx::Item x;
    x.kind = tx::ItemKind::PointerMove;
    x.q = std::max<uint64_t>({head.nextQ, d_.arena->lastQ() + 1, d_.log->lastQ() + 1});
    x.t = d_.platform->nowEpoch ? d_.platform->nowEpoch() : 0;
    x.stub = head.stub;
    x.why = tx::moveWhySlug(tx::MoveWhy::ToolFold);
    x.sup.assign(sup.begin() + (long)at, sup.begin() + (long)std::min(sup.size(), at + tx::kMaxListItems));
    tx::StoreState state = tx::StoreState::NotStored;
    if (!(s = d_.log->store(x, *d_.arena, state)).ok()) return s.err == Err::TooLarge ? Status::success() : s;
    head.nextQ = x.q + 1;
  }
  (void)d_.log->writeHead(head);
  out.step = CompactStep::ToolsFolded;
  hlog::logf("compact(%s): text under the v5.1.2 keep: %u results of the oldest turns now show as stubs (%u B)",
             d_.chatKey.c_str(), (unsigned)sup.size(), (unsigned)saved);
  return Status::success();
}

// The daily cap and this chat's wait (a manual run: the manual day's ceiling, answered).
bool CompactDriver::held(const IdleCtx& ctx, const CompactCtl::Chat& chat) {
  const uint32_t today = calendarDay(ctx.now, ctx.tzMinutes);
  if (today > ctl_.day || ctl_.day > today + 1) {   // a new day; a clock set back one day keeps the count
    ctl_.day = today;
    ctl_.paid = 0;
  }
  if (manual_) {
    if (ctl_.paid < kCompactPaidPerDay + kCompactManualRequests) return false;
    endManual(texts::device_notes::kCompactFailed);
    return true;
  }
  // A wait longer than the longest one was set by a clock that ran ahead: it does not hold.
  const bool waits = chat.next > ctx.now && chat.next <= ctx.now + compactWaitS(UINT32_MAX);
  return waits || ctl_.paid >= kCompactPaidPerDay;
}

// One attempt: the daily cap and this chat's wait, one rung, one exchange, the swap.
Status CompactDriver::attempt(const IdleCtx& ctx, const SlicePlan& plan, CompactOutcome& out) {
  loadCtl();
  const CompactCtl::Chat before = ctl_.chat(d_.chatKey);
  out.step = CompactStep::Waiting;
  if (held(ctx, before)) return Status::success();
  std::vector<SummarizerRung> rungs;
  if (d_.rungs) d_.rungs(rungs);
  if (rungs.empty()) {
    if (manual_) endManual(texts::device_notes::kCompactFailed);
    return Status::success();
  }
  const SummarizerRung& rung = rungs[rungFor(before.fails, rungs.size())];
  ctl_.paid += 1;   // one attempt, counted before it is sent: a cut during the request still counts
  saveCtl();
  ++windowAsks_;
  manualAsks_ += manual_;
  Asked asked;
  Status s = ask(rung, plan, ctx.now, asked);
  out.requests = asked.requests;
  out.ran = asked.requests > 0;
  if (asked.parked && d_.park) d_.park(rung);
  if (s.ok() && plan.belowKeep)
    hlog::logf("compact(%s): the arena has %u B free: this slice leaves under the v5.1.2 text keep (%u B kept)",
               d_.chatKey.c_str(), (unsigned)d_.arena->freeBytes(), (unsigned)(plan.restText));
  if (s.ok()) s = swap(plan, asked.summary, ctx.now, out);
  settle(rung, plan, before.fails, ctx.now, s, out);
  return s.err == Err::TooLarge ? s : Status::success();
}

// The attempt's outcome into the control record: a stored digest clears the chat's failures,
// anything else counts one and sets the wait.
void CompactDriver::settle(const SummarizerRung& rung, const SlicePlan& plan, uint32_t fails, uint64_t now,
                           const Status& s, CompactOutcome& out) {
  CompactCtl::Chat& c = ctl_.chat(d_.chatKey);
  if (s.ok() && out.step == CompactStep::Stored) {
    c.fails = 0;
    c.next = 0;
    manualStored_ += manual_;
    hlog::logf("compact(%s): day %s through q%llu, %u turns on %s/%s", d_.chatKey.c_str(), plan.day.c_str(),
               (unsigned long long)plan.thru, (unsigned)plan.turns, rung.host.c_str(), rung.model.c_str());
  } else {
    if (out.step != CompactStep::Refused) out.step = CompactStep::Failed;
    c.fails = fails + 1;
    c.next = now + compactWaitS(c.fails);
    hlog::logf("compact(%s): %s/%s failed (%s), next try in %lus", d_.chatKey.c_str(), rung.host.c_str(),
               rung.model.c_str(), s.ok() ? "storage" : tx::errSlug(s.err), (unsigned long)compactWaitS(c.fails));
    if (manual_) endManual(texts::device_notes::kCompactFailed);
  }
  saveCtl();
  windowQ_ = d_.arena->lastQ();
}

namespace {

// The summarizer's one message, in an arena of its own (PSRAM on the device). Escaping grows a
// line: twice the text covers line ends and quotes, six times any byte.
Status messageArena(const Platform& p, const SlicePlan& plan, uint64_t now, tx::TxArena& arena) {
  auto alloc = p.allocLarge ? p.allocLarge : [](size_t n) { return std::malloc(n); };
  auto release = p.freeLarge ? p.freeLarge : [](void* q) { std::free(q); };
  tx::Item msg;
  msg.q = 1;
  msg.turn = 1;
  msg.kind = tx::ItemKind::User;
  msg.t = now;
  msg.x = plan.input;
  Status s;
  for (size_t grow : {size_t(2), size_t(6)}) {
    if (!arena.init(plan.input.size() * grow + 4096, alloc, release).ok())
      return Status::fail(Err::TooLarge, "plain::CompactDriver::ask");
    if ((s = arena.append(msg)).ok() || s.err != Err::TooLarge) break;
  }
  return s;
}

// An answer is taken when it is the model's whole text, not cut, and not thin for its input.
Status answerOf(const ExchangeResult& r, bool cut, const SlicePlan& plan, std::string& summary) {
  if (cut || r.err != ExchangeErr::None || r.step.stop == StopReason::MaxTokens || !r.step.calls.empty())
    return Status::fail(Err::Io, "plain::CompactDriver::ask");
  if (plan.input.size() >= kCompactThinSlice && r.step.text.size() < kCompactThinFloor)
    return Status::fail(Err::InvalidArgument, "plain::CompactDriver::ask");
  summary = r.step.text;
  return Status::success();
}

}  // namespace

namespace {

// What one rung answered.
struct RungAnswer {
  std::string summary;
  uint8_t requests = 0;
  bool parked = false;
};

// One summarizer exchange on one rung (the driver's attempt and a migration's digest 0).
Status askRung(const CompactDeps& d, const SummarizerRung& rung, const SlicePlan& plan, uint64_t now,
               RungAnswer& ans) {
  const WireCodec* wire = rung.wire;
  Status s = wire ? Status::success() : wireFor(rung.dialect, wire);
  if (!s.ok() || !wire) return s.ok() ? Status::fail(Err::NotFound, "plain::CompactDriver::ask") : s;
  tx::TxArena arena;
  if (!(s = messageArena(*d.platform, plan, now, arena)).ok()) return s;
  TurnCtx turn;
  turn.chatKey = d.chatKey;
  turn.chatId = d.chatId;
  turn.turn = 1;
  turn.trigger = Trigger::Notice;
  turn.host = rung.host;
  turn.model = rung.model;
  turn.dialect = rung.dialect;
  turn.startedAt = now;
  turn.startedMs = d.platform->nowMs ? d.platform->nowMs() : 0;
  SystemBlocks system;
  system.core = st::kPrompt;
  const std::vector<ToolSpec> tools;
  RenderInput in;
  in.turn = &turn;
  in.head = &rung.endpoint;
  in.system = &system;
  in.tools = &tools;
  in.items = &arena;
  // Ruling 89 item 4: a cut answer is asked once more with room to finish; never stored cut.
  for (uint16_t cap : {kSummarizerMaxTokens, kSummarizerRetryMaxTokens}) {
    in.maxTokens = cap;
    CappedWire capped(*wire, cap);
    ExchangeRequest req;
    req.wire = &capped;
    req.render = &in;
    req.timeoutMs = kCompactRequestMs;
    req.deadlineMs = kCompactRequestMs;
    ExchangeResult r;
    s = runExchange(ExchangeDeps{d.transport, d.platform}, req, r);
    ans.requests = (uint8_t)std::min<uint32_t>(255, ans.requests + r.requests);
    ans.parked |= r.parkHost;
    if (!s.ok()) return s;
    const bool cut = capped.cut || r.step.stop == StopReason::MaxTokens || r.err == ExchangeErr::Cut;
    if (!cut || cap == kSummarizerRetryMaxTokens) return answerOf(r, cut, plan, ans.summary);
  }
  return Status::fail(Err::Io, "plain::CompactDriver::ask");
}

}  // namespace

Status CompactDriver::ask(const SummarizerRung& rung, const SlicePlan& plan, uint64_t now, Asked& out) {
  RungAnswer ans;
  const Status s = askRung(d_, rung, plan, now, ans);
  out.summary = std::move(ans.summary);
  out.requests = ans.requests;
  out.parked = ans.parked;
  return s;
}

// The swap. The s line goes to storage first (log.appendLine), so a full arena never refuses a
// paid answer and a cut leaves storage ahead of RAM, never behind. Everything that can throw
// (the digest's copy into `live` included) happens before the store or after thru moved, so
// RAM never disagrees with storage about which items a digest covers.
Status CompactDriver::swap(const SlicePlan& plan, const std::string& summary, uint64_t now, CompactOutcome& out) {
  tx::HeadState& head = *d_.head();
  std::vector<tx::DigestEntry>& live = *d_.digests();
  tx::DigestEntry d;
  Status s = digestFor(plan, summary, d.text);
  if (!s.ok()) return s;
  if (!live.empty() && live.back().thru >= plan.thru) return Status::fail(Err::InvalidArgument, "plain::swap");
  d.day = plan.day;
  d.q = std::max<uint64_t>({head.nextQ, d_.arena->lastQ() + 1, d_.log->lastQ() + 1});
  d.thru = plan.thru;
  tx::Item item;
  std::string line;
  if (!(s = tx::digestItem(d, 0, now, item)).ok() || !(s = tx::encodeLine(item, line)).ok()) return s;
  live.push_back(d);   // taken back below unless storage holds the line
  tx::StoreState state = tx::StoreState::NotStored;
  try {
    s = d_.log->appendLine(line.data(), line.size(), state);
  } catch (const std::bad_alloc&) {   // whether the line landed is the log's to say
    s = Status::success();
    state = d_.log->lastQ() >= d.q ? tx::StoreState::Stored : tx::StoreState::NotStored;
  }
  if (!s.ok() || state != tx::StoreState::Stored) {   // storage refused: nothing anywhere, thru unmoved
    live.pop_back();
    out.step = CompactStep::Refused;
    return Status::success();
  }
  head.nextQ = d.q + 1;
  head.thru = d.thru;
  (void)d_.arena->dropThrough(d.thru);   // RAM now matches storage: each covered item shown once, as the digest
  (void)d_.arena->appendLine(line.data(), line.size());   // after the drop there is room
  out.step = CompactStep::Stored;
  out.digests = 1;
  out.turnsFolded = plan.turns;
  out.thru = head.thru;
  std::string block;
  tx::SettleReport rep;
  (void)tx::settleDigests(*d_.files, d_.chatKey, tx::profileOfHead(head.profile), live, block, rep);
  (void)d_.log->writeHead(head);   // a cache: a refusal is rebuilt from the s item
  if (!d_.chatsum.load || !d_.chatsum.save) return Status::success();
  const std::string blob = d_.chatsum.load();
  std::string mirrored;
  tx::MirrorFacts facts{d_.chatId, head.thru, plan.lastRow, (uint32_t)now};
  if (tx::mirrorChatsum(blob, facts, live, mirrored).ok() && mirrored != blob) (void)d_.chatsum.save(mirrored);
  return Status::success();
}

Status compactSlice(const CompactDeps& deps, const IdleCtx& ctx, CompactOutcome& out) {
  CompactDriver driver(deps);
  return driver.step(ctx, out);
}

// ---- digest 0 of a migration (tx/migrate.h) -----------------------------------------------------

namespace {

// The old summary as the assistant text of one closed turn at `at`, in an arena of its own: planSlice
// then renders it as the compaction input rules render any turn.
Status summaryArena(const Platform& p, const tx::Digest0Ask& ask, tx::TxArena& arena) {
  auto alloc = p.allocLarge ? p.allocLarge : [](size_t n) { return std::malloc(n); };
  auto release = p.freeLarge ? p.freeLarge : [](void* q) { std::free(q); };
  tx::Item a;
  a.q = 1;
  a.turn = 1;
  a.kind = tx::ItemKind::Assistant;
  a.t = ask.at;
  a.x = ask.summary;
  a.fin = true;
  tx::Item e;
  e.q = 2;
  e.turn = 1;
  e.kind = tx::ItemKind::TurnEnd;
  e.t = ask.at;
  e.end.cause = "import";
  Status s;
  for (size_t grow : {size_t(2), size_t(6)}) {
    if (!arena.init(ask.summary.size() * grow + 4096, alloc, release).ok())
      return Status::fail(Err::TooLarge, "plain::digest0Answer");
    arena.clear();
    if ((s = arena.append(a)).ok()) return arena.append(e);
    if (s.err != Err::TooLarge) break;
  }
  return s;
}

Status digest0Run(const CompactDeps& d, const tx::Digest0Ask& ask, tx::Digest0Answer& out) {
  std::vector<SummarizerRung> rungs;
  if (d.rungs) d.rungs(rungs);
  if (rungs.empty()) return Status::fail(Err::NotFound, "plain::digest0Answer");
  if (ask.summary.empty() || !ask.at) return Status::fail(Err::InvalidArgument, "plain::digest0Answer");
  const SummarizerRung& rung = rungs[std::min<size_t>(ask.attempt, rungs.size() - 1)];
  tx::TxArena arena;
  Status s = summaryArena(*d.platform, ask, arena);
  if (!s.ok()) return s;
  SliceRules rules;
  rules.now = ask.at + 2 * 86400;   // the turn is never "today" for the day rule
  rules.tzMinutes = ask.tzMinutes;
  rules.dayStartMin = ask.dayStartMin;
  rules.high = 1;
  rules.low = 0;
  rules.manual = true;
  SlicePlan plan;
  if (!(s = planSlice(arena, 0, rules, plan)).ok()) return s;
  if (!plan.thru) return Status::fail(Err::InvalidArgument, "plain::digest0Answer");
  RungAnswer ans;
  s = askRung(d, rung, plan, ask.now ? ask.now : rules.now, ans);
  if (ans.parked && d.park) d.park(rung);
  if (!s.ok()) return s;
  if (!(s = digestFor(plan, ans.summary, out.text)).ok()) return s;
  out.summary = ans.summary;
  out.day = plan.day;
  return Status::success();
}

}  // namespace

Status digest0Answer(const CompactDeps& deps, const tx::Digest0Ask& ask, tx::Digest0Answer& out) {
  out = tx::Digest0Answer{};
  if (!deps.platform || !deps.transport) return Status::fail(Err::InvalidArgument, "plain::digest0Answer");
  PsramFirstScope scope;
  try {
    Status s = digest0Run(deps, ask, out);
    if (!s.ok()) out = tx::Digest0Answer{};
    return s;
  } catch (const std::bad_alloc&) {
    out = tx::Digest0Answer{};
    return Status::fail(Err::TooLarge, "plain::digest0Answer");
  }
}

tx::Digest0Fn digest0Asker(CompactDeps deps) {
  return [deps](const tx::Digest0Ask& ask, tx::Digest0Answer& out) { return digest0Answer(deps, ask, out); };
}

namespace {
IdleStepFn idleOf(CompactDriver* driver) {
  return [driver](const IdleCtx& ctx) {
    CompactOutcome out;
    IdleOutcome o;
    o.worked = driver->step(ctx, out).ok() && out.ran;   // a request was one unit of idle work
    return o;
  };
}
}  // namespace

Status installCompaction(PlainHooks& hooks, const CompactDeps& deps) {
  if (!deps.platform || !deps.transport || !deps.log || !deps.arena)
    return Status::fail(Err::InvalidArgument, "plain::installCompaction");
  if (!headNamed(deps)) return noHead(deps, "plain::installCompaction.headKey");
  std::shared_ptr<CompactDriver> owned = std::make_shared<CompactDriver>(deps);
  IdleStepFn step = idleOf(owned.get());
  return hooks.idleStep(HookSlot::Compaction, [owned, step](const IdleCtx& ctx) { return step(ctx); });
}

Status installCompaction(PlainHooks& hooks, CompactDriver& driver) {
  return hooks.idleStep(HookSlot::Compaction, idleOf(&driver));
}

}  // namespace plain
}  // namespace agent
