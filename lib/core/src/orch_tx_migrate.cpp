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

// A month name next to [s, e): the word before it ("Nov 17", "Nov. 17") or after it ("17 Nov",
// "17th of" is not one, "17th Nov" is).
bool besideMonth(const std::string& t, size_t s, size_t e) {
  size_t b = s;
  for (int k = 0; k < 2 && b > 0 && (t[b - 1] == ' ' || t[b - 1] == '.'); ++k) --b;
  size_t a = b;
  while (a > 0 && isAlpha(t[a - 1])) --a;
  if (a < b && monthWord(t, a, b)) return true;
  size_t f = e;
  if (f + 2 <= t.size() && (f + 2 == t.size() || !isAlpha(t[f + 2]))) {
    const std::string suf = t.substr(f, 2);
    if (suf == "st" || suf == "nd" || suf == "rd" || suf == "th") f += 2;
  }
  for (int k = 0; k < 2 && f < t.size() && (t[f] == ' ' || t[f] == '.' || t[f] == ','); ++k) ++f;
  size_t g = f;
  while (g < t.size() && isAlpha(t[g])) ++g;
  return monthWord(t, f, g);
}

}  // namespace

std::vector<std::string> figuresOf(const std::string& t) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < t.size()) {
    if (!isDigit(t[i])) {
      ++i;
      continue;
    }
    const size_t s = i;
    while (i < t.size() && (isDigit(t[i]) || ((t[i] == '.' || t[i] == ',') && i + 1 < t.size() && isDigit(t[i + 1]))))
      ++i;
    const size_t e = i;
    const bool dateBefore = s >= 2 && (t[s - 1] == '-' || t[s - 1] == ':' || t[s - 1] == '/') && isDigit(t[s - 2]);
    const bool dateAfter = e + 1 < t.size() && (t[e] == '-' || t[e] == ':' || t[e] == '/') && isDigit(t[e + 1]);
    if (dateBefore || dateAfter) continue;
    std::string fig;
    for (size_t k = s; k < e; ++k)
      if (t[k] != ',') fig += t[k];
    if (fig.size() < 2) continue;
    const bool plain = fig.find('.') == std::string::npos;
    if (plain && fig.size() == 4 && (fig.compare(0, 2, "19") == 0 || fig.compare(0, 2, "20") == 0)) continue;
    if (besideMonth(t, s, e)) continue;
    if (std::find(out.begin(), out.end(), fig) == out.end()) out.push_back(fig);
  }
  return out;
}

bool hasPathLikeToken(const std::string& text) {
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && isSpace(text[i])) ++i;
    size_t s = i;
    while (i < text.size() && !isSpace(text[i])) ++i;
    size_t e = i;
    auto trimmable = [](char c) { return std::strchr("()[]{}\"'.,;:!?<>*`", c) != nullptr; };
    while (s < e && trimmable(text[s]) && text[s] != '/' && text[s] != '\\') ++s;
    while (e > s && trimmable(text[e - 1])) --e;
    if (e <= s) continue;
    const std::string tok = text.substr(s, e - s);
    for (size_t k = 0; k < tok.size(); ++k) {
      if (tok[k] != '/' && tok[k] != '\\') continue;
      const bool before = k > 0, after = k + 1 < tok.size();
      if (!after) continue;
      if (!before) {
        if (isAlpha(tok[k + 1]) || tok[k + 1] == '.') return true;   // "/data", "/.cfg"
        continue;
      }
      if (isDigit(tok[k - 1]) && isDigit(tok[k + 1])) continue;       // "1/2"
      if (tok[k - 1] == ' ' || tok[k + 1] == ' ') continue;
      return true;
    }
    const size_t dot = tok.rfind('.');
    if (dot != std::string::npos && dot > 0 && tok.size() - dot - 1 >= 2 && tok.size() - dot - 1 <= 4) {
      bool ext = true, letter = false, nameLetter = false;
      for (size_t k = dot + 1; k < tok.size(); ++k) {
        ext &= isLowerAlnum(tok[k]);
        letter |= isAlpha(tok[k]);
      }
      for (size_t k = 0; k < dot; ++k) nameLetter |= isAlpha(tok[k]);
      if (ext && letter && nameLetter) return true;
    }
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

bool importable(const DayRow& r) {
  return !r.ours && (r.role == RowRole::Owner || r.role == RowRole::Assistant) && !r.text.empty();
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

Status BatchBuilder::feed(const std::vector<DayRow>& rows, bool last,
                          const std::function<bool(const Item&, uint64_t)>& emit) {
  if (stopped_) return Status::success();
  Item it;
  auto put = [&](uint64_t row) {
    if (!emit(it, row)) stopped_ = true;
    ++q_;
    return !stopped_;
  };
  if (!noted_) {
    noted_ = true;
    q_ = plan_.q0;
    turn_ = plan_.turnBase;
    it = Item{};
    it.q = q_;
    it.kind = ItemKind::DeviceNote;
    it.note = plan_.named ? NoteClass::Omit : NoteClass::Notice;
    it.t = plan_.firstT ? plan_.firstT : plan_.namedLastT;
    it.x = importNote(plan_);
    if (!put(0)) return Status::success();
  }
  for (const DayRow& r : rows) {
    if (r.id <= plan_.cut || r.id > plan_.hi || !importable(r)) continue;
    if (r.role == RowRole::Owner || !open_) {
      if (open_) {
        it = Item{};
        it.q = q_;
        it.kind = ItemKind::TurnEnd;
        it.turn = turn_;
        it.t = lastT_;
        it.end.cause = "import";
        if (!put(0)) return Status::success();
      }
      ++turn_;
      open_ = true;
    }
    rowItem(r, q_, turn_, plan_.tzMinutes, it);
    lastT_ = r.t;
    if (!put(r.id)) return Status::success();
  }
  if (last && open_) {
    it = Item{};
    it.q = q_;
    it.kind = ItemKind::TurnEnd;
    it.turn = turn_;
    it.t = lastT_;
    it.end.cause = "import";
    open_ = false;
    put(0);
  }
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
  num(s, "tries", m.tries);
  num(s, "hi", m.hi);
  num(s, "d0done", m.d0Done);
  if (m.d0.have) {
    num(s, "d0", 1);
    num(s, "d0anc", m.d0.anchor);
    num(s, "d0acc", m.d0.accepted);
    num(s, "d0exp", m.d0.exported);
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
  m.tries = (uint32_t)u("tries");
  m.hi = u("hi");
  m.d0Done = u("d0done") != 0;
  if (u("d0")) {
    D0Plan& d = m.d0;
    d.have = true;
    d.anchor = u("d0anc") != 0;
    d.accepted = u("d0acc") != 0;
    d.exported = u("d0exp") != 0;
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
      char chunk[kFillSliceBytes];
      size_t got = 0;
      if (!files_.readAt(path, (size_t)off_, chunk, sizeof chunk, got))
        return Status::fail(Err::Io, "tx::migrate read");
      if (got == 0) {   // segment end: a torn tail is dropped
        ++idx_;
        off_ = 0;
        buf_.clear();
        continue;
      }
      if (buf_.size() > kMaxLineBytes) buf_.clear();   // no line is this long: the rest of it is dropped
      buf_.append(chunk, got);
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
  std::string buf_;
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
    if (d.arena) q = std::max<uint64_t>(q, d.arena->lastQ() + 1);
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
    if (!m.d0.accepted && !d.fold.summary.empty())
      s += m.d0.exported ? std::string(mt::kSummarySaved) + m.d0.path + mt::kEnd : std::string(mt::kSummaryNotSaved);
    return s;
  }

  Status planDigest0() {
    Covered c;
    Status s = coveredRows(c);
    if (!s.ok()) return s;
    D0Plan& p = m.d0;
    p = D0Plan{};
    p.at = c.lastT ? c.lastT : d.fold.foldEpoch ? d.fold.foldEpoch : d.now;
    p.anchor = c.rows > 0 || !d.fold.summary.empty();
    p.verdict = d.fold.summary.empty() ? Digest0Verdict::NoSummary : Digest0Verdict::Asked;
    if (p.anchor && !d.fold.summary.empty() && d.digest0 && p.at) {
      const uint32_t days =
          c.firstT ? (uint32_t)(dayNumber(c.lastT, d.tzMinutes, d.dayStartMin) - dayNumber(c.firstT, d.tzMinutes, d.dayStartMin) + 1) : 1;
      while (m.tries < kDigest0Tries) {
        Digest0Ask ask;
        ask.summary = d.fold.summary;
        ask.at = p.at;
        ask.now = d.now;
        ask.tzMinutes = d.tzMinutes;
        ask.dayStartMin = d.dayStartMin;
        ask.attempt = m.tries;
        ++m.tries;
        if (!(s = save()).ok()) return s;   // counted before it is asked
        Digest0Answer a;
        ++out.asks;
        if (!d.digest0(ask, a).ok()) continue;
        std::vector<std::string> figs = figuresOf(a.summary);
        std::vector<bool> fromTool;
        if (!(s = toolFigures(figs, fromTool)).ok()) return s;
        p.verdict = checkDigest0(a.summary, days, [&](const std::string& f) {
          for (size_t i = 0; i < figs.size(); ++i)
            if (figs[i] == f) return (bool)fromTool[i];
          return false;
        });
        if (p.verdict == Digest0Verdict::Accepted) {
          DigestEntry e{a.day, a.text, 2, 1};
          Item it;
          DigestEntry back;
          std::string header;
          const bool valid = digestItem(e, 0, 1, it).ok() && digestOf(it, back).ok() && a.text.size() <= kDigestMaxBytes &&
                             !hasPathLikeToken(a.text) && dayHeader(a.day, header).ok() &&
                             a.text.compare(0, header.size() + 2, header + digest_text::kDaySep) == 0;
          if (!valid) p.verdict = Digest0Verdict::Text;
        }
        if (p.verdict == Digest0Verdict::Accepted) {
          p.accepted = true;
          p.text = a.text;
          p.day = a.day;
        }
        break;
      }
    }
    if (p.anchor && !p.accepted && !d.fold.summary.empty()) {
      p.path = exportPath(p.at);
      p.exported = d.exportText && d.exportText(p.path, d.fold.summary);
      out.exported = p.exported;
    }
    p.q0 = nextQ();
    p.sq = p.q0 + 1;
    p.tz = d.tzMinutes;
    p.seg = log.segment();
    p.off = log.segmentBytes();
    p.have = true;
    return save();
  }

  // The items of the planned digest-0 step: the anchor note, then digest 0 when accepted.
  void digest0Items(std::vector<Item>& items) const {
    items.clear();
    if (!m.d0.anchor) return;
    if (m.d0.anchorStored) {
      if (m.d0.accepted) items.push_back(digest0Item());
      return;
    }
    Item h;
    h.q = m.d0.q0;
    h.kind = ItemKind::DeviceNote;
    h.note = m.d0.accepted ? NoteClass::Notice : NoteClass::Omit;
    h.t = m.d0.at;
    h.x = anchorText();
    items.push_back(h);
    if (m.d0.accepted) items.push_back(digest0Item());
  }
  Item digest0Item() const {
    Item sItem;
    DigestEntry e{m.d0.day, m.d0.text, m.d0.sq, m.d0.q0};
    if (!digestItem(e, 0, m.d0.at, sItem).ok()) sItem = Item{};   // checked when planned: never here
    return sItem;
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
    if (log.lastQ() >= a.q || (d.arena && d.arena->lastQ() >= a.q)) {   // another writer took this q
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
    if (m.d0.accepted) {   // the swap's order: the s line is stored; thru moves, settle, head
      head.thru = std::max<uint64_t>(head.thru, m.d0.q0);
      std::vector<DigestEntry> live{DigestEntry{m.d0.day, m.d0.text, m.d0.sq, m.d0.q0}};
      std::string block;
      SettleReport rep;
      (void)settleDigests(*d.files, d.chatKey, Profile::Standard, live, block, rep);
    }
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
    if (d.arena)
      for (size_t i = 0; i < d.arena->count(); ++i) {
        ArenaEntry e;
        if (d.arena->header(i, e).ok()) t = std::max(t, e.turn);
      }
    return Status::success();
  }

  // The plan for rows lo < id <= hi: what fits the budget (newest first, whole turns), the rest
  // named. `any` false when no row is importable.
  Status planBatch(uint64_t lo, uint64_t hi, ImportPlan& p, bool& any) {
    p = ImportPlan{};
    p.lo = lo;
    p.cut = lo;
    p.hi = hi;
    p.tzMinutes = d.tzMinutes;
    std::string line;
    Item it;
    auto cost = [&](const DayRow& r) {
      rowItem(r, 1, 1, d.tzMinutes, it);
      return encodeLine(it, line).ok() ? line.size() : 0;
    };
    uint64_t total = 0, rows = 0, lastOwner = 0, lastRow = 0;
    Status s = eachRow(lo, hi, [&](const DayRow& r) {
      if (!importable(r)) return;
      total += cost(r);
      ++rows;
      lastRow = r.id;
      if (r.role == RowRole::Owner) lastOwner = r.id;
    });
    if (!s.ok()) return s;
    any = rows > 0;
    if (!any) return Status::success();
    // Walk forward: a row is named while the rows from it on are over the budget, and after that
    // until the next owner row (a turn is imported whole) when one is left; the newest row is
    // always imported.
    uint64_t before = 0;
    bool importing = false;
    bool firstSeen = false;
    s = eachRow(lo, hi, [&](const DayRow& r) {
      if (!importable(r)) return;
      const uint64_t c = cost(r);
      if (!importing) {
        const bool fits = total - before <= d.importBudget;
        const bool turnStart = r.role == RowRole::Owner || !firstSeen || r.id > lastOwner;
        if ((fits && turnStart) || r.id == lastRow) importing = true;
      }
      firstSeen = true;
      before += c;
      if (!importing) {
        ++p.named;
        p.cut = r.id;
        if (r.t) p.namedLastT = std::max(p.namedLastT, r.t);
        return;
      }
      ++p.imported;
      if (r.role == RowRole::Owner || p.imported == 1) ++p.turns;
      if (r.t && !p.firstT) p.firstT = r.t;
      if (r.t) p.lastT = std::max(p.lastT, r.t);
    });
    if (!s.ok()) return s;
    p.q0 = nextQ();
    return maxTurn(p.turnBase);
  }

  struct Matched {
    bool note = false;
    uint64_t row = 0;
    bool open = false;
    uint32_t turn = 0;
    uint64_t t = 0;
  };
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
      going = applyItem(a, it);
      if (!going) return false;
      if (it.kind == ItemKind::User) maxM = std::max(maxM, it.m);
      if (out.items == before) {   // already stored: remember how far the stored part reaches
        if (it.kind == ItemKind::DeviceNote) got.note = true;
        if (row) got.row = row;
        if (it.kind == ItemKind::User || it.kind == ItemKind::Assistant) {
          got.open = true;
          got.turn = it.turn;
          got.t = it.t;
        }
        if (it.kind == ItemKind::TurnEnd) got.open = false;
      } else if (row) {
        ++out.imported;
        if (it.kind == ItemKind::User) ++out.turns;
      }
      return true;
    };
    std::vector<DayRow> page;
    page.reserve(kRowPage);
    uint64_t after = m.plan.cut;
    while (going) {
      bool end = false;
      page.clear();
      if (!(s = d.rows->read(after, m.plan.hi, kRowPage, page, end)).ok()) return s;
      if (!page.empty()) after = page.back().id;
      if (!(s = b.feed(page, end || page.empty(), emit)).ok()) return s;
      if (end || page.empty()) break;
    }
    if (a.result == Applied::Overtaken) {
      overtaken = true;
      return Status::success();
    }
    if (!a.status.ok()) return a.status;
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

  Status rowsStep() {
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
      return rowsStep();   // plan the rest after what is stored
    }
    out.named += m.plan.named;
    m.hi = std::max(m.hi, m.plan.hi);
    m.batch = false;
    m.step = MigrationStep::Done;
    m.reentry = true;
    return save();
  }

  Status digest0Step() {
    Status s;
    if (!m.d0.have) {
      m.step = MigrationStep::Digest0;
      if (!(s = planDigest0()).ok()) return s;
    }
    bool overtaken = false, anchorStored = false;
    if (!(s = applyDigest0(overtaken, anchorStored)).ok()) return s;
    if (overtaken && anchorStored && m.d0.accepted && head.thru < m.d0.q0) {
      // the anchor is stored before the other writer's items: digest 0 follows them, covering the
      // anchor alone (thru stays the anchor's q, so nothing after it is hidden)
      out.overtaken = true;
      m.d0.anchorStored = true;
      m.d0.sq = nextQ();
      m.d0.seg = log.segment();
      m.d0.off = log.segmentBytes();
      if (!(s = save()).ok()) return s;
      return digest0Step();
    }
    if (overtaken) {   // never a digest over items it did not summarize: the old summary is named
      out.overtaken = true;
      m.d0.anchorStored = false;
      m.d0.accepted = false;
      if (!d.fold.summary.empty() && !m.d0.exported) {
        m.d0.path = exportPath(m.d0.at);
        m.d0.exported = d.exportText && d.exportText(m.d0.path, d.fold.summary);
      }
      m.d0.q0 = nextQ();
      m.d0.sq = m.d0.q0 + 1;
      m.d0.seg = log.segment();
      m.d0.off = log.segmentBytes();
      if (!(s = save()).ok()) return s;
      return digest0Step();
    }
    m.d0Done = m.d0.accepted;
    m.step = MigrationStep::Rows;
    return save();
  }

  // A log with items and no marker (lost, or engine 2 ran here without a migration): hi is the
  // newest mirrored owner row (never below the old pointer), then past it the assistant rows the
  // log already holds as the a items after that owner row.
  Status fallbackHi(uint64_t& hi) {
    std::vector<uint32_t> segs;
    Status s = segmentList(*d.files, d.chatKey, segs);
    if (!s.ok()) return s;
    uint64_t lastM = 0;
    std::vector<std::string> tail;
    bool after = false;
    if (!segs.empty()) {
      LineCursor cur(*d.files, d.chatKey);
      if (!(s = cur.start(segs.front(), 0)).ok()) return s;
      std::string line;
      LineHeader h;
      Item it;
      for (bool have = true; have;) {
        if (!(s = cur.next(line, h, have)).ok()) return s;
        if (!have || (h.kind != ItemKind::User && h.kind != ItemKind::Assistant)) continue;
        if (!decodeLine(line.data(), line.size(), it).ok()) continue;
        if (it.kind == ItemKind::User) {
          after = it.m && it.m >= lastM;
          if (after) {
            lastM = it.m;
            tail.clear();
          }
        } else if (after) {
          tail.push_back(it.x);
        }
      }
    }
    hi = std::max(lastM, d.fold.thru);
    std::vector<DayRow> page;
    size_t j = 0;
    for (uint64_t from = hi;;) {
      bool end = false;
      page.clear();
      if (!(s = d.rows->read(from, 0, kRowPage, page, end)).ok()) return s;
      for (const DayRow& r : page) {
        from = r.id;
        if (!importable(r)) continue;
        Item it;
        rowItem(r, 1, 1, d.tzMinutes, it);
        if (r.role == RowRole::Owner || j >= tail.size() || tail[j] != it.x) return Status::success();
        ++j;
        hi = r.id;
      }
      if (end || page.empty()) return Status::success();
    }
  }

  Status go() {
    const bool had = readMarker(*d.files, d.chatKey, m);
    Status s = log.open(head);
    if (!s.ok()) return s;
    if (!had) {
      m = Marker{};
      if (log.lastQ() > 0) {   // items without a marker: only rows since are new
        m.reentry = true;
        m.step = MigrationStep::Done;
        if (!(s = fallbackHi(m.hi)).ok()) return s;
      }
    }
    out.reentry = m.reentry;
    if (!m.reentry && (m.step == MigrationStep::NotStarted || m.step == MigrationStep::Digest0)) {
      if (!(s = digest0Step()).ok()) return s;
    }
    if (!(s = rowsStep()).ok()) return s;
    out.digest0 = m.d0Done;
    out.verdict = m.d0.have ? m.d0.verdict : Digest0Verdict::NoSummary;
    out.exported = m.d0.exported;
    out.hi = m.hi;
    out.step = MigrationStep::Done;
    return Status::success();
  }
};

bool depsOk(const MigrateDeps& d) { return d.files && d.rows && chatKeyOk(d.chatKey); }

}  // namespace

Status migrate(const MigrateDeps& deps, MigrationOutcome& out) {
  out = MigrationOutcome{};
  if (!depsOk(deps)) return Status::fail(Err::InvalidArgument, "tx::migrate");
  try {
    Run run(deps, out);
    const Status s = run.go();
    if (!s.ok()) out.step = run.m.step;
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
    if (readMarker(files, chatKey, m)) {
      out = m.reentry;
      return Status::success();
    }
    std::vector<uint32_t> segs;
    Status s = segmentList(files, chatKey, segs);
    if (!s.ok()) return s;
    LineCursor cur(files, chatKey);
    if (!segs.empty() && (s = cur.start(segs.front(), 0)).ok()) {
      std::string line;
      LineHeader h;
      bool have = false;
      if ((s = cur.next(line, h, have)).ok()) out = have;
    }
    return s;
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
    const bool marked = readMarker(*d.files, d.chatKey, run.m);
    if (re && !marked && !(s = run.fallbackHi(run.m.hi)).ok()) return s;
    uint64_t newest = 0;
    if (!(s = d.rows->newest(newest)).ok()) return s;
    const OldFold& f = d.fold;
    if (f.thru && f.summary.empty()) anomaly(r, "pointer-without-summary");
    if (!f.thru && !f.summary.empty()) anomaly(r, "summary-without-pointer");
    if (f.shift && f.shift <= f.thru) anomaly(r, "shift-not-after-pointer");
    if (f.thru > newest) anomaly(r, "pointer-past-rows");
    std::vector<int64_t> days;
    uint64_t lastT = 0;
    s = run.eachRow(0, 0, [&](const DayRow& row) {
      ++r.rows;
      if (row.ours) ++r.ours;
      const bool conv = row.role == RowRole::Owner || row.role == RowRole::Assistant;
      if (!conv) ++r.toolRows;
      if (conv && !row.ours) {
        ++r.conversation;
        if (row.text.empty()) anomaly(r, "empty-text");
        if (row.text.size() > kTier1OwnerText) anomaly(r, "text-over-tier1");
        if (!re && row.id <= f.thru) ++r.covered;
        if (!re && row.id > f.thru && f.shift && row.id <= f.shift) ++r.betweenPointers;
      }
      if (!row.t) anomaly(r, "row-without-time");
      if (row.t && row.t < lastT) anomaly(r, "time-backwards");
      if (row.t) {
        lastT = row.t;
        const int64_t dn = dayNumber(row.t, d.tzMinutes, d.dayStartMin);
        if (days.empty() || days.back() != dn) {
          if (std::find(days.begin(), days.end(), dn) == days.end()) days.push_back(dn);
        }
      }
    });
    if (!s.ok()) return s;
    r.days = (uint32_t)days.size();
    const uint64_t lo = re ? run.m.hi : std::max(run.m.hi, f.thru);
    ImportPlan p;
    bool any = false;
    if (newest > lo) {
      run.head = HeadState{};   // planning only: no log is opened (it may write)
      if (!(s = run.planBatch(lo, newest, p, any)).ok()) return s;
    }
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
