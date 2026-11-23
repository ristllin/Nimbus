// The digest block, its archive file, the recent-files list and the chatsum mirror
// (tx/digest.h, PRD section 3; lane RB-C1). Pure functions over the digests plus three
// storage paths (archiveDigest, loadDigests, settleDigests), each a few bounded reads and
// at most one append per digest moved. No clock, no globals, no model call.
#include "nimbus/orch/tx/digest.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <set>

#include "nimbus/mem_cap.h"
#include "nimbus/orch/day_start.h"   // RB-REC5: digestDay is the running day
#include "nimbus/orch/tx/rules.h"

namespace nimbus {
namespace orch {
namespace tx {

namespace {

// ---- dates ------------------------------------------------------------------------------

bool digits(const std::string& s, size_t at, size_t n, int& out) {
  out = 0;
  for (size_t i = at; i < at + n; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    out = out * 10 + (s[i] - '0');
  }
  return true;
}

bool leapYear(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

// "YYYY-MM-DD" with a real calendar date.
bool parseDay(const std::string& day, int& y, int& m, int& d) {
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (day.size() != 10 || day[4] != '-' || day[7] != '-') return false;
  if (!digits(day, 0, 4, y) || !digits(day, 5, 2, m) || !digits(day, 8, 2, d)) return false;
  if (y < 1970 || m < 1 || m > 12 || d < 1) return false;
  return d <= kDays[m - 1] + (m == 2 && leapYear(y) ? 1 : 0);
}

// Days since 1970-01-01 (Howard Hinnant's days_from_civil).
int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// ---- one line of text -------------------------------------------------------------------

// The code point of the UTF-8 sequence at s[i] and its length; 0 when it is not valid
// (truncated, a stray continuation byte, overlong, a surrogate or past U+10FFFF).
// The length a UTF-8 lead byte announces (0 for a continuation or invalid byte).
size_t seqLen(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c >> 5) == 6) return 2;
  if ((c >> 4) == 14) return 3;
  return (c >> 3) == 30 ? 4 : 0;
}

size_t utf8At(const std::string& s, size_t i, uint32_t& cp) {
  static const unsigned char kLeadMask[] = {0, 0x7F, 0x1F, 0x0F, 0x07};
  static const uint32_t kMin[] = {0, 0, 0x80, 0x800, 0x10000};
  const size_t n = seqLen((unsigned char)s[i]);
  if (n == 0 || i + n > s.size()) return 0;
  cp = (unsigned char)s[i] & kLeadMask[n];
  for (size_t k = 1; k < n; ++k) {
    const unsigned char cc = (unsigned char)s[i + k];
    if ((cc & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | (cc & 0x3F);
  }
  if (cp < kMin[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
  return n;
}

// A character that would open a line, hide text or reorder it: shown as a space.
bool breaksLine(uint32_t cp) {
  if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) return true;
  if (cp == 0x2028 || cp == 0x2029 || cp == 0x061C || cp == 0x200E || cp == 0x200F) return true;
  return (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069);
}

// `in` as one line: breaking characters become spaces, invalid bytes '?', runs of spaces
// one, no space at either end.
std::string oneLine(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size();) {
    uint32_t cp = 0;
    const size_t n = utf8At(in, i, cp);
    std::string piece = n == 0 ? std::string("?") : breaksLine(cp) ? std::string(" ") : in.substr(i, n);
    i += n == 0 ? 1 : n;
    if (piece == " " && (out.empty() || out.back() == ' ')) continue;
    out += piece;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

// Is `text` already one clean line (what oneLine would leave as it is)?
bool cleanLine(const std::string& text) { return !text.empty() && oneLine(text) == text; }

// Where to cut `body` so it is at most `limit` bytes: after the last sentence end that
// fits, else at the last space, else at a whole character.
size_t cutAt(const std::string& body, size_t limit) {
  for (size_t i = limit; i > 0; --i) {
    const char c = body[i - 1];
    const bool ends = i == body.size() || body[i] == ' ';
    if ((c == '.' || c == '!' || c == '?') && ends) return i;
  }
  const size_t sp = body.rfind(' ', limit);
  if (sp != std::string::npos && sp > 0) return sp;
  return (size_t)nimbus::utf8CapLen(body.c_str(), (int)body.size(), (int)limit);
}

// ---- digests ----------------------------------------------------------------------------

// A stored cumulative summary: several clean lines, each ending with '\n', under the hard cap.
bool validSummaryText(const std::string& text) {
  if (!isSummaryText(text) || text.size() > kSummaryMaxBytes) return false;
  for (size_t at = 0, nl; (nl = text.find('\n', at)) != std::string::npos; at = nl + 1)
    if (!cleanLine(text.substr(at, nl - at))) return false;
  return true;
}

bool validDigest(const DigestEntry& d) {
  int y = 0, m = 0, dd = 0;
  if (!parseDay(d.day, y, m, dd)) return false;
  if (isSummaryText(d.text)) return validSummaryText(d.text);
  return d.text.size() <= kDigestMaxBytes && cleanLine(d.text);
}

// The newest cumulative summary of a run (its index), or n when there is none.
size_t newestSummary(const std::vector<DigestEntry>& digests, size_t n) {
  for (size_t i = n; i-- > 0;)
    if (isSummaryText(digests[i].text)) return i;
  return n;
}

// The longest run of whole lines of `text` from its start that fits `cap` (no last line end).
std::string wholeLinesUpTo(const std::string& text, size_t cap) {
  size_t end = 0, at = 0;
  for (size_t nl; (nl = text.find('\n', at)) != std::string::npos && nl <= cap; at = nl + 1) end = nl;
  return text.substr(0, end);
}

// Every digest valid, thru strictly rising.
bool validRun(const std::vector<DigestEntry>& digests) {
  for (size_t i = 0; i < digests.size(); ++i) {
    if (!validDigest(digests[i])) return false;
    if (i > 0 && digests[i].thru <= digests[i - 1].thru) return false;
  }
  return true;
}

// How many of the newest of digests[0, n) fit `cap` as lines joined by '\n'.
size_t newestThatFit(const std::vector<DigestEntry>& digests, size_t n, size_t cap) {
  size_t used = 0, k = 0;
  while (k < n) {
    const size_t add = digests[n - 1 - k].text.size() + (k ? 1 : 0);
    if (used + add > cap) break;
    used += add;
    ++k;
  }
  return k;
}

std::string joined(const std::vector<DigestEntry>& digests, size_t from, size_t to) {
  std::string out;
  for (size_t i = from; i < to; ++i) {
    if (i > from) out += '\n';
    out += digests[i].text;
  }
  return out;
}

// ---- files ------------------------------------------------------------------------------

// The whole file (the month archive is small: about a month of digests). False when the
// storage refused a read; a missing file is empty.
bool readWhole(TxFiles& files, const std::string& path, std::string& out) {
  out.clear();
  size_t size = 0;
  if (!files.size(path, size)) return true;
  std::vector<char> buf(kFillSliceBytes);
  while (out.size() < size) {
    size_t got = 0;
    if (!files.readAt(path, out.size(), buf.data(), buf.size(), got)) return false;
    if (got == 0) break;
    out.append(buf.data(), got);
  }
  return true;
}

bool holdsLine(const std::string& file, const std::string& line) {
  size_t at = 0;
  for (size_t nl = file.find('\n'); nl != std::string::npos; nl = file.find('\n', at)) {
    if (nl - at == line.size() && file.compare(at, line.size(), line) == 0) return true;
    at = nl + 1;
  }
  return false;
}

bool isArchiveName(const std::string& n) {
  int y = 0, m = 0;
  return n.size() == 18 && n.compare(0, 8, "digests-") == 0 && n.compare(15, 3, ".md") == 0 && n[12] == '-' &&
         digits(n, 8, 4, y) && digits(n, 13, 2, m);
}

bool segmentNumber(const std::string& n, uint32_t& seg) {
  int v = 0;
  if (n.size() != 16 || n.compare(0, 4, "seg-") != 0 || n.compare(10, 6, ".jsonl") != 0) return false;
  if (!digits(n, 4, 6, v) || v == 0) return false;
  seg = (uint32_t)v;
  return true;
}

// The last whole line of one month file ("" when it has none).
bool lastWholeLine(TxFiles& files, const std::string& path, std::string& line, DigestScan& scan) {
  line.clear();
  size_t size = 0;
  if (!files.size(path, size)) return false;
  const size_t want = std::min(size, (size_t)(2 * kDigestMaxBytes + 4));
  std::string tail(want, '\0');
  size_t got = 0;
  if (want && !files.readAt(path, size - want, &tail[0], want, got)) return false;
  scan.reads += want ? 1 : 0;
  scan.bytes += got;
  tail.resize(got);
  const size_t end = tail.rfind('\n');
  if (end == std::string::npos) return true;
  const size_t start = end == 0 ? std::string::npos : tail.rfind('\n', end - 1);
  if (start == std::string::npos && got < size) return true;   // a line longer than a digest: none
  const size_t from = start == std::string::npos ? 0 : start + 1;
  line = tail.substr(from, end - from);
  return true;
}

// The last whole line of every month file. The newest archived digest is the last line of
// the file it went to, and no digest newer than it is archived (archiving runs in q order
// and stops at a refusal), so the first of these a backward scan meets is that digest,
// whatever order the months' names have (a clock set back can archive an older month last).
bool archiveMarks(TxFiles& files, const std::string& dir, const std::vector<std::string>& names,
                  std::set<std::string>& marks, DigestScan& scan) {
  marks.clear();
  for (const std::string& n : names) {
    if (!isArchiveName(n)) continue;
    std::string line;
    if (!lastWholeLine(files, dir + "/" + n, line, scan)) return false;
    if (!line.empty()) marks.insert(line);
  }
  return true;
}

// The s items of one segment, in file order. Lines that fail their framing or crc are
// counted and dropped; s items that are not valid digests are dropped.
bool segmentDigests(TxFiles& files, const std::string& path, std::vector<DigestEntry>& out, DigestScan& scan) {
  out.clear();
  std::vector<char> buf(kFillSliceBytes);
  std::string carry;
  bool skipping = false;   // inside a line over kMaxLineBytes: dropped to its end
  Item scratch;
  for (size_t off = 0;;) {
    size_t got = 0;
    if (!files.readAt(path, off, buf.data(), buf.size(), got)) return false;
    ++scan.reads;
    scan.bytes += got;
    if (got == 0) break;
    off += got;
    for (size_t i = 0; i < got; ++i) {
      const char c = buf[i];
      if (c != '\n') {
        if (!skipping) carry += c;
        if (carry.size() > kMaxLineBytes) {
          skipping = true;
          carry.clear();
        }
        continue;
      }
      LineHeader h;
      if (skipping || !checkLine(carry.data(), carry.size(), h).ok()) {
        ++scan.torn;
      } else if (h.kind == ItemKind::Digest && decodeLine(carry.data(), carry.size(), scratch).ok()) {
        DigestEntry d;
        if (digestOf(scratch, d).ok()) out.push_back(d);
      }
      skipping = false;
      carry.clear();
    }
  }
  if (!carry.empty()) ++scan.torn;   // a torn tail
  return true;
}

// The chat's segment numbers, newest first, and the archive marks.
bool chatFiles(TxFiles& files, const std::string& dir, std::vector<uint32_t>& segs, std::set<std::string>& marks,
               DigestScan& scan) {
  std::vector<std::string> names;
  if (!files.list(dir, names) || !archiveMarks(files, dir, names, marks, scan)) return false;
  for (const std::string& n : names) {
    uint32_t seg = 0;
    if (segmentNumber(n, seg)) segs.push_back(seg);
  }
  std::sort(segs.rbegin(), segs.rend());
  return true;
}

// The digests newer than the newest archived one, newest segment first, stopping at it.
Status scanBack(TxFiles& files, const std::string& chatKey, const std::string& dir, std::vector<DigestEntry>& found,
                DigestScan& scan) {
  std::vector<uint32_t> segs;
  std::set<std::string> marks;
  if (!chatFiles(files, dir, segs, marks, scan)) return Status::fail(Err::Io, "tx::loadDigests");
  std::vector<DigestEntry> seg;
  for (uint32_t s : segs) {
    std::string path;
    if (!segmentPath(chatKey, s, path).ok() || !segmentDigests(files, path, seg, scan))
      return Status::fail(Err::Io, "tx::loadDigests");
    ++scan.segments;
    std::sort(seg.begin(), seg.end(), [](const DigestEntry& a, const DigestEntry& b) { return a.q > b.q; });
    for (const DigestEntry& d : seg) {
      if (isSummaryText(d.text)) {   // the newest summary covers everything older (the swap archived it)
        found.push_back(d);
        scan.reachedArchive = true;
        return Status::success();
      }
      if (marks.count(d.text)) {
        scan.reachedArchive = true;
        return Status::success();
      }
      found.push_back(d);
    }
  }
  return Status::success();
}

// Oldest first, one entry per q (a replay may store a q twice), thru rising.
void liveOf(std::vector<DigestEntry>& found, std::vector<DigestEntry>& live) {
  std::stable_sort(found.begin(), found.end(), [](const DigestEntry& a, const DigestEntry& b) { return a.q < b.q; });
  for (const DigestEntry& d : found)
    if (live.empty() || (d.q > live.back().q && d.thru > live.back().thru)) live.push_back(d);
}

// ---- the mirror blob --------------------------------------------------------------------

// The row id after ';' in a v5.1.1 numeric field ("1794000000;m0000a1f3" -> 0xa1f3), 0 when
// there is none or it is not "m" + hex.
uint64_t pointerOf(const std::string& field) {
  const size_t semi = field.find(';');
  if (semi == std::string::npos || semi + 1 >= field.size() || field[semi + 1] != 'm') return 0;
  uint64_t v = 0;
  for (size_t i = semi + 2; i < field.size() && i < semi + 2 + 16; ++i) {
    const char c = field[i];
    const int h = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (h < 0) break;
    v = v * 16 + (uint64_t)h;
  }
  return v;
}

std::string fieldOf(const std::string& rec, size_t index) {
  size_t at = 0;
  for (size_t i = 0; i < index; ++i) {
    at = rec.find('\x1F', at);
    if (at == std::string::npos) return std::string();
    ++at;
  }
  const size_t end = rec.find('\x1F', at);
  return rec.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

bool validChatId(const std::string& id) {
  if (id.empty()) return false;
  for (unsigned char c : id)
    if (c < 0x20 || c == 0x7F) return false;
  return true;
}

std::string mirrorRecord(const MirrorFacts& f, const std::string& text) {
  std::string rec = f.chatId + "\x1F" "0\x1F" "0\x1F" + std::to_string(f.epoch);
  if (f.thruRow) {
    char ptr[24];
    std::snprintf(ptr, sizeof(ptr), ";m%08llx", (unsigned long long)f.thruRow);
    rec += ptr;
  }
  return rec + "\x1F" "0\x1F" "0\x1F" "0\x1F" + text + "\x1E";
}

// Ruling 30: a refused allocation is never an abort. A body that runs out of memory
// answers TooLarge with its outputs cleared; nothing already stored changes (the only
// storage writes are whole appends, made after every allocation they need).
template <class Body, class Clear>
Status guarded(const char* where, Body&& body, Clear&& clear) {
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
  try {
    return body();
  } catch (const std::bad_alloc&) {
    clear();
    return Status::fail(Err::TooLarge, where);
  }
#else
  (void)where;
  (void)clear;
  return body();
#endif
}

}  // namespace

// ---- public -------------------------------------------------------------------------------

uint32_t blockCapFor(Profile profile) {
  switch (profile) {
    case Profile::Tight: return kDigestBlockTight;
    case Profile::Lean: return kDigestBlockLean;
    case Profile::Standard:
    case Profile::Wide:
    case Profile::Full: return kDigestBlockStandard;
  }
  return kDigestBlockStandard;   // a damaged value is full's (ruling 83 item 4), never lean's
}

Status dayHeader(const std::string& day, std::string& out) {
  out.clear();
  int y = 0, m = 0, d = 0;
  if (!parseDay(day, y, m, d)) return Status::fail(Err::InvalidArgument, "tx::dayHeader");
  // From the date itself, not through photoWhen: a day is a date whether or not a clock was
  // set at its noon (RB-REC5: photoWhen dates nothing before the clock-valid line).
  static const char kWd[] = "ThuFriSatSunMonTueWed";   // day 0 (1970-01-01) was a Thursday
  static const char kMo[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const int64_t n = daysFromCivil(y, m, d);
  char buf[24];
  std::snprintf(buf, sizeof buf, "%.3s %d %.3s %d", kWd + 3 * (n % 7), d, kMo + 3 * (m - 1), y);
  out = buf;
  return Status::success();
}

Status digestDay(uint64_t epoch, int16_t tzMinutes, uint16_t dayStartMin, std::string& out) {
  out.clear();
  char buf[16];
  if (!day::runningDate(epoch, tzMinutes, dayStartMin, buf, sizeof buf)) return Status::fail(Err::InvalidArgument, "tx::digestDay");
  try {
    out.assign(buf, 10);
  } catch (const std::bad_alloc&) {   // ruling 30: a refused allocation is a status, never an abort
    out.clear();
    return Status::fail(Err::TooLarge, "tx::digestDay");
  }
  return Status::success();
}

Status digestText(const std::string& day, const std::string& body, std::string& out) {
  out.clear();
  std::string head;
  if (!dayHeader(day, head).ok()) return Status::fail(Err::InvalidArgument, "tx::digestText: day");
  std::string text = oneLine(body);
  if (text.empty()) return Status::fail(Err::InvalidArgument, "tx::digestText: empty");
  head += digest_text::kDaySep;
  const size_t limit = kDigestMaxBytes - head.size();
  if (text.size() > limit) {
    text.resize(cutAt(text, limit));
    while (!text.empty() && text.back() == ' ') text.pop_back();
  }
  out = head + text;
  return Status::success();
}

Status digestItem(const DigestEntry& d, uint32_t turn, uint64_t t, Item& out) {
  if (!validDigest(d) || d.q == 0 || d.thru >= d.q) return Status::fail(Err::InvalidArgument, "tx::digestItem");
  out = Item{};
  out.kind = ItemKind::Digest;
  out.q = d.q;
  out.turn = turn;
  out.t = t;
  out.x = d.text;
  out.day = d.day;
  out.thru = d.thru;
  return Status::success();
}

Status digestOf(const Item& s, DigestEntry& out) {
  out = DigestEntry{s.day, s.x, s.q, s.thru};
  if (s.kind != ItemKind::Digest || s.ext || !validDigest(out)) return Status::fail(Err::InvalidArgument, "tx::digestOf");
  return Status::success();
}

static Status digestBlockBody(const std::vector<DigestEntry>& digests, Profile profile, std::string& block,
                   std::vector<DigestEntry>& archive) {
  block.clear();
  archive.clear();
  if (!validRun(digests)) return Status::fail(Err::InvalidArgument, "tx::digestBlock");
  const size_t k = newestSummary(digests, digests.size());
  if (k < digests.size()) {   // the newest summary covers everything before it: it alone is shown
    for (size_t i = 0; i < k; ++i)
      if (!isSummaryText(digests[i].text)) archive.push_back(digests[i]);   // day digests it took in
    block = wholeLinesUpTo(digests[k].text, kSummaryMaxBytes);
    return Status::success();
  }
  const size_t keep = newestThatFit(digests, digests.size(), blockCapFor(profile));
  const size_t from = digests.size() - keep;
  archive.assign(digests.begin(), digests.begin() + (long)from);
  block = joined(digests, from, digests.size());
  return Status::success();
}

static Status archiveDigestBody(TxFiles& files, const std::string& chatKey, const DigestEntry& digest) {
  std::string path;
  if (!validDigest(digest) || !digestArchivePath(chatKey, digest.day.substr(0, 7), path).ok())
    return Status::fail(Err::InvalidArgument, "tx::archiveDigest");
  std::string file;
  if (!readWhole(files, path, file)) return Status::fail(Err::Io, "tx::archiveDigest: read");
  if (holdsLine(file, digest.text)) return Status::success();
  // A torn tail (a cut inside an earlier append) ends before its line end. When it is the
  // start of this digest, the rest completes it (a cut just before the line end must not
  // archive the digest twice); otherwise it is closed first so this line is never glued on.
  const size_t nl = file.rfind('\n');
  const std::string torn = file.substr(nl == std::string::npos ? 0 : nl + 1);
  const bool resumes = !torn.empty() && digest.text.compare(0, torn.size(), torn) == 0;
  const std::string line =
      resumes ? digest.text.substr(torn.size()) + "\n" : (torn.empty() ? "" : "\n") + digest.text + "\n";
  if (!files.append(path, line.data(), line.size())) return Status::fail(Err::Io, "tx::archiveDigest");
  return Status::success();
}

static Status loadDigestsBody(TxFiles& files, const std::string& chatKey, const HeadState& head, std::vector<DigestEntry>& live,
                   DigestScan* scanOut) {
  live.clear();
  DigestScan scan;
  std::string dir;
  if (!txDir(chatKey, dir).ok()) return Status::fail(Err::InvalidArgument, "tx::loadDigests");
  Status st = Status::success();
  if (head.thru != 0) {
    std::vector<DigestEntry> found;
    st = scanBack(files, chatKey, dir, found, scan);
    if (st.ok()) liveOf(found, live);
  }
  if (scanOut) *scanOut = scan;
  return st;
}

static Status settleDigestsBody(TxFiles& files, const std::string& chatKey, Profile profile, std::vector<DigestEntry>& live,
                     std::string& block, SettleReport& report) {
  report = SettleReport{};
  std::vector<DigestEntry> over;
  Status st = digestBlock(live, profile, block, over);
  if (!st.ok()) return Status::fail(st.err, "tx::settleDigests");
  for (const DigestEntry& d : over) {
    st = archiveDigest(files, chatKey, d);
    if (st.err == Err::Io || st.err == Err::TooLarge) break;   // a refusal: it moves next time
    if (!st.ok()) return Status::fail(st.err, "tx::settleDigests");
    ++report.moved;
  }
  report.pending = over.size() - report.moved;
  const size_t k = newestSummary(live, live.size());
  if (k == live.size()) {
    live.erase(live.begin(), live.begin() + (long)report.moved);
    return Status::success();
  }
  // Before the newest summary: the archived day digests and every older summary (superseded) leave;
  // a day digest whose move was refused stays live and moves next time. No allocation.
  size_t seen = 0;
  auto leaves = [&](const DigestEntry& d) {
    if (d.q >= live[k].q) return false;
    if (isSummaryText(d.text)) return true;
    return seen++ < report.moved;
  };
  live.erase(std::remove_if(live.begin(), live.end(), leaves), live.end());
  return Status::success();
}

static Status recentFilesListBody(const std::vector<RecentFile>& files, int16_t tzMinutes, std::string& out) {
  out.clear();
  std::map<std::string, uint64_t> newest;
  for (const RecentFile& f : files) {
    const std::string path = oneLine(f.path);
    if (path.empty() || path.size() > kRecentPathMaxBytes) continue;
    uint64_t& w = newest[path];
    w = std::max(w, f.lastWrite);
  }
  std::vector<RecentFile> order;
  for (const auto& kv : newest) order.push_back(RecentFile{kv.first, kv.second});
  std::stable_sort(order.begin(), order.end(),
                   [](const RecentFile& a, const RecentFile& b) { return a.lastWrite > b.lastWrite; });
  for (const RecentFile& f : order) {
    std::string line = f.path, when;
    // A write time the clock cannot show (a bad timestamp) reads as not known: the path alone.
    if (!photoWhen(f.lastWrite, tzMinutes, when).ok()) when.clear();
    if (!when.empty()) line += digest_text::kWhenOpen + when + digest_text::kWhenClose;
    const size_t add = line.size() + (out.empty() ? 0 : 1);
    if (out.size() + add > kRecentFilesMaxBytes) break;
    out += (out.empty() ? "" : "\n") + line;
  }
  return Status::success();
}

bool changesFileSet(const std::string& tool) {
  return tool == "delete_file" || tool == "move_file" || tool == "rename_file";
}

Status RecentFilesBlock::atOpen(bool cold, const Source& source, int16_t tzMinutes) {
  return cold ? rebuild(source, tzMinutes) : Status::success();
}

Status RecentFilesBlock::atClose(bool movedOrDeleted, const Source& source, int16_t tzMinutes) {
  return movedOrDeleted ? rebuild(source, tzMinutes) : Status::success();
}

Status RecentFilesBlock::rebuild(const Source& source, int16_t tzMinutes) {
  ++rebuilds_;
  text_.clear();
  std::vector<RecentFile> files;
  if (!source || !source(files)) return Status::fail(Err::Io, "tx::RecentFilesBlock");
  return recentFilesList(files, tzMinutes, text_);
}

static Status chatsumMirrorBody(const std::vector<DigestEntry>& digests, uint64_t thru, std::string& out) {
  out.clear();
  if (!validRun(digests)) return Status::fail(Err::InvalidArgument, "tx::chatsumMirror");
  size_t n = 0;
  while (n < digests.size() && digests[n].thru <= thru) ++n;
  const size_t k = newestSummary(digests, n);
  if (k < n) {   // the summary in force, in whole lines from its start
    out = wholeLinesUpTo(digests[k].text, kChatsumMirrorMaxBytes);
    return Status::success();
  }
  const size_t keep = newestThatFit(digests, n, kChatsumMirrorMaxBytes);
  out = joined(digests, n - keep, n);
  return Status::success();
}

static Status mirrorChatsumBody(const std::string& blob, const MirrorFacts& facts, const std::vector<DigestEntry>& digests,
                     std::string& out) {
  out.clear();
  if (!validChatId(facts.chatId)) return Status::fail(Err::InvalidArgument, "tx::mirrorChatsum");
  std::string text;
  Status st = chatsumMirror(digests, facts.thru, text);
  if (!st.ok()) return Status::fail(st.err, "tx::mirrorChatsum");
  out = blob;
  if (text.empty()) return Status::success();
  std::vector<std::string> others;   // whole records of other chats, oldest first
  uint64_t old = 0;
  for (size_t at = 0, end; (end = blob.find('\x1E', at)) != std::string::npos; at = end + 1) {
    const std::string rec = blob.substr(at, end - at + 1);
    if (rec.find('\x1F') == facts.chatId.size() && rec.compare(0, facts.chatId.size(), facts.chatId) == 0)
      old = std::max(old, pointerOf(fieldOf(rec, 3)));
    else
      others.push_back(rec);
  }
  if (facts.thruRow < old) return Status::success();   // the pointer never moves back
  const std::string mine = mirrorRecord(facts, text);
  size_t total = mine.size();
  for (const std::string& r : others) total += r.size();
  size_t drop = 0;
  while (drop < others.size() && (others.size() - drop + 1 > kMirrorMaxRecords || total > kMirrorBlobMaxBytes))
    total -= others[drop++].size();
  out.clear();
  for (size_t i = drop; i < others.size(); ++i) out += others[i];
  out += mine;
  return Status::success();
}

// ---- the public entry points: bodies above, under the allocation guard -------------------

Status digestBlock(const std::vector<DigestEntry>& digests, Profile profile, std::string& block,
                   std::vector<DigestEntry>& archive) {
  return guarded(
      "tx::digestBlock: memory", [&] { return digestBlockBody(digests, profile, block, archive); },
      [&] {
        block.clear();
        archive.clear();
      });
}

Status archiveDigest(TxFiles& files, const std::string& chatKey, const DigestEntry& digest) {
  return guarded(
      "tx::archiveDigest: memory", [&] { return archiveDigestBody(files, chatKey, digest); }, [] {});
}

Status loadDigests(TxFiles& files, const std::string& chatKey, const HeadState& head, std::vector<DigestEntry>& live,
                   DigestScan* scan) {
  return guarded(
      "tx::loadDigests: memory", [&] { return loadDigestsBody(files, chatKey, head, live, scan); },
      [&] { live.clear(); });
}

Status settleDigests(TxFiles& files, const std::string& chatKey, Profile profile, std::vector<DigestEntry>& live,
                     std::string& block, SettleReport& report) {
  // live keeps every digest not archived: the erase of the archived ones allocates nothing.
  return guarded(
      "tx::settleDigests: memory", [&] { return settleDigestsBody(files, chatKey, profile, live, block, report); },
      [&] { block.clear(); });
}

Status recentFilesList(const std::vector<RecentFile>& files, int16_t tzMinutes, std::string& out) {
  return guarded(
      "tx::recentFilesList: memory", [&] { return recentFilesListBody(files, tzMinutes, out); },
      [&] { out.clear(); });
}

Status chatsumMirror(const std::vector<DigestEntry>& digests, uint64_t thru, std::string& out) {
  return guarded(
      "tx::chatsumMirror: memory", [&] { return chatsumMirrorBody(digests, thru, out); }, [&] { out.clear(); });
}

Status mirrorChatsum(const std::string& blob, const MirrorFacts& facts, const std::vector<DigestEntry>& digests,
                     std::string& out) {
  return guarded(
      "tx::mirrorChatsum: memory", [&] { return mirrorChatsumBody(blob, facts, digests, out); },
      [&] { out.clear(); });
}

// ---- the cumulative summary (lane RB-C3) ------------------------------------------------------

namespace {

const char* const kTitles[kSummarySections] = {digest_text::kFacts, digest_text::kDecisions, digest_text::kQuantities,
                                               digest_text::kOpen, digest_text::kFiles};
const char kLetters[kSummarySections + 1] = "FDQOP";

// The section a letter names (either case); false for any other byte.
bool sectionOf(char c, SummarySection& out) {
  for (size_t i = 0; i < kSummarySections; ++i)
    if ((c | 32) == (kLetters[i] | 32)) {
      out = (SummarySection)i;
      return true;
    }
  return false;
}

// Digits at s[at..]: the number and where they end; false for none, 0 or more than nine.
bool idAt(const std::string& s, size_t at, uint32_t& id, size_t& end) {
  id = 0;
  end = at;
  while (end < s.size() && end - at < 9 && s[end] >= '0' && s[end] <= '9') id = id * 10 + (uint32_t)(s[end++] - '0');
  return end > at && id != 0 && (end == s.size() || s[end] < '0' || s[end] > '9');
}

// photoWhen's text ("Tue 17 Nov 2026 13:02"): a weekday, a day number, and a time at the end.
bool looksLikeWhen(const std::string& w) {
  const size_t n = w.size();
  return n >= 15 && w[3] == ' ' && w[4] >= '0' && w[4] <= '9' && w[n - 3] == ':' && w[n - 1] >= '0' && w[n - 1] <= '9';
}

bool validLine(const SummaryLine& l) {
  return l.id != 0 && (size_t)l.section < kSummarySections && l.text.size() <= kSummaryLineMaxBytes &&
         cleanLine(l.text);
}

std::string lineText(const SummaryLine& l) { return std::string(1, kLetters[(size_t)l.section]) + std::to_string(l.id) + " " + l.text; }

// Where a new line of `section` goes: after the last line of its section or of any section before it.
size_t insertAt(const Summary& s, SummarySection section) {
  size_t at = 0;
  for (size_t i = 0; i < s.lines.size(); ++i)
    if ((uint8_t)s.lines[i].section <= (uint8_t)section) at = i + 1;
  return at;
}

SummaryLine* findLine(Summary& s, uint32_t id) {
  for (SummaryLine& l : s.lines)
    if (l.id == id) return &l;
  return nullptr;
}

// A change line without its leading spaces and '*' bullet.
std::string changeLine(const std::string& raw) {
  size_t at = 0;
  while (at < raw.size() && (raw[at] == ' ' || raw[at] == '\t')) ++at;
  if (at + 1 < raw.size() && raw[at] == '*' && raw[at + 1] == ' ') at += 2;
  return raw.substr(at);
}

// The text after a change's head ("+F", "=Q3"), past an optional ':': false when no space follows.
bool textAfter(const std::string& line, size_t end, std::string& text) {
  if (end < line.size() && line[end] == ':') ++end;
  text = line.substr(end);
  return line.size() > end && line[end] == ' ';
}

// An add's text. Digits right after the letter ("+Q12 eggs", "+F2026-11-04 ...", "+F1 text") stay in the text:
// only an answer that numbers all its adds makes them ids (parseSummaryEdits), and then only the number and its
// separator are left out. Lane E2-FIX7 (M2): a quantity or a date is never read as an id by itself.
bool addOf(const std::string& line, SummaryEdit& e) {
  e.op = SummaryOp::Add;
  if (e.section == SummarySection::Files) return false;   // Files: the device's own section
  if (line.size() < 3 || line[2] < '0' || line[2] > '9') return textAfter(line, 2, e.text);
  size_t end = 2;
  uint32_t n = 0;
  size_t sep = idAt(line, 2, n, end) ? end : 2;
  if (sep > 2 && sep < line.size() && line[sep] == ':') ++sep;
  if (sep > 2 && sep < line.size() && line[sep] == ' ') {
    e.named = n;
    e.namedLen = (uint8_t)(sep - 2);
  }
  e.text = line.substr(2);
  // Digits and nothing else ("+Q7") say no fact: unread, as before, never a line that reads "7".
  return e.text.find_first_not_of("0123456789: \t") != std::string::npos;
}

// One change line: false when it is not one ("=" alone sets `none`).
bool editOf(const std::string& raw, SummaryEdit& e, bool& none) {
  const std::string line = changeLine(raw);
  none = line == "=";
  const char op = line.empty() ? 0 : line[0];
  if ((op != '+' && op != '=' && op != '-') || line.size() < 2 || !sectionOf(line[1], e.section)) return false;
  if (op == '+') return addOf(line, e);
  size_t end = 2;
  if (!idAt(line, 2, e.id, end)) return false;
  e.op = op == '=' ? SummaryOp::Replace : SummaryOp::Drop;
  if (e.op == SummaryOp::Drop) return true;
  return textAfter(line, end, e.text);
}

size_t sectionBytes(const Summary& s, size_t section) {
  size_t n = 0;
  for (const SummaryLine& l : s.lines)
    if ((size_t)l.section == section) n += lineText(l).size() + 1;
  return n ? n + std::strlen(kTitles[section]) + 1 : 0;
}

}  // namespace

namespace {
// One change on the summary; false when it names no line it may change (unread).
bool applyOne(Summary& s, const SummaryEdit& e, const std::string& text, uint32_t& next, std::vector<SummaryLine>& left) {
  if (e.op == SummaryOp::Add) {
    if (text.empty() || e.section == SummarySection::Files) return false;
    bool twice = false;
    for (const SummaryLine& o : s.lines) twice |= o.section == e.section && o.text == text;
    if (!twice) s.lines.insert(s.lines.begin() + (long)insertAt(s, e.section), SummaryLine{e.section, next++, text});
    return true;
  }
  SummaryLine* l = findLine(s, e.id);
  if (!l || l->section == SummarySection::Files || (e.op == SummaryOp::Replace && text.empty())) return false;
  left.push_back(*l);
  if (e.op == SummaryOp::Replace) l->text = text;
  else s.lines.erase(s.lines.begin() + (l - s.lines.data()));
  return true;
}
}  // namespace

uint32_t Summary::nextId() const {
  uint32_t top = 0;
  for (const SummaryLine& l : lines) top = std::max(top, l.id);
  return top + 1;
}

char summaryLetter(SummarySection s) { return (size_t)s < kSummarySections ? kLetters[(size_t)s] : '?'; }

bool isSummaryText(const std::string& text) { return !text.empty() && text.back() == '\n'; }

namespace {
// encodeSummary without its size cap (fitSummary measures with it).
Status encodeLines(const Summary& s, std::string& out) {
  out.clear();
  std::set<uint32_t> ids;
  int section = -1;
  for (const SummaryLine& l : s.lines) {
    if (!validLine(l) || !ids.insert(l.id).second || (int)l.section < section)
      return Status::fail(Err::InvalidArgument, "tx::encodeSummary");
    if ((int)l.section != section) out += std::string(kTitles[(size_t)l.section]) + "\n";
    section = (int)l.section;
    out += lineText(l) + "\n";
  }
  if (s.archived) out += std::string(digest_text::kArchived) + "\n";
  if (out.empty()) out = std::string(digest_text::kNothingYet) + "\n";
  return Status::success();
}
}  // namespace

static Status encodeSummaryBody(const Summary& s, std::string& out) {
  const Status st = encodeLines(s, out);
  if (!st.ok()) return st;
  if (out.size() > kSummaryMaxBytes) {
    out.clear();
    return Status::fail(Err::InvalidArgument, "tx::encodeSummary: over its cap");
  }
  return Status::success();
}

namespace {
// One stored line into `out`: a title moves `section` on, a device line sets its flag, an id line is a
// line of the section in force. False for anything encodeSummary does not write.
bool readStoredLine(const std::string& line, int& section, Summary& out) {
  for (size_t i = (size_t)(section + 1); i < kSummarySections; ++i)
    if (line == kTitles[i]) return section = (int)i, true;
  if (line == digest_text::kNothingYet) return true;
  if (line == digest_text::kArchived) return out.archived = true;
  SummaryLine l;
  size_t end = 0;
  if (section < 0 || line.empty() || line[0] != kLetters[section] || !idAt(line, 1, l.id, end)) return false;
  if (end >= line.size() || line[end] != ' ') return false;
  l.section = (SummarySection)section;
  l.text = line.substr(end + 1);
  out.lines.push_back(l);
  return true;
}
}  // namespace

static Status parseSummaryBody(const std::string& text, Summary& out) {
  out = Summary{};
  if (!validSummaryText(text)) return Status::fail(Err::InvalidArgument, "tx::parseSummary");
  int section = -1;
  for (size_t at = 0, nl; (nl = text.find('\n', at)) != std::string::npos; at = nl + 1)
    if (!readStoredLine(text.substr(at, nl - at), section, out))
      return out = Summary{}, Status::fail(Err::InvalidArgument, "tx::parseSummary");
  std::string again;
  if (!encodeSummary(out, again).ok() || again != text) return out = Summary{}, Status::fail(Err::InvalidArgument, "tx::parseSummary");
  return Status::success();
}

std::string summaryLineText(const std::string& raw, size_t cap) {
  std::string text = oneLine(raw);
  if (text.size() > cap) {
    text.resize(cutAt(text, cap));
    while (!text.empty() && text.back() == ' ') text.pop_back();
  }
  return text;
}

namespace {
// Do the adds' numbers run 1, 2, 3 ... in answer order, across the answer or in every section by itself?
bool numberedRun(const std::vector<const SummaryEdit*>& adds) {
  bool across = true, perSection = true;
  uint32_t next = 1, nextIn[kSummarySections] = {1, 1, 1, 1, 1};
  for (const SummaryEdit* e : adds) {
    across = across && e->named == next++;
    perSection = perSection && e->named == nextIn[(size_t)e->section]++;
  }
  return across || perSection;
}

// The answer numbers its adds (lane E2-CMP's probes: "+F1 ...", "+F2 ...", "+D1 ..."): at least two adds, every
// add carries a number and a separator, and the numbers run from 1. Only then are the numbers ids and left out;
// in every other answer the digits are the text's own (a quantity, a date), and they stay.
void takeNumbering(SummaryEdits& out) {
  std::vector<const SummaryEdit*> adds;
  for (const SummaryEdit& e : out.edits)
    if (e.op == SummaryOp::Add) adds.push_back(&e);
  if (adds.size() < 2) return;
  for (const SummaryEdit* e : adds)
    if (!e->named) return;
  if (!numberedRun(adds)) return;
  for (SummaryEdit& e : out.edits) {
    if (e.op != SummaryOp::Add) continue;
    e.text.erase(0, e.namedLen);
    ++out.numbered;
  }
}
}  // namespace

SummaryEdits parseSummaryEdits(const std::string& answer) {
  SummaryEdits out;
  for (size_t at = 0; at <= answer.size();) {
    size_t nl = answer.find('\n', at);
    if (nl == std::string::npos) nl = answer.size();
    std::string line = answer.substr(at, nl - at);
    at = nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (oneLine(line).empty()) continue;
    SummaryEdit e;
    bool none = false;
    if (editOf(line, e, none)) out.edits.push_back(e);
    else if (none) out.none = true;
    else ++out.unread;
  }
  takeNumbering(out);
  return out;
}

Status applySummaryEdits(Summary& s, const std::vector<SummaryEdit>& edits, SummaryFrame frame,
                         std::vector<SummaryLine>& left, size_t& unread) {
  unread = 0;
  const size_t frameBytes = frame ? frame(std::string()).size() : 0;
  uint32_t next = s.nextId();   // monotonic in this pass: a line dropped here never lends its id to an add
  for (const SummaryEdit& e : edits) {
    std::string text = summaryLineText(e.text, kSummaryLineMaxBytes - frameBytes);
    if (!text.empty() && frame) text = frame(text);
    if (!applyOne(s, e, text, next, left)) ++unread;
  }
  return Status::success();
}

void noteSummaryFile(Summary& s, const std::string& path, const std::string& when, bool deleted,
                     std::vector<SummaryLine>& left) {
  if (path.empty() || path.size() > kRecentPathMaxBytes || !cleanLine(path)) return;
  uint32_t id = 0;
  for (size_t i = 0; i < s.lines.size(); ++i) {
    const SummaryLine& l = s.lines[i];
    // The path alone, or the path and one time suffix: never a longer path that starts the same way.
    const std::string open = path + digest_text::kWhenOpen;
    const bool timed = l.text.compare(0, open.size(), open) == 0 && l.text.back() == digest_text::kWhenClose[0] &&
                       looksLikeWhen(l.text.substr(open.size(), l.text.size() - open.size() - 1));
    const bool same = l.section == SummarySection::Files && (l.text == path || timed);
    if (!same) continue;
    id = l.id;
    left.push_back(l);
    s.lines.erase(s.lines.begin() + (long)i);
    break;
  }
  if (deleted) return;
  const std::string text = when.empty() ? path : path + digest_text::kWhenOpen + oneLine(when) + digest_text::kWhenClose;
  s.lines.insert(s.lines.begin() + (long)insertAt(s, SummarySection::Files),
                 SummaryLine{SummarySection::Files, id ? id : s.nextId(), text});
}

static Status fitSummaryBody(Summary& s, size_t capBytes, std::vector<SummaryLine>& left) {
  std::string text;
  const size_t cap = std::min<size_t>(capBytes, kSummaryMaxBytes);
  for (;;) {
    const Status st = encodeLines(s, text);
    if (!st.ok()) return st;
    if (text.size() <= cap) return Status::success();
    if (s.lines.empty()) return Status::fail(Err::InvalidArgument, "tx::fitSummary: cap under one line");
    size_t worst = kSummarySections;
    uint64_t worstLoad = 0;
    for (size_t i = 0; i < kSummarySections; ++i) {
      const uint64_t load = (uint64_t)sectionBytes(s, i) * 1000 / kSummaryShare[i];
      if (load > worstLoad) worst = i, worstLoad = load;
    }
    for (size_t i = 0; i < s.lines.size(); ++i)
      if ((size_t)s.lines[i].section == worst) {
        left.push_back(s.lines[i]);
        s.lines.erase(s.lines.begin() + (long)i);
        break;
      }
    s.archived = true;
  }
}

Status archiveSummaryLines(TxFiles& files, const std::string& chatKey, const std::string& day,
                           const std::vector<SummaryLine>& left) {
  for (const SummaryLine& l : left) {
    DigestEntry d;
    d.day = day;
    Status st = digestText(day, lineText(l), d.text);
    if (!st.ok()) return st;
    st = archiveDigest(files, chatKey, d);
    if (!st.ok()) return st;
  }
  return Status::success();
}

Status encodeSummary(const Summary& s, std::string& out) {
  return guarded("tx::encodeSummary: memory", [&] { return encodeSummaryBody(s, out); }, [&] { out.clear(); });
}

Status parseSummary(const std::string& text, Summary& out) {
  return guarded("tx::parseSummary: memory", [&] { return parseSummaryBody(text, out); }, [&] { out = Summary{}; });
}

Status fitSummary(Summary& s, size_t capBytes, std::vector<SummaryLine>& left) {
  return guarded("tx::fitSummary: memory", [&] { return fitSummaryBody(s, capBytes, left); }, [&] {  });
}

uint64_t rowThru(const std::vector<Item>& items, uint64_t thru) {
  uint64_t row = 0;
  for (const Item& it : items)
    if (it.kind == ItemKind::User && it.q <= thru) row = std::max(row, it.m);
  return row;
}

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
