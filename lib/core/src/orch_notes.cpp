// notes.h - engine 2's notes and the `note` tool (lane RB-N1; PRD section 4 "Notes").
//
// A shelf is one file of lines. A note line is 'key: value' or 'key [rule]: value'; any other
// line (a heading the owner wrote, a line from a later build) is kept as it is by every
// rewrite and is never shown as a note. A call reads the person's two note shelves, decides,
// and writes whole files through NotesIo::write (atomic at the seam). A move between shelves
// writes the new shelf first and then removes the old line; when that second write is refused
// the first is put back, so a refused write leaves both shelves as they were. A key found on
// both shelves (a cut between those two writes) is healed by the next write of that key.
#include "nimbus/orch/notes.h"

#include <algorithm>
#include <cstdlib>
#include <new>

#include "nimbus/orch/vector_memory.h"   // kOwnerNs
#include "nimbus/utf8.h"

namespace nimbus {
namespace orch {
namespace notes {

namespace {

constexpr char kRuleMark[] = " [rule]";
constexpr size_t kChatIdMax = 24;
constexpr char kKindLeftOut[] = "kind=keep ";   // a pending set whose call left kind out
constexpr char kSeqMark[] = "seq ";             // the pending file's last id given out

// ---- model-facing results (cataloged as engine-2 tool_results) -----------------------------
constexpr char kArgsTaken[] = "the arguments of note are op, key, value, kind and pin; not read: ";
constexpr char kBadOp[] = "op is set or remove";
constexpr char kBadKey[] = "key is a slug of a-z, 0-9 and -, 1 to 32 characters";
constexpr char kBadValue[] = "value is one line of 1 to 300 B of text with no control characters";
constexpr char kBadKind[] = "kind is rule or fact";
constexpr char kBadPin[] = "pin is true or false";
constexpr char kNoPerson[] = "not saved: this chat has no person notes can be kept for";
constexpr char kNoCard[] = "not saved: this person's notes are kept on the memory card, and no card is in";
constexpr char kNoFlash[] = "not saved: the device's internal storage is not available";
constexpr char kUnreadable[] = "not saved: the notes could not be read; nothing was changed";
constexpr char kUnwritten[] = "not saved: the notes could not be written; nothing was changed";
constexpr char kNoMemory[] = "not saved: the device ran out of memory; nothing was changed";
constexpr char kNoNote[] = "no note with key ";
constexpr char kSavedPinned[] = "saved, pinned (in every request): ";
constexpr char kSavedMore[] = "saved, unpinned (found by search): ";
constexpr char kSavedOverflow[] = "saved, unpinned (found by search), because pinned notes are full: ";
constexpr char kRemoved[] = "removed from ";
constexpr char kNoRoom[] = "not saved: no room for ";
constexpr char kLargest[] = "; largest notes: ";
constexpr char kNothingRemoved[] = "; nothing was removed";
constexpr char kKept[] = "kept for the owner's approval as ";
constexpr char kKeptOutside[] = ", not run: this turn read outside content";
constexpr char kKeptUnattended[] = ", not run: no person's message started this turn";
constexpr char kPendingFull[] = "not kept: the list waiting for the owner's approval is full (";
constexpr char kNoPending[] = "nothing waiting for approval with id ";
constexpr char kDropped[] = "dropped ";

const std::vector<const char*> kArgs = {"op", "key", "value", "kind", "pin"};

struct Line {
  std::string raw;   // as stored (a note line is re-formatted from `note`)
  bool isNote = false;
  Note note;
};
using Lines = std::vector<Line>;

Lines splitLines(const std::string& file) {
  Lines out;
  size_t at = 0;
  while (at < file.size()) {
    size_t end = file.find('\n', at);
    if (end == std::string::npos) end = file.size();
    Line l;
    l.raw = file.substr(at, end - at);
    if (!l.raw.empty() && l.raw.back() == '\r') l.raw.pop_back();   // a hand edit with CRLF
    l.isNote = parseLine(l.raw, l.note);
    if (l.isNote) l.raw = formatLine(l.note);
    if (!l.raw.empty()) out.push_back(std::move(l));
    at = end + 1;
  }
  return out;
}

std::string joinLines(const Lines& lines) {
  std::string out;
  for (const Line& l : lines) out += l.raw + "\n";
  return out;
}

size_t bytesOf(const Lines& lines) {
  size_t n = 0;
  for (const Line& l : lines) n += l.raw.size() + 1;
  return n;
}

int findKey(const Lines& lines, const std::string& key) {
  for (size_t i = 0; i < lines.size(); ++i)
    if (lines[i].isNote && lines[i].note.key == key) return static_cast<int>(i);
  return -1;
}

void dropKey(Lines& lines, const std::string& key) {
  lines.erase(std::remove_if(lines.begin(), lines.end(), [&](const Line& l) { return l.isNote && l.note.key == key; }),
              lines.end());
}

// Put the note in place of its old line, or at the end.
void putNote(Lines& lines, const Note& n) {
  Line l;
  l.isNote = true;
  l.note = n;
  l.raw = formatLine(n);
  const int at = findKey(lines, n.key);
  if (at < 0) lines.push_back(std::move(l));
  else lines[static_cast<size_t>(at)] = std::move(l);
  int seen = 0;   // a duplicate of the key on this same shelf (a hand edit) goes
  lines.erase(std::remove_if(lines.begin(), lines.end(),
                             [&](const Line& x) { return x.isNote && x.note.key == n.key && seen++ > 0; }),
              lines.end());
}

std::string used(size_t n, size_t cap) { return "used " + groupedBytes(n) + " of " + groupedBytes(cap) + " B"; }

bool isSlugChar(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }

bool validChatId(const std::string& id) {
  if (id.empty() || id.size() > kChatIdMax) return false;
  bool digit = false;
  for (char c : id) {
    if (c >= '0' && c <= '9') digit = true;
    else if (c != '-') return false;
  }
  return digit;
}

}  // namespace

// ---- the request a call carries -------------------------------------------------------------
struct Notebook::Request {
  bool remove = false;
  Note note;
  bool hasKind = false;
  int pin = -1;
};

struct Notebook::Shelves {
  Caps caps;
  Lines pinned;
  Lines more;
};

namespace {

bool readOp(ArduinoJson::JsonObjectConst a, bool& remove, std::string& err) {
  const JsonVariantConst op = a["op"];
  const char* s = op.is<const char*>() ? op.as<const char*>() : nullptr;
  if (s && std::string(s) == "set") return remove = false, true;
  if (s && std::string(s) == "remove") return remove = true, true;
  err = kBadOp;
  return false;
}

bool unknownArgs(ArduinoJson::JsonObjectConst a, std::string& err) {
  std::string extra;
  for (JsonPairConst p : a) {
    const std::string k = p.key().c_str();
    if (std::none_of(kArgs.begin(), kArgs.end(), [&](const char* x) { return k == x; }))
      extra += (extra.empty() ? "" : ", ") + k.substr(0, kKeyMax);
  }
  if (extra.empty()) return false;
  err = kArgsTaken + extra;
  return true;
}

bool readKind(ArduinoJson::JsonObjectConst a, Kind& kind, bool& has, std::string& err) {
  const JsonVariantConst k = a["kind"];
  if (k.isNull()) return has = false, true;
  const std::string s = k.is<const char*>() ? k.as<const char*>() : "";
  has = true;
  if (s == "rule") return kind = Kind::Rule, true;
  if (s == "fact") return kind = Kind::Fact, true;
  err = kBadKind;
  return false;
}

bool readPin(ArduinoJson::JsonObjectConst a, int& pin, std::string& err) {
  const JsonVariantConst p = a["pin"];
  if (p.isNull()) return pin = -1, true;
  if (!p.is<bool>()) return err = kBadPin, false;
  pin = p.as<bool>() ? 1 : 0;
  return true;
}

bool readValue(ArduinoJson::JsonObjectConst a, bool remove, std::string& value, std::string& err) {
  if (remove) return true;
  const JsonVariantConst v = a["value"];
  value = v.is<const char*>() ? v.as<const char*>() : "";
  if (validValue(value)) return true;
  err = kBadValue;
  return false;
}

}  // namespace

static bool parseRequest(ArduinoJson::JsonObjectConst a, bool& remove, Note& n, bool& hasKind, int& pin,
                         std::string& err) {
  if (unknownArgs(a, err) || !readOp(a, remove, err)) return false;
  const JsonVariantConst k = a["key"];
  n.key = k.is<const char*>() ? k.as<const char*>() : "";
  if (!validKey(n.key)) return err = kBadKey, false;
  return readValue(a, remove, n.value, err) && readKind(a, n.kind, hasKind, err) && readPin(a, pin, err);
}

// ---- pure rules -----------------------------------------------------------------------------
const char* shelfSlug(Shelf s) {
  switch (s) {
    case Shelf::Pinned: return "pinned";
    case Shelf::More: return "more";
    case Shelf::Pending: return "pending";
  }
  return "pending";
}

size_t fsBlocks(size_t bytes, size_t block) {
  size_t blocks = 0, room = 0;
  while (room < bytes) {
    size_t pointers = 0;
    if (blocks) {
      size_t i = blocks;
      pointers = 1;
      while (!(i & 1u)) i >>= 1, ++pointers;
    }
    room += block - 4 * pointers;
    ++blocks;
  }
  return blocks;
}

size_t flashBound(const Caps& c, size_t block) {
  const size_t p = fsBlocks(c.pinned, block), m = fsBlocks(c.more, block), q = fsBlocks(c.pending, block);
  return (2 + p + m + q + std::max(p, std::max(m, q))) * block;
}

std::string personKey(const Principal& who) {
  if (who.owner) return who.ns == kOwnerNs ? std::string(kOwnerNs) : std::string();
  const std::string prefix = "chat:";
  if (who.ns.compare(0, prefix.size(), prefix) != 0) return std::string();
  const std::string id = who.ns.substr(prefix.size());
  return validChatId(id) ? id : std::string();
}

bool validKey(const std::string& key) {
  return !key.empty() && key.size() <= kKeyMax && std::all_of(key.begin(), key.end(), isSlugChar);
}

namespace {

bool hasC0(const std::string& v) {
  return std::any_of(v.begin(), v.end(), [](char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return u < 0x20 || u == 0x7f;
  });
}

// C1 controls are U+0080..U+009F (C2 80..C2 9F); U+2028 and U+2029 are E2 80 A8 and E2 80 A9.
bool hasC1OrSeparator(const std::string& v) {
  for (size_t i = 0; i + 1 < v.size(); ++i) {
    const unsigned char a = static_cast<unsigned char>(v[i]), b = static_cast<unsigned char>(v[i + 1]);
    if (a == 0xC2 && b >= 0x80 && b <= 0x9F) return true;
    const unsigned char c = i + 2 < v.size() ? static_cast<unsigned char>(v[i + 2]) : 0;
    if (a == 0xE2 && b == 0x80 && (c == 0xA8 || c == 0xA9)) return true;
  }
  return false;
}

}  // namespace

bool validValue(const std::string& value) {
  if (value.empty() || value.size() > kValueMax || !utf8Valid(value)) return false;
  if (value.find_first_not_of(' ') == std::string::npos) return false;   // blank
  return !hasC0(value) && !hasC1OrSeparator(value);
}

std::string formatLine(const Note& n) { return n.key + (n.kind == Kind::Rule ? kRuleMark : "") + ": " + n.value; }

bool parseLine(const std::string& line, Note& out) {
  const size_t colon = line.find(": ");
  if (colon == std::string::npos) return false;
  std::string key = line.substr(0, colon);
  Kind kind = Kind::Fact;
  const size_t mark = sizeof(kRuleMark) - 1;
  if (key.size() > mark && key.compare(key.size() - mark, mark, kRuleMark) == 0) {
    key.resize(key.size() - mark);
    kind = Kind::Rule;
  }
  const std::string value = line.substr(colon + 2);
  if (!validKey(key) || !validValue(value)) return false;
  out = Note{key, value, kind};
  return true;
}

std::string groupedBytes(size_t n) {
  std::string digits = std::to_string(n), out;
  for (size_t i = 0; i < digits.size(); ++i) {
    if (i && (digits.size() - i) % 3 == 0) out += ',';
    out += digits[i];
  }
  return out;
}

const std::vector<const char*>& noteArgs() { return kArgs; }

// ---- the notebook ---------------------------------------------------------------------------
bool Notebook::load(const std::string& person, Shelves& s) {
  std::string p, m;
  if (!io_.read(person, Shelf::Pinned, p) || !io_.read(person, Shelf::More, m)) return false;
  s.caps = io_.caps(person);
  s.pinned = splitLines(p);
  s.more = splitLines(m);
  return true;
}

namespace {

// The kLargestNamed largest notes over both shelves, largest first ("big 61 B, mid 36 B").
std::string largest(const Lines& a, const Lines& b) {
  std::vector<std::pair<size_t, std::string>> all;
  for (const Lines* s : {&a, &b})
    for (const Line& l : *s)
      if (l.isNote) all.emplace_back(l.raw.size() + 1, l.note.key);
  std::sort(all.begin(), all.end(), [](const auto& x, const auto& y) {
    return x.first != y.first ? x.first > y.first : x.second < y.second;
  });
  std::string out;
  for (size_t i = 0; i < all.size() && i < kLargestNamed; ++i)
    out += (i ? ", " : "") + all[i].second + " " + groupedBytes(all[i].first) + " B";
  return out;
}

}  // namespace

namespace {

// Where a set goes: the note as stored, whether the call wants it pinned, the shelf it lands on.
struct Placement {
  Note note;
  bool wantPin = false;
  bool toPinned = false;
  bool fits = false;
  size_t add = 0;   // the line's bytes with its '\n'
};

size_t bytesWithout(const Lines& lines, const std::string& key) {
  Lines rest = lines;
  dropKey(rest, key);
  return bytesOf(rest);
}

// A pin left out keeps an existing note on its shelf and pins a new one; a kind left out keeps the
// old kind. A note kept unpinned with pin left out may go pinned when unpinned is full.
Placement place(const Note& asked, bool hasKind, int pin, const Lines& pinned, const Lines& more, const Caps& caps) {
  const int inPinned = findKey(pinned, asked.key), inMore = findKey(more, asked.key);
  Placement p;
  p.note = asked;
  if (!hasKind && inPinned >= 0) p.note.kind = pinned[static_cast<size_t>(inPinned)].note.kind;
  else if (!hasKind && inMore >= 0) p.note.kind = more[static_cast<size_t>(inMore)].note.kind;
  p.wantPin = pin >= 0 ? pin == 1 : (inPinned >= 0 || inMore < 0);
  p.add = formatLine(p.note).size() + 1;
  const bool pinFits = bytesWithout(pinned, asked.key) + p.add <= caps.pinned;
  const bool moreFits = bytesWithout(more, asked.key) + p.add <= caps.more;
  p.toPinned = (p.wantPin && pinFits) || (pin < 0 && !moreFits && pinFits);
  p.fits = p.toPinned || moreFits;
  return p;
}

// Write the target shelf, then the other one when it held the key; a refused second write puts the
// first back. "" when both landed, else the error text.
const char* commitMove(NotesIo& io, const std::string& person, bool toPinned, const Lines& pinned, const Lines& more,
                       bool otherHad) {
  const Shelf ts = toPinned ? Shelf::Pinned : Shelf::More, os = toPinned ? Shelf::More : Shelf::Pinned;
  std::string targetOld;
  if (!io.read(person, ts, targetOld)) return kUnreadable;
  if (!io.write(person, ts, joinLines(toPinned ? pinned : more))) return kUnwritten;
  if (otherHad && !io.write(person, os, joinLines(toPinned ? more : pinned))) {
    (void)io.write(person, ts, targetOld);   // put the first shelf back: nothing changed
    return kUnwritten;
  }
  return "";
}

}  // namespace

ToolResult Notebook::runSet(const Request& r, const std::string& person, Shelves& s) {
  const Placement p = place(r.note, r.hasKind, r.pin, s.pinned, s.more, s.caps);
  if (!p.fits)
    return ToolResult::fail(kNoRoom + groupedBytes(p.add) + " B (pinned " + used(bytesOf(s.pinned), s.caps.pinned) +
                            ", unpinned " + used(bytesOf(s.more), s.caps.more) + ")" + kLargest +
                            largest(s.pinned, s.more) + kNothingRemoved);
  // The target shelf keeps the old line's place; the other shelf loses its copy.
  Lines& target = p.toPinned ? s.pinned : s.more;
  Lines& other = p.toPinned ? s.more : s.pinned;
  putNote(target, p.note);
  const bool otherHad = findKey(other, p.note.key) >= 0;
  dropKey(other, p.note.key);
  const char* err = commitMove(io_, person, p.toPinned, s.pinned, s.more, otherHad);
  if (*err) return ToolResult::fail(err);
  const char* head = p.toPinned ? kSavedPinned : p.wantPin ? kSavedOverflow : kSavedMore;
  const size_t cap = p.toPinned ? s.caps.pinned : s.caps.more;
  return ToolResult::ok(head + formatLine(p.note) + " (" + used(bytesOf(target), cap) + (p.toPinned ? "" : " unpinned") +
                        ")");
}

ToolResult Notebook::runRemove(const Request& r, const std::string& person, Shelves& s) {
  const bool inPinned = findKey(s.pinned, r.note.key) >= 0, inMore = findKey(s.more, r.note.key) >= 0;
  if (!inPinned && !inMore) return ToolResult::fail(kNoNote + r.note.key);
  std::string pinnedOld;
  if (inPinned && inMore && !io_.read(person, Shelf::Pinned, pinnedOld)) return ToolResult::fail(kUnreadable);
  dropKey(s.pinned, r.note.key);
  dropKey(s.more, r.note.key);
  if (inPinned && !io_.write(person, Shelf::Pinned, joinLines(s.pinned))) return ToolResult::fail(kUnwritten);
  if (inMore && !io_.write(person, Shelf::More, joinLines(s.more))) {
    if (inPinned) (void)io_.write(person, Shelf::Pinned, pinnedOld);
    return ToolResult::fail(kUnwritten);
  }
  const bool p = inPinned;
  return ToolResult::ok(kRemoved + std::string(p ? "pinned: " : "unpinned: ") + r.note.key + " (" +
                        used(bytesOf(p ? s.pinned : s.more), p ? s.caps.pinned : s.caps.more) + ")");
}

ToolResult Notebook::run(const Request& r, const std::string& person) {
  Shelves s;
  if (!load(person, s)) return ToolResult::fail(kUnreadable);
  return r.remove ? runRemove(r, person, s) : runSet(r, person, s);
}

// ---- pending: one line per kept call ---------------------------------------------------------
//   p<n> <outside|unattended> set <pin|more|keep> <note line>
//   p<n> <outside|unattended> remove <key>
namespace {

std::string pendingLine(const PendingNote& p) {
  if (p.remove) return p.id + " " + p.why + " remove " + p.note.key;
  const char* pin = p.pin == 1 ? "pin" : p.pin == 0 ? "more" : "keep";
  return p.id + " " + p.why + " set " + pin + " " + (p.kindGiven ? "" : kKindLeftOut) + formatLine(p.note);
}

bool nextWord(const std::string& line, size_t& at, std::string& word) {
  const size_t sp = line.find(' ', at);
  if (sp == std::string::npos) return false;
  word = line.substr(at, sp - at);
  at = sp + 1;
  return true;
}

bool parsePending(const std::string& line, PendingNote& p) {
  size_t at = 0;
  std::string op, pin;
  if (!nextWord(line, at, p.id) || !nextWord(line, at, p.why) || !nextWord(line, at, op)) return false;
  if (op == "remove") {
    p.remove = true;
    p.note.key = line.substr(at);
    return validKey(p.note.key);
  }
  if (op != "set" || !nextWord(line, at, pin)) return false;
  p.pin = pin == "pin" ? 1 : pin == "more" ? 0 : -1;
  const size_t mark = sizeof(kKindLeftOut) - 1;
  p.kindGiven = line.compare(at, mark, kKindLeftOut) != 0;
  return parseLine(line.substr(p.kindGiven ? at : at + mark), p.note);
}

std::vector<PendingNote> parsePendingFile(const std::string& file) {
  std::vector<PendingNote> out;
  for (const Line& l : splitLines(file)) {
    PendingNote p;
    if (parsePending(l.raw, p)) out.push_back(std::move(p));
  }
  return out;
}

// The last id given out, from the file's "seq <n>" line and its entries: an id is never given
// twice, so an answer to an old id never reaches a newer call.
unsigned long lastId(const std::string& file, const std::vector<PendingNote>& all) {
  unsigned long top = 0;
  for (const PendingNote& p : all)
    if (p.id.size() > 1 && p.id[0] == 'p') top = std::max(top, std::strtoul(p.id.c_str() + 1, nullptr, 10));
  for (const Line& l : splitLines(file))
    if (l.raw.compare(0, sizeof(kSeqMark) - 1, kSeqMark) == 0)
      top = std::max(top, std::strtoul(l.raw.c_str() + sizeof(kSeqMark) - 1, nullptr, 10));
  return top;
}

// The file with its seq line set to `n` (first line) and `entry` appended.
std::string withEntry(const std::string& file, unsigned long n, const std::string& entry) {
  std::string out = kSeqMark + std::to_string(n) + "\n";
  for (const Line& l : splitLines(file))
    if (l.raw.compare(0, sizeof(kSeqMark) - 1, kSeqMark) != 0) out += l.raw + "\n";
  return out + entry + "\n";
}

}  // namespace

ToolResult Notebook::keepRequest(const Request& r, const std::string& person, Origin why) {
  std::string file;
  if (!io_.read(person, Shelf::Pending, file)) return ToolResult::fail(kUnreadable);
  const unsigned long n = lastId(file, parsePendingFile(file)) + 1;
  PendingNote p;
  p.id = "p" + std::to_string(n);
  p.remove = r.remove;
  p.pin = r.pin;
  p.note = r.note;
  p.kindGiven = r.hasKind;
  if (!r.hasKind) p.note.kind = Kind::Fact;
  p.why = why == Origin::Tainted ? "outside" : "unattended";
  const std::string next = withEntry(file, n, pendingLine(p));
  const size_t cap = io_.caps(person).pending;
  if (next.size() > cap) return ToolResult::fail(kPendingFull + used(file.size(), cap) + "); nothing was changed");
  if (!io_.write(person, Shelf::Pending, next)) return ToolResult::fail(kUnwritten);
  return ToolResult::ok(kKept + p.id + (why == Origin::Tainted ? kKeptOutside : kKeptUnattended) + ": " +
                        (r.remove ? "remove " + r.note.key : formatLine(p.note)));
}

namespace {

// Who the call is for, and whether their storage is there: "" with the error set otherwise.
std::string personFor(NotesIo& io, const Principal& who, std::string& err) {
  const std::string person = personKey(who);
  if (person.empty()) return err = kNoPerson, std::string();
  if (!io.available(person)) return err = person == kOwnerNs ? kNoFlash : kNoCard, std::string();
  return person;
}

}  // namespace

ToolResult Notebook::call(ArduinoJson::JsonObjectConst args, const Principal& who, Origin origin) {
  try {
    Request r;
    std::string err;
    if (!parseRequest(args, r.remove, r.note, r.hasKind, r.pin, err)) return ToolResult::fail(err);
    const std::string person = personFor(io_, who, err);
    if (person.empty()) return ToolResult::fail(err);
    return origin == Origin::Attended ? run(r, person) : keepRequest(r, person, origin);
  } catch (const std::bad_alloc&) {
    return ToolResult::fail(kNoMemory);
  }
}

ToolResult Notebook::keep(ArduinoJson::JsonObjectConst args, const Principal& who, Origin why) {
  return call(args, who, why == Origin::Attended ? Origin::Tainted : why);
}

std::string Notebook::pinnedBlock(const Principal& who) {
  try {
    std::string err, file;
    const std::string person = personFor(io_, who, err);
    if (person.empty() || !io_.read(person, Shelf::Pinned, file)) return std::string();
    const size_t cap = io_.caps(person).pinned;
    std::string out;
    for (const Line& l : splitLines(file)) {
      if (!l.isNote) continue;
      if (out.size() + l.raw.size() + 1 > cap) break;   // whole lines only
      out += l.raw + "\n";
    }
    return out;
  } catch (const std::bad_alloc&) {
    return std::string();
  }
}

bool Notebook::unpinned(const Principal& who, std::vector<Note>& out) {
  out.clear();
  try {
    std::string err, file;
    const std::string person = personFor(io_, who, err);
    if (person.empty() || !io_.read(person, Shelf::More, file)) return false;
    for (const Line& l : splitLines(file))
      if (l.isNote) out.push_back(l.note);
    return true;
  } catch (const std::bad_alloc&) {
    out.clear();
    return false;
  }
}

bool Notebook::pending(const Principal& who, std::vector<PendingNote>& out) {
  out.clear();
  try {
    std::string err, file;
    const std::string person = personFor(io_, who, err);
    if (person.empty() || !io_.read(person, Shelf::Pending, file)) return false;
    out = parsePendingFile(file);
    return true;
  } catch (const std::bad_alloc&) {
    out.clear();
    return false;
  }
}

namespace {

// The pending file without entry `id`; false when there is none.
bool withoutEntry(const std::string& file, const std::string& id, PendingNote& found, std::string& rest) {
  bool hit = false;
  rest.clear();
  for (const Line& l : splitLines(file)) {
    PendingNote p;
    if (!hit && parsePending(l.raw, p) && p.id == id) {
      found = p;
      hit = true;
      continue;
    }
    rest += l.raw + "\n";
  }
  return hit;
}

}  // namespace

ToolResult Notebook::approve(const Principal& who, const std::string& id) {
  try {
    std::string err, file, rest;
    const std::string person = personFor(io_, who, err);
    if (person.empty()) return ToolResult::fail(err);
    if (!io_.read(person, Shelf::Pending, file)) return ToolResult::fail(kUnreadable);
    PendingNote p;
    if (!withoutEntry(file, id, p, rest)) return ToolResult::fail(kNoPending + id.substr(0, kKeyMax));
    Request r;
    r.remove = p.remove;
    r.note = p.note;
    r.hasKind = !p.remove && p.kindGiven;
    r.pin = p.pin;
    // Dropped first, so a cut between the two writes loses the approval, never runs it twice.
    if (!io_.write(person, Shelf::Pending, rest)) return ToolResult::fail(kUnwritten);
    const ToolResult ran = run(r, person);
    if (!ran.success) (void)io_.write(person, Shelf::Pending, file);   // refused: it waits again
    return ran;
  } catch (const std::bad_alloc&) {
    return ToolResult::fail(kNoMemory);
  }
}

ToolResult Notebook::reject(const Principal& who, const std::string& id) {
  try {
    std::string err, file, rest;
    const std::string person = personFor(io_, who, err);
    if (person.empty()) return ToolResult::fail(err);
    if (!io_.read(person, Shelf::Pending, file)) return ToolResult::fail(kUnreadable);
    PendingNote p;
    if (!withoutEntry(file, id, p, rest)) return ToolResult::fail(kNoPending + id.substr(0, kKeyMax));
    if (!io_.write(person, Shelf::Pending, rest)) return ToolResult::fail(kUnwritten);
    return ToolResult::ok(kDropped + id);
  } catch (const std::bad_alloc&) {
    return ToolResult::fail(kNoMemory);
  }
}

void Notebook::registerInto(ToolRegistry& reg, const std::string& description, const std::string& schema,
                            OriginFn origin) {
  reg.add("note", description,
          [this, origin](ArduinoJson::JsonObjectConst args, const Principal& who) {
            return call(args, who, origin ? origin() : Origin::Attended);
          },
          schema);
}

}  // namespace notes
}  // namespace orch
}  // namespace nimbus
