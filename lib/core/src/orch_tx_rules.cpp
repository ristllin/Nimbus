// The stub rule and its neighbours (tx/rules.h, PRD section 2), lane RB-T3. Every function
// here is pure: its answer depends only on its arguments (no clock, no file, no global
// state), so the same stored facts always give the same bytes and a restart renders the
// same request. The model-facing templates of these rules are the k* strings below.
#include "nimbus/orch/tx/rules.h"

#include "nimbus/orch/day_start.h"
#include "nimbus/orch/tx/photo_text.h"
#include "nimbus/orch/tx/result_text.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace nimbus {
namespace orch {
namespace tx {

namespace {

// ---- templates (model-text class "stub templates") ----------------------------------------
constexpr const char kStubOpen[] = "[stub ";
constexpr const char kArgMid[] = "B sha:";
constexpr const char kArgNotContent[] = ": an argument sent earlier, not content; ";
constexpr const char kArgFile[] = " shows the file";
constexpr const char kOutsideMid[] = "B outside content, not shown: ";
constexpr const char kNoCardOutside[] = "full result not kept: no storage card";
constexpr const char kNoClock[] = "--:--";
constexpr const char kOmissionMid[] = " earlier turns, ";
constexpr const char kOmissionTail[] = ", not shown: search finds them]";
constexpr const char kOmissionPlain[] = ", not shown]";   // E2-FIX5: search not bound
constexpr const char kTextContinues[] = "\n[text continues: ";
constexpr const char kTextMore[] = "B more: ";
constexpr const char kTextNotKept[] = "not kept: no storage card";

// ---- one-line UTF-8 text -------------------------------------------------------------------

// The sequence length a lead byte starts and the allowed range of its second byte (0 when
// the byte cannot start one).
size_t leadOf(unsigned char c, unsigned char& lo, unsigned char& hi) {
  lo = 0x80;
  hi = 0xBF;
  if (c >= 0xC2 && c <= 0xDF) return 2;
  if (c >= 0xE0 && c <= 0xEF) {
    lo = c == 0xE0 ? 0xA0 : lo;   // no overlongs
    hi = c == 0xED ? 0x9F : hi;   // no surrogates
    return 3;
  }
  if (c >= 0xF0 && c <= 0xF4) {
    lo = c == 0xF0 ? 0x90 : lo;
    hi = c == 0xF4 ? 0x8F : hi;   // at most U+10FFFF
    return 4;
  }
  return 0;
}

// Length of the valid UTF-8 sequence at s[i] (RFC 3629: no overlongs, no surrogates, at
// most U+10FFFF), or 0 when the bytes there are not one.
size_t utf8Len(const std::string& s, size_t i) {
  const unsigned char c = static_cast<unsigned char>(s[i]);
  if (c < 0x80) return 1;
  unsigned char lo, hi;
  const size_t n = leadOf(c, lo, hi);
  if (n == 0 || i + n > s.size()) return 0;
  const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
  if (c1 < lo || c1 > hi) return 0;
  for (size_t k = 2; k < n; ++k)
    if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return 0;
  return n;
}

// The rest of src from i is the start of a sequence the source cut off (dropped, not '?').
bool partialTail(const std::string& s, size_t i) {
  unsigned char lo, hi;
  const size_t n = leadOf(static_cast<unsigned char>(s[i]), lo, hi);
  if (n == 0 || i + n <= s.size()) return false;
  for (size_t k = i + 1; k < s.size(); ++k)
    if ((static_cast<unsigned char>(s[k]) & 0xC0) != 0x80) return false;
  return true;
}

// The code point of the valid n-byte sequence at s[i].
uint32_t codePointAt(const std::string& s, size_t i, size_t n) {
  static const unsigned char kLeadMask[] = {0, 0x7F, 0x1F, 0x0F, 0x07};
  uint32_t cp = static_cast<unsigned char>(s[i]) & kLeadMask[n];
  for (size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
  return cp;
}

// A character a renderer may break a line at: C0 and C1 controls, DEL, U+2028, U+2029.
bool isLineBreaking(uint32_t cp) { return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0x2028 || cp == 0x2029; }
// A bidi control, which can reorder what the line shows.
bool isBidiControl(uint32_t cp) {
  return cp == 0x200E || cp == 0x200F || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069);
}

// What a character becomes on a stub line: itself (nullptr), a space (line-breaking) or '?'
// (a bidi control).
const char* replacementOf(const std::string& s, size_t i, size_t n) {
  const uint32_t cp = codePointAt(s, i, n);
  if (isLineBreaking(cp)) return " ";
  return isBidiControl(cp) ? "?" : nullptr;
}

// Appends src as one line of valid UTF-8, at most maxBytes of it: see replacementOf; a byte
// that starts no valid sequence becomes '?'; a sequence cut at the end of src is dropped; the
// cut falls on a character. `frame`: '[' and ']' become '(' and ')', so a field set by the
// model cannot close the stub or open a '[device]' line inside it.
void appendLine(std::string& out, const std::string& src, size_t maxBytes, bool frame = false) {
  size_t used = 0;
  for (size_t i = 0; i < src.size();) {
    const size_t n = utf8Len(src, i);
    if (n == 0 && partialTail(src, i)) break;
    const size_t emit = n ? n : 1;
    if (used + emit > maxBytes) break;
    const char* r = n ? replacementOf(src, i, n) : "?";
    if (r) out += r;
    else if (frame && src[i] == '[') out += '(';
    else if (frame && src[i] == ']') out += ')';
    else out.append(src, i, n);
    used += r ? 1 : emit;
    i += emit;
  }
}

// A tool name keeps [A-Za-z0-9_.-]; anything else becomes '?'.
void appendTool(std::string& out, const std::string& tool) {
  for (size_t i = 0; i < tool.size() && i < kStubToolMaxBytes; ++i) {
    const char c = tool[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                    c == '.' || c == '-';
    out += ok ? c : '?';
  }
}

bool isHex8(const std::string& s) {
  if (s.size() != 8) return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  return true;
}

// A model-chosen field on one line, framed so it cannot close a stub.
std::string oneLineOf(const std::string& src, size_t maxBytes) {
  std::string out;
  appendLine(out, src, maxBytes, true);
  return out;
}

bool isHhmm(const std::string& s) {
  auto d = [](char c) { return c >= '0' && c <= '9'; };
  return s.size() == 5 && d(s[0]) && d(s[1]) && s[2] == ':' && d(s[3]) && d(s[4]);
}

bool isRef(const std::string& s) {
  if (s.empty() || s.size() > 16) return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
  return true;
}

// ---- stamps (Ruling 11) -----------------------------------------------------------------------

// The fields of "Tue 17 Nov 2026 13:02" (photoWhen, the digest day header and omissions),
// read from the shared stamp's calendar (day_start.h) at a 00:00 day start.
struct StampParts {
  std::string wday, day, mon, year, hhmm;
};
Status stampParts(uint64_t epoch, int16_t tz, StampParts& p, const char* where) {
  static const char kMonths[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char buf[day::kPlainCap];   // "2026-11-17 Tue 13:02"
  if (day::stamp(epoch, tz, 0, buf, sizeof buf) != day::StampForm::Full) return Status::fail(Err::InvalidArgument, where);
  const int mon = (buf[5] - '0') * 10 + (buf[6] - '0');
  p.year.assign(buf, 4);
  p.mon.assign(kMonths + 3 * (mon - 1), 3);
  p.day.assign(buf[8] == '0' ? buf + 9 : buf + 8, buf + 10);
  p.wday.assign(buf + 11, 3);
  p.hhmm.assign(buf + 15, 5);
  return Status::success();
}

// ---- superseded reads ---------------------------------------------------------------------------

bool isPathWrite(const std::string& n) {
  return n == "write_file" || n == "append_file" || n == "edit_file" || n == "delete_file" || n == "move_file";
}
bool isPathRead(const std::string& n) { return n == "read_file" || n == "read_pdf" || n == "file_stat"; }

constexpr int kArgsDepth = 8;

bool parseArgs(const std::string& args, JsonDocument& doc) {
  if (args.empty() || args[0] != '{') return false;   // a long-argument stub is not JSON
  return !deserializeJson(doc, args, DeserializationOption::NestingLimit(kArgsDepth));
}

std::string topString(const JsonDocument& doc, const char* key) {
  JsonVariantConst v = doc[key];
  return v.is<const char*>() ? std::string(v.as<const char*>()) : std::string();
}

// What one write makes stale: the paths a file write names. No tool is special for what
// it stores (ruling 41): a write that names no path makes nothing stale.
struct WriteTarget {
  std::set<std::string> paths;
};

// What an earlier read call is about, parsed once per call: the path of a file read;
// `relevant` false for every other call.
struct ReadKey {
  bool relevant = false;
  std::string key;
};
ReadKey readKeyOf(const Item& call) {
  ReadKey k;
  k.relevant = isPathRead(call.n);
  JsonDocument doc;
  if (k.relevant && parseArgs(call.args, doc)) k.key = topString(doc, "path");
  return k;
}

bool readMatches(const ReadKey& k, const WriteTarget& w) { return k.relevant && w.paths.count(k.key) > 0; }

// What the write makes stale; false when it makes nothing stale.
bool targetOf(const Item& writeCall, WriteTarget& w) {
  if (!isPathWrite(writeCall.n)) return false;
  JsonDocument doc;
  if (!parseArgs(writeCall.args, doc)) return false;
  for (const char* key : {"path", "from", "to"}) {
    const std::string p = topString(doc, key);
    if (!p.empty()) w.paths.insert(p);
  }
  return !w.paths.empty();
}

// Results x items already superseded, and the pointer (the newest x item's stub).
uint64_t priorMoves(const std::vector<Item>& earlier, std::set<uint64_t>& already) {
  uint64_t pointer = 0;
  for (const Item& it : earlier)
    if (it.kind == ItemKind::PointerMove) {
      already.insert(it.sup.begin(), it.sup.end());
      pointer = std::max(pointer, it.stub);
    }
  return pointer;
}

Status superseded(const Item& writeCall, const Item* writeResult, const std::vector<Item>& earlier,
                  std::vector<uint64_t>& out) {
  out.clear();
  if (writeCall.kind != ItemKind::Call) return Status::fail(Err::InvalidArgument, "tx::supersededBy");
  WriteTarget w;
  (void)writeResult;   // only its ok flag matters (supersededBy's second form)
  if (!targetOf(writeCall, w)) return Status::success();
  std::set<uint64_t> already;
  const uint64_t pointer = priorMoves(earlier, already);
  std::map<std::string, ReadKey> lastCall;   // call id -> what its newest c item reads
  for (const Item& it : earlier) {
    if (writeCall.q && it.q >= writeCall.q) break;   // a read after the write is fresh
    if (it.kind == ItemKind::Call) lastCall[it.id] = readKeyOf(it);
    if (it.kind != ItemKind::Result || already.count(it.q)) continue;
    if (it.cls != ResultClass::Receipt && it.q <= pointer) continue;   // already a stub
    auto c = lastCall.find(it.id);
    if (c != lastCall.end() && readMatches(c->second, w)) out.push_back(it.q);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return Status::success();
}

}  // namespace

// ---- stub texts --------------------------------------------------------------------------------------

// A kept result's stub after its frame (lane E2-FIX9): what is true of it, then the way back (view when bound, else a
// file read's call cut to keep the stub within kStubMaxBytes, else none).
void keptTail(const StubFacts& f, const std::string& q, std::string& s) {
  s += std::string(kResultFactsOpen) + kResultShownInFull;
  if (f.bound.view) {
    s += kResultViewOpen + q + kPhotoViewClose;
  } else if (!f.again.empty()) {
    const size_t fixed = s.size() + std::strlen(kResultAgainOpen) + std::strlen(kPhotoViewClose) + std::strlen(kResultTailClose);
    const size_t room = kStubMaxBytes > fixed ? std::min(kStubWayBackMaxBytes, kStubMaxBytes - fixed) : 0;
    if (room) s += kResultAgainOpen + oneLineOf(f.again, room) + kPhotoViewClose;
  }
  s += kResultTailClose;
}

Status stubText(const StubFacts& f, std::string& out) {
  out.clear();
  if (f.tool.empty() || f.q == 0) return Status::fail(Err::InvalidArgument, "tx::stubText");
  const std::string q = std::to_string(f.q);
  std::string s = kStubOpen;
  appendTool(s, f.tool);
  if (f.cls == ResultClass::Outside) {
    // The call and the size only: no byte of the result is read.
    s += '(';
    appendLine(s, f.args, kStubArgsPreviewBytes, true);
    s += ") r" + q + " " + std::to_string(f.bytes) + kOutsideMid;
    if (f.noCard) s += kNoCardOutside;
    else if (f.bound.view) s += "view id=r" + q;
    else if (!f.again.empty()) s += oneLineOf(f.again, kStubWayBackMaxBytes);
    else s.resize(s.size() - 2);   // E2-FIX5: no call reads it again: "..., not shown]"
    s += ']';
    out.swap(s);
    return Status::success();
  }
  s += " r" + q + " " + std::to_string(f.bytes) + "B " + std::to_string(f.lines) + "L ";
  s += isHhmm(f.hhmm) ? f.hhmm : std::string(kNoClock);
  if (!f.path.empty()) {
    s += " path=";
    appendLine(s, f.path, kStubPathMaxBytes, true);
  }
  if (isHex8(f.sha8)) s += " sha8=" + f.sha8;
  s += "] ";
  // Lane E2-FIX9: a result the card kept says it was shown in full earlier and is not attached now, then the way back:
  // view when bound (the bytes the head saw), else a file read's call (E2-VERB), else none (E2-FIX5). No preview:
  // the bench's head read a stub's first 200 B as all the tool had returned and withdrew its correct answers
  // (lanes/E2-FIX9/evidence/probe-stub.jsonl: 5 of 16 with the preview, 0 of 16 without). With no card the preview
  // is all that is left of the result, so it stays, with what is true of it.
  if (!f.noCard) {
    keptTail(f, q, s);
    out.swap(s);
    return Status::success();
  }
  const std::string tail = std::string(kResultTailOpen) + kPhotoNotAttached + kResultNoCard + kResultTailClose;
  const size_t room = kStubMaxBytes > s.size() + tail.size() ? kStubMaxBytes - s.size() - tail.size() : 0;
  appendLine(s, f.preview, std::min(kStubPreviewBytes, room));
  s += tail;
  out.swap(s);
  return Status::success();
}

Status longArgStub(uint32_t bytes, const std::string& sha8, uint64_t q, std::string& out, const std::string& again,
                   const BoundTools& bound) {
  out.clear();
  if (q == 0 || !isHex8(sha8)) return Status::fail(Err::InvalidArgument, "tx::longArgStub");
  out = std::string(kStubOpen) + std::to_string(bytes) + kArgMid + sha8 + kArgNotContent;
  if (!again.empty()) out += oneLineOf(again, kStubWayBackMaxBytes) + kArgFile;
  else if (bound.view) out += "view id=r" + std::to_string(q);
  else out.resize(out.size() - 2);   // E2-FIX5: '...; not content]'
  out += ']';
  return Status::success();
}

// The tail of a photo's line: what is true of it in this request, and the way back when view is bound.
void photoTail(std::string& s, const std::string& ref8, bool viewBound) {
  s += kPhotoNotAttached;
  if (viewBound) s += kPhotoViewOpen + ref8 + kPhotoViewClose;
  s += ']';
}

Status photoStub(const std::string& ref8, const std::string& when, const std::string& note, std::string& out,
                 bool viewBound) {
  if (note.empty()) return photoStub(ref8, when, out, viewBound);
  if (!isRef(ref8)) {
    out.clear();
    return Status::fail(Err::InvalidArgument, "tx::photoStub");
  }
  std::string s = "[photo p" + ref8;
  if (!when.empty()) {
    s += ' ';
    appendLine(s, when, 40, true);
  }
  s += ": ";
  appendLine(s, note, kPhotoNoteMax, true);
  s += "; ";
  photoTail(s, ref8, viewBound);
  out.swap(s);
  return Status::success();
}

Status longTextHead(const std::string& text, size_t cap, const std::string& where, std::string& out) {
  if (!cap) {
    out.clear();
    return Status::fail(Err::InvalidArgument, "tx::longTextHead");
  }
  if (text.size() <= cap) {
    out = text;
    return Status::success();
  }
  size_t n = cap;   // on a character: never a sequence cut in two
  while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80) --n;
  std::string s = text.substr(0, n);
  s += kTextContinues;
  s += std::to_string(text.size() - n);
  s += kTextMore;
  s += where.empty() ? std::string(kTextNotKept) : where;
  s += ']';
  out.swap(s);
  return Status::success();
}

// E2-FIX5: kept with no bound tool to read it: the line ends '<rest>B more]'. A separate body, so the
// four-argument form (engine 1's inbox calls it) stays as it was.
Status longTextHead(const std::string& text, size_t cap, const std::string& where, std::string& out, bool kept) {
  const Status st = longTextHead(text, cap, where, out);
  const std::string notKept = std::string(kTextMore) + kTextNotKept + "]";
  if (st.ok() && kept && where.empty() && text.size() > cap && out.size() >= notKept.size() &&
      out.compare(out.size() - notKept.size(), notKept.size(), notKept) == 0)
    out.replace(out.size() - notKept.size(), notKept.size(), "B more]");
  return st;
}

Status photoStub(const std::string& ref8, const std::string& when, std::string& out, bool viewBound) {
  if (!isRef(ref8)) {
    out.clear();
    return Status::fail(Err::InvalidArgument, "tx::photoStub");
  }
  std::string s = "[photo p" + ref8;   // built apart: `when` may be `out`
  if (!when.empty()) {
    s += ' ';
    appendLine(s, when, 40, true);
  }
  s += ": ";
  photoTail(s, ref8, viewBound);
  out.swap(s);
  return Status::success();
}

Status stubTime(uint64_t epoch, int16_t tz, std::string& out) {
  out.clear();
  if (!day::clockSet(epoch)) return Status::success();
  StampParts p;
  Status st = stampParts(epoch, tz, p, "tx::stubTime");
  if (st.ok()) out = p.hhmm;
  return st;
}

Status photoWhen(uint64_t epoch, int16_t tz, std::string& out) {
  out.clear();
  if (!day::clockSet(epoch)) return Status::success();
  StampParts p;
  Status st = stampParts(epoch, tz, p, "tx::photoWhen");
  if (st.ok()) out = p.wday + " " + p.day + " " + p.mon + " " + p.year + " " + p.hhmm;
  return st;
}

Status omissionDates(uint64_t first, uint64_t last, int16_t tz, std::string& out) {
  out.clear();
  if (!day::clockSet(first)) first = last;   // turns stored before the clock was set
  if (!day::clockSet(last)) return Status::success();
  if (last < first) return Status::fail(Err::InvalidArgument, "tx::omissionDates");
  StampParts a, b;
  Status st = stampParts(first, tz, a, "tx::omissionDates");
  if (st.ok()) st = stampParts(last, tz, b, "tx::omissionDates");
  if (!st.ok()) return st;
  if (a.year != b.year) out = a.day + " " + a.mon + " " + a.year + "-" + b.day + " " + b.mon + " " + b.year;
  else if (a.mon != b.mon) out = a.day + " " + a.mon + "-" + b.day + " " + b.mon + " " + b.year;
  else if (a.day != b.day) out = a.day + "-" + b.day + " " + b.mon + " " + b.year;
  else out = a.day + " " + a.mon + " " + a.year;
  return Status::success();
}

// ---- superseded reads and day views ------------------------------------------------------------------

Status supersededBy(const Item& writeCall, const std::vector<Item>& earlier, std::vector<uint64_t>& out) {
  return superseded(writeCall, nullptr, earlier, out);
}

Status supersededBy(const Item& writeCall, const Item& writeResult, const std::vector<Item>& earlier,
                    std::vector<uint64_t>& out) {
  out.clear();
  if (writeResult.kind != ItemKind::Result) return Status::fail(Err::InvalidArgument, "tx::supersededBy");
  if (!writeResult.ok) return Status::success();   // a failed write changed nothing
  return superseded(writeCall, &writeResult, earlier, out);
}

// ---- whole or stub -----------------------------------------------------------------------------------

std::string wayBack(const std::string& tool, const std::string& args) {
  const bool read = tool == "read_file", write = tool == "write_file" || tool == "append_file" || tool == "edit_file";
  JsonDocument doc;
  if ((!read && !write) || !parseArgs(args, doc)) return std::string();
  const std::string path = topString(doc, "path");
  if (path.empty()) return std::string();
  std::string out = "read_file path=" + path;
  if (!read) return out;
  for (const char* k : {"offset", "limit", "column", "from", "to"}) {
    JsonVariantConst v = doc[k];
    if (v.is<uint64_t>()) out += std::string(" ") + k + "=" + std::to_string(v.as<uint64_t>());
    else if (v.is<const char*>()) out += std::string(" ") + k + "=" + v.as<const char*>();
  }
  return out;
}

bool looksLikeStub(const std::string& text) {
  size_t a = 0, b = text.size();
  while (a < b && (text[a] == ' ' || text[a] == '\n' || text[a] == '\r' || text[a] == '\t')) ++a;
  while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\n' || text[b - 1] == '\r' || text[b - 1] == '\t')) --b;
  if (b - a > kStubMaxBytes || text.compare(a, std::strlen(kStubOpen), kStubOpen) != 0) return false;
  if (std::find(text.begin() + (long)a, text.begin() + (long)b, '\n') != text.begin() + (long)b) return false;
  return text[b - 1] == ']' || text[b - 1] == ')';
}

Status showResult(const Item& r, const ShowFacts& f, ShowAs& out) {
  out = ShowAs::Whole;
  if (r.kind != ItemKind::Result || (f.superseded && !std::is_sorted(f.superseded->begin(), f.superseded->end())))
    return Status::fail(Err::InvalidArgument, "tx::showResult");
  if (f.superseded && std::binary_search(f.superseded->begin(), f.superseded->end(), r.q)) out = ShowAs::Stub;
  else if (r.cls == ResultClass::Receipt) out = ShowAs::Whole;   // tier 1: verbatim until compacted
  else if (r.q <= f.pointer) out = ShowAs::Stub;
  else if (r.cls == ResultClass::Outside && r.turn != f.openTurn) out = ShowAs::Stub;
  else if (r.turn == f.openTurn && r.q <= f.ladderStubThrough) out = ShowAs::Stub;
  return Status::success();
}

// ---- photos ------------------------------------------------------------------------------------------

Status visiblePhotos(const PhotoFacts& f, std::vector<std::string>& out) {
  out.clear();
  const size_t n = f.candidates.size();
  if (f.ownerTurnsSince.size() != n || (!f.bytes.empty() && f.bytes.size() != n))
    return Status::fail(Err::InvalidArgument, "tx::visiblePhotos");
  if (f.mode == PhotoMode::None) return Status::success();
  const size_t maxCount = photosInViewMax(f.mode), maxBytes = photoBytesInViewMax(f.mode);
  size_t bytes = 0;
  for (size_t i = 0; i < n; ++i) {   // the arrival turn's own photos first, whatever their place
    if (f.ownerTurnsSince[i] != 0 || out.size() >= kArrivalPhotosMax) continue;
    out.push_back(f.candidates[i]);
    bytes += f.bytes.empty() ? 0 : f.bytes[i];
  }
  if (f.noCard) return Status::success();   // an earlier photo cannot be read back
  for (size_t i = 0; i < n; ++i) {          // then the earlier ones, newest first
    if (f.ownerTurnsSince[i] == 0) continue;
    const size_t size = f.bytes.empty() ? 0 : f.bytes[i];
    if (out.size() >= maxCount || bytes + size > maxBytes) break;   // the oldest fall out first
    bytes += size;
    out.push_back(f.candidates[i]);
  }
  return Status::success();
}

// ---- the overflow ladder -----------------------------------------------------------------------------

Status nextRung(const LadderFacts& f, LadderRung& out) {
  out = LadderRung::None;
  if (f.bodyCap == 0) return Status::fail(Err::InvalidArgument, "tx::nextRung");
  if (f.bodyBytes <= f.bodyCap) return Status::success();
  const uint8_t next = static_cast<uint8_t>(f.applied) + 1;
  out = next >= static_cast<uint8_t>(LadderRung::SizeGuard) ? LadderRung::SizeGuard : static_cast<LadderRung>(next);
  return Status::success();
}

Status omissionSpan(const std::vector<TurnSpan>& turns, size_t needBytes, uint32_t& omitCount) {
  omitCount = 0;
  for (size_t i = 1; i < turns.size(); ++i)
    if (turns[i].turn <= turns[i - 1].turn) return Status::fail(Err::InvalidArgument, "tx::omissionSpan");
  // The oldest of the newest kNewestOwnerTurnsKept owner turns bounds what may go.
  size_t owners = 0, bound = turns.size();
  for (size_t i = turns.size(); i-- > 0;)
    if (turns[i].owner && ++owners == kNewestOwnerTurnsKept) {
      bound = i;
      break;
    }
  if (owners < kNewestOwnerTurnsKept) return Status::success();   // nothing is old enough
  size_t freed = 0;
  while (omitCount < bound && freed < needBytes) freed += turns[omitCount++].bytes;
  return Status::success();
}

Status omissionLine(uint32_t turns, const std::string& dates, std::string& out, const BoundTools& bound) {
  if (turns == 0) {
    out.clear();
    return Status::fail(Err::InvalidArgument, "tx::omissionLine");
  }
  std::string s = "[" + std::to_string(turns) + kOmissionMid;   // built apart: `dates` may be `out`
  appendLine(s, dates, kOmissionDatesMaxBytes, true);
  const char* tail = bound.search ? kOmissionTail : kOmissionPlain;
  s += dates.empty() ? tail + 2 : tail;   // no dates: "..., not shown: ..."
  out.swap(s);
  return Status::success();
}

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
