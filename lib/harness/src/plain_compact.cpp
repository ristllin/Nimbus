// The compaction driver of engine 2 (plain/compact.h, lane RB-C2; PRD section 3).
#include "nimbus/harness/plain/compact.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <cstdlib>
#include <memory>
#include <new>
#include <utility>

#include "nimbus/harness/http.h"
#include "nimbus/harness/log.h"
#include "nimbus/harness/plain/exchange.h"
#include "nimbus/harness/plain/rails.h"
#include "nimbus/harness/plain/texts.h"
#include "nimbus/harness/platform.h"
#include "nimbus/harness/turn_memory.h"
#include "nimbus/orch/model_caps.h"   // the summarizer's structured form by row (lane E2-R4b)
#include "nimbus/orch/day_start.h"   // RB-REC5: the one day rule
#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/log.h"
#include "nimbus/orch/tx/rules.h"

namespace agent {
namespace plain {

namespace tx = nimbus::orch::tx;
namespace day = nimbus::orch::day;
namespace st = texts::summarizer;

namespace {
// The causes of an answer that was sent but not taken: only these earn the second try (never a storage
// or summary-in-force refusal, which a second request could not change).
const char kNotTakenRefused[] = "plain::CompactDriver.refused";
const char kNotTakenForm[] = "plain::CompactDriver.form";
const char kNotTakenHttp400[] = "plain::CompactDriver.http400";
const char kNotTakenChanges[] = "plain::CompactDriver.changes";
const char kNotTakenEmpty[] = "plain::CompactDriver.empty";
// Milliseconds left until `untilMs` on the platform clock (0 when past it; the clock may wrap).
uint32_t msLeft(const Platform& p, uint32_t untilMs) {
  const int32_t left = (int32_t)(untilMs - (p.nowMs ? p.nowMs() : untilMs));
  return left > 0 ? (uint32_t)left : 0;
}
// One request's window: its own 45 s, or less inside an urgent attempt (what is left of its time).
uint32_t requestWindow(const Platform& p, uint32_t untilMs) {
  return untilMs ? std::min(kCompactRequestMs, msLeft(p, untilMs)) : kCompactRequestMs;
}
bool answerNotTaken(const Status& s) {
  return s.err == Err::InvalidArgument && s.where &&
         (s.where == kNotTakenRefused || s.where == kNotTakenForm || s.where == kNotTakenHttp400 ||
          s.where == kNotTakenChanges || s.where == kNotTakenEmpty);
}

}  // namespace

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
    case CompactStep::Prepared: return "prepared";
    case CompactStep::Swapped: return "swapped";
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

// The stub rule's facts over the resident items: the pointer (the newest x item's stub) and the stub
// lists (tx::stubList: the tool fold's, never a superseded-read list), sorted.
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
    if (tx::stubList(it)) out.superseded.insert(out.superseded.end(), it.sup.begin(), it.sup.end());
  }
  std::sort(out.superseded.begin(), out.superseded.end());
  out.superseded.erase(std::unique(out.superseded.begin(), out.superseded.end()), out.superseded.end());
  return Status::success();
}
tx::ShowFacts showFactsOf(const ShowState& s) {   // between turns: no turn open, no ladder
  tx::ShowFacts f;
  f.pointer = s.pointer;
  f.superseded = &s.superseded;
  return f;
}
bool shownWhole(const tx::Item& r, const ShowState& s) {
  tx::ShowAs as = tx::ShowAs::Stub;
  return tx::showResult(r, showFactsOf(s), as).ok() && as == tx::ShowAs::Whole;
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
    case tx::ItemKind::Call: return n + it.id.size() + it.n.size() + tx::shownArgs(it, showFactsOf(s)).size();   // whole (E2-VERB)
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
  u.tainted |= itemOutside(it);   // one item rule (RB-RL: plain/rails.h)
  switch (it.kind) {
    case tx::ItemKind::User:
      u.owner = u.content = true;
      u.text += it.x.size();
      break;
    case tx::ItemKind::Assistant:
      u.content = true;
      u.text += it.x.size();
      break;
    case tx::ItemKind::Intent: u.intents.push_back(it.id); break;
    case tx::ItemKind::Result:
      u.intents.erase(std::remove(u.intents.begin(), u.intents.end(), it.id), u.intents.end());
      break;
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

// The oldest of the newest kNewestOwnerTurnsKept owner turns that fit under H (ruling 106 item 2:
// "never fewer than the newest six owner turns when they fit"); the newest owner turn always stays.
size_t newestOwnersFrom(const std::vector<Unit>& units, const SliceRules& r) {
  uint32_t owners = 0;
  size_t bytes = 0, from = units.size();
  for (size_t i = units.size(); i-- > 0;) {
    bytes += units[i].bytes;
    if (owners && bytes > r.high) return from;   // the older ones do not fit
    from = i;
    if (units[i].owner && ++owners == tx::kNewestOwnerTurnsKept) return i;
  }
  return owners ? from : units.size();   // fewer than six, all fitting: every one stays
}

// The first unit a slice may not reach.
size_t protectedFrom(const std::vector<Unit>& units, const SliceRules& r) {
  if (r.readBack) return heldFrom(units, r);
  return std::min(heldFrom(units, r), newestOwnersFrom(units, r));
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

// Drop a UTF-8 sequence cut at the end of `s`.
void clipUtf8Tail(std::string& s) {
  size_t n = s.size(), back = 0;
  while (n > 0 && back < 4 && ((unsigned char)s[n - 1] & 0xC0) == 0x80) --n, ++back;
  if (!n) return;
  const unsigned char lead = (unsigned char)s[n - 1];
  const size_t want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
  if (want > 1 && want != back + 1) s.resize(n - 1);
}

// A photo's note on the summarizer's input: one line, at most kPhotoNoteMax, '[' and ']' as '(' and
// ')' and control bytes as spaces, so a note (a migrated description, a transcription) cannot open
// a line of its own or a bracketed marker there.
std::string noteLine(const std::string& note) {
  if (note.empty()) return std::string();
  std::string out = " ";
  for (size_t i = 0; i < note.size() && i < tx::kPhotoNoteMax; ++i) {
    const unsigned char c = (unsigned char)note[i];
    out += c == '[' ? '(' : c == ']' ? ')' : c < 0x20 || c == 0x7f ? ' ' : (char)c;
  }
  clipUtf8Tail(out);
  return out;
}

void renderItem(const tx::Item& it, bool tainted, int16_t tz, std::string& out) {
  std::string hhmm;
  (void)tx::stubTime(it.t, tz, hhmm);
  switch (it.kind) {
    case tx::ItemKind::User:
      out += "[" + hhmm + "]" + st::kOwner + it.x;
      for (const tx::Part& p : it.parts)
        if (p.kind == tx::PartKind::Image) out += st::kPhoto + noteLine(p.note);
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

// A file-change call's path and whether it deletes: "path" in its arguments, or for a `more` call the
// tool it routes ("name") and that tool's "path" ("args": {"path": ...}). False when the call changes no
// file or names no path (or its arguments are a long-argument stub).
bool fileCall(const tx::Item& c, std::string& path, bool& deleted) {
  JsonDocument doc;
  if (c.args.empty() || c.args.size() > tx::kTier1CallArgs || deserializeJson(doc, c.args)) return false;
  const bool more = c.n == "more";
  const std::string tool = more ? std::string(doc["name"] | "") : c.n;
  const char* p = more ? (doc["args"]["path"] | (const char*)nullptr) : (doc["path"] | (const char*)nullptr);
  if (!fileChange(tool) || !p) return false;
  path = p;
  deleted = tool == "delete_file";
  return true;
}

// What renderUnit reads beside the text: the newest owner row, and the turn's file changes.
struct UnitFacts {
  uint64_t lastRow = 0;
  std::vector<FileNote> files;
};

Status renderUnit(const tx::TxArena& arena, const Unit& u, int16_t tz, std::string& out, UnitFacts& facts) {
  tx::Item it;
  std::vector<std::pair<std::string, FileNote>> calls;   // call id, its file change
  for (size_t i = u.first; i <= u.last; ++i) {
    Status s = arena.get(i, it);
    if (!s.ok()) return s;
    if (it.kind == tx::ItemKind::User && it.m > facts.lastRow) facts.lastRow = it.m;
    FileNote f;
    if (it.kind == tx::ItemKind::Call && fileCall(it, f.path, f.deleted)) calls.emplace_back(it.id, f);
    for (const auto& c : calls)   // a successful result of such a call: the device's Files line
      if (it.kind == tx::ItemKind::Result && it.ok && c.first == it.id)
        facts.files.push_back(FileNote{c.second.path, it.t, c.second.deleted});
    renderItem(it, u.tainted, tz, out);
  }
  return Status::success();
}

// The day line the input shows before the first turn of each day: "Tue 17 Nov 2026:".
std::string dayLine(const std::string& day) {
  std::string head;
  return tx::dayHeader(day, head).ok() ? head + ":\n" : std::string();
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

// May a turn with text join the slice: a dated turn, and what stays after it at least L (the owner's
// recent part), with at least keepText of owner and assistant text (the v5.1.2 keep, only for an
// owner's recent part that reaches it: ruling 106 item 2). The arena's room is the one
// real limit that may take a slice under them (ruling 91 table/F1): never silently, the plan says so.
bool joins(const Unit& u, const SliceRules& r, SlicePlan& plan) {
  if (!u.content) return true;
  if (u.day.empty()) return false;   // no clock reached it or its neighbors: no date to state
  if (r.manual) return true;
  const bool keepsL = plan.rest >= u.bytes && plan.rest - u.bytes >= r.low;
  if (keepsL && plan.restText >= u.text + r.keepText) return true;
  if (pressed(r, plan)) return plan.belowKeep = true;
  if (!plan.turns && keepsL) plan.textHeld = true;
  return false;
}

void take(const Unit& u, std::string& text, UnitFacts& facts, SlicePlan& out) {
  if (text.size() > kCompactSliceMaxBytes) {   // one turn over the input cap: summarized from its start
    clipUtf8(text, kCompactSliceMaxBytes - sizeof(st::kCut));
    text += st::kCut;
    text += '\n';
  }
  out.input += text;
  out.lastRow = facts.lastRow;
  out.files.insert(out.files.end(), facts.files.begin(), facts.files.end());
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

namespace {
// Device items alone are never a slice (they go with the next turn); stored items the arena could not
// hold are named, never silent.
void finishPlan(const SliceRules& r, uint64_t thru, SlicePlan& out) {
  if (out.turns) {
    if (r.omittedThru > thru)
      out.input.insert(0, std::string(st::kNotLoaded) + (r.searchBound ? st::kNotLoadedSearch : st::kNotLoadedEnd) + "\n");
    return;
  }
  const size_t resident = out.resident, text = out.restText;
  const bool held = out.textHeld;
  out = SlicePlan{};
  out.resident = out.rest = resident;
  out.restText = text;
  out.textHeld = held;
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
  const size_t stop = protectedFrom(units, r);
  std::string text, shownDay;
  for (size_t i = 0; i < stop && joins(units[i], r, out); ++i) {
    text.clear();
    if (units[i].content && units[i].day != shownDay) text = dayLine(units[i].day);   // a slice may span days
    UnitFacts facts;
    facts.lastRow = out.lastRow;
    if (!(s = renderUnit(arena, units[i], r.tzMinutes, text, facts)).ok()) return s;
    if (units[i].content && out.turns && out.input.size() + text.size() > kCompactSliceMaxBytes) break;
    if (units[i].content) shownDay = units[i].day;
    take(units[i], text, facts, out);
  }
  finishPlan(r, thru, out);
  return Status::success();
}

namespace {

// One closed unit's items of one kind that render whole and have a stub form, as the tool fold takes
// them (oldest first) while the transcript is still over `target`; `left` is what it still renders.
// Results first, then long call arguments (lane E2-VERB, ruling 106: older and larger results
// first, then older call arguments).
struct FoldScan {
  const tx::TxArena& arena;
  uint64_t thru;
  const ShowState& show;
  size_t target;
  tx::ItemKind kind;
};
bool foldable(const tx::Item& it, const ShowState& show) {
  if (it.st.empty()) return false;   // none to show
  if (it.kind == tx::ItemKind::Result) return shownWhole(it, show);
  return &tx::shownArgs(it, showFactsOf(show)) == &it.args;   // a call still showing its arguments
}
Status foldUnit(const FoldScan& f, const Unit& u, size_t& left, std::vector<uint64_t>& sup) {
  tx::Item it;
  for (size_t k = u.first; k <= u.last && left > f.target; ++k) {
    tx::ArenaEntry e;
    Status s = f.arena.header(k, e);
    if (!s.ok()) return s;
    if (e.q <= f.thru || e.kind != f.kind) continue;
    if (!(s = f.arena.get(k, it)).ok()) return s;
    if (!foldable(it, f.show)) continue;   // already a stub, or none to show
    const size_t whole = shownBytes(it, f.show);
    const size_t stub = kItemFramingBytes + it.id.size() + (it.kind == tx::ItemKind::Call ? it.n.size() : 0) + it.st.size();
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
  const size_t stop = protectedFrom(units, r);
  size_t left = rest;
  for (tx::ItemKind kind : {tx::ItemKind::Result, tx::ItemKind::Call})   // results, then arguments
    for (size_t i = 0; i < stop && left > r.low && units[i].closed; ++i)   // thru never jumps a turn; nor a fold
      if (!(s = foldUnit(FoldScan{arena, thru, show, r.low, kind}, units[i], left, sup)).ok()) return s;
  std::sort(sup.begin(), sup.end());
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

// The head a resident turn's t item names ("host/model"), read at every step (ruling 91 blockers/F5):
// the marks follow a head switched in the chat without a new driver (markHeads below). The host is
// read as the engine reads it (providerSlugOf: a slug, or an endpoint host such as
// api.anthropic.com); the installed head stands when the t item names no head or names its model
// (its routing is known only there).
tx::HeadKey headOfTurn(const tx::Item& it, const tx::HeadKey& installed) {
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

// Lane E2-R4b (router design section 6 item 6): the heads whose marks compaction keeps, the installed head
// first, then every head a resident t item names (each read as above, once). Its marks are the largest of
// theirs, so a switch to a smaller window adds no digest for the small head (the ladder leaves older turns
// out of its requests at render time) and a switch back renders every stored turn again.
std::vector<tx::HeadKey> markHeads(const tx::TxArena& arena, const tx::HeadKey& installed) {
  std::vector<tx::HeadKey> heads{installed};
  tx::Item it;
  for (size_t i = arena.count(); i-- > 0;) {
    tx::ArenaEntry e;
    if (!arena.header(i, e).ok() || e.kind != tx::ItemKind::TurnOpen || !arena.get(i, it).ok()) continue;
    const tx::HeadKey k = headOfTurn(it, installed);
    const bool seen = std::any_of(heads.begin(), heads.end(), [&k](const tx::HeadKey& h) {
      return h.host == k.host && h.model == k.model && h.routed == k.routed;
    });
    if (!seen) heads.push_back(k);
  }
  return heads;
}

// The budget with the largest marks among markHeads; the installed head's failure is the answer's.
Status largestMarks(const tx::TxArena& arena, const tx::HeadKey& installed, tx::Profile profile,
                    const tx::EngineSettings& set, tx::HeadBudget& out) {
  Status s = tx::budgetFor(installed, profile, set, out);
  if (!s.ok()) return s;
  tx::HeadBudget b;
  for (const tx::HeadKey& k : markHeads(arena, installed))
    if (tx::budgetFor(k, profile, set, b).ok() && b.marks.high > out.marks.high) out = b;
  return Status::success();
}

// The whole prompt of the newest turn's last request (its e item's lastPromptTokens), 0 when none
// is resident. Never the turn's summed promptTokens: a two-request turn on an OpenAI wire would read
// twice its size, and a cached Anthropic request would read only its uncached part (T4-ADV-01).
uint32_t lastPromptTokens(const tx::TxArena& arena, uint64_t& endQ) {
  endQ = 0;
  for (size_t i = arena.count(); i-- > 0;) {
    tx::ArenaEntry e;
    if (!arena.header(i, e).ok() || e.kind != tx::ItemKind::TurnEnd) continue;
    tx::Item it;
    endQ = e.q;
    return arena.get(i, it).ok() ? it.end.lastPromptTokens : 0;
  }
  return 0;
}

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
  if (ctx.ownerWaiting && ctx.urgency == Urgency::None) return Status::success();   // the owner's message goes first
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

// Lane E2-FIX9: a turn's attempt that cannot run says why in the log, every time (an idle step stays quiet: the
// next turn asks again), so a conversation never grows past its due mark without a line.
// (The reasons are whole log formats, never separate literals: the model-text catalog reads a plain engine file's
// string literals.)
bool CompactDriver::turnAsks(const IdleCtx& ctx) const { return ctx.urgency != Urgency::None; }

Status CompactDriver::run(const IdleCtx& ctx, CompactOutcome& out) {
  out.thru = d_.head()->thru;
  if (!ctx.clockSet || !ctx.now) {   // the day rule cannot be applied without a clock
    if (turnAsks(ctx)) hlog::logf("compact(%s): not compacted at this turn: the clock is not set", d_.chatKey.c_str());
    if (manual_) endManual(texts::device_notes::kCompactNoClock);
    return Status::success();
  }
  const bool urgent = ctx.urgency != Urgency::None;
  if (!manual_ && !urgent && !automaticMayAsk(ctx)) return Status::success();
  SlicePlan plan;
  bool due = false;
  Status s = planIfDue(ctx, plan, due);
  if (!s.ok()) {
    if (turnAsks(ctx))
      hlog::logf("compact(%s): not compacted at this turn: the slice could not be planned", d_.chatKey.c_str());
    if (manual_) endManual(texts::device_notes::kCompactFailed);   // answered, never left waiting
    return s;
  }
  if (ctx.urgency == Urgency::Now) due = cycle_ = true;   // the request does not fit: compact first
  if (!due) return urgent || manual_ ? s : prepare(ctx, out);
  return onDue(ctx, plan, out);
}

// Due: the summary prepared ahead is swapped in with no request, else one attempt (or nothing to take).
Status CompactDriver::onDue(const IdleCtx& ctx, const SlicePlan& plan, CompactOutcome& out) {
  if (!manual_ && (plan.thru || pendingValid())) return crossing(ctx, plan, out);
  if (pendingValid()) return swapPending(ctx.now, out);
  if (!plan.thru) {
    if (turnAsks(ctx))
      hlog::logf("compact(%s): not compacted at this turn: no turn may go (the newest six owner turns, an open turn or a "
                 "running job hold them)", d_.chatKey.c_str());
    return noSlice(plan, out);
  }
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
  // v5.1.2's text keep binds only an owner's recent part that reaches it (ruling 106 item 2); an owner's
  // smaller profile is his choice.
  rules.keepText = b.profile == tx::Profile::Full && b.marks.low >= b.releasedKeep ? b.releasedKeep : 0;
  rules.manual = manual_;
  rules.jobRunning = d_.jobRunning;
  rules.omittedThru = d_.omittedThru ? d_.omittedThru() : 0;
  rules.arenaFree = d_.arena->freeBytes();
  rules.arenaRoom = compactArenaRoom(d_.arena->capacity());
  rules.searchBound = d_.searchBound;
  return rules;
}

Status CompactDriver::planIfDue(const IdleCtx& ctx, SlicePlan& plan, bool& due) {
  // The head's marks at its profile (tx::budgetFor). No head named: nothing is compacted, never a
  // fixed smaller profile in its place (ruling 83 item 4).
  if (!headNamed(d_)) return noHead(d_, "plain::CompactDriver.headKey");
  tx::HeadBudget& b = budget_;
  const tx::EngineSettings set = d_.settings ? d_.settings() : tx::EngineSettings{};
  Status s = largestMarks(*d_.arena, d_.headKey, tx::profileOfHead(d_.head()->profile), set, b);
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
  uint64_t endQ = 0;
  const bool hot = lastPromptTokens(*d_.arena, endQ) >= b.tokens.due && endQ != spentHotQ_;   // each reading once
  const bool room = rules_.arenaFree < rules_.arenaRoom;
  if (plan.resident > b.marks.high || hot || room) cycle_ = true;
  hotQ_ = endQ;
  due = manual_ || cycle_;
  if (cycle_ && !plan.thru && (!plan.textHeld || plan.resident <= b.marks.low)) endCycle();   // L reached, or nothing may go
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
  if (!back.init(kCompactReadBackBytes, alloc, release).ok()) return Status::success();   // short of memory: the RAM plan
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
    if (s.ok()) endCycle();   // nothing left to fold: the cycle ends until the transcript changes
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
  // A wait longer than the longest one was set by a clock that ran ahead: it does not hold. Lane E2-FIX9: nor does
  // any wait for a turn that opens over the due mark (or a request that does not fit), nor the daily count: it tries
  // again, inside kCompactUrgentMs, at every turn until one succeeds, instead of growing for hours (on the bench unit
  // a conversation grew from 36k to 169k tokens with no attempt and no line); idle steps keep both, and a turn's
  // attempt past the count says so.
  const bool urgent = ctx.urgency != Urgency::None;
  const bool waits = !urgent && chat.next > ctx.now && chat.next <= ctx.now + compactWaitS(UINT32_MAX);
  const bool capped = ctl_.paid >= kCompactPaidPerDay;
  if (urgent && capped)
    hlog::logf("compact(%s): %u attempts today, past the daily %u: a turn over the due mark asks anyway", d_.chatKey.c_str(),
               (unsigned)ctl_.paid, (unsigned)kCompactPaidPerDay);
  return waits || (capped && !urgent);
}

// One attempt: the daily cap and this chat's wait, one rung, one exchange, the swap.
Status CompactDriver::attempt(const IdleCtx& ctx, const SlicePlan& plan, CompactOutcome& out, bool prepareOnly) {
  loadCtl();
  const CompactCtl::Chat before = ctl_.chat(d_.chatKey);
  out.step = CompactStep::Waiting;
  if (held(ctx, before)) return Status::success();
  std::vector<SummarizerRung> rungs;
  if (d_.rungs) d_.rungs(rungs);
  if (rungs.empty()) {
    if (turnAsks(ctx)) hlog::logf("compact(%s): not compacted at this turn: no summarizer is set up", d_.chatKey.c_str());
    if (manual_) endManual(texts::device_notes::kCompactFailed);
    return Status::success();
  }
  const SummarizerRung& rung = rungs[rungFor(before.fails, rungs.size())];
  ctl_.paid += 1;   // one attempt, counted before it is sent: a cut during the request still counts
  saveCtl();
  ++windowAsks_;
  manualAsks_ += manual_;
  if (ctx.urgency != Urgency::None && ctx.waiting) ctx.waiting();   // the turn waits: its status line says so
  tz_ = ctx.tzMinutes;
  bool parked = false;
  const Status s = askAndKeep(ctx, rung, plan, prepareOnly, out, parked);
  if (parked && d_.park) d_.park(rung);
  settle(rung, plan, before.fails, ctx.now, s, out);
  return s.err == Err::TooLarge ? s : Status::success();
}

// The form a rung is asked in first: lines for a model whose provider refused the structured form by name.
AnswerForm CompactDriver::formFor(const SummarizerRung& rung) const {
  const std::string key = rung.host + "/" + rung.model;
  return std::find(noForm_.begin(), noForm_.end(), key) != noForm_.end() ? AnswerForm::Lines : firstAnswerForm(rung);
}

// A provider that refused the form by name: its model is asked in lines from now on (until a restart).
void CompactDriver::noteForm(const SummarizerRung& rung, const Status& s) {
  const std::string key = rung.host + "/" + rung.model;
  if (s.where != kNotTakenForm || noForm_.size() >= kNoFormModels ||
      std::find(noForm_.begin(), noForm_.end(), key) != noForm_.end())
    return;
  noForm_.push_back(key);
  hlog::logf("compact(%s): %s takes no structured form: lines from now on", d_.chatKey.c_str(), key.c_str());
}

// The second try: only for an answer sent and not taken, never after a refusal (the same content goes to the same
// model), never past an urgent attempt's time.
bool CompactDriver::secondTry(const Status& s, const Asked& asked, AnswerForm form, uint32_t untilMs) const {
  if (!answerNotTaken(s) || s.where == kNotTakenRefused || !asked.requests || asked.parked || form != AnswerForm::Changes)
    return false;
  if (untilMs && msLeft(*d_.platform, untilMs) < kCompactMinRequestMs) {
    hlog::logf("compact(%s): no time left in this turn for a second try", d_.chatKey.c_str());
    return false;
  }
  return true;
}

// The exchange and the merge. An answer that was sent but not taken (secondTry) is asked once more at once as free
// lines, then the stated wait holds.
Status CompactDriver::askAndKeep(const IdleCtx& ctx, const SummarizerRung& rung, const SlicePlan& plan,
                                 bool prepareOnly, CompactOutcome& out, bool& parked) {
  AnswerForm form = formFor(rung);
  const bool urgent = ctx.urgency != Urgency::None && d_.platform->nowMs;
  // An urgent attempt's end; inside a crossing, the crossing's own (lane E2-FIX15: one bound for all its slices).
  const uint32_t untilMs = !urgent ? 0 : crossingUntilMs_ ? crossingUntilMs_ : d_.platform->nowMs() + kCompactUrgentMs;
  Status s;
  for (int tries = 0; tries < 2; ++tries) {
    Asked asked;
    s = ask(rung, plan, ctx.now, form, untilMs, asked);
    out.requests = (uint8_t)std::min<uint32_t>(255, out.requests + asked.requests);
    out.ran = out.ran || asked.requests > 0;
    parked = parked || asked.parked;
    noteForm(rung, s);
    if (s.ok()) s = keep(plan, asked.summary, ctx.now, prepareOnly, out);
    if (!secondTry(s, asked, form, untilMs)) return s;
    form = AnswerForm::Lines;
    ctl_.paid += 1;   // the second request is counted like the first
    saveCtl();
    hlog::logf("compact(%s): asking once more as lines", d_.chatKey.c_str());
  }
  return s;
}

// The attempt's outcome into the control record: a stored digest clears the chat's failures,
// anything else counts one and sets the wait.
void CompactDriver::settle(const SummarizerRung& rung, const SlicePlan& plan, uint32_t fails, uint64_t now,
                           const Status& s, CompactOutcome& out) {
  CompactCtl::Chat& c = ctl_.chat(d_.chatKey);
  if (s.ok() && (out.step == CompactStep::Stored || out.step == CompactStep::Prepared)) {
    c.fails = 0;
    c.next = 0;
    manualStored_ += manual_;
    hlog::logf("compact(%s): summary %s through q%llu (%s), %u turns on %s/%s", d_.chatKey.c_str(),
               out.step == CompactStep::Prepared ? "prepared" : "updated", (unsigned long long)plan.thru,
               plan.day.c_str(), (unsigned)plan.turns, rung.host.c_str(), rung.model.c_str());
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
Status messageArena(const Platform& p, const std::string& message, uint64_t now, tx::TxArena& arena) {
  auto alloc = p.allocLarge ? p.allocLarge : [](size_t n) { return std::malloc(n); };
  auto release = p.freeLarge ? p.freeLarge : [](void* q) { std::free(q); };
  tx::Item msg;
  msg.q = 1;
  msg.turn = 1;
  msg.kind = tx::ItemKind::User;
  msg.t = now;
  msg.x = message;
  Status s;
  for (size_t grow : {size_t(2), size_t(6)}) {
    if (!arena.init(message.size() * grow + 4096, alloc, release).ok())
      return Status::fail(Err::TooLarge, "plain::CompactDriver::ask");
    if ((s = arena.append(msg)).ok() || s.err != Err::TooLarge) break;
  }
  return s;
}

// How an answer not taken began, for its log line (ruling 116: a failure leaves its cause in the log; the
// answer never did before lane E2-CMP): the first bytes, cut whole, line ends shown as '|'.
std::string answerHead(const std::string& answer) {
  constexpr size_t kHead = 80;   // the whole line stays under hlog's 192 B with a long chat key
  size_t n = std::min(answer.size(), kHead);
  while (n < answer.size() && n > 0 && ((unsigned char)answer[n] & 0xC0) == 0x80) --n;
  std::string head = answer.substr(0, n);
  for (char& c : head)
    if (c == '\n' || c == '\r' || c == '\t') c = '|';
  return head;
}

// An answer is taken when it is the model's whole text, not cut, not refused, and not thin for its input.
// `thinInput`: the input size the thin rule reads (a day digest of migration's digest 0); 0: none (an
// update's answer is changes, and "=" is a whole answer). A refusal is an answer not taken
// (InvalidArgument, so the second try runs); a cut or a failed exchange is Io.
Status answerOf(const std::string& chatKey, const ExchangeResult& r, bool cut, size_t thinInput, std::string& summary) {
  if (r.err == ExchangeErr::None && r.step.stop == StopReason::Refusal) {
    const std::string& said = r.step.refusal.empty() ? r.step.text : r.step.refusal;
    hlog::logf("compact(%s): answer not taken (refused, %u B): %s", chatKey.c_str(), (unsigned)said.size(), answerHead(said).c_str());
    return Status::fail(Err::InvalidArgument, kNotTakenRefused);
  }
  if (cut || r.err != ExchangeErr::None || r.step.stop == StopReason::MaxTokens || !r.step.calls.empty())
    return Status::fail(Err::Io, "plain::CompactDriver::ask");
  if (thinInput >= kCompactThinSlice && r.step.text.size() < kCompactThinFloor)
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
  nimbus::orch::TokenUsage usage;   // every billed response of the rung's requests (lane E2-CMP)
};

// One summarizer exchange on one rung (the driver's attempt and a migration's digest 0).
// The summarizer's request: the summary so far (system text), the one message, and the input size the
// thin rule reads (0: none).
struct RungMessage {
  std::string sofar, message;
  size_t thinInput = 0;
  const char* schema = nullptr;   // the answer's structured form (AnswerForm::Changes); null: free text
  // An empty change list over this slice is a whole answer: true on a row whose structured summary is
  // recorded working (Anthropic's); false elsewhere, where it is asked once more as lines (lane E2-R4b).
  bool emptyListTaken = true;
  uint32_t untilMs = 0;           // an urgent attempt's end on the platform clock; 0: none (lane E2-FIX7)
};

// The provider's own words in a refusal body: its "message" when it has one, else the body.
std::string providerWords(const std::string& body) {
  static const char kKey[] = "\"message\":\"";
  const size_t at = body.find(kKey);
  return at == std::string::npos ? body : body.substr(at + sizeof(kKey) - 1);
}

// A provider's refusal body that names the structured form (its field or its type).
bool namesTheForm(const std::string& body) {
  std::string low = body;
  for (char& c : low) c = (char)std::tolower((unsigned char)c);
  for (const char* w : {"output_config", "output_format", "json_schema", "structured"})
    if (low.find(w) != std::string::npos) return true;
  return false;
}

// One exchange's answer, taken or not (answerOf), and the structured form read back as lines.
Status takeAnswer(const std::string& chatKey, const RungMessage& m, const ExchangeResult& r, bool cut, std::string& summary) {
  if (m.schema && r.err == ExchangeErr::Other && r.httpStatus == 400) {
    // Lane E2-FIX7 (M6): the form is named as the cause only when the provider's own words name it (and then the
    // model is asked in lines from then on); any other HTTP 400 is logged as what it is, with the provider's words,
    // and asked once more in lines without marking the model.
    if (namesTheForm(r.providerBody)) {
      hlog::logf("compact(%s): answer not taken (the form was refused, %u B): %s", chatKey.c_str(),
                 (unsigned)r.providerBody.size(), answerHead(providerWords(r.providerBody)).c_str());
      return Status::fail(Err::InvalidArgument, kNotTakenForm);
    }
    hlog::logf("compact(%s): the summarizer request was refused (HTTP 400, %u B): %s", chatKey.c_str(),
               (unsigned)r.providerBody.size(), answerHead(providerWords(r.providerBody)).c_str());
    return Status::fail(Err::InvalidArgument, kNotTakenHttp400);   // lines once more; the model is not marked
  }
  const Status s = answerOf(chatKey, r, cut, m.thinInput, summary);
  if (!s.ok() || !m.schema) return s;
  // The change list read back as lines. A text that is not that object (a host that ignored the form)
  // is read as lines as it stands: the parser decides, and no request is paid twice for it.
  std::string lines;
  if (!changeLinesOf(summary, lines)) return s;
  if (lines == "=" && !m.emptyListTaken && !m.message.empty()) {
    hlog::logf("compact(%s): answer not taken (an empty change list on a row without a recorded structured summary)",
               chatKey.c_str());
    return Status::fail(Err::InvalidArgument, kNotTakenEmpty);
  }
  summary = std::move(lines);
  return s;
}

// The summary so far rides in the system text (the one message keeps under a line's cap), AHEAD of the prompt
// (lane E2-FIX9): the prompt ends with its "nothing to change" answer, "=", and a summary right after it read as
// "=Facts and standing preferences:", which the summarizer then wrote as its answer for a slice with nothing new
// (evidence/probes.jsonl: 2 of 12 with the summary after the prompt, 0 of 12 with it first). The blocks render in
// the order core, notes, digests; an empty one is left out.
SystemBlocks summarizerSystem(const std::string& sofar) {
  SystemBlocks system;
  system.core = sofar.empty() ? std::string(st::kPrompt) : sofar;   // no summary yet: the prompt alone, as before
  if (!sofar.empty()) system.digests = st::kPrompt;
  return system;
}

Status askRung(const CompactDeps& d, const SummarizerRung& rung, const RungMessage& m, uint64_t now,
               RungAnswer& ans) {
  const WireCodec* wire = rung.wire;
  Status s = wire ? Status::success() : wireFor(rung.dialect, wire);
  if (!s.ok() || !wire) return s.ok() ? Status::fail(Err::NotFound, "plain::CompactDriver::ask") : s;
  tx::TxArena arena;
  if (!(s = messageArena(*d.platform, m.message, now, arena)).ok()) return s;
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
  const SystemBlocks system = summarizerSystem(m.sofar);
  const std::vector<ToolSpec> tools;
  RenderInput in;
  in.turn = &turn;
  in.head = &rung.endpoint;
  in.system = &system;
  in.tools = &tools;
  in.items = &arena;
  in.answerSchema = m.schema;
  // Ruling 89 item 4: a cut answer is asked once more with room to finish; never stored cut. Lane E2-R4b
  // (router design section 6 item 8): the two rungs are cap CEILINGS the exchange applies inside its own cap
  // rule and time fit (pre-flight), so an uncapped row sends none, a slow model is asked for what fits, and
  // the second rung is the exchange's cut retry, sent only when it gives more room (the retry-gain rule).
  const uint32_t window = requestWindow(*d.platform, m.untilMs);
  if (window < kCompactMinRequestMs) return Status::fail(Err::Io, "plain::CompactDriver.time");
  ExchangeRequest req;
  req.wire = wire;
  req.render = &in;
  req.timeoutMs = window;
  req.deadlineMs = window;
  req.capCeiling = kSummarizerMaxTokens;
  req.retryCeiling = kSummarizerRetryMaxTokens;
  req.summary = true;   // E2-REP5: RoundKind::Fold, the design's summary kind (section 6 item 8)
  ExchangeResult r;
  s = runExchange(ExchangeDeps{d.transport, d.platform}, req, r);
  ans.requests = (uint8_t)std::min<uint32_t>(255, ans.requests + r.requests);
  if (r.step.usage.responses) ans.usage += r.step.usage;   // paid whether or not the answer is taken
  ans.parked |= r.parkHost;
  if (!s.ok()) return s;
  const bool cut = r.step.stop == StopReason::MaxTokens || r.err == ExchangeErr::Cut;
  return takeAnswer(d.chatKey, m, r, cut, ans.summary);
}

}  // namespace

// The row's structured summary (lane E2-R4b, router design section 5 item 8): none for a host no table
// slot names (a routed or custom head: an Anthropic-shaped proxy may not have the form).
namespace {
nimbus::orch::caps::Structured summarizerStructuredOf(const SummarizerRung& rung) {
  namespace caps = nimbus::orch::caps;
  caps::Slot slot;
  if (!caps::slotFromHost(rung.host.c_str(), &slot) || rung.endpoint.routed) return caps::Structured::None;
  return caps::capsFor(slot, rung.model.c_str(), false).row.summarizerStructured;
}
}  // namespace

AnswerForm firstAnswerForm(const SummarizerRung& rung) {
  return summarizerStructuredOf(rung) == nimbus::orch::caps::Structured::None ? AnswerForm::Lines : AnswerForm::Changes;
}

bool changeLinesOf(const std::string& json, std::string& lines) {
  lines.clear();
  JsonDocument doc;
  if (deserializeJson(doc, json) || !doc.is<JsonObjectConst>() || !doc["changes"].is<JsonArrayConst>()) return false;
  JsonArrayConst list = doc["changes"].as<JsonArrayConst>();
  for (JsonVariantConst v : list) {
    if (!v.is<const char*>()) return lines.clear(), false;
    std::string item = v.as<const char*>();
    for (char& c : item)
      if (c == '\r') c = '\n';
    lines += item + "\n";   // an item holding line ends is several lines, as free text would be
  }
  if (list.size() == 0) lines = "=";
  return true;
}

Status CompactDriver::ask(const SummarizerRung& rung, const SlicePlan& plan, uint64_t now, AnswerForm form,
                          uint32_t untilMs, Asked& out) {
  RungAnswer ans;
  RungMessage m;
  m.schema = form == AnswerForm::Changes ? st::kChangesSchema : nullptr;
  m.emptyListTaken = summarizerStructuredOf(rung) == nimbus::orch::caps::Structured::AnthropicOutputConfig;
  m.untilMs = untilMs;
  Status s = messageFor(plan, m.sofar, m.message);
  if (s.ok()) s = askRung(d_, rung, m, now, ans);
  if (ans.usage.responses && d_.spent) d_.spent(rung, ans.usage);   // every paid request is counted
  out.summary = std::move(ans.summary);
  out.requests = ans.requests;
  out.parked = ans.parked;
  return s;
}

// The swap. The s line goes to storage first (log.appendLine), so a full arena never refuses a
// paid answer and a cut leaves storage ahead of RAM, never behind. Everything that can throw
// (the digest's copy into `live` included) happens before the store or after thru moved, so
// RAM never disagrees with storage about which items a digest covers.
Status CompactDriver::swap(const SlicePlan& plan, const std::string& text, uint64_t now, CompactOutcome& out) {
  tx::HeadState& head = *d_.head();
  std::vector<tx::DigestEntry>& live = *d_.digests();
  tx::DigestEntry d;
  d.text = text;
  if (!live.empty() && live.back().thru >= plan.thru) return Status::fail(Err::InvalidArgument, "plain::swap");
  // The day digests the summary took in go to the archive first, so a restart that stops at the newest
  // summary (tx::loadDigests) leaves none behind; a refusal stores nothing and is tried again.
  if (!archiveLegacy().ok()) {
    out.step = CompactStep::Refused;
    return Status::success();
  }
  d.day = plan.day;
  d.q = std::max<uint64_t>({head.nextQ, d_.arena->lastQ() + 1, d_.log->lastQ() + 1});
  d.thru = plan.thru;
  tx::Item item;
  std::string line;
  Status s;
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

// ---- the cumulative summary: what an update builds on, the merge, ahead of time ------------------

namespace {

// A new line of a slice that read outside content sits inside the data frame; any line's frame tags
// are made inert first, so no summary line opens or closes a frame.
std::string framedLine(const std::string& clean) { return std::string(st::kFrameOpen) + inert(clean) + st::kFrameClose; }
std::string inertLine(const std::string& clean) { return inert(clean); }
bool framedIn(const tx::Summary& s, uint32_t id) {
  for (const tx::SummaryLine& l : s.lines)
    if (l.id == id) return l.text.compare(0, sizeof(st::kFrameOpen) - 1, st::kFrameOpen) == 0;
  return true;   // no such line: applySummaryEdits counts it unread
}

}  // namespace

bool CompactDriver::pendingValid() const {
  return !pending_.text.empty() && pending_.baseThru == d_.head()->thru && pending_.thru > d_.head()->thru &&
         pending_.thru <= d_.arena->lastQ();
}

Status CompactDriver::baseFor(tx::Summary& base, std::string& baseText, std::string& legacy) const {
  base = tx::Summary{};
  baseText.clear();
  legacy.clear();
  if (pendingValid()) baseText = pending_.text;
  const std::vector<tx::DigestEntry>& live = *d_.digests();
  for (size_t i = live.size(); baseText.empty() && i-- > 0;)
    if (tx::isSummaryText(live[i].text)) baseText = live[i].text;
  if (baseText.empty()) {   // no summary yet: the day digests the chat holds are read as dated lines
    for (const tx::DigestEntry& d : live) legacy += d.text + "\n";
    return Status::success();
  }
  const Status s = tx::parseSummary(baseText, base);
  if (!s.ok()) hlog::logf("compact(%s): the summary in force does not read back: not updated", d_.chatKey.c_str());
  return s;
}

Status CompactDriver::messageFor(const SlicePlan& plan, std::string& sofar, std::string& message) const {
  tx::Summary base;
  std::string legacy;
  const Status s = baseFor(base, sofar, legacy);
  if (!s.ok()) return s;
  if (base.lines.empty() && !base.archived) sofar.clear();
  message = std::string(st::kSince) + "\n" + legacy + plan.input;
  return Status::success();
}

// The answer's changes applied to the summary so far in code (never a rewrite), the device's Files
// lines, the head's budget; every line that left goes to the archive before anything is kept.
// A taken answer: merged into the summary, then kept ahead of time or swapped in.
Status CompactDriver::keep(const SlicePlan& plan, const std::string& answer, uint64_t now, bool prepareOnly,
                           CompactOutcome& out) {
  if (plan.belowKeep)
    hlog::logf("compact(%s): the arena has %u B free: this slice leaves under the v5.1.2 text keep (%u B kept)",
               d_.chatKey.c_str(), (unsigned)d_.arena->freeBytes(), (unsigned)(plan.restText));
  std::string text;
  const Status s = merge(plan, answer, text);
  if (!s.ok()) return s;
  if (!prepareOnly) return swap(plan, text, now, out);
  keepPending(plan, text, out);
  return s;
}

Status CompactDriver::merge(const SlicePlan& plan, const std::string& answer, std::string& text) {
  tx::Summary sum;
  std::string baseText, legacy;
  Status s = baseFor(sum, baseText, legacy);
  if (!s.ok()) return s;
  const tx::SummaryEdits edits = tx::parseSummaryEdits(answer);
  if (!edits.taken()) {   // prose or a rewrite: never taken as the summary
    hlog::logf("compact(%s): the answer held no change (%u lines unread): not taken", d_.chatKey.c_str(),
               (unsigned)edits.unread);
    hlog::logf("compact(%s): answer not taken (no change line, %u B): %s", d_.chatKey.c_str(), (unsigned)answer.size(), answerHead(answer).c_str());
    return Status::fail(Err::InvalidArgument, kNotTakenChanges);
  }
  if (edits.frame && edits.edits.empty() && !edits.none)   // lane E2-FIX9: "=Facts and standing preferences:"
    hlog::logf("compact(%s): the answer named %u summary titles and no change: nothing to change", d_.chatKey.c_str(),
               (unsigned)edits.frame);
  if (edits.numbered)
    hlog::logf("compact(%s): %u adds numbered from 1: the numbers were read as ids", d_.chatKey.c_str(),
               (unsigned)edits.numbered);
  std::vector<tx::SummaryLine> left;
  size_t unread = 0;
  if (!(s = takeEdits(plan, edits.edits, sum, left, unread)).ok()) return s;
  addFiles(plan, sum, left);
  const size_t cap = budget_.summaryBytes ? budget_.summaryBytes : tx::kSummaryMaxBytes;
  if (!(s = tx::fitSummary(sum, cap, left)).ok() || !(s = tx::encodeSummary(sum, text)).ok()) return s;
  if (unread + edits.unread)
    hlog::logf("compact(%s): %u lines of the answer were not changes", d_.chatKey.c_str(), (unsigned)(unread + edits.unread));
  return archiveLeft(plan, left);
}

// The answer's changes on the summary; a slice that read outside content rewrites or drops only framed lines.
Status CompactDriver::takeEdits(const SlicePlan& plan, const std::vector<tx::SummaryEdit>& edits, tx::Summary& sum,
                                std::vector<tx::SummaryLine>& left, size_t& unread) const {
  size_t held = 0;
  std::vector<tx::SummaryEdit> taken;
  for (const tx::SummaryEdit& e : edits)
    if (plan.tainted && e.op != tx::SummaryOp::Add && !framedIn(sum, e.id)) ++held;
    else taken.push_back(e);
  const Status s = tx::applySummaryEdits(sum, taken, plan.tainted ? framedLine : inertLine, left, unread);
  unread += held;
  return s;
}

// The device's Files lines from the slice's file changes; never a frame tag in the system text.
void CompactDriver::addFiles(const SlicePlan& plan, tx::Summary& sum, std::vector<tx::SummaryLine>& left) const {
  for (const FileNote& f : plan.files) {
    if (f.path.find_first_of("<>") != std::string::npos) continue;
    std::string when;
    if (!tx::photoWhen(f.at, tz_, when).ok()) when.clear();
    tx::noteSummaryFile(sum, f.path, when, f.deleted, left);
  }
}

// Every line that left goes to the archive before anything is kept (a refusal: the attempt fails).
Status CompactDriver::archiveLeft(const SlicePlan& plan, const std::vector<tx::SummaryLine>& left) const {
  if (left.empty()) return Status::success();
  if (!d_.files) {   // no card: nothing can hold them; the conversation itself is not stored either
    hlog::logf("compact(%s): %u summary lines left with no storage to archive them", d_.chatKey.c_str(), (unsigned)left.size());
    return Status::success();
  }
  const Status s = tx::archiveSummaryLines(*d_.files, d_.chatKey, plan.day, left);
  if (!s.ok()) hlog::logf("compact(%s): %u summary lines could not be archived: not taken", d_.chatKey.c_str(), (unsigned)left.size());
  return s;
}

Status CompactDriver::archiveLegacy() {
  if (!d_.files) return Status::success();
  for (const tx::DigestEntry& d : *d_.digests()) {
    if (tx::isSummaryText(d.text)) continue;
    const Status s = tx::archiveDigest(*d_.files, d_.chatKey, d);
    if (!s.ok()) return s;
  }
  return Status::success();
}

void CompactDriver::keepPending(const SlicePlan& plan, const std::string& text, CompactOutcome& out) {
  const bool chained = pendingValid();
  pending_.baseThru = chained ? pending_.baseThru : d_.head()->thru;
  pending_.turns = (chained ? pending_.turns : 0) + plan.turns;
  pending_.text = text;
  pending_.thru = plan.thru;
  pending_.lastRow = plan.lastRow;
  pending_.day = plan.day;
  out.step = CompactStep::Prepared;
}

// Ahead of time: once the last request reached kPreparePercent of the due mark, the turns already older
// than the recent part go into the pending summary (one slice per step, the usual gates).
Status CompactDriver::prepare(const IdleCtx& ctx, CompactOutcome& out) {
  uint64_t endQ = 0;
  if ((uint64_t)lastPromptTokens(*d_.arena, endQ) * 100 < (uint64_t)budget_.tokens.due * kPreparePercent) return Status::success();
  if (!pendingValid()) pending_ = Pending{};
  SlicePlan plan;
  const Status s = planSlice(*d_.arena, pendingValid() ? pending_.thru : d_.head()->thru, rules_, plan);
  if (!s.ok() || !plan.thru) return s;
  return attempt(ctx, plan, out, true);
}

// Lane E2-FIX15: a crossing of the due mark is one event. Slice after slice goes into the pending summary (no request
// changes meanwhile) until no turn may go, an urgent step's time or kCompactCrossingSlices is spent, or an attempt
// fails; then the summary is swapped in once. The cycle ends there unless slices remain that only the slice count or
// the time held back (a very large backlog: the next step goes on with it). Before, the cycle stayed open while the newest six
// owner turns held more than the recent part, so every turn open folded the one turn that had just aged out of them
// and swapped (the bench: 15 updates in a row, each rewriting the whole cached request).
Status CompactDriver::crossing(const IdleCtx& ctx, SlicePlan plan, CompactOutcome& out) {
  if (pendingValid()) {   // prepared ahead of time (ruling 106 item 3): swapped at once, no request, no wait
    endCycle();
    return swapPending(ctx.now, out);
  }
  uint32_t slices = 0;
  bool failed = false;
  const Status prep = prepareSlices(ctx, plan, out, slices, failed);
  if (!prep.ok()) return prep;
  if (failed || !plan.thru) endCycle();
  if (!pendingValid()) return Status::success();   // nothing prepared: the failed attempt logged its cause
  const uint8_t requests = out.requests;
  const bool ran = out.ran;
  const Status s = swapPending(ctx.now, out);
  out.requests = requests;
  out.ran = ran;
  if (out.step == CompactStep::Swapped) out.step = CompactStep::Stored;   // asked in this crossing
  hlog::logf("compact(%s): one summary write for this crossing (%u slices prepared, %u requests)", d_.chatKey.c_str(),
             (unsigned)slices, (unsigned)requests);
  return s.err == Err::TooLarge ? s : Status::success();   // as attempt: a refused swap stores nothing and is tried again
}

// A crossing's slices into the pending summary, one attempt each, while crossingMayAsk; `plan` is what is left after
// them. The turn's status line says once that it waits, however many slices. `failed`: an attempt was not taken.
Status CompactDriver::prepareSlices(const IdleCtx& ctx, SlicePlan& plan, CompactOutcome& out, uint32_t& slices,
                                    bool& failed) {
  const bool urgent = ctx.urgency != Urgency::None;
  const uint32_t startMs = urgent && d_.platform->nowMs ? d_.platform->nowMs() : 0;
  crossingUntilMs_ = urgent && d_.platform->nowMs ? startMs + kCompactUrgentMs : 0;
  bool said = false;
  IdleCtx once = ctx;
  once.waiting = [&ctx, &said] {
    if (!said && ctx.waiting) ctx.waiting();
    said = true;
  };
  for (; plan.thru && slices < kCompactCrossingSlices && crossingMayAsk(urgent, startMs); ++slices) {
    CompactOutcome one;
    const Status s = attempt(once, plan, one, true);
    out.requests = (uint8_t)std::min<uint32_t>(255, out.requests + one.requests);
    out.ran = out.ran || one.ran;
    if (!s.ok() || one.step != CompactStep::Prepared) {
      out.step = one.step;
      failed = s.ok() && one.step != CompactStep::Waiting;   // held by a wait or the count: the cycle goes on later
      crossingUntilMs_ = 0;
      return s;
    }
    if (!planSlice(*d_.arena, pending_.thru, rules_, plan).ok()) plan = SlicePlan{};
  }
  crossingUntilMs_ = 0;
  return Status::success();
}

// Another slice in this crossing: an urgent step inside its time, an idle one inside its window's requests.
bool CompactDriver::crossingMayAsk(bool urgent, uint32_t startMs) const {
  if (urgent) return !d_.platform->nowMs || (uint32_t)(d_.platform->nowMs() - startMs) < kCompactUrgentMs;
  return manual_ || windowAsks_ < kCompactRequestsPerWindow;   // an idle window's requests (RB-C2)
}

// At the due mark: the pending summary is swapped in with no request.
Status CompactDriver::swapPending(uint64_t now, CompactOutcome& out) {
  SlicePlan plan;
  plan.thru = pending_.thru;
  plan.day = pending_.day;
  plan.lastRow = pending_.lastRow;
  plan.turns = pending_.turns;
  const Status s = swap(plan, pending_.text, now, out);
  windowQ_ = d_.arena->lastQ();   // the summary's own s item is no turn: the idle window goes on (lane E2-FIX15)
  if (!s.ok() || out.step != CompactStep::Stored) return s;
  out.step = CompactStep::Swapped;
  manualStored_ += manual_;
  hlog::logf("compact(%s): the summary prepared ahead swapped in through q%llu, %u turns", d_.chatKey.c_str(),
             (unsigned long long)plan.thru, (unsigned)plan.turns);
  pending_ = Pending{};
  return s;
}

// ---- the owner's settings and the budget view ----------------------------------------------------

TurnCeiling ownerCeiling(const tx::EngineSettings& settings) {
  const tx::EngineSettings s = tx::clampSettings(settings);
  TurnCeiling c;
  c.rounds = s.maxRounds;
  c.hardWallMs = s.hardTimeS * 1000u;
  c.softWallMs = c.hardWallMs > kLastRoundReserveMs ? c.hardWallMs - kLastRoundReserveMs : c.hardWallMs / 2;
  return c;
}

Status budgetView(const BudgetViewIn& in, BudgetView& out) {
  out = BudgetView{};
  if (!in.budget || !in.system || !in.tools || !in.live || !in.arena || !in.budget->bodyMilliBytesPerToken)
    return Status::fail(Err::InvalidArgument, "plain::budgetView");
  const tx::HeadBudget& b = *in.budget;
  const SystemBlocks& system = *in.system;
  const std::vector<ToolSpec>& tools = *in.tools;
  const std::vector<tx::DigestEntry>& live = *in.live;
  const tx::TxArena& arena = *in.arena;
  const uint64_t thru = in.thru;
  const tx::EngineSettings settings = in.settings;
  auto tokens = [&](size_t bytes) { return (uint32_t)((uint64_t)bytes * 1000 / b.bodyMilliBytesPerToken); };
  size_t toolBytes = 0;
  for (const ToolSpec& t : tools) toolBytes += t.name.size() + t.description.size() + t.schemaJson.size();
  std::string block;
  std::vector<tx::DigestEntry> archive;
  if (!live.empty() && !tx::digestBlock(live, b.profile, block, archive).ok()) block.clear();
  const size_t digests = system.digests.size() > block.size() ? system.digests.size() - block.size() : 0;
  out.systemTokens = tokens(system.core.size() + system.notes.size() + digests);
  out.toolsTokens = tokens(toolBytes);
  out.fixedTokens = out.systemTokens + out.toolsTokens;
  out.summaryTokens = tokens(block.size());
  out.recentTokens = tokens(residentBytes(arena, thru));
  out.summaryBudgetTokens = b.summaryTokens;
  out.recentPartTokens = b.recentTokens;
  out.dueTokens = b.tokens.due;
  out.aimTokens = b.tokens.aim;
  const tx::EngineSettings s = tx::clampSettings(settings);
  out.maxRounds = s.maxRounds;
  out.hardTimeS = s.hardTimeS;
  out.nominal = b.bodyInterceptBytes == 0;
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
  RungMessage m;
  m.message = plan.input;
  m.thinInput = plan.input.size();
  s = askRung(d, rung, m, ask.now ? ask.now : rules.now, ans);
  if (ans.usage.responses && d.spent) d.spent(rung, ans.usage);
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
    const bool ok = driver->step(ctx, out).ok();
    // Idle: a request was one unit of idle work. Urgent (the engine's own ask): thru moved.
    o.worked = ok && (ctx.urgency == Urgency::None ? out.ran
                                                   : out.step == CompactStep::Stored || out.step == CompactStep::Swapped);
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
