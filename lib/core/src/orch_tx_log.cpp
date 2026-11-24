// The transcript log (tx/log.h): append-only segments, head.json as a crc-checked cache,
// the restart fill and bulk payloads. Lane RB-T2.
//
// Every reader goes through one forward scan of a segment in kFillSliceBytes slices with a
// carry buffer of one line: a line is handed on only when it ends in '\n' and passes
// checkLine (framing, crc, header); anything else is counted as torn and dropped.
#include "nimbus/orch/tx/log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>

namespace nimbus {
namespace orch {
namespace tx {

namespace {

// ---- small helpers --------------------------------------------------------------------

// q + 1, saturating (a stray crc-valid q of 2^64 - 1 must not wrap next_q to 0).
uint64_t qAfter(uint64_t q) { return q == UINT64_MAX ? q : q + 1; }

const char kTornBreak[] = "~\n";   // ends a torn tail: never a line end after a whole line's bytes
constexpr size_t kTornBreakBytes = 2;

uint32_t crc32(const char* data, size_t len) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    c ^= static_cast<uint8_t>(data[i]);
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

const char kHex[] = "0123456789abcdef";
const char kTailHead[] = ",\"z\":\"";
constexpr size_t kTailHeadBytes = sizeof kTailHead - 1;
const char kHeadName[] = "head.json";
const char kHeadTmpName[] = "head.json.tmp";

void putU64(std::string& s, uint64_t v) {
  char buf[24];
  size_t n = 0;
  do {
    buf[n++] = static_cast<char>('0' + v % 10);
    v /= 10;
  } while (v);
  while (n) s += buf[--n];
}

// Strict unsigned decimal: digits only, no overflow.
bool parseU64(const char* p, size_t n, uint64_t& v) {
  if (n == 0 || n > 20) return false;
  v = 0;
  for (size_t i = 0; i < n; ++i) {
    if (p[i] < '0' || p[i] > '9') return false;
    const uint64_t d = static_cast<uint64_t>(p[i] - '0');
    if (v > (UINT64_MAX - d) / 10) return false;
    v = v * 10 + d;
  }
  return true;
}

bool segmentNumber(const std::string& name, uint32_t& seg) {
  // seg-NNNNNN.jsonl
  if (name.size() != 16 || name.compare(0, 4, "seg-") != 0 || name.compare(10, 6, ".jsonl") != 0) return false;
  uint64_t v = 0;
  if (!parseU64(name.data() + 4, 6, v) || v == 0 || v > kMaxSegment) return false;
  seg = static_cast<uint32_t>(v);
  return true;
}

// ---- the forward scan -----------------------------------------------------------------

struct ScanStats {
  size_t torn = 0;      // framing or crc failed (cuts, garbage)
  size_t invalid = 0;   // crc-valid, but checkLine cannot read its header (a writer bug)
  size_t reads = 0;
  uint64_t bytes = 0;
};

struct LineAt {
  const char* data;
  size_t len;       // newline included
  uint64_t end;     // file offset just past the newline
  LineHeader h;
};

// The carry buffer of one scan: bytes [start, start + have) of the file.
struct Carry {
  char* b;
  size_t have = 0;
  uint64_t start = 0;
  bool skipping = false;   // inside garbage longer than any line: drop up to its end
};

// Hand every whole line in the carry to fn; keep the unfinished rest. False when fn stopped.
template <typename Fn>
bool walkLines(Carry& c, ScanStats& st, Fn& fn) {
  size_t at = 0;
  bool going = true;
  while (going) {
    const char* nl = static_cast<const char*>(std::memchr(c.b + at, '\n', c.have - at));
    if (!nl) break;
    const size_t end = static_cast<size_t>(nl - c.b) + 1;
    LineAt line{c.b + at, end - at, c.start + end, LineHeader{}};
    const Status hs = c.skipping ? Status::fail(Err::Torn, "skip") : checkLine(line.data, line.len, line.h);
    const bool good = hs.ok();
    st.torn += hs.err == Err::Torn;
    st.invalid += hs.err == Err::InvalidArgument;
    c.skipping = false;
    going = !good || fn(line);
    at = end;
  }
  if (at) std::memmove(c.b, c.b + at, c.have - at);
  c.have -= at;
  c.start += at;
  if (c.have >= kMaxLineBytes) {   // no line end within the longest line: garbage
    c.skipping = true;
    c.start += c.have;
    c.have = 0;
  }
  return going;
}

// Read `path` from `from` to its end; call fn(const LineAt&) for each whole, crc-valid
// line, in file order. fn returns false to stop. NotFound when the file is missing.
template <typename Fn>
Status scanFile(TxFiles& files, const std::string& path, uint64_t from, std::vector<char>& buf, ScanStats& st,
                Fn fn) {
  size_t total = 0;
  if (!files.size(path, total)) return Status::fail(Err::NotFound, "tx::TxLog scan");
  if (from > total) return Status::fail(Err::InvalidArgument, "tx::TxLog scan");
  if (from == total) return Status::success();
  if (buf.size() < kMaxLineBytes + kFillSliceBytes) buf.resize(kMaxLineBytes + kFillSliceBytes);
  Carry c{buf.data()};
  c.start = from;
  for (uint64_t off = from; off < total;) {
    const size_t want = static_cast<size_t>(std::min<uint64_t>(kFillSliceBytes, total - off));
    size_t got = 0;
    if (!files.readAt(path, static_cast<size_t>(off), c.b + c.have, want, got)) return Status::fail(Err::Io, "tx::TxLog scan");
    ++st.reads;
    st.bytes += got;
    if (got == 0) break;
    off += got;
    c.have += got;
    if (!walkLines(c, st, fn)) return Status::success();
  }
  st.torn += c.have || c.skipping;   // a torn tail: no end, dropped
  return Status::success();
}

}  // namespace

// ---- what a scan learns for head.json --------------------------------------------------

struct ScanFacts {
  uint64_t maxQ = 0;
  bool haveS = false, haveX = false, haveU = false, haveT = false;
  uint64_t sQ = 0, thru = 0;
  uint64_t xQ = 0, stub = 0;
  uint64_t hw = 0, mHi = 0;
  uint64_t tQ = 0;
  uint32_t tTurn = 0;
  uint8_t attempt = 0;
  std::string profile;
  uint64_t eQ = 0;
  uint32_t eTurn = 0;
  uint64_t lastReqAt = 0;
  uint64_t lastLineQ = 0;   // q of the last whole line in scan order
  bool all() const { return haveS && haveX && haveU && haveT; }
};

namespace {


// Is the item at q newer than the newest seen so far (newest wins by q)?
bool newer(bool& have, uint64_t& atQ, uint64_t q) {
  if (have && q <= atQ) return false;
  have = true;
  atQ = q;
  return true;
}

// The kinds whose fields head.json keeps, from a decoded item.
void observeDecoded(ScanFacts& f, const LineHeader& h, const Item& it) {
  if (h.kind == ItemKind::Digest && newer(f.haveS, f.sQ, h.q)) f.thru = it.thru;
  if (h.kind == ItemKind::PointerMove && newer(f.haveX, f.xQ, h.q)) f.stub = it.stub;
  if (h.kind == ItemKind::User) {
    f.haveU = true;
    for (uint64_t s : it.src) f.hw = std::max(f.hw, s);
    f.mHi = std::max(f.mHi, it.m);
  }
  if (h.kind == ItemKind::TurnOpen && newer(f.haveT, f.tQ, h.q)) {
    f.tTurn = h.turn;
    f.attempt = it.attempt;
    f.profile = it.profile;
  }
}

// Newest wins by q, so segments may be scanned in any order. Only s, x, u and t lines are
// decoded; the others are read from their header.
void observe(ScanFacts& f, const LineAt& l, Item& scratch) {
  const LineHeader& h = l.h;
  f.maxQ = std::max(f.maxQ, h.q);
  f.lastLineQ = h.q;
  if (h.kind == ItemKind::Assistant || h.kind == ItemKind::TurnEnd) f.lastReqAt = std::max(f.lastReqAt, h.t);
  if (h.kind == ItemKind::TurnEnd && h.q > f.eQ) {
    f.eQ = h.q;
    f.eTurn = h.turn;
  }
  const bool kept = h.kind == ItemKind::Digest || h.kind == ItemKind::PointerMove || h.kind == ItemKind::User ||
                    h.kind == ItemKind::TurnOpen;
  if (kept && decodeLine(l.data, l.len, scratch).ok()) observeDecoded(f, h, scratch);
}

// ---- head.json ------------------------------------------------------------------------

void putStr(std::string& s, const std::string& v) {
  s += '"';
  for (unsigned char c : v) {
    if (c == '"' || c == '\\') {
      s += '\\';
      s += static_cast<char>(c);
    } else if (c < 0x20) {
      s += "\\u00";
      s += kHex[c >> 4];
      s += kHex[c & 15];
    } else {
      s += static_cast<char>(c);
    }
  }
  s += '"';
}

void field(std::string& s, const char* key, uint64_t v) {
  s += ",\"";
  s += key;
  s += "\":";
  putU64(s, v);
}

void encodeHead(const HeadState& h, std::string& s) {
  s = "{\"v\":";
  putU64(s, h.v);
  field(s, "next_q", h.nextQ);
  field(s, "thru", h.thru);
  field(s, "stub", h.stub);
  field(s, "seg", h.seg);
  field(s, "end", h.end);
  field(s, "end_q", h.endQ);
  field(s, "thru_seg", h.thruSeg);
  field(s, "thru_off", h.thruOffset);
  field(s, "hw", h.hw);
  field(s, "m_hi", h.mHi);
  s += ",\"profile\":";
  putStr(s, h.profile);
  s += ",\"pinned\":[";
  for (size_t i = 0; i < h.pinned.size(); ++i) {
    if (i) s += ',';
    putStr(s, h.pinned[i]);
  }
  s += ']';
  field(s, "last_req_at", h.lastReqAt);
  field(s, "open_turn", h.openTurn);
  field(s, "attempts", h.attempts);
  if (!h.refusedPhotos.empty()) {   // E2-REP4: written only when there is one (older heads stay byte for byte)
    s += ",\"refused_photos\":[";
    for (size_t i = 0; i < h.refusedPhotos.size(); ++i) {
      if (i) s += ',';
      putStr(s, h.refusedPhotos[i]);
    }
    s += ']';
  }
  const uint32_t crc = crc32(s.data(), s.size());
  s += kTailHead;
  for (int i = 7; i >= 0; --i) s += kHex[(crc >> (4 * i)) & 15u];
  s += "\"}\n";
}

struct HeadCursor {
  const char* p;
  const char* end;
  bool eat(char c) {
    if (p >= end || *p != c) return false;
    ++p;
    return true;
  }
};

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// One escape after '\': only what putStr writes (\" \\ \u00XX).
bool readEscape(HeadCursor& c, std::string& out) {
  if (c.p >= c.end) return false;
  const char e = *c.p++;
  if (e == '"' || e == '\\') {
    out += e;
    return true;
  }
  if (e != 'u' || c.end - c.p < 4 || c.p[0] != '0' || c.p[1] != '0') return false;
  const int hi = hexVal(c.p[2]), lo = hexVal(c.p[3]);
  if (hi < 0 || lo < 0) return false;
  out += static_cast<char>(hi * 16 + lo);
  c.p += 4;
  return true;
}

bool readStr(HeadCursor& c, std::string& out, size_t cap) {
  out.clear();
  if (!c.eat('"')) return false;
  while (c.p < c.end && out.size() <= cap) {
    const char ch = *c.p++;
    if (ch == '"') return true;
    if (static_cast<unsigned char>(ch) < 0x20) return false;
    if (ch != '\\') out += ch;
    else if (!readEscape(c, out)) return false;
  }
  return false;
}

bool readNum(HeadCursor& c, uint64_t& v) {
  const char* s = c.p;
  while (c.p < c.end && *c.p >= '0' && *c.p <= '9') ++c.p;
  return parseU64(s, static_cast<size_t>(c.p - s), v);
}

bool readStrList(HeadCursor& c, std::vector<std::string>& out) {
  out.clear();
  if (!c.eat('[')) return false;
  if (c.eat(']')) return true;
  for (;;) {
    std::string s;
    if (out.size() >= kHeadMaxPinned || !readStr(c, s, kHeadMaxString)) return false;
    out.push_back(s);
    if (c.eat(']')) return true;
    if (!c.eat(',')) return false;
  }
}

// The numeric keys of head.json: range, and a bit for the four every head.json must hold.
struct NumKey {
  const char* key;
  uint64_t min, max;
  unsigned required;
  void (*set)(HeadState&, uint64_t);
};
const NumKey kNumKeys[] = {
    {"v", 1, 1, 1u, [](HeadState& h, uint64_t v) { h.v = static_cast<uint8_t>(v); }},
    {"next_q", 1, UINT64_MAX, 2u, [](HeadState& h, uint64_t v) { h.nextQ = v; }},
    {"seg", 1, kMaxSegment, 4u, [](HeadState& h, uint64_t v) { h.seg = static_cast<uint32_t>(v); }},
    {"end", 0, UINT64_MAX, 8u, [](HeadState& h, uint64_t v) { h.end = v; }},
    {"end_q", 0, UINT64_MAX, 0, [](HeadState& h, uint64_t v) { h.endQ = v; }},
    {"thru", 0, UINT64_MAX, 0, [](HeadState& h, uint64_t v) { h.thru = v; }},
    {"stub", 0, UINT64_MAX, 0, [](HeadState& h, uint64_t v) { h.stub = v; }},
    {"thru_seg", 1, kMaxSegment, 0, [](HeadState& h, uint64_t v) { h.thruSeg = static_cast<uint32_t>(v); }},
    {"thru_off", 0, UINT64_MAX, 0, [](HeadState& h, uint64_t v) { h.thruOffset = v; }},
    {"hw", 0, UINT64_MAX, 0, [](HeadState& h, uint64_t v) { h.hw = v; }},
    {"m_hi", 0, UINT64_MAX, 0, [](HeadState& h, uint64_t v) { h.mHi = v; }},
    {"last_req_at", 0, UINT64_MAX, 0, [](HeadState& h, uint64_t v) { h.lastReqAt = v; }},
    {"open_turn", 0, UINT32_MAX, 0, [](HeadState& h, uint64_t v) { h.openTurn = static_cast<uint32_t>(v); }},
    {"attempts", 0, 255, 0, [](HeadState& h, uint64_t v) { h.attempts = static_cast<uint8_t>(v); }},
};

bool headField(const std::string& key, HeadCursor& c, HeadState& h, unsigned& seen) {
  if (key == "profile") return readStr(c, h.profile, kHeadMaxString);
  if (key == "pinned") return readStrList(c, h.pinned);
  if (key == "refused_photos") return readStrList(c, h.refusedPhotos) && h.refusedPhotos.size() <= kHeadMaxRefusedPhotos;
  if (c.p < c.end && (*c.p == '"' || *c.p == '[')) {   // a key a newer writer added
    std::string skip;
    std::vector<std::string> skipList;
    return *c.p == '"' ? readStr(c, skip, kHeadMaxString) : readStrList(c, skipList);
  }
  uint64_t v = 0;
  if (!readNum(c, v)) return false;
  for (const NumKey& k : kNumKeys) {
    if (key != k.key) continue;
    if (v < k.min || v > k.max) return false;
    k.set(h, v);
    seen |= k.required;
  }
  return true;
}

// The fixed tail ',"z":"<8 hex>"}' and its crc over everything before it.
bool headCrcOk(const char* data, size_t len) {
  const char* tail = data + len - kLineTailBytes;
  if (std::memcmp(tail, kTailHead, kTailHeadBytes) != 0 || tail[14] != '"' || tail[15] != '}') return false;
  uint32_t want = 0;
  for (int i = 0; i < 8; ++i) {
    const int d = hexVal(tail[kTailHeadBytes + i]);
    if (d < 0) return false;
    want = (want << 4) | static_cast<uint32_t>(d);
  }
  return crc32(data, len - kLineTailBytes) == want;
}

bool decodeHead(const char* data, size_t len, HeadState& out) {
  out = HeadState{};
  if (len < kLineTailBytes + 3 || data[len - 1] != '\n' || !headCrcOk(data, len - 1)) return false;
  HeadCursor c{data, data + len - 1 - kLineTailBytes};
  if (!c.eat('{')) return false;
  unsigned seen = 0;
  std::string key;
  do {
    if (!readStr(c, key, 32) || !c.eat(':') || !headField(key, c, out, seen)) return false;
  } while (c.eat(','));
  return c.p == c.end && seen == 15u && out.thruSeg <= out.seg;
}

bool headWithinLimits(const HeadState& h) {
  if (h.profile.size() > kHeadMaxString || h.pinned.size() > kHeadMaxPinned) return false;
  if (h.refusedPhotos.size() > kHeadMaxRefusedPhotos) return false;
  for (const std::string& p : h.pinned)
    if (p.size() > kHeadMaxString) return false;
  for (const std::string& p : h.refusedPhotos)
    if (p.size() > kHeadMaxString) return false;
  return true;
}

// ---- paths ----------------------------------------------------------------------------

bool keyChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

Status chatFile(const std::string& chatKey, const char* name, const char* where, std::string& out) {
  out.clear();
  if (!chatKeyOk(chatKey)) return Status::fail(Err::InvalidArgument, where);
  out = std::string(kTxRoot) + "/" + chatKey + "/" + name;
  return Status::success();
}

// "b/<turn>.txb@<off>+<len>"
bool parseHandle(const std::string& h, uint32_t& turn, uint64_t& off, uint64_t& len) {
  const size_t at = h.find('@'), plus = h.find('+');
  if (h.compare(0, 2, "b/") != 0 || at == std::string::npos || plus == std::string::npos || plus < at || at < 7 ||
      h.compare(at - 4, 4, ".txb") != 0)
    return false;
  uint64_t t = 0;
  if (!parseU64(h.data() + 2, at - 6, t) || t == 0 || t > UINT32_MAX) return false;
  if (!parseU64(h.data() + at + 1, plus - at - 1, off) || !parseU64(h.data() + plus + 1, h.size() - plus - 1, len))
    return false;
  turn = static_cast<uint32_t>(t);
  return len > 0 && len <= kBulkMaxBytes && off <= UINT64_MAX - len;
}

}  // namespace

// ==== paths ==============================================================================

bool chatKeyOk(const std::string& chatKey) {
  if (chatKey.empty() || chatKey.size() > kChatKeyMaxBytes) return false;
  for (char c : chatKey)
    if (!keyChar(c)) return false;
  return true;
}

Status txDir(const std::string& chatKey, std::string& out) {
  out.clear();
  if (!chatKeyOk(chatKey)) return Status::fail(Err::InvalidArgument, "tx::txDir");
  out = std::string(kTxRoot) + "/" + chatKey;
  return Status::success();
}

Status segmentPath(const std::string& chatKey, uint32_t seg, std::string& out) {
  out.clear();
  if (seg == 0 || seg > kMaxSegment) return Status::fail(Err::InvalidArgument, "tx::segmentPath");
  char name[24];
  std::snprintf(name, sizeof name, "seg-%06u.jsonl", static_cast<unsigned>(seg));
  return chatFile(chatKey, name, "tx::segmentPath", out);
}

Status headPath(const std::string& chatKey, std::string& out) {
  return chatFile(chatKey, kHeadName, "tx::headPath", out);
}

Status bulkPath(const std::string& chatKey, uint32_t turn, std::string& out) {
  out.clear();
  if (turn == 0) return Status::fail(Err::InvalidArgument, "tx::bulkPath");
  char name[24];
  std::snprintf(name, sizeof name, "b/%u.txb", static_cast<unsigned>(turn));
  return chatFile(chatKey, name, "tx::bulkPath", out);
}

Status digestArchivePath(const std::string& chatKey, const std::string& yyyymm, std::string& out) {
  out.clear();
  const char* m = yyyymm.c_str();
  bool ok = yyyymm.size() == 7 && m[4] == '-';
  for (int i : {0, 1, 2, 3, 5, 6}) ok = ok && m[i] >= '0' && m[i] <= '9';
  const int month = ok ? (m[5] - '0') * 10 + (m[6] - '0') : 0;
  if (month < 1 || month > 12) return Status::fail(Err::InvalidArgument, "tx::digestArchivePath");
  return chatFile(chatKey, ("digests-" + yyyymm + ".md").c_str(), "tx::digestArchivePath", out);
}

Status migratedPath(const std::string& chatKey, std::string& out) {
  return chatFile(chatKey, "migrated.json", "tx::migratedPath", out);
}

// ==== open and head.json =================================================================

Status TxLog::segments(std::vector<uint32_t>& out) {
  out.clear();
  std::vector<std::string> names;
  if (!files_.list(dir_, names)) return Status::fail(Err::Io, "tx::TxLog::open");
  uint32_t seg = 0;
  for (const std::string& n : names)
    if (segmentNumber(n, seg)) out.push_back(seg);
  std::sort(out.begin(), out.end());
  return Status::success();
}

Status TxLog::readHeadFile(const std::string& path, HeadState& out) {
  size_t n = 0, got = 0;
  if (!files_.size(path, n)) return Status::fail(Err::NotFound, "tx::TxLog::open");
  if (n == 0 || n > kHeadMaxBytes) return Status::fail(Err::Torn, "tx::TxLog::open");
  std::string buf(n, '\0');
  if (!files_.readAt(path, 0, &buf[0], n, got) || got != n) return Status::fail(Err::Io, "tx::TxLog::open");
  if (!decodeHead(buf.data(), n, out)) return Status::fail(Err::Torn, "tx::TxLog::open");
  return Status::success();
}

// Does head.json agree with the segments? The line it says ends the open segment must be
// there (crc-valid, q below next_q) and the fill start must sit on a line start.
Status TxLog::checkHead(const HeadState& h, const std::vector<uint32_t>& segs) {
  const Status bad = Status::fail(Err::InvalidArgument, "tx::TxLog::open: head.json does not match");
  const bool haveSeg = std::binary_search(segs.begin(), segs.end(), h.seg);
  const bool haveThru = std::binary_search(segs.begin(), segs.end(), h.thruSeg);
  if (!haveSeg || !haveThru || h.thruSeg > h.seg || (h.thruSeg == h.seg && h.thruOffset > h.end)) return bad;
  return lastLineOk(h) && thruLineOk(h) ? Status::success() : bad;
}

// The line that ends at h.end of segment h.seg is whole, crc-valid, the one head.json names
// (end_q) and below next_q: a head.json of another state of the log does not pass.
bool TxLog::lastLineOk(const HeadState& h) {
  std::string path;
  size_t size = 0;
  if (!segmentPath(chatKey_, h.seg, path).ok() || !files_.size(path, size) || size < h.end) return false;
  if (h.end == 0) return h.endQ == 0;
  LineHeader lh;
  return lineBefore(path, h.end, lh) && lh.q == h.endQ && lh.q < h.nextQ;
}

// The whole, crc-valid line that ends at `end` of `path`: one slice first (an open holds the
// card lock for one bounded read), the longest line only when it is longer than a slice.
bool TxLog::lineBefore(const std::string& path, uint64_t end, LineHeader& lh) {
  std::string tail;
  size_t start = 0;
  if (!lastLine(path, end, kFillSliceBytes, tail, start) && !lastLine(path, end, kMaxLineBytes + 1, tail, start))
    return false;
  return checkLine(tail.data() + start, tail.size() - start, lh).ok();
}

// Read up to `cap` bytes ending at `end` of `path`: true when they hold the whole line that
// ends there (at `start` of `tail`).
bool TxLog::lastLine(const std::string& path, uint64_t end, size_t cap, std::string& tail, size_t& start) {
  const size_t span = static_cast<size_t>(std::min<uint64_t>(end, cap));
  size_t got = 0;
  tail.assign(span, '\0');
  if (span < 2 || !files_.readAt(path, static_cast<size_t>(end - span), &tail[0], span, got) || got != span ||
      tail.back() != '\n')
    return false;
  const size_t prev = tail.find_last_of('\n', span - 2);
  start = prev == std::string::npos ? 0 : prev + 1;
  return prev != std::string::npos || end == span;
}

// The fill start sits just past a whole line that a digest covers (q <= thru).
bool TxLog::thruLineOk(const HeadState& h) {
  if (h.thruOffset == 0) return true;
  std::string path;
  LineHeader lh;
  return segmentPath(chatKey_, h.thruSeg, path).ok() && lineBefore(path, h.thruOffset, lh) && lh.q <= h.thru;
}

// The lines appended after head.json was written: from (seg, end) to the newest segment.
Status TxLog::catchUp(HeadState& h, const std::vector<uint32_t>& segs) {
  ScanFacts f;
  Item scratch;
  std::vector<char> buf;
  ScanStats st;
  for (uint32_t seg : segs) {
    if (seg < h.seg) continue;
    std::string path;
    Status s = segmentPath(chatKey_, seg, path);
    if (s.ok()) s = scanFile(files_, path, seg == h.seg ? h.end : 0, buf, st, [&](const LineAt& l) {
      observe(f, l, scratch);
      return true;
    });
    if (!s.ok()) return s;
  }
  applyLater(f, h);
  if (f.haveS && f.thru > h.thru) {
    h.thru = f.thru;
    return locateThru(h.thru, h.thruSeg, h.thruOffset, segs, h.thruSeg, h.thruOffset);
  }
  return Status::success();
}

// What lines written after head.json say, on top of it.
void TxLog::applyLater(const ScanFacts& f, HeadState& h) {
  h.nextQ = std::max(h.nextQ, qAfter(f.maxQ));
  if (f.lastLineQ) h.endQ = f.lastLineQ;
  if (f.haveX) h.stub = std::max(h.stub, f.stub);
  if (f.haveU) {
    h.hw = std::max(h.hw, f.hw);
    h.mHi = std::max(h.mHi, f.mHi);
  }
  if (f.haveT) {
    h.openTurn = f.eQ > f.tQ ? 0 : f.tTurn;
    h.attempts = f.eQ > f.tQ ? 0 : f.attempt;
    h.profile = f.profile;
  } else if (f.eQ && f.eTurn == h.openTurn) {
    h.openTurn = 0;
    h.attempts = 0;
  }
  h.lastReqAt = std::max(h.lastReqAt, f.lastReqAt);
}

// Forward from a known fill start: the end of the last line with q <= thru.
Status TxLog::locateThru(uint64_t thru, uint32_t fromSeg, uint64_t fromOff, const std::vector<uint32_t>& segs,
                         uint32_t& outSeg, uint64_t& outOff) {
  uint32_t seg = fromSeg;
  uint64_t off = fromOff;
  bool past = false;
  std::vector<char> buf;
  ScanStats st;
  for (uint32_t s : segs) {
    if (s < fromSeg || past) continue;
    std::string path;
    Status r = segmentPath(chatKey_, s, path);
    if (r.ok()) r = scanFile(files_, path, s == fromSeg ? fromOff : 0, buf, st, [&](const LineAt& l) {
      if (l.h.q > thru) return !(past = true);
      seg = s;
      off = l.end;
      return true;
    });
    if (!r.ok()) return r;
  }
  outSeg = seg;
  outOff = off;
  return Status::success();
}

// From the segments alone: newest segment first until the newest s, x, u and t items are
// known, then the line of thru, newest segment first.
Status TxLog::rebuildFrom(const std::vector<uint32_t>& segs, HeadState& out) {
  out = HeadState{};
  if (segs.empty()) return Status::success();
  ScanFacts f;
  Item scratch;
  std::vector<char> buf;
  ScanStats st;
  for (auto it = segs.rbegin(); it != segs.rend() && !(f.all() && f.maxQ); ++it) {
    std::string path;
    Status s = segmentPath(chatKey_, *it, path);
    if (s.ok()) s = scanFile(files_, path, 0, buf, st, [&](const LineAt& l) {
      observe(f, l, scratch);
      return true;
    });
    if (!s.ok() && s.err != Err::NotFound) return s;
    if (it == segs.rbegin()) out.endQ = f.lastLineQ;   // the open segment's last whole line
  }
  out.nextQ = qAfter(f.maxQ);
  out.thru = f.thru;
  out.stub = f.stub;
  out.hw = f.hw;
  out.mHi = f.mHi;
  out.profile = f.profile;
  out.openTurn = f.haveT && f.eQ < f.tQ ? f.tTurn : 0;
  out.attempts = out.openTurn ? f.attempt : 0;
  out.lastReqAt = f.lastReqAt;
  out.seg = segs.back();
  return locateBackward(out.thru, segs, out.thruSeg, out.thruOffset);
}

// The end of the last line with q <= thru, newest segment first; the oldest segment's
// start when there is none (thru 0, or everything after thru).
Status TxLog::locateBackward(uint64_t thru, const std::vector<uint32_t>& segs, uint32_t& outSeg, uint64_t& outOff) {
  outSeg = segs.front();
  outOff = 0;
  std::vector<char> buf;
  ScanStats st;
  for (auto it = segs.rbegin(); thru && it != segs.rend(); ++it) {
    std::string path;
    bool found = false;
    uint64_t end = 0;
    Status s = segmentPath(chatKey_, *it, path);
    if (s.ok()) s = scanFile(files_, path, 0, buf, st, [&](const LineAt& l) {
      if (l.h.q > thru) return false;
      found = true;
      end = l.end;
      return true;
    });
    if (!s.ok() && s.err != Err::NotFound) return s;
    if (found) {
      outSeg = *it;
      outOff = end;
      break;
    }
  }
  return Status::success();
}

Status TxLog::adopt(const HeadState& h, const std::vector<uint32_t>& segs) {
  seg_ = segs.empty() ? 1 : segs.back();
  segBytes_ = 0;
  tailTorn_ = false;
  if (!segs.empty()) {
    std::string path;
    size_t size = 0, got = 0;
    if (!segmentPath(chatKey_, seg_, path).ok() || !files_.size(path, size)) return Status::fail(Err::Io, "tx::TxLog::open");
    segBytes_ = size;
    char last = '\n';
    if (size && (!files_.readAt(path, size - 1, &last, 1, got) || got != 1)) return Status::fail(Err::Io, "tx::TxLog::open");
    tailTorn_ = last != '\n';
  }
  maxQ_ = h.nextQ ? h.nextQ - 1 : 0;
  lastLineQ_ = h.endQ;
  thru_ = h.thru;
  stub_ = h.stub;
  thruSeg_ = h.thruSeg;
  thruOff_ = h.thruOffset;
  needsSync_ = false;
  open_ = true;
  return Status::success();
}

Status TxLog::open(HeadState& head) {
  open_ = false;
  lost_ += held_;   // never written in order now: a reopen merges what RAM holds itself (plain reopen)
  held_ = 0;
  heldFrom_ = 0;
  Status st = txDir(chatKey_, dir_);
  if (!st.ok()) return Status::fail(Err::InvalidArgument, "tx::TxLog::open");
  std::vector<uint32_t> segs;
  if (!(st = segments(segs)).ok()) return st;
  if (segs.empty() && !(st = provenFresh()).ok()) return st;
  HeadState h;
  bool have = segs.empty();
  // The temp first: a whole one exists only when a cut or a refusal stopped its rename, so
  // it is newer than head.json.
  for (const char* name : {kHeadTmpName, kHeadName}) {
    if (have) break;
    have = readHeadFile(dir_ + "/" + name, h).ok() && checkHead(h, segs).ok() && catchUp(h, segs).ok();
  }
  const bool rebuilt = !have;
  if (rebuilt && !(st = rebuildFrom(segs, h)).ok()) return st;
  if (!(st = adopt(h, segs)).ok()) return st;
  h.seg = seg_;
  h.end = segBytes_;
  head = h;
  if (rebuilt) (void)writeHead(h);   // best effort: the next boot does not rebuild again
  return Status::success();
}

// No segment is listed: a fresh chat only when nothing of an earlier one can be read (lane
// E2-STORE, finding S1). A head file that names stored items, or a first segment that answers
// a size, means the listing missed them: Io (cannot tell), never an empty conversation. A head
// file that is missing or torn names nothing; one that names nothing yet (next_q 1, no line,
// no digest) is a chat that never stored an item.
Status TxLog::provenFresh() {
  const Status cannotTell = Status::fail(Err::Io, "tx::TxLog::open: no segment listed, yet the chat has stored state");
  for (const char* name : {kHeadTmpName, kHeadName}) {
    HeadState h;
    const Status st = readHeadFile(dir_ + "/" + name, h);
    if (st.err == Err::Io) return st;
    if (st.ok() && (h.nextQ > 1 || h.endQ || h.end || h.seg > 1 || h.thru || h.stub)) return cannotTell;
  }
  std::string path;
  size_t size = 0;
  if (segmentPath(chatKey_, 1, path).ok() && files_.size(path, size)) return cannotTell;
  return Status::success();
}

Status TxLog::rebuildHead(HeadState& out) {
  Status st = txDir(chatKey_, dir_);
  if (!st.ok()) return Status::fail(Err::InvalidArgument, "tx::TxLog::rebuildHead");
  std::vector<uint32_t> segs;
  if (!(st = segments(segs)).ok()) return st;
  if (!(st = rebuildFrom(segs, out)).ok()) return st;
  if (!segs.empty()) {
    std::string path;
    size_t size = 0;
    if (segmentPath(chatKey_, segs.back(), path).ok() && files_.size(path, size)) out.end = size;
  }
  return Status::success();
}

Status TxLog::writeHead(const HeadState& head) {
  if (!open_) return Status::fail(Err::InvalidArgument, "tx::TxLog::writeHead: log not open");
  if (!headWithinLimits(head) || head.v != 1) return Status::fail(Err::InvalidArgument, "tx::TxLog::writeHead");
  HeadState h = head;
  h.nextQ = std::max<uint64_t>({h.nextQ, qAfter(maxQ_), 1});
  if (h.thru >= h.nextQ || h.stub >= h.nextQ)
    return Status::fail(Err::InvalidArgument, "tx::TxLog::writeHead: thru or stub past the log");
  h.stub = std::max(h.stub, stub_);
  if (h.thru > thru_) {
    std::vector<uint32_t> segs;
    Status st = segments(segs);
    if (st.ok()) st = locateThru(h.thru, thruSeg_, thruOff_, segs, thruSeg_, thruOff_);
    if (!st.ok()) return st;
    thru_ = h.thru;
  }
  h.thru = thru_;
  h.thruSeg = thruSeg_;
  h.thruOffset = thruOff_;
  h.seg = seg_;
  h.end = segBytes_;
  h.endQ = lastLineQ_;
  stub_ = h.stub;
  std::string text;
  encodeHead(h, text);
  if (text.size() > kHeadMaxBytes) return Status::fail(Err::InvalidArgument, "tx::TxLog::writeHead: over 16 KB");
  const std::string tmp = dir_ + "/" + kHeadTmpName;
  if (!files_.writeWhole(tmp, text) || !files_.rename(tmp, dir_ + "/" + kHeadName))
    return Status::fail(Err::Io, "tx::TxLog::writeHead");
  return Status::success();
}

// ==== appends ============================================================================

// After a refused append the segment may hold part of the line: learn its real size and
// whether it still ends on a line end.
Status TxLog::syncTail() {
  std::string path;
  Status st = segmentPath(chatKey_, seg_, path);
  if (!st.ok()) return st;
  size_t size = 0, got = 0;
  if (!files_.size(path, size)) {
    if (segBytes_ == 0) needsSync_ = false;   // never created: nothing was written
    return Status::success();
  }
  char last = '\n';
  if (size && (!files_.readAt(path, size - 1, &last, 1, got) || got != 1)) return Status::success();
  segBytes_ = size;
  tailTorn_ = last != '\n';
  needsSync_ = false;
  return Status::success();
}

Status TxLog::writeLine(const char* line, size_t len, uint64_t q, StoreState& state) {
  state = StoreState::NotStored;
  if (!open_) return Status::fail(Err::NotStored, "tx::TxLog::append: log not open");
  if (needsSync_) (void)syncTail();
  // Unknown tail after a refusal we could not check: start with a line end to be safe.
  bool lead = tailTorn_ || needsSync_;
  if (segBytes_ > 0 && segBytes_ + (lead ? kTornBreakBytes : 0) + len > kSegmentRollBytes) {
    if (seg_ >= kMaxSegment) return Status::fail(Err::TooLarge, "tx::TxLog::append: segments exhausted");
    ++seg_;
    segBytes_ = 0;
    tailTorn_ = needsSync_ = lead = false;
  }
  std::string path;
  Status st = segmentPath(chatKey_, seg_, path);
  if (!st.ok()) return st;
  const char* data = line;
  size_t n = len;
  if (lead) {
    if (line != line_.data()) line_.assign(line, len);
    line_.insert(0, kTornBreak, kTornBreakBytes);
    data = line_.data();
    n = line_.size();
  }
  if (!files_.append(path, data, n)) {
    needsSync_ = true;
    return Status::fail(Err::NotStored, "tx::TxLog::append");
  }
  segBytes_ += n;
  lastLineQ_ = q;
  tailTorn_ = false;
  maxQ_ = std::max(maxQ_, q);
  state = StoreState::Stored;
  return Status::success();
}

// Lane E2-BFIX1: a line of a higher q never goes to storage before the items store() holds.
static Status heldFirst(const char* where) { return Status::fail(Err::NotStored, where); }

Status TxLog::append(const Item& item, StoreState& state) {
  state = StoreState::NotStored;
  Status st = encodeLine(item, line_);
  if (!st.ok()) return st;
  if (held_) return heldFirst("tx::TxLog::append: held items go first");
  return writeLine(line_.data(), line_.size(), item.q, state);
}

Status TxLog::appendLine(const char* line, size_t len, StoreState& state) {
  state = StoreState::NotStored;
  LineHeader h;
  Status st = checkLine(line, len, h);
  if (!st.ok()) return Status::fail(st.err, "tx::TxLog::appendLine");
  if (held_) return heldFirst("tx::TxLog::appendLine: held items go first");
  return writeChecked(line, len, h.q, state);
}

Status TxLog::writeChecked(const char* line, size_t len, uint64_t q, StoreState& state) {
  if (line[len - 1] != '\n') {
    line_.assign(line, len);
    line_ += '\n';
    return writeLine(line_.data(), line_.size(), q, state);
  }
  return writeLine(line, len, q, state);
}

// The arena's line at `at` to storage (it passed checkLine when the arena took it).
Status TxLog::writeArenaLine(const TxArena& arena, size_t at, uint64_t q, StoreState& state) {
  state = StoreState::NotStored;
  const char* data = nullptr;
  size_t len = 0;
  const Status st = arena.line(at, data, len);
  if (!st.ok() || len == 0) return st.ok() ? Status::fail(Err::Torn, "tx::TxLog::writeArenaLine") : st;
  return writeChecked(data, len, q, state);
}

Status TxLog::flushHeld(TxArena& arena) {
  if (!held_) return Status::success();
  size_t from = arena.count();   // the held items are the arena's newest: walk back to the oldest
  ArenaEntry e;
  while (from > 0 && arena.header(from - 1, e).ok() && e.q >= heldFrom_) --from;
  uint32_t found = 0;
  for (size_t i = from; i < arena.count(); ++i) {
    if (!arena.header(i, e).ok() || !(e.flags & kItemNotStored)) continue;
    StoreState state = StoreState::NotStored;
    (void)writeArenaLine(arena, i, e.q, state);
    if (state != StoreState::Stored) {   // refused: the rest stay held, from this one
      heldFrom_ = e.q;
      uint32_t left = 0;
      for (size_t j = i; j < arena.count(); ++j)
        if (arena.header(j, e).ok() && (e.flags & kItemNotStored)) ++left;
      if (found + left < held_) lost_ += held_ - found - left;   // held items the arena let go
      held_ = left;
      return Status::success();
    }
    (void)arena.setFlags(i, static_cast<uint8_t>(e.flags & ~kItemNotStored));
    ++found;
  }
  if (found < held_) lost_ += held_ - found;   // held items the arena let go: never written
  held_ = 0;
  heldFrom_ = 0;
  return Status::success();
}

Status TxLog::store(const Item& item, TxArena& arena, StoreState& state) {
  state = StoreState::NotStored;
  Status st = arena.append(item);
  if (!st.ok()) return st;
  const size_t at = arena.count() - 1;
  if (held_) (void)flushHeld(arena);   // the new item is not flagged yet: only the held ones go
  (void)writeArenaLine(arena, at, item.q, state);
  if (state == StoreState::Stored) {
    if (held_) {   // storage took this line and refused an older one: the held items are given up
      lost_ += held_;
      held_ = 0;
      heldFrom_ = 0;
    }
    return Status::success();
  }
  ArenaEntry e;
  if (arena.header(at, e).ok()) (void)arena.setFlags(at, static_cast<uint8_t>(e.flags | kItemNotStored));
  if (!open_) return Status::success();   // a closed log: the caller's reopen merges what RAM holds
  if (!held_) heldFrom_ = item.q;
  ++held_;
  return Status::success();
}

// ==== bulk ===============================================================================

Status TxLog::appendBulk(uint32_t turn, const std::string& payload, std::string& handle) {
  handle.clear();
  std::string path;
  Status st = bulkPath(chatKey_, turn, path);
  if (!st.ok() || payload.empty()) return Status::fail(Err::InvalidArgument, "tx::TxLog::appendBulk");
  if (payload.size() > kBulkMaxBytes) return Status::fail(Err::TooLarge, "tx::TxLog::appendBulk");
  // Appends of kBulkChunkBytes at most, so no one card-lock hold passes the 150 ms gate
  // (Ruling 19: 16 KB holds 28 ms). The offset is read after the last chunk (TxFiles::size
  // cannot tell a missing file from a refusal): the payload ends at the file's end. A
  // refused chunk leaves bytes no handle names; without a size there is no handle.
  for (size_t at = 0; at < payload.size(); at += kBulkChunkBytes)
    if (!files_.append(path, payload.data() + at, std::min(kBulkChunkBytes, payload.size() - at)))
      return Status::fail(Err::NotStored, "tx::TxLog::appendBulk");
  size_t size = 0;
  if (!files_.size(path, size) || size < payload.size()) return Status::fail(Err::NotStored, "tx::TxLog::appendBulk");
  const size_t off = size - payload.size();
  handle = "b/";
  putU64(handle, turn);
  handle += ".txb@";
  putU64(handle, off);
  handle += '+';
  putU64(handle, payload.size());
  return Status::success();
}

Status TxLog::readBulk(const std::string& handle, size_t offset, size_t limit, std::string& out) {
  out.clear();
  uint32_t turn = 0;
  uint64_t at = 0, len = 0;
  std::string path;
  if (!parseHandle(handle, turn, at, len) || !bulkPath(chatKey_, turn, path).ok())
    return Status::fail(Err::InvalidArgument, "tx::TxLog::readBulk");
  if (offset >= len || limit == 0) return Status::success();
  const size_t want = static_cast<size_t>(std::min<uint64_t>(limit, len - offset));
  size_t size = 0;
  if (!files_.size(path, size)) {
    // A file that is gone (30-day retention) is NotFound only when the folder's listing says so; a
    // listing that fails, or that names the file, is a read that failed (ruling 116: never "absent").
    std::vector<std::string> names;
    const size_t slash = path.rfind('/');
    const bool listed = files_.list(path.substr(0, slash), names);
    const bool there = listed && std::find(names.begin(), names.end(), path.substr(slash + 1)) != names.end();
    return Status::fail(listed && !there ? Err::NotFound : Err::Io, "tx::TxLog::readBulk");
  }
  if (size < at + len) return Status::fail(Err::NotFound, "tx::TxLog::readBulk");
  out.resize(want);
  size_t done = 0;
  while (done < want) {
    size_t got = 0;
    const size_t slice = std::min(want - done, kFillSliceBytes);
    if (!files_.readAt(path, static_cast<size_t>(at + offset + done), &out[done], slice, got) || got == 0) {
      out.clear();
      return Status::fail(Err::Io, "tx::TxLog::readBulk");
    }
    done += got;
  }
  return Status::success();
}

Status TxLog::pruneBulk(uint32_t beforeTurn, size_t& removed) {
  removed = 0;
  std::string dir;
  if (!txDir(chatKey_, dir).ok()) return Status::fail(Err::InvalidArgument, "tx::TxLog::pruneBulk");
  std::vector<std::string> names;
  if (!files_.list(dir + "/b", names)) return Status::fail(Err::Io, "tx::TxLog::pruneBulk");
  for (const std::string& n : names) {
    uint64_t turn = 0;
    if (n.size() < 5 || n.compare(n.size() - 4, 4, ".txb") != 0 || !parseU64(n.data(), n.size() - 4, turn)) continue;
    if (turn < beforeTurn && files_.remove(dir + "/b/" + n)) ++removed;
  }
  return Status::success();
}

// ==== the restart fill ===================================================================

namespace {

// Give up the oldest resident turn (a run of items with the first item's turn number).
bool dropOldestTurn(TxArena& arena, FillReport& r, uint32_t keepTurn) {
  ArenaEntry first, e;
  if (!arena.count() || !arena.header(0, first).ok() || first.turn == keepTurn) return false;
  uint64_t last = first.q;
  size_t n = 0;
  for (size_t i = 0; i < arena.count() && arena.header(i, e).ok() && e.turn == first.turn; ++i, ++n) last = e.q;
  if (!arena.dropThrough(last).ok()) return false;
  r.dropped += n;
  r.droppedThru = std::max(r.droppedThru, last);
  return true;
}

}  // namespace

namespace {

// One line of the fill: skip what the arena has or a digest covers, drop what decodeLine
// cannot read, make room by giving up the oldest turn, append the line's bytes as stored.
struct FillLine {
  const HeadState& head;
  TxArena& arena;
  FillReport& report;
  Item scratch;
  Status err = Status::success();

  bool operator()(const LineAt& l) {
    if (l.h.q <= head.thru || l.h.q <= arena.lastQ()) {
      ++report.skipped;
      return true;
    }
    if (!decodeLine(l.data, l.len, scratch).ok()) {
      ++report.invalid;
      return true;
    }
    while (recordBytes(l.len) > arena.freeBytes() && dropOldestTurn(arena, report, l.h.turn)) {
    }
    if (recordBytes(l.len) > arena.freeBytes()) {   // larger than the whole arena
      ++report.dropped;
      report.droppedThru = std::max(report.droppedThru, l.h.q);
      return true;
    }
    err = arena.appendLine(l.data, l.len);
    report.lines += err.ok();
    return err.ok();
  }
};

}  // namespace

Status TxLog::fill(const HeadState& head, TxArena& arena, FillReport& report) {
  report = FillReport{};
  Status st = txDir(chatKey_, dir_);
  if (!st.ok() || !arena.capacity()) return Status::fail(Err::InvalidArgument, "tx::TxLog::fill");
  std::vector<uint32_t> segs;
  if (!segments(segs).ok()) return Status::fail(Err::Io, "tx::TxLog::fill");
  std::vector<char> buf;
  ScanStats ss;
  FillLine one{head, arena, report, Item{}, Status::success()};
  for (uint32_t seg : segs) {
    if (seg < head.thruSeg) continue;
    std::string path;
    st = segmentPath(chatKey_, seg, path);
    if (st.ok()) st = scanFile(files_, path, seg == head.thruSeg ? head.thruOffset : 0, buf, ss, std::ref(one));
    if (st.ok()) st = one.err;
    if (!st.ok()) return st.err == Err::NotFound ? Status::fail(Err::Io, "tx::TxLog::fill") : st;
  }
  report.torn = ss.torn;
  report.invalid += ss.invalid;
  report.reads = ss.reads;
  report.bytes = ss.bytes;
  return Status::success();
}

Status TxLog::fill(const HeadState& head, TxArena& arena) {
  FillReport r;
  return fill(head, arena, r);
}

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
