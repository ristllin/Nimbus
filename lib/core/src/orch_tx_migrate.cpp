// Conversation migration and the re-entry rule (tx/migrate.h), lane RB-MG1a.
//
// Steps and their resume points (all in /mem/tx/<chatKey>/migrated.json, written by temp and
// rename with a crc tail before the step acts):
//   Digest0  tries counted before each ask; then the plan (anchor note at q0, digest 0 at q0 + 1
//            when accepted, else the old summary exported first) before the first item.
//   Rows     the batch plan (rows lo < id <= hi, the ones up to cut named) before its note.
//   Done     hi: the newest row accounted for. A later start is a re-entry from hi.
// A planned step's items are regenerated from the plan and compared line for line with what
// the segments hold from the plan's start; the step goes on after the last line that matches.
#include "nimbus/orch/tx/migrate.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "nimbus/orch/tx/digest.h"
#include "nimbus/orch/tx/rules.h"

namespace nimbus {
namespace orch {
namespace tx {

namespace mt = migrate_text;

// ==== pure helpers =========================================================================

namespace {

uint32_t crc32(const char* data, size_t len) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    c ^= (uint8_t)data[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

void clipUtf8(std::string& s, size_t cap) {
  if (s.size() <= cap) return;
  size_t n = cap;
  while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) --n;
  s.resize(n);
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isAlpha(char c) { return ((c | 32) >= 'a' && (c | 32) <= 'z'); }
bool isLowerAlnum(char c) { return (c >= 'a' && c <= 'z') || isDigit(c); }
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

int64_t floorDiv(int64_t a, int64_t b) { return a / b - ((a % b) != 0 && ((a < 0) != (b < 0))); }

// The local day number of `epoch` where a day starts at dayStartMin past midnight.
int64_t dayNumber(uint64_t epoch, int16_t tz, uint16_t dayStartMin) {
  return floorDiv((int64_t)epoch + (int64_t)tz * 60 - (int64_t)dayStartMin * 60, 86400);
}

std::string dayString(uint64_t epoch, int16_t tz, uint16_t dayStartMin) {
  int64_t z = dayNumber(epoch, tz, dayStartMin) + 719468;
  const int64_t era = floorDiv(z, 146097);
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  const int64_t y = (int64_t)yoe + era * 400 + (m <= 2);
  char buf[16];
  std::snprintf(buf, sizeof buf, "%04d-%02u-%02u", (int)y, m, d);
  return buf;
}

std::string when(uint64_t epoch, int16_t tz) {
  std::string out;
  if (!epoch || !photoWhen(epoch, tz, out).ok()) out.clear();
  return out;
}

// Where to cut `s` to at most `cap` bytes: after the last sentence end, else at the last space,
// in the second half of the room; else on a character.
size_t cutAt(const std::string& s, size_t cap) {
  if (s.size() <= cap) return s.size();
  size_t at = s.rfind(". ", cap - 1);
  if (at != std::string::npos && at + 1 > cap / 2) return at + 1;
  at = s.rfind(' ', cap);
  if (at != std::string::npos && at > cap / 2) return at;
  size_t n = cap;
  while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) --n;
  return n;
}

// The old summary carried as digest 0 (ruling 84 item 6, lane RB-H81): its first
// kOldSummaryMaxBytes, verbatim, as digest texts of `day` (each within kDigestMaxBytes) that fit
// the full digest block together. The first part opens the fence, dated `asOf` and saying its
// paths and totals may be out of date; the last closes it, or, when not all of it fits (or only
// one part may be stored), says where the rest is (`path`, the export file) or that it is cut.
// Empty when no part can be built.
// One pass of oldSummaryParts over `text`: the parts, and their bytes in the block (each and its
// line end). Empty when a part cannot be built.
struct SummaryForm {
  const std::string& summary;   // the whole old summary
  const std::string& day;
  const std::string& asOf;
  const std::string& cutClose;  // the close when not all of it fits
  size_t lead;                  // the day header's bytes
  bool onePart;
};
std::vector<std::string> summaryPass(const std::string& text, const SummaryForm& f, size_t& total) {
  namespace m = migrate_text;
  const std::string &summary = f.summary, &day = f.day, &asOf = f.asOf, &cutClose = f.cutClose;
  const size_t lead = f.lead;
  const bool onePart = f.onePart;
  const size_t closeRoom = std::max(cutClose.size(), sizeof(m::kOldClose) - 1);
  std::vector<std::string> parts;
  size_t at = 0;
  total = 0;
  while (at < text.size()) {
    const std::string prefix = parts.empty() ? m::kOldOpen + asOf + m::kOldStale : std::string(m::kOldMore);
    if (lead + prefix.size() + closeRoom + 16 > kDigestMaxBytes) return {};
    const size_t take = cutAt(text.substr(at), kDigestMaxBytes - lead - prefix.size() - closeRoom);
    if (!take) return {};
    std::string body = prefix + text.substr(at, take), out;
    at += take;
    const bool last = at >= text.size() || onePart;
    const bool whole = at >= text.size() && text.size() == summary.size();
    if (last) body += whole ? std::string(m::kOldClose) : cutClose;
    if (!digestText(day, body, out).ok()) return {};
    total += out.size() + 1;
    parts.push_back(out);
    if (last) break;
  }
  return parts;
}

// The old summary carried as digest 0 (ruling 84 item 6, lane RB-H81): its first
// kOldSummaryMaxBytes, verbatim, as digest texts of `day` (each within kDigestMaxBytes) that fit
// the full digest block together. The first part opens the fence, dated `asOf` and saying its
// paths and totals may be out of date; the last closes it, or, when not all of it fits (or only
// one part may be stored), says where the rest is (`path`, the export file) or that it is cut.
// Empty when no part can be built.
std::vector<std::string> oldSummaryParts(const std::string& summary, const std::string& day, const std::string& asOf,
                                         const std::string& path, bool onePart) {
  namespace m = migrate_text;
  std::string header, text = summary;
  if (!dayHeader(day, header).ok() || summary.empty()) return {};
  text.resize(cutAt(text, kOldSummaryMaxBytes));
  const size_t lead = header.size() + sizeof(digest_text::kDaySep) - 1;
  // A cut is always named: where the rest is, or that it is cut (never the close of a whole summary).
  const std::string cutClose = path.empty() ? std::string(m::kOldCutHere) : m::kOldCut + path + m::kOldCutEnd;
  for (int round = 0; round < 16; ++round) {
    size_t total = 0;
    std::vector<std::string> parts = summaryPass(text, SummaryForm{summary, day, asOf, cutClose, lead, onePart}, total);
    if (parts.empty() || total <= blockCapFor(Profile::Full) + 1) return parts;
    const size_t over = total - blockCapFor(Profile::Full) + 64;
    if (text.size() <= over) return {};
    text.resize(cutAt(text, text.size() - over));
  }
  return {};
}

const char* const kMonths[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};

// Is the word of letters ending just before `end` (or starting at `start`) a month name?
bool monthWord(const std::string& t, size_t start, size_t end) {
  if (end <= start || end - start < 3 || end - start > 9) return false;
  std::string w;
  for (size_t i = start; i < end; ++i) w += (char)(t[i] | 32);
  for (const char* m : kMonths)
    if (w.compare(0, 3, m) == 0) {
      static const char* const kFull[] = {"january", "february", "march", "april", "may", "june", "july",
                                          "august", "september", "october", "november", "december"};
      if (w.size() == 3) return true;
      for (const char* f : kFull)
        if (w == f) return true;
      return w == "sept";
    }
  return false;
}

// The word right before s, past at most two spaces or dots ("Nov 17", "Nov. 17").
bool monthBefore(const std::string& t, size_t s) {
  size_t b = s;
  for (int k = 0; k < 2 && b > 0 && (t[b - 1] == ' ' || t[b - 1] == '.'); ++k) --b;
  size_t a = b;
  while (a > 0 && isAlpha(t[a - 1])) --a;
  return a < b && monthWord(t, a, b);
}

// An ordinal suffix at f ("17th"): its length, else 0.
size_t ordinalAt(const std::string& t, size_t f) {
  if (f + 2 > t.size() || (f + 2 < t.size() && isAlpha(t[f + 2]))) return 0;
  const std::string suf = t.substr(f, 2);
  return suf == "st" || suf == "nd" || suf == "rd" || suf == "th" ? 2 : 0;
}

// The word right after e, past an ordinal and at most two spaces, dots or commas ("17 Nov", "17th Nov").
bool monthAfter(const std::string& t, size_t e) {
  size_t f = e + ordinalAt(t, e);
  for (int k = 0; k < 2 && f < t.size() && (t[f] == ' ' || t[f] == '.' || t[f] == ','); ++k) ++f;
  size_t g = f;
  while (g < t.size() && isAlpha(t[g])) ++g;
  return monthWord(t, f, g);
}

bool dateSep(char c) { return c == '-' || c == ':' || c == '/'; }

// The digit run at i (digits with '.' or ',' between digits); i moves past it.
void digitRun(const std::string& t, size_t& i) {
  while (i < t.size() && (isDigit(t[i]) || ((t[i] == '.' || t[i] == ',') && i + 1 < t.size() && isDigit(t[i + 1])))) ++i;
}

// The run [s, e) as a figure: its group separators dropped.
std::string figureText(const std::string& t, size_t s, size_t e) {
  std::string fig;
  for (size_t k = s; k < e; ++k)
    if (t[k] != ',') fig += t[k];
  return fig;
}

// A part of a date or a clock time: "2026-11-17", "13:02", a year alone, a day beside a month.
bool datePart(const std::string& t, size_t s, size_t e, const std::string& fig) {
  if (s >= 2 && dateSep(t[s - 1]) && isDigit(t[s - 2])) return true;
  if (e + 1 < t.size() && dateSep(t[e]) && isDigit(t[e + 1])) return true;
  const bool year = fig.size() == 4 && fig.find('.') == std::string::npos &&
                    (fig.compare(0, 2, "19") == 0 || fig.compare(0, 2, "20") == 0);
  return year || monthBefore(t, s) || monthAfter(t, e);
}

// The token around text[i..]: whitespace-separated, outer punctuation trimmed (a leading slash
// kept); i moves past it. Empty at the end.
std::string nextToken(const std::string& text, size_t& i) {
  while (i < text.size() && isSpace(text[i])) ++i;
  size_t s = i;
  while (i < text.size() && !isSpace(text[i])) ++i;
  size_t e = i;
  auto trimmable = [](char c) { return std::strchr("()[]{}\"'.,;:!?<>*`", c) != nullptr; };
  while (s < e && trimmable(text[s])) ++s;
  while (e > s && trimmable(text[e - 1])) --e;
  return text.substr(s, e - s);
}

// A slash or backslash that joins two parts of a name ("notes/boat.md", "/data"), not a fraction.
bool slashPath(const std::string& tok) {
  for (size_t k = 0; k + 1 < tok.size(); ++k) {
    if (tok[k] != '/' && tok[k] != '\\') continue;
    if (k == 0) {
      if (isAlpha(tok[1]) || tok[1] == '.') return true;   // "/data", "/.cfg"
      continue;
    }
    if (!(isDigit(tok[k - 1]) && isDigit(tok[k + 1]))) return true;   // "1/2" is not one
  }
  return false;
}

// A name with an extension of 2 to 4 lower-case letters or digits ("boat.md", "photo.jpeg").
bool extPath(const std::string& tok) {
  const size_t dot = tok.rfind('.');
  if (dot == std::string::npos || dot == 0 || tok.size() - dot - 1 < 2 || tok.size() - dot - 1 > 4) return false;
  bool ext = true, letter = false, nameLetter = false;
  for (size_t k = dot + 1; k < tok.size(); ++k) {
    ext &= isLowerAlnum(tok[k]);
    letter |= isAlpha(tok[k]);
  }
  for (size_t k = 0; k < dot; ++k) nameLetter |= isAlpha(tok[k]);
  return ext && letter && nameLetter;
}

}  // namespace

std::vector<std::string> figuresOf(const std::string& t) {
  std::vector<std::string> out;
  for (size_t i = 0; i < t.size();) {
    if (!isDigit(t[i])) {
      ++i;
      continue;
    }
    const size_t s = i;
    digitRun(t, i);
    const std::string fig = figureText(t, s, i);
    if (fig.size() < 2 || datePart(t, s, i, fig)) continue;
    if (std::find(out.begin(), out.end(), fig) == out.end()) out.push_back(fig);
  }
  return out;
}

bool hasPathLikeToken(const std::string& text) {
  for (size_t i = 0; i < text.size();) {
    const std::string tok = nextToken(text, i);
    if (!tok.empty() && (slashPath(tok) || extPath(tok))) return true;
  }
  return false;
}

const char* digest0VerdictSlug(Digest0Verdict v) {
  switch (v) {
    case Digest0Verdict::Accepted: return "accepted";
    case Digest0Verdict::NoSummary: return "no-summary";
    case Digest0Verdict::Asked: return "not-answered";
    case Digest0Verdict::Size: return "size";
    case Digest0Verdict::Path: return "path";
    case Digest0Verdict::Figure: return "figure";
    case Digest0Verdict::Text: return "text";
  }
  return "text";
}

Digest0Verdict checkDigest0(const std::string& summary, uint32_t daysCovered,
                            const std::function<bool(const std::string&)>& toolFigure) {
  const uint64_t cap = (uint64_t)kDigestMaxBytes * (daysCovered ? daysCovered : 1);
  if (summary.size() > cap) return Digest0Verdict::Size;
  if (hasPathLikeToken(summary)) return Digest0Verdict::Path;
  if (toolFigure)
    for (const std::string& f : figuresOf(summary))
      if (toolFigure(f)) return Digest0Verdict::Figure;
  return Digest0Verdict::Accepted;
}

const char* migrationStepSlug(MigrationStep s) {
  switch (s) {
    case MigrationStep::NotStarted: return "not-started";
    case MigrationStep::Digest0: return "digest0";
    case MigrationStep::Rows: return "rows";
    case MigrationStep::Done: return "done";
  }
  return "not-started";
}

// ==== the batch ============================================================================

namespace {

// A row a batch imports: an owner or assistant row with text the earlier engine wrote, or one of
// engine 2's own at or below `oursUpTo` (the P3 repair: those items are hidden under digest 0).
bool importable(const DayRow& r, uint64_t oursUpTo) {
  return (!r.ours || r.id <= oursUpTo) && (r.role == RowRole::Owner || r.role == RowRole::Assistant) && !r.text.empty();
}

// The u or a item a row becomes (q and turn are the caller's).
void rowItem(const DayRow& r, uint64_t q, uint32_t turn, int16_t tz, Item& it) {
  it = Item{};
  it.q = q;
  it.turn = turn;
  it.t = r.t;
  it.x = r.text;
  if (r.role == RowRole::Owner) {
    it.kind = ItemKind::User;
    it.tzMinutes = tz;
    it.m = r.id;
    clipUtf8(it.x, kTier1OwnerText);
    if (r.photo && r.photoRef.size() > 1 && r.photoRef[0] == 'p') {   // the photo and its note (RB-H81)
      Part p{PartKind::Image, r.photoRef, "image/jpeg", 0, r.text};
      clipUtf8(p.note, kPhotoNoteMax);
      if (p.note.size() == r.text.size()) it.x.clear();   // the note holds it all: not twice
      it.parts.push_back(p);
    }
  } else {
    it.kind = ItemKind::Assistant;
    it.fin = true;
    clipUtf8(it.x, kTier1AssistantText);
  }
}

std::string count(uint64_t n) { return std::to_string(n); }

}  // namespace

std::string importNote(const ImportPlan& p) {
  std::string s = mt::kDevice;
  if (p.turns) {
    s += count(p.turns);
    s += p.turns == 1 ? mt::kOneTurn : mt::kTurns;
    const std::string a = when(p.firstT, p.tzMinutes), b = when(p.lastT, p.tzMinutes);
    if (!a.empty() && !b.empty()) s += std::string(mt::kBetween) + a + mt::kAnd + b;
    s += p.turns == 1 ? mt::kWas : mt::kWere;
    s += mt::kAnswered;
  }
  if (p.named) {
    s += p.turns ? std::string(mt::kNamedThe) : std::string(mt::kNamedThe).substr(1);
    s += count(p.named);
    s += p.named == 1 ? mt::kNamedOne : mt::kNamedMany;
    const std::string c = when(p.firstT ? p.firstT : p.namedLastT, p.tzMinutes);
    if (!c.empty() && p.turns) s += std::string(mt::kNamedBefore) + c;
    s += p.named == 1 ? mt::kIs : mt::kAre;
    s += mt::kNamedNotShown;
  }
  return s;
}

bool BatchBuilder::put(const Item& it, uint64_t row, const Emit& emit) {
  if (!emit(it, row)) stopped_ = true;
  ++q_;
  return !stopped_;
}

bool BatchBuilder::closeTurn(const Emit& emit) {
  Item e;
  e.q = q_;
  e.kind = ItemKind::TurnEnd;
  e.turn = turn_;
  e.t = lastT_;
  e.end.cause = "import";
  open_ = false;
  return put(e, 0, emit);
}

bool BatchBuilder::note(const Emit& emit) {
  noted_ = true;
  q_ = plan_.q0;
  turn_ = plan_.turnBase;
  Item h;
  h.q = q_;
  h.kind = ItemKind::DeviceNote;
  h.note = plan_.named ? NoteClass::Omit : NoteClass::Notice;
  h.t = plan_.firstT ? plan_.firstT : plan_.namedLastT;
  h.x = importNote(plan_);
  return put(h, 0, emit);
}

// One row: an owner row (or the first row) opens a turn, closing the one before.
bool BatchBuilder::row(const DayRow& r, const Emit& emit) {
  if (r.id <= plan_.cut || r.id > plan_.hi || !importable(r, plan_.oursUpTo)) return true;
  if (r.role == RowRole::Owner || !open_) {
    if (open_ && !closeTurn(emit)) return false;
    ++turn_;
    open_ = true;
  }
  Item it;
  rowItem(r, q_, turn_, plan_.tzMinutes, it);
  lastT_ = r.t;
  return put(it, r.id, emit);
}

Status BatchBuilder::feed(const std::vector<DayRow>& rows, bool last, const Emit& emit) {
  if (stopped_ || (!noted_ && !note(emit))) return Status::success();
  for (const DayRow& r : rows)
    if (!row(r, emit)) return Status::success();
  if (last && open_) (void)closeTurn(emit);
  return Status::success();
}

// ==== storage: the marker and a line reader ================================================

namespace {

constexpr const char* kMarkerTmp = ".tmp";

struct D0Plan {
  bool have = false;      // planned (the fields below hold)
  bool anchor = false;    // an anchor note is written
  bool accepted = false;  // digest 0 follows it
  bool exported = false;
  bool carried = false;   // no rewrite taken: the old summary itself is digest 0 (RB-H81)
  bool onePart = false;   // carried after another writer's items: its first part only
  Digest0Verdict verdict = Digest0Verdict::NoSummary;
  bool anchorStored = false;   // the anchor is stored; the plan is the digest alone (after an overtaking)
  uint64_t q0 = 0;             // the anchor's q
  uint64_t sq = 0;             // digest 0's q (q0 + 1 unless the anchor was stored before an overtaking)
  uint32_t seg = 1;
  uint64_t off = 0;
  uint64_t at = 0;
  int16_t tz = 0;
  std::string text, day, path;
};

struct Marker {
  MigrationStep step = MigrationStep::NotStarted;
  bool reentry = false;   // the first migration is complete
  bool repair = false;    // the first migration runs over engine-2 items stored with no marker (P3)
  uint32_t tries = 0;     // digest-0 asks so far
  uint64_t hi = 0;        // the newest row accounted for
  bool d0Done = false;    // digest 0 stored (outcome)
  D0Plan d0;
  bool batch = false;
  ImportPlan plan;
  uint32_t seg = 1;
  uint64_t off = 0;
};

void jsonStr(std::string& out, const std::string& s) {
  out += '"';
  for (char c : s) {
    const unsigned char u = (unsigned char)c;
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (u < 0x20) {
      char b[8];
      std::snprintf(b, sizeof b, "\\u%04x", u);
      out += b;
    } else {
      out += c;
    }
  }
  out += '"';
}

void num(std::string& out, const char* key, uint64_t v) {
  out += ",\"";
  out += key;
  out += "\":";
  out += std::to_string(v);
}
void str(std::string& out, const char* key, const std::string& v) {
  out += ",\"";
  out += key;
  out += "\":";
  jsonStr(out, v);
}

std::string encodeMarker(const Marker& m) {
  std::string s = "{\"v\":1";
  num(s, "st", (uint64_t)m.step);
  num(s, "re", m.reentry);
  if (m.repair) num(s, "rep", 1);
  num(s, "tries", m.tries);
  num(s, "hi", m.hi);
  num(s, "d0done", m.d0Done);
  if (m.d0.have) {
    num(s, "d0", 1);
    num(s, "d0anc", m.d0.anchor);
    num(s, "d0acc", m.d0.accepted);
    num(s, "d0exp", m.d0.exported);
    num(s, "d0car", m.d0.carried);
    num(s, "d0one", m.d0.onePart);
    num(s, "d0ver", (uint64_t)m.d0.verdict);
    num(s, "d0q", m.d0.q0);
    num(s, "d0sq", m.d0.sq);
    num(s, "d0as", m.d0.anchorStored);
    num(s, "d0tz", (uint64_t)(int64_t)m.d0.tz);
    num(s, "d0seg", m.d0.seg);
    num(s, "d0off", m.d0.off);
    num(s, "d0at", m.d0.at);
    str(s, "d0text", m.d0.text);
    str(s, "d0day", m.d0.day);
    str(s, "d0path", m.d0.path);
  }
  if (m.batch) {
    const ImportPlan& p = m.plan;
    num(s, "b", 1);
    num(s, "bq", p.q0);
    num(s, "btb", p.turnBase);
    num(s, "blo", p.lo);
    num(s, "bcut", p.cut);
    num(s, "bhi", p.hi);
    num(s, "bturns", p.turns);
    num(s, "bimp", p.imported);
    num(s, "bft", p.firstT);
    num(s, "blt", p.lastT);
    num(s, "bnamed", p.named);
    num(s, "bnlt", p.namedLastT);
    if (p.oursUpTo) num(s, "bou", p.oursUpTo);
    num(s, "btz", (uint64_t)(int64_t)p.tzMinutes);
    num(s, "bseg", m.seg);
    num(s, "boff", m.off);
  }
  char z[16];
  std::snprintf(z, sizeof z, "%08x", (unsigned)crc32(s.data(), s.size()));
  s += ",\"z\":\"";
  s += z;
  s += "\"}\n";
  return s;
}

bool decodeMarker(const std::string& text, Marker& m) {
  const size_t zk = text.rfind(",\"z\":\"");
  if (zk == std::string::npos || text.size() < zk + 16) return false;
  char z[16];
  std::snprintf(z, sizeof z, "%08x", (unsigned)crc32(text.data(), zk));
  if (text.compare(zk + 6, 8, z) != 0) return false;
  JsonDocument doc;
  if (deserializeJson(doc, text.data(), text.size()) || !doc.is<JsonObject>()) return false;
  if ((doc["v"] | 0) != 1) return false;
  auto u = [&](const char* k) { return doc[k] | (uint64_t)0; };
  m = Marker{};
  const uint64_t st = u("st");
  if (st > (uint64_t)MigrationStep::Done) return false;
  m.step = (MigrationStep)st;
  m.reentry = u("re") != 0;
  m.repair = u("rep") != 0;
  m.tries = (uint32_t)u("tries");
  m.hi = u("hi");
  m.d0Done = u("d0done") != 0;
  if (u("d0")) {
    D0Plan& d = m.d0;
    d.have = true;
    d.anchor = u("d0anc") != 0;
    d.accepted = u("d0acc") != 0;
    d.exported = u("d0exp") != 0;
    d.carried = u("d0car") != 0;
    d.onePart = u("d0one") != 0;
    d.verdict = (Digest0Verdict)std::min<uint64_t>(u("d0ver"), (uint64_t)Digest0Verdict::Text);
    d.q0 = u("d0q");
    d.sq = u("d0sq");
    d.anchorStored = u("d0as") != 0;
    d.tz = (int16_t)(int64_t)u("d0tz");
    d.seg = (uint32_t)u("d0seg");
    d.off = u("d0off");
    d.at = u("d0at");
    d.text = doc["d0text"] | "";
    d.day = doc["d0day"] | "";
    d.path = doc["d0path"] | "";
  }
  if (u("b")) {
    ImportPlan& p = m.plan;
    m.batch = true;
    p.q0 = u("bq");
    p.turnBase = (uint32_t)u("btb");
    p.lo = u("blo");
    p.cut = u("bcut");
    p.hi = u("bhi");
    p.turns = (uint32_t)u("bturns");
    p.imported = (uint32_t)u("bimp");
    p.firstT = u("bft");
    p.lastT = u("blt");
    p.named = (uint32_t)u("bnamed");
    p.namedLastT = u("bnlt");
    p.oursUpTo = u("bou");
    p.tzMinutes = (int16_t)(int64_t)u("btz");
    m.seg = (uint32_t)u("bseg");
    m.off = u("boff");
  }
  return true;
}

bool readWhole(TxFiles& files, const std::string& path, std::string& out) {
  size_t n = 0;
  if (!files.size(path, n) || n == 0 || n > kMarkerMaxBytes) return false;
  out.assign(n, '\0');
  size_t got = 0;
  return files.readAt(path, 0, &out[0], n, got) && got == n;
}

// The marker: the temp first (a whole one exists only when a cut or refusal stopped its rename).
bool readMarker(TxFiles& files, const std::string& chatKey, Marker& m) {
  std::string path, text;
  if (!migratedPath(chatKey, path).ok()) return false;
  for (const std::string& p : {path + kMarkerTmp, path})
    if (readWhole(files, p, text) && decodeMarker(text, m)) return true;
  return false;
}

Status writeMarker(TxFiles& files, const std::string& chatKey, const Marker& m) {
  std::string path;
  Status s = migratedPath(chatKey, path);
  if (!s.ok()) return s;
  const std::string text = encodeMarker(m);
  if (text.size() > kMarkerMaxBytes) return Status::fail(Err::TooLarge, "tx::migrate marker");
  if (!files.writeWhole(path + kMarkerTmp, text) || !files.rename(path + kMarkerTmp, path))
    return Status::fail(Err::Io, "tx::migrate marker");
  return Status::success();
}

uint32_t segNumber(const std::string& name) {
  if (name.size() != 16 || name.compare(0, 4, "seg-") != 0 || name.compare(10, 6, ".jsonl") != 0) return 0;
  uint32_t n = 0;
  for (size_t i = 4; i < 10; ++i) {
    if (!isDigit(name[i])) return 0;
    n = n * 10 + (uint32_t)(name[i] - '0');
  }
  return n;
}

Status segmentList(TxFiles& files, const std::string& chatKey, std::vector<uint32_t>& out) {
  std::string dir;
  Status s = txDir(chatKey, dir);
  if (!s.ok()) return s;
  std::vector<std::string> names;
  if (!files.list(dir, names)) return Status::fail(Err::Io, "tx::migrate list");
  out.clear();
  for (const std::string& n : names)
    if (uint32_t k = segNumber(n)) out.push_back(k);
  std::sort(out.begin(), out.end());
  return Status::success();
}

// The whole, crc-valid lines of the segments from (seg, off) on, in file order.
class LineCursor {
 public:
  LineCursor(TxFiles& files, const std::string& chatKey) : files_(files), chatKey_(chatKey) {}
  Status start(uint32_t seg, uint64_t off) {
    Status s = segmentList(files_, chatKey_, segs_);
    if (!s.ok()) return s;
    idx_ = 0;
    while (idx_ < segs_.size() && segs_[idx_] < seg) ++idx_;
    off_ = idx_ < segs_.size() && segs_[idx_] == seg ? off : 0;
    buf_.clear();
    pos_ = 0;
    return Status::success();
  }
  // The next line (newline included) and its header; `have` false at the end.
  Status next(std::string& line, LineHeader& h, bool& have) {
    have = false;
    while (idx_ < segs_.size()) {
      const size_t nl = buf_.find('\n', pos_);
      if (nl != std::string::npos) {
        line.assign(buf_, pos_, nl + 1 - pos_);
        pos_ = nl + 1;
        if (checkLine(line.data(), line.size(), h).ok()) {
          have = true;
          return Status::success();
        }
        continue;
      }
      if (pos_) {
        buf_.erase(0, pos_);
        pos_ = 0;
      }
      std::string path;
      Status s = segmentPath(chatKey_, segs_[idx_], path);
      if (!s.ok()) return s;
      chunk_.resize(kFillSliceBytes);   // on the heap (PSRAM-first on the device), never the task's stack
      size_t got = 0;
      if (!files_.readAt(path, (size_t)off_, &chunk_[0], chunk_.size(), got))
        return Status::fail(Err::Io, "tx::migrate read");
      if (got == 0) {   // segment end: a torn tail is dropped
        ++idx_;
        off_ = 0;
        buf_.clear();
        continue;
      }
      if (buf_.size() > kMaxLineBytes) buf_.clear();   // no line is this long: the rest of it is dropped
      buf_.append(chunk_.data(), got);
      off_ += got;
    }
    return Status::success();
  }

 private:
  TxFiles& files_;
  std::string chatKey_;
  std::vector<uint32_t> segs_;
  size_t idx_ = 0;
  uint64_t off_ = 0;
  std::string buf_, chunk_;
  size_t pos_ = 0;
};

// ==== the migration ========================================================================

struct Run {
  const MigrateDeps& d;
  MigrationOutcome& out;
  TxLog log;
  HeadState head;
  Marker m;
  Run(const MigrateDeps& deps, MigrationOutcome& o) : d(deps), out(o), log(*deps.files, deps.chatKey) {}

  uint64_t nextQ() const {
    uint64_t q = std::max<uint64_t>(head.nextQ, log.lastQ() + 1);
    if (d.arena && !d.ramFollows) q = std::max<uint64_t>(q, d.arena->lastQ() + 1);
    return std::max<uint64_t>(q, 1);
  }
  Status save() { return writeMarker(*d.files, d.chatKey, m); }

  // Every row of after < id <= upTo, page by page.
  Status eachRow(uint64_t after, uint64_t upTo, const std::function<void(const DayRow&)>& fn) {
    std::vector<DayRow> page;
    page.reserve(kRowPage);
    for (;;) {
      bool end = false;
      page.clear();
      Status s = d.rows->read(after, upTo, kRowPage, page, end);
      if (!s.ok()) return s;
      for (const DayRow& r : page) {
        if (r.id <= after || (upTo && r.id > upTo)) return Status::fail(Err::InvalidArgument, "tx::migrate rows");
        fn(r);
        after = r.id;
      }
      if (end || page.empty()) return Status::success();
    }
  }

  // ---- digest 0 ----
  struct Covered {
    uint32_t rows = 0;
    uint64_t firstT = 0, lastT = 0;
  };
  Status coveredRows(Covered& c) {
    if (!d.fold.thru) return Status::success();
    return eachRow(0, d.fold.thru, [&](const DayRow& r) {
      ++c.rows;
      if (r.t && !c.firstT) c.firstT = r.t;
      if (r.t) c.lastT = std::max(c.lastT, r.t);
    });
  }
  // For each figure: does a tool row of the covered rows hold it while no owner row does?
  Status toolFigures(const std::vector<std::string>& figs, std::vector<bool>& fromTool) {
    std::vector<bool> inTool(figs.size(), false), inOwner(figs.size(), false);
    if (!figs.empty() && d.fold.thru) {
      Status s = eachRow(0, d.fold.thru, [&](const DayRow& r) {
        if (r.role != RowRole::Tool && r.role != RowRole::Owner) return;
        const std::vector<std::string> mine = figuresOfAll(r.text);
        for (size_t i = 0; i < figs.size(); ++i)
          if (std::find(mine.begin(), mine.end(), figs[i]) != mine.end())
            (r.role == RowRole::Tool ? inTool : inOwner)[i] = true;
      });
      if (!s.ok()) return s;
    }
    fromTool.assign(figs.size(), false);
    for (size_t i = 0; i < figs.size(); ++i) fromTool[i] = inTool[i] && !inOwner[i];
    return Status::success();
  }
  // A row's figures for the comparison: every digit run (dates included: a tool row's date
  // parts are figures it holds too, the digest's own dates were left out of figuresOf).
  static std::vector<std::string> figuresOfAll(const std::string& t) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < t.size()) {
      if (!isDigit(t[i])) {
        ++i;
        continue;
      }
      std::string fig;
      while (i < t.size() && (isDigit(t[i]) || ((t[i] == '.' || t[i] == ',') && i + 1 < t.size() && isDigit(t[i + 1])))) {
        if (t[i] != ',') fig += t[i];
        ++i;
      }
      if (fig.size() >= 2) out.push_back(fig);
    }
    return out;
  }

  std::string exportPath(uint64_t at) const {
    const uint64_t day = d.fold.foldEpoch ? d.fold.foldEpoch : at ? at : d.now;
    return std::string(kExportDir) + "/summary-" + dayString(day, d.tzMinutes, d.dayStartMin) + "-" + d.chatKey + ".md";
  }

  std::string anchorText() const {
    std::string s = mt::kDevice;
    const std::string w = when(m.d0.at, m.d0.tz);
    s += w.empty() ? std::string(mt::kEarlierUndated) : std::string(mt::kEarlierThrough) + w;
    s += mt::kEarlierKept;
    if (!m.d0.accepted && !d.fold.summary.empty() && m.d0.exported) s += std::string(mt::kSummarySaved) + m.d0.path + mt::kEnd;
    else if (!m.d0.accepted && !m.d0.carried && !d.fold.summary.empty()) s += mt::kSummaryNotSaved;
    return s;
  }

  // A digest text the driver would store for `day`: valid, its day header first, no path in it.
  static bool validText(const Digest0Answer& a) {
    DigestEntry e{a.day, a.text, 2, 1};
    Item it;
    DigestEntry back;
    std::string header;
    if (!digestItem(e, 0, 1, it).ok() || !digestOf(it, back).ok() || !dayHeader(a.day, header).ok()) return false;
    return a.text.size() <= kDigestMaxBytes && !hasPathLikeToken(a.text) &&
           a.text.compare(0, header.size() + 2, header + digest_text::kDaySep) == 0;
  }

  // The verdict on one answer: the acceptance rule, then the text the driver made of it.
  Status judge(const Digest0Answer& a, uint32_t days, Digest0Verdict& v) {
    const std::vector<std::string> figs = figuresOf(a.summary);
    std::vector<bool> fromTool;
    Status s = toolFigures(figs, fromTool);
    if (!s.ok()) return s;
    v = checkDigest0(a.summary, days, [&](const std::string& f) {
      for (size_t i = 0; i < figs.size(); ++i)
        if (figs[i] == f) return (bool)fromTool[i];
      return false;
    });
    if (v == Digest0Verdict::Accepted && !validText(a)) v = Digest0Verdict::Text;
    return Status::success();
  }

  // Ask until an answer comes or the tries are spent (each counted in the marker first).
  Status askDigest0(uint32_t days) {
    D0Plan& p = m.d0;
    while (m.tries < kDigest0Tries) {
      Digest0Ask ask;
      ask.summary = d.fold.summary;
      ask.at = p.at;
      ask.now = d.now;
      ask.tzMinutes = d.tzMinutes;
      ask.dayStartMin = d.dayStartMin;
      ask.attempt = m.tries++;
      Status s = save();   // counted before it is asked
      if (!s.ok()) return s;
      Digest0Answer a;
      ++out.asks;
      if (!d.digest0(ask, a).ok()) continue;
      if (!(s = judge(a, days, p.verdict)).ok()) return s;
      p.accepted = p.verdict == Digest0Verdict::Accepted;
      if (p.accepted) {
        p.text = a.text;
        p.day = a.day;
      }
      return Status::success();
    }
    return Status::success();
  }

  uint32_t daysCovered(const Covered& c) const {
    if (!c.firstT) return 1;
    return (uint32_t)(dayNumber(c.lastT, d.tzMinutes, d.dayStartMin) - dayNumber(c.firstT, d.tzMinutes, d.dayStartMin) + 1);
  }

  Status planDigest0() {
    Covered c;
    Status s = coveredRows(c);
    if (!s.ok()) return s;
    D0Plan& p = m.d0;
    p = D0Plan{};
    const bool summary = !d.fold.summary.empty();
    p.at = c.lastT ? c.lastT : d.fold.foldEpoch ? d.fold.foldEpoch : d.now;
    p.tz = d.tzMinutes;
    p.anchor = c.rows > 0 || summary;
    p.verdict = summary ? Digest0Verdict::Asked : Digest0Verdict::NoSummary;
    if (p.anchor && summary && d.digest0 && p.at && !(s = askDigest0(daysCovered(c))).ok()) return s;
    if (p.anchor && summary && !p.accepted) {   // the old summary: exported (an extra) and carried
      p.path = exportPath(p.at);
      p.exported = d.exportText && d.exportText(p.path, d.fold.summary);
      out.exported = p.exported;
      p.day = dayString(p.at, d.tzMinutes, d.dayStartMin);
      p.carried = p.at && !carriedParts(p).empty();
    }
    p.q0 = nextQ();
    p.sq = p.q0 + 1;
    p.tz = d.tzMinutes;
    p.seg = log.segment();
    p.off = log.segmentBytes();
    p.have = true;
    return save();
  }

  // The old summary's digest parts for a plan (a pure function of the fold and the plan).
  std::vector<std::string> carriedParts(const D0Plan& p) const {
    return oldSummaryParts(d.fold.summary, p.day, when(d.fold.foldEpoch ? d.fold.foldEpoch : p.at, p.tz),
                           p.exported ? p.path : std::string(), p.onePart);
  }
  // Digest 0 as live entries: the accepted rewrite, or the carried parts (part k at sq + k, covering
  // the anchor and the parts before it).
  std::vector<DigestEntry> digest0Entries() const {
    if (m.d0.accepted) return {DigestEntry{m.d0.day, m.d0.text, m.d0.sq, m.d0.q0}};
    std::vector<DigestEntry> out;
    if (!m.d0.carried) return out;
    uint64_t thru = m.d0.q0;
    for (const std::string& text : carriedParts(m.d0)) {
      const uint64_t q = m.d0.sq + out.size();
      out.push_back(DigestEntry{m.d0.day, text, q, thru});
      thru = q;
    }
    return out;
  }
  // The items of the planned digest-0 step: the anchor note, then digest 0 (rewrite or carried).
  void digest0Items(std::vector<Item>& items) const {
    items.clear();
    if (!m.d0.anchor) return;
    const bool digest = m.d0.accepted || m.d0.carried;
    if (!m.d0.anchorStored) {
      Item h;
      h.q = m.d0.q0;
      h.kind = ItemKind::DeviceNote;
      h.note = digest ? NoteClass::Notice : NoteClass::Omit;
      h.t = m.d0.at;
      h.x = anchorText();
      items.push_back(h);
    }
    for (const DigestEntry& e : digest0Entries()) {
      Item sItem;
      if (!digestItem(e, 0, m.d0.at, sItem).ok()) continue;   // checked when planned: never here
      items.push_back(sItem);
    }
  }

  // A migrated chat starts at the head's default profile (owner ruling 81, ruling 83 item 4); a slug
  // already there (an owner's pin, a re-entry) stays.
  void stampProfile() {
    if (head.profile.empty()) head.profile = profileSlug(kDefaultProfile);
  }

  // ---- applying a planned step ----
  // Compare the stored lines from (seg, off) with the planned items one by one (`next` makes the
  // next one; false at the end), and append the rest. matched: how many were already stored.
  enum class Applied { Done, Overtaken };
  struct Apply {
    LineCursor cur;
    bool reading = true;
    uint64_t q = 0;            // the q the next item must have
    std::string line, mine;
    Applied result = Applied::Done;
    Status status;
    explicit Apply(TxFiles& f, const std::string& k) : cur(f, k) {}
  };
  Status applyStart(Apply& a, uint32_t seg, uint64_t off, uint64_t q0) {
    a.q = q0;
    return a.cur.start(seg, off);
  }
  // One planned item: true to go on.
  bool applyItem(Apply& a, const Item& it) {
    if (it.q != a.q) {
      a.status = Status::fail(Err::InvalidArgument, "tx::migrate plan");
      return false;
    }
    if (!(a.status = encodeLine(it, a.mine)).ok()) return false;
    while (a.reading) {
      LineHeader h;
      bool have = false;
      if (!(a.status = a.cur.next(a.line, h, have)).ok()) return false;
      if (!have) {
        a.reading = false;
        break;
      }
      if (h.q < a.q) continue;   // a q stored twice: readers keep the first
      if (h.q == a.q && a.line == a.mine) {
        ++a.q;
        return true;
      }
      a.result = Applied::Overtaken;
      return false;
    }
    if (log.lastQ() >= a.q || (d.arena && !d.ramFollows && d.arena->lastQ() >= a.q)) {   // another writer took this q
      a.result = Applied::Overtaken;
      return false;
    }
    StoreState st = StoreState::NotStored;
    a.status = log.appendLine(a.mine.data(), a.mine.size(), st);
    if (!a.status.ok()) return false;
    ++a.q;
    ++out.items;
    return true;
  }

  Status applyDigest0(bool& overtaken, bool& anchorStored) {
    overtaken = false;
    std::vector<Item> items;
    digest0Items(items);
    Apply a(*d.files, d.chatKey);
    Status s = applyStart(a, m.d0.seg, m.d0.off, items.empty() ? m.d0.q0 : items.front().q);
    if (!s.ok()) return s;
    anchorStored = m.d0.anchorStored;
    for (const Item& it : items) {
      if (!applyItem(a, it)) break;
      anchorStored |= it.kind == ItemKind::DeviceNote;
    }
    if (a.result == Applied::Overtaken) {
      overtaken = true;
      return Status::success();
    }
    if (!a.status.ok()) return a.status;
    std::vector<DigestEntry> live = digest0Entries();
    if (!live.empty()) {   // the swap's order: the s lines are stored; thru moves, settle, head
      head.thru = std::max<uint64_t>(head.thru, live.back().thru);
      std::string block;
      SettleReport rep;
      (void)settleDigests(*d.files, d.chatKey, profileOfHead(head.profile), live, block, rep);   // the owner's pinned profile, else full
    }
    stampProfile();
    head.nextQ = nextQ();
    (void)log.writeHead(head);   // a cache: a refusal is rebuilt from the items
    return Status::success();
  }

  // ---- the rows batch ----
  Status maxTurn(uint32_t& t) {
    t = 0;
    LineCursor cur(*d.files, d.chatKey);
    Status s = cur.start(head.thruSeg, head.thruOffset);
    std::string line;
    LineHeader h;
    for (bool have = true; s.ok() && have;)
      if ((s = cur.next(line, h, have)).ok() && have) t = std::max(t, h.turn);
    if (!s.ok()) return s;
    if (d.arena && !d.ramFollows)
      for (size_t i = 0; i < d.arena->count(); ++i) {
        ArenaEntry e;
        if (d.arena->header(i, e).ok()) t = std::max(t, e.turn);
      }
    return Status::success();
  }

  // The import budget: the binding's, else the head's (importBudgetFor), never a fixed figure.
  size_t budget() const { return d.importBudget ? d.importBudget : importBudgetFor(d.head, d.arena ? d.arena->capacity() : kActiveChatArenaBytes); }

  // The stored line bytes a row would take (the import budget's measure).
  size_t rowCost(const DayRow& r) {
    Item it;
    std::string line;
    rowItem(r, 1, 1, d.tzMinutes, it);
    return encodeLine(it, line).ok() ? line.size() : 0;
  }

  // What the first pass learns about the importable rows of a range.
  struct Totals {
    uint64_t bytes = 0, rows = 0, lastOwner = 0, lastRow = 0;
  };
  Status totals(uint64_t lo, uint64_t hi, uint64_t oursUpTo, Totals& t) {
    return eachRow(lo, hi, [&](const DayRow& r) {
      if (!importable(r, oursUpTo)) return;
      t.bytes += rowCost(r);
      ++t.rows;
      t.lastRow = r.id;
      if (r.role == RowRole::Owner) t.lastOwner = r.id;
    });
  }

  // The second pass, one row: named while the rows from it on are over the budget, and after that
  // until the next owner row (a turn is imported whole) when one is left; the newest row is always
  // imported.
  struct Cutter {
    const Totals& t;
    size_t budget;
    uint64_t before = 0;
    bool importing = false, first = true;
    bool imports(const DayRow& r, uint64_t cost) {
      if (!importing) {
        const bool fits = t.bytes - before <= budget;
        const bool turnStart = r.role == RowRole::Owner || first || r.id > t.lastOwner;
        importing = (fits && turnStart) || r.id == t.lastRow;
      }
      first = false;
      before += cost;
      return importing;
    }
  };
  static void count(ImportPlan& p, const DayRow& r, bool imported) {
    if (!imported) {
      ++p.named;
      p.cut = r.id;
      if (r.t) p.namedLastT = std::max(p.namedLastT, r.t);
      return;
    }
    ++p.imported;
    if (r.role == RowRole::Owner || p.imported == 1) ++p.turns;
    if (r.t && !p.firstT) p.firstT = r.t;
    if (r.t) p.lastT = std::max(p.lastT, r.t);
  }

  // The plan for rows lo < id <= hi. `any` false when no row is importable.
  Status planBatch(uint64_t lo, uint64_t hi, ImportPlan& p, bool& any) {
    p = ImportPlan{};
    p.lo = p.cut = lo;
    p.hi = hi;
    p.tzMinutes = d.tzMinutes;
    Status s;
    if (m.repair && !m.reentry && m.d0Done && !(s = oursBound(hi, p.oursUpTo)).ok()) return s;
    Totals t;
    s = totals(lo, hi, p.oursUpTo, t);
    any = s.ok() && t.rows > 0;
    if (!any) return s;
    Cutter cut{t, budget()};
    s = eachRow(lo, hi, [&](const DayRow& r) {
      if (importable(r, p.oursUpTo)) count(p, r, cut.imports(r, rowCost(r)));
    });
    if (!s.ok()) return s;
    p.q0 = nextQ();
    return maxTurn(p.turnBase);
  }

  // How far the already stored part of a batch reaches.
  struct Matched {
    bool note = false;
    uint64_t row = 0;
    bool open = false;
    uint32_t turn = 0;
    uint64_t t = 0;
    void saw(const Item& it, uint64_t r) {
      note |= it.kind == ItemKind::DeviceNote;
      if (r) row = r;
      if (it.kind == ItemKind::User || it.kind == ItemKind::Assistant) {
        open = true;
        turn = it.turn;
        t = it.t;
      }
      if (it.kind == ItemKind::TurnEnd) open = false;
    }
  };

  // The batch's rows from the source, page by page, into the builder.
  Status feedRows(BatchBuilder& b, const BatchBuilder::Emit& emit, const bool& going) {
    std::vector<DayRow> page;
    page.reserve(kRowPage);
    for (uint64_t after = m.plan.cut; going;) {
      bool end = false;
      page.clear();
      Status s = d.rows->read(after, m.plan.hi, kRowPage, page, end);
      if (!s.ok()) return s;
      if (!page.empty()) after = page.back().id;
      end |= page.empty();
      if (!(s = b.feed(page, end, emit)).ok() || end) return s;
    }
    return Status::success();
  }

  Status applyBatch(bool& overtaken, Matched& got) {
    overtaken = false;
    Apply a(*d.files, d.chatKey);
    Status s = applyStart(a, m.seg, m.off, m.plan.q0);
    if (!s.ok()) return s;
    BatchBuilder b(m.plan);
    bool going = true;
    uint64_t maxM = 0;
    auto emit = [&](const Item& it, uint64_t row) {
      const uint32_t before = out.items;
      if (!(going = applyItem(a, it))) return false;
      if (it.kind == ItemKind::User) maxM = std::max(maxM, it.m);
      if (out.items == before) got.saw(it, row);   // already stored
      else if (row) ++out.imported, out.turns += it.kind == ItemKind::User;
      return true;
    };
    if (!(s = feedRows(b, emit, going)).ok()) return s;
    overtaken = a.result == Applied::Overtaken;
    if (overtaken || !a.status.ok()) return a.status;
    stampProfile();
    head.nextQ = nextQ();
    head.mHi = std::max(head.mHi, maxM);   // head.json's m_hi: the newest row mirrored
    (void)log.writeHead(head);
    return Status::success();
  }

  // Another writer stored items where the batch's own belonged (the binding let the engine store
  // before Done). What is stored stays; a turn left open is closed; the rest is planned again
  // after it.
  Status settleOvertaken(const Matched& got) {
    out.overtaken = true;
    if (got.note) out.named += m.plan.named;
    bool closed = false;
    if (got.open) {   // a refused marker save after an earlier pass appended it: not twice
      LineCursor cur(*d.files, d.chatKey);
      Status s = cur.start(m.seg, m.off);
      std::string line;
      LineHeader h, last;
      for (bool have = true; s.ok() && have;)
        if ((s = cur.next(line, h, have)).ok() && have) last = h;
      if (!s.ok()) return s;
      closed = last.kind == ItemKind::TurnEnd && last.turn == got.turn && last.q > m.plan.q0;
    }
    if (got.open && !closed) {
      Item e;
      e.q = nextQ();
      e.kind = ItemKind::TurnEnd;
      e.turn = got.turn;
      e.t = got.t;
      e.end.cause = "import";
      StoreState st = StoreState::NotStored;
      Status s = log.append(e, st);
      if (!s.ok()) return s;
      if (st != StoreState::Stored) return Status::fail(Err::NotStored, "tx::migrate append");
      ++out.items;
    }
    m.hi = std::max(m.hi, got.note ? std::max(m.plan.cut, got.row) : m.plan.lo);
    m.batch = false;
    return save();
  }

  // A loop, not a recursion (the tg_poll stack gate): an overtaken batch is planned again after
  // what is stored.
  Status rowsStep() {
    for (;;) {
      if (!m.batch) {
        uint64_t newest = 0;
        Status s = d.rows->newest(newest);
        if (!s.ok()) return s;
        const uint64_t lo = m.reentry ? m.hi : std::max(m.hi, d.fold.thru);
        bool any = false;
        if (newest > lo && !(s = planBatch(lo, newest, m.plan, any)).ok()) return s;
        if (!any) {   // nothing to import: account for what was looked at
          const uint64_t hi = std::max(m.hi, std::max(lo, newest));
          if (m.reentry && hi == m.hi && m.step == MigrationStep::Done) return Status::success();
          m.hi = hi;
          m.step = MigrationStep::Done;
          m.reentry = true;
          return save();
        }
        m.batch = true;
        m.seg = log.segment();
        m.off = log.segmentBytes();
        m.step = MigrationStep::Rows;
        if (!(s = save()).ok()) return s;
      }
      bool overtaken = false;
      Matched got;
      Status s = applyBatch(overtaken, got);
      if (!s.ok()) return s;
      if (overtaken) {
        if (!(s = settleOvertaken(got)).ok()) return s;
        continue;   // plan the rest after what is stored
      }
      out.named += m.plan.named;
      m.hi = std::max(m.hi, m.plan.hi);
      m.batch = false;
      m.step = MigrationStep::Done;
      m.reentry = true;
      return save();
    }
  }

  Status digest0Step() {
    for (;;) {
      Status s;
      if (!m.d0.have) {
        m.step = MigrationStep::Digest0;
        if (!(s = planDigest0()).ok()) return s;
      }
      bool overtaken = false, anchorStored = false;
      if (!(s = applyDigest0(overtaken, anchorStored)).ok()) return s;
      if (overtaken) {
        if (!(s = replanOvertaken(anchorStored)).ok()) return s;
        continue;
      }
      m.d0Done = m.d0.accepted || m.d0.carried;
      m.step = MigrationStep::Rows;
      return save();
    }
  }

  // Another writer stored items where the digest-0 step's own belonged.
  Status replanOvertaken(bool anchorStored) {
    out.overtaken = true;
    if (anchorStored && (m.d0.accepted || m.d0.carried) && head.thru < m.d0.q0) {
      // the anchor is stored before the other writer's items: digest 0 follows them, covering the
      // anchor alone (thru stays the anchor's q, so nothing after it is hidden)
      m.d0.anchorStored = true;
      m.d0.onePart = m.d0.carried;   // a second part would cover the other writer's items
      m.d0.sq = nextQ();
    } else {   // never a rewrite over items it did not summarize: the old summary itself (RB-H81)
      m.d0.anchorStored = false;
      m.d0.accepted = false;
      if (!d.fold.summary.empty() && !m.d0.exported) {
        m.d0.path = exportPath(m.d0.at);
        m.d0.exported = d.exportText && d.exportText(m.d0.path, d.fold.summary);
      }
      m.d0.day = dayString(m.d0.at, m.d0.tz, d.dayStartMin);
      m.d0.carried = m.d0.anchor && !d.fold.summary.empty() && m.d0.at && !carriedParts(m.d0).empty();
      m.d0.q0 = nextQ();
      m.d0.sq = m.d0.q0 + 1;
    }
    m.d0.seg = log.segment();
    m.d0.off = log.segmentBytes();
    return save();
  }


  // The newest mirrored owner row in the log and the assistant texts stored after it.
  Status logTail(uint64_t& lastM, std::vector<std::string>& tail) {
    std::vector<uint32_t> segs;
    Status s = segmentList(*d.files, d.chatKey, segs);
    if (!s.ok() || segs.empty()) return s;
    LineCursor cur(*d.files, d.chatKey);
    if (!(s = cur.start(segs.front(), 0)).ok()) return s;
    std::string line;
    LineHeader h;
    Item it;
    bool after = false;
    for (bool have = true; have;) {
      if (!(s = cur.next(line, h, have)).ok()) return s;
      if (!have || (h.kind != ItemKind::User && h.kind != ItemKind::Assistant)) continue;
      if (!decodeLine(line.data(), line.size(), it).ok()) continue;
      if (it.kind == ItemKind::Assistant) {
        if (after) tail.push_back(it.x);
        continue;
      }
      after = it.m && it.m >= lastM;
      if (after) lastM = it.m, tail.clear();
    }
    return Status::success();
  }

  // A log with items and no marker (lost, or engine 2 ran here without a migration): hi is the
  // newest mirrored owner row (never below the old pointer), then past it the assistant rows the
  // log already holds as the a items after that owner row.
  Status fallbackHi(uint64_t& hi) {
    uint64_t lastM = 0;
    std::vector<std::string> tail;
    Status s = logTail(lastM, tail);
    if (!s.ok()) return s;
    hi = std::max(lastM, d.fold.thru);
    std::vector<DayRow> page;
    size_t j = 0;
    for (uint64_t from = hi;;) {
      bool end = false;
      page.clear();
      if (!(s = d.rows->read(from, 0, kRowPage, page, end)).ok()) return s;
      for (const DayRow& r : page) {
        from = r.id;
        if (!importable(r, 0)) continue;
        Item it;
        rowItem(r, 1, 1, d.tzMinutes, it);
        if (r.role == RowRole::Owner || j >= tail.size() || tail[j] != it.x) return Status::success();
        ++j;
        hi = r.id;
      }
      if (end || page.empty()) return Status::success();
    }
  }

  // Items with no marker (P3): true when they came from a migration whose marker was lost, false
  // when engine 2 stored them before any import. Lane E2-FIX3 (finding R6): the import's own items
  // anywhere in the log say it ran (digest 0's anchor note, a turn the import closed), also when
  // engine 2's items come first (the P3 repair appends after them; a digest-only import has no
  // imported u item). Else the first mirrored owner row decides: the earlier engine's is an imported
  // u item. No u item naming a row, or a row the source cannot find, counts as imported: nothing is
  // imported twice.
  static bool importItem(const LineHeader& h, const std::string& line) {
    if (h.kind != ItemKind::DeviceNote && h.kind != ItemKind::TurnEnd) return false;
    Item it;
    if (!decodeLine(line.data(), line.size(), it).ok()) return false;
    return h.kind == ItemKind::TurnEnd ? it.end.cause == "import" : it.x.find(mt::kEarlierKept) != std::string::npos;
  }
  Status importedBefore(bool& imported) {
    imported = true;
    std::vector<uint32_t> segs;
    Status s = segmentList(*d.files, d.chatKey, segs);
    if (!s.ok() || segs.empty()) return s;
    LineCursor cur(*d.files, d.chatKey);
    if (!(s = cur.start(segs.front(), 0)).ok()) return s;
    std::string line;
    LineHeader h;
    uint64_t firstM = 0;
    for (bool have = true; have;) {
      if (!(s = cur.next(line, h, have)).ok()) return s;
      if (!have) break;
      if (importItem(h, line)) return Status::success();
      Item it;
      if (!firstM && h.kind == ItemKind::User && decodeLine(line.data(), line.size(), it).ok()) firstM = it.m;
    }
    if (!firstM) return Status::success();
    std::vector<DayRow> page;
    bool end = false;
    if (!(s = d.rows->read(firstM - 1, firstM, 1, page, end)).ok()) return s;
    imported = page.empty() || page.front().id != firstM || !page.front().ours;
    return Status::success();
  }

  // The P3 repair under digest 0: engine 2's own rows that the stored items hold (up to the
  // newest mirrored owner row and its reply, so before the next owner row of engine 2's), never
  // the rows of turns still in RAM, which reopen() stores after the import. With no mirrored row
  // in the log every row up to `hi` counts.
  Status oursBound(uint64_t hi, uint64_t& upTo) {
    uint64_t lastM = 0;
    std::vector<std::string> tail;
    Status s = logTail(lastM, tail);
    if (!s.ok()) return s;
    upTo = hi;
    if (!lastM) return Status::success();
    bool found = false;
    return eachRow(lastM, hi, [&](const DayRow& r) {
      if (found || !r.ours || r.role != RowRole::Owner) return;
      found = true;
      upTo = r.id - 1;
    });
  }

  // No marker: a fresh chat, a lost marker after an import, or engine-2 items stored before one.
  Status startWithoutMarker() {
    m = Marker{};
    if (log.lastQ() == 0) return Status::success();
    bool imported = false;
    Status s = importedBefore(imported);
    if (!s.ok()) return s;
    if (!imported) {
      m.repair = true;   // P3: engine 2 stored before the import: the first migration in full
      return Status::success();
    }
    m.reentry = true;   // a lost marker: only rows since the newest mirrored one are new
    m.step = MigrationStep::Done;
    return fallbackHi(m.hi);
  }

  Status go() {
    const bool had = readMarker(*d.files, d.chatKey, m);
    Status s = log.open(head);
    if (!s.ok()) return s;
    if (!had && !(s = startWithoutMarker()).ok()) return s;
    out.reentry = m.reentry;
    if (!m.reentry && (m.step == MigrationStep::NotStarted || m.step == MigrationStep::Digest0)) {
      if (!(s = digest0Step()).ok()) return s;
    }
    if (!(s = rowsStep()).ok()) return s;
    out.digest0 = m.d0Done;
    out.carried = m.d0Done && m.d0.carried && !m.d0.accepted;
    out.verdict = m.d0.have ? m.d0.verdict : Digest0Verdict::NoSummary;
    out.exported = m.d0.exported;
    out.hi = m.hi;
    out.step = MigrationStep::Done;
    return Status::success();
  }
};

// The import needs a budget: the binding's, or a named head's (owner ruling 81: never a fixed one).
bool depsOk(const MigrateDeps& d) {
  return d.files && d.rows && chatKeyOk(d.chatKey) && (d.importBudget || importBudgetFor(d.head, kActiveChatArenaBytes));
}

}  // namespace

size_t importBudgetFor(const HeadKey& head, size_t arenaBytes) {
  HeadBudget b;
  if (head.host.empty() || head.model.empty() || !budgetFor(head, Profile::Full, b).ok()) return 0;
  return std::min<size_t>(b.marks.high, arenaBytes - arenaBytes / 4);
}

Status migrate(const MigrateDeps& deps, MigrationOutcome& out) {
  out = MigrationOutcome{};
  if (!depsOk(deps)) return Status::fail(Err::InvalidArgument, "tx::migrate");
  try {
    std::unique_ptr<Run> run(new Run(deps, out));   // off the caller's stack (the tg_poll gate, E2-FIX2B)
    const Status s = run->go();
    if (!s.ok()) out.step = run->m.step;
    return s;
  } catch (const std::bad_alloc&) {
    out.step = MigrationStep::NotStarted;
    return Status::fail(Err::TooLarge, "tx::migrate");
  }
}

Status isReentry(TxFiles& files, const std::string& chatKey, bool& out) {
  out = false;
  if (!chatKeyOk(chatKey)) return Status::fail(Err::InvalidArgument, "tx::isReentry");
  try {
    Marker m;
    if (readMarker(files, chatKey, m)) out = m.reentry;
    return Status::success();
  } catch (const std::bad_alloc&) {
    return Status::fail(Err::TooLarge, "tx::isReentry");
  }
}

namespace {

void anomaly(MigrationReport& r, const char* slug) {
  ++r.anomalies;
  if (std::find(r.anomalyClasses.begin(), r.anomalyClasses.end(), slug) == r.anomalyClasses.end())
    r.anomalyClasses.push_back(slug);
}

}  // namespace

namespace {

void foldAnomalies(const OldFold& f, uint64_t newest, MigrationReport& r) {
  if (f.thru && f.summary.empty()) anomaly(r, "pointer-without-summary");
  if (!f.thru && !f.summary.empty()) anomaly(r, "summary-without-pointer");
  if (f.shift && f.shift <= f.thru) anomaly(r, "shift-not-after-pointer");
  if (f.thru > newest) anomaly(r, "pointer-past-rows");
}

// The dry run's count of one row (counts and anomaly classes only, never content).
struct RowCounter {
  const MigrateDeps& d;
  MigrationReport& r;
  bool re;
  std::vector<int64_t> days;
  uint64_t lastT = 0;
  void conversation(const DayRow& row) {
    const OldFold& f = d.fold;
    ++r.conversation;
    if (row.text.empty()) anomaly(r, "empty-text");
    if (row.text.size() > kTier1OwnerText) anomaly(r, "text-over-tier1");
    if (!re && row.id <= f.thru) ++r.covered;
    if (!re && row.id > f.thru && f.shift && row.id <= f.shift) ++r.betweenPointers;
  }
  void time(const DayRow& row) {
    if (!row.t) {
      anomaly(r, "row-without-time");
      return;
    }
    if (row.t < lastT) anomaly(r, "time-backwards");
    lastT = row.t;
    const int64_t dn = dayNumber(row.t, d.tzMinutes, d.dayStartMin);
    if (std::find(days.begin(), days.end(), dn) == days.end()) days.push_back(dn);
  }
  void operator()(const DayRow& row) {
    ++r.rows;
    r.ours += row.ours;
    const bool conv = row.role == RowRole::Owner || row.role == RowRole::Assistant;
    if (!conv) ++r.toolRows;
    if (conv && !row.ours) conversation(row);
    time(row);
  }
};

}  // namespace

Status dryRun(const MigrateDeps& d, MigrationReport& r) {
  r = MigrationReport{};
  if (!depsOk(d)) return Status::fail(Err::InvalidArgument, "tx::dryRun");
  try {
    MigrationOutcome scratch;
    Run run(d, scratch);
    bool re = false;
    Status s = isReentry(*d.files, d.chatKey, re);
    if (!s.ok()) return s;
    r.reentry = re;
    (void)readMarker(*d.files, d.chatKey, run.m);
    uint64_t newest = 0;
    if (!(s = d.rows->newest(newest)).ok()) return s;
    foldAnomalies(d.fold, newest, r);
    RowCounter counter{d, r, re};
    if (!(s = run.eachRow(0, 0, [&](const DayRow& row) { counter(row); })).ok()) return s;
    r.days = (uint32_t)counter.days.size();
    const uint64_t lo = re ? run.m.hi : std::max(run.m.hi, d.fold.thru);
    ImportPlan p;
    bool any = false;
    run.head = HeadState{};   // planning only: no log is opened (it may write)
    if (newest > lo && !(s = run.planBatch(lo, newest, p, any)).ok()) return s;
    r.toImport = p.imported;
    r.toName = p.named;
    if (p.named) anomaly(r, "rows-over-budget");
    return Status::success();
  } catch (const std::bad_alloc&) {
    r = MigrationReport{};
    return Status::fail(Err::TooLarge, "tx::dryRun");
  }
}

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
