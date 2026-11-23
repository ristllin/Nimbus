// tx/search.h - reading one chat's stored conversation back by words and by id (lane E2-SRCH).
#include "nimbus/orch/tx/search.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

#include "nimbus/orch/tx/old_engine.h"   // rowIdString

namespace nimbus {
namespace orch {
namespace tx {

namespace {

constexpr size_t kWindowMax = kMaxLineBytes + kSliceBytes;   // the widest backward window
constexpr char kSegPrefix[] = "seg-";
constexpr char kSegSuffix[] = ".jsonl";

char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }
bool wordByte(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80;
}

// Case-insensitive find of a lower-case needle in [hay, hay + n).
bool containsFolded(const char* hay, size_t n, const std::string& needle) {
  const size_t m = needle.size();
  if (m == 0) return true;
  if (m > n) return false;
  for (size_t i = 0; i + m <= n; ++i) {
    if (lower(hay[i]) != needle[0]) continue;
    size_t j = 1;
    while (j < m && lower(hay[i + j]) == needle[j]) ++j;
    if (j == m) return true;
  }
  return false;
}

bool allIn(const char* hay, size_t n, const std::vector<std::string>& words) {
  if (words.empty()) return false;
  for (const std::string& w : words)
    if (!containsFolded(hay, n, w)) return false;
  return true;
}

// The segment numbers of a chat, ascending. Io when the folder cannot be listed.
Status segmentsOf(TxFiles& files, const std::string& chatKey, std::vector<uint32_t>& out) {
  out.clear();
  std::string dir;
  if (!txDir(chatKey, dir).ok()) return Status::fail(Err::InvalidArgument, "tx::search.segments");
  std::vector<std::string> names;
  if (!files.list(dir, names)) return Status::fail(Err::Io, "tx::search.segments");
  const size_t pre = sizeof(kSegPrefix) - 1, suf = sizeof(kSegSuffix) - 1;
  for (const std::string& n : names) {
    if (n.size() != pre + 6 + suf || n.compare(0, pre, kSegPrefix) != 0 || n.compare(pre + 6, suf, kSegSuffix) != 0)
      continue;
    uint32_t v = 0;
    bool digits = true;
    for (size_t i = pre; i < pre + 6; ++i) {
      if (n[i] < '0' || n[i] > '9') digits = false;
      v = v * 10 + uint32_t(n[i] - '0');
    }
    if (digits && v >= 1 && v <= kMaxSegment) out.push_back(v);
  }
  std::sort(out.begin(), out.end());
  return Status::success();
}

// [from, from + want) of a file into `buf`, in kSliceBytes reads. false: a read was refused or the
// file is shorter than asked.
bool readSpan(TxFiles& files, const std::string& path, size_t from, size_t want, std::string& buf) {
  buf.resize(want);
  size_t done = 0;
  while (done < want) {
    size_t got = 0;
    const size_t slice = std::min(want - done, kSliceBytes);
    if (!files.readAt(path, from + done, &buf[done], slice, got) || got == 0) return false;
    done += got;
  }
  return true;
}

struct Line {
  const char* data = nullptr;
  size_t len = 0;            // with its '\n'
  uint32_t seg = 0;
  uint64_t start = 0, end = 0;
};

// The chat's lines, newest first. pos() is the cursor of the next line it would return: lines
// that end at or before (seg, off), and every older segment.
class Backward {
 public:
  Backward(TxFiles& f, const std::string& chatKey, std::vector<uint32_t> segs)
      : f_(f), key_(chatKey), segs_(std::move(segs)) {}

  // From the newest line (seg 0), or from a cursor.
  Status start(uint32_t seg, uint64_t off) {
    if (segs_.empty()) return Status::success();
    if (seg == 0) {
      idx_ = segs_.size() - 1;
      return openSeg(true, 0);
    }
    auto it = std::lower_bound(segs_.begin(), segs_.end(), seg);
    if (it == segs_.end() || *it != seg) {   // that segment is gone: start at the next older one
      if (it == segs_.begin()) {
        idx_ = 0;
        done_ = true;
        return Status::success();
      }
      idx_ = size_t(it - segs_.begin()) - 1;
      return openSeg(true, 0);
    }
    idx_ = size_t(it - segs_.begin());
    return openSeg(false, off);
  }

  // The next line, newest first; `none` when the conversation's start is reached.
  Status next(Line& out, bool& none) {
    none = false;
    while (true) {
      if (done_) {
        none = true;
        return Status::success();
      }
      if (spanAt_ > 0) {
        const auto& s = spans_[--spanAt_];
        out.data = win_.data() + (s.first - winStart_);
        out.len = s.second - s.first;
        out.seg = segs_[idx_];
        out.start = s.first;
        out.end = s.second;
        return Status::success();
      }
      Status st = refill();
      if (!st.ok()) return st;
    }
  }

  // The cursor of the next line next() would return.
  void pos(uint32_t& seg, uint64_t& off) const {
    if (done_ || segs_.empty()) {
      seg = segs_.empty() ? 1 : segs_.front();
      off = 0;
      return;
    }
    seg = segs_[idx_];
    off = spanAt_ > 0 ? spans_[spanAt_ - 1].second : end_;
  }
  bool atStart() const {
    if (done_ || segs_.empty()) return true;
    return spanAt_ == 0 && end_ == 0 && idx_ == 0;
  }

 private:
  Status openSeg(bool atEnd, uint64_t off) {
    std::string path;
    if (!segmentPath(key_, segs_[idx_], path).ok()) return Status::fail(Err::InvalidArgument, "tx::search.seg");
    size_t n = 0;
    if (!f_.size(path, n)) return Status::fail(Err::Io, "tx::search.size");
    path_ = path;
    end_ = atEnd ? n : std::min<uint64_t>(off, n);
    spans_.clear();
    spanAt_ = 0;
    return Status::success();
  }

  // The next window below end_ (or the next older segment).
  Status refill() {
    spans_.clear();
    spanAt_ = 0;
    if (end_ == 0) {
      if (idx_ == 0) {
        done_ = true;
        return Status::success();
      }
      --idx_;
      return openSeg(true, 0);
    }
    size_t w = 4 * kSliceBytes;   // read in kSliceBytes calls; one window keeps the line overlap small
    while (true) {
      const uint64_t from = end_ > w ? end_ - w : 0;
      if (!readSpan(f_, path_, size_t(from), size_t(end_ - from), win_)) return Status::fail(Err::Io, "tx::search.read");
      winStart_ = from;
      size_t first = 0;
      if (from > 0) {
        const size_t nl = win_.find('\n');
        if (nl == std::string::npos || nl + 1 >= win_.size()) {
          if (w < kWindowMax) {
            w = std::min(w * 2, kWindowMax);
            continue;
          }
          end_ = from;   // no line end in the widest window: not a stored line, nothing to read
          return Status::success();
        }
        first = nl + 1;
      }
      for (size_t at = first; at < win_.size();) {
        const size_t nl = win_.find('\n', at);
        if (nl == std::string::npos) break;   // a torn tail of the open segment
        spans_.push_back({from + at, from + nl + 1});
        at = nl + 1;
      }
      spanAt_ = spans_.size();
      end_ = from + first;
      return Status::success();
    }
  }

  TxFiles& f_;
  std::string key_;
  std::vector<uint32_t> segs_;
  size_t idx_ = 0;
  std::string path_;
  uint64_t end_ = 0;
  bool done_ = false;
  std::string win_;
  uint64_t winStart_ = 0;
  std::vector<std::pair<uint64_t, uint64_t>> spans_;
  size_t spanAt_ = 0;
};

// Every line of one segment from `off`, oldest first, until `cap` bytes were read or `fn` stops.
template <typename Fn>
Status forwardLines(TxFiles& files, const std::string& path, size_t cap, Fn fn) {
  size_t size = 0;
  if (!files.size(path, size)) return Status::fail(Err::Io, "tx::search.size");
  std::string carry, slice;
  size_t at = 0;
  while (at < size && at < cap) {
    const size_t want = std::min(kSliceBytes, size - at);
    if (!readSpan(files, path, at, want, slice)) return Status::fail(Err::Io, "tx::search.read");
    at += want;
    carry += slice;
    size_t from = 0;
    for (size_t nl = carry.find('\n'); nl != std::string::npos; nl = carry.find('\n', from)) {
      if (!fn(carry.data() + from, nl + 1 - from)) return Status::success();
      from = nl + 1;
    }
    carry.erase(0, from);
    if (carry.size() > kMaxLineBytes) carry.clear();   // not a stored line
  }
  return Status::success();
}

bool parseU64(const std::string& s, size_t from, size_t to, uint64_t& v) {
  if (from >= to || to - from > 19) return false;
  v = 0;
  for (size_t i = from; i < to; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + uint64_t(s[i] - '0');
  }
  return true;
}

// ---- positions and cursors ------------------------------------------------------------------
// A position in the range, newest first: "t<seg>.<off>" (the transcript: the lines that end at or
// before off in seg, and every older segment), "o<token>~<k>" (the earlier engine's rows: the store's
// page at token, its first k rows considered), "z" (the start of the range: nothing older).
struct Pos {
  enum Kind : uint8_t { T = 0, O = 1, Z = 2 };
  Kind kind = Z;
  uint32_t seg = 0;
  uint64_t off = 0;
  std::string token;
  size_t skip = 0;
};
bool samePos(const Pos& a, const Pos& b) {
  if (a.kind != b.kind) return false;
  if (a.kind == Pos::T) return a.seg == b.seg && a.off == b.off;
  if (a.kind == Pos::O) return a.token == b.token && a.skip == b.skip;
  return true;
}
std::string posText(const Pos& p) {
  if (p.kind == Pos::T) return "t" + std::to_string(p.seg) + "." + std::to_string(p.off);
  if (p.kind == Pos::O) return "o" + p.token + "~" + std::to_string(p.skip);
  return "z";
}
Pos tPos(uint32_t seg, uint64_t off) {
  Pos p;
  p.kind = Pos::T;
  p.seg = seg;
  p.off = off;
  return p;
}
Pos oPos(const std::string& token, size_t skip) {
  Pos p;
  p.kind = Pos::O;
  p.token = token;
  p.skip = skip;
  return p;
}
bool oldTokenByte(char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || ch == 'm' || ch == ':'; }
bool parseOldPos(const std::string& c, Pos& out) {
  const size_t tilde = c.find('~');
  uint64_t k = 0;
  if (tilde == std::string::npos || !parseU64(c, tilde + 1, c.size(), k) || k > 4096) return false;
  out = oPos(c.substr(1, tilde - 1), size_t(k));
  return out.token.size() <= 32 && std::all_of(out.token.begin(), out.token.end(), oldTokenByte);
}
bool parseTPos(const std::string& c, Pos& out) {
  const size_t dot = c.find('.');
  uint64_t seg = 0, off = 0;
  if (dot == std::string::npos || !parseU64(c, 1, dot, seg) || !parseU64(c, dot + 1, c.size(), off)) return false;
  if (seg < 1 || seg > kMaxSegment) return false;
  out = tPos(uint32_t(seg), off);
  return true;
}
bool parsePos(const std::string& c, Pos& out) {
  if (c == "z") return out = Pos{}, true;
  if (c.empty()) return false;
  if (c[0] == 'o') return parseOldPos(c, out);
  return c[0] == 't' && parseTPos(c, out);
}

// A cursor: "<window start>,<window end>,<rank>"; the end is empty when it is not known yet (the
// next window: read until the window's budget).
struct Cursor {
  bool fresh = true;
  Pos start;
  bool haveEnd = false;
  Pos end;
  uint32_t rank = 0;
};
bool parseCursor(const std::string& c, Cursor& out) {
  out = Cursor{};
  if (c.empty()) return true;
  const size_t a = c.find(','), b = a == std::string::npos ? a : c.find(',', a + 1);
  uint64_t rank = 0;
  if (b == std::string::npos || !parsePos(c.substr(0, a), out.start) ||
      !parseU64(c, b + 1, c.size(), rank) || rank > kSearchRankedMax)
    return false;
  out.fresh = false;
  out.rank = uint32_t(rank);
  out.haveEnd = b > a + 1;
  return !out.haveEnd || parsePos(c.substr(a + 1, b - a - 1), out.end);
}
std::string cursorText(const Pos& start, const Pos* end, uint32_t rank) {
  return posText(start) + "," + (end ? posText(*end) : std::string()) + "," + std::to_string(rank);
}

bool searchedKind(ItemKind k) {
  return k == ItemKind::User || k == ItemKind::Assistant || k == ItemKind::DeviceNote || k == ItemKind::Digest ||
         k == ItemKind::Result;
}
bool ownResult(const std::string& n) { return n == "search" || n == "view"; }

HitSource sourceOf(ItemKind k) {
  switch (k) {
    case ItemKind::User: return HitSource::Person;
    case ItemKind::Assistant: return HitSource::Assistant;
    case ItemKind::DeviceNote: return HitSource::Device;
    case ItemKind::Digest: return HitSource::Summary;
    default: return HitSource::Tool;
  }
}

bool inRange(uint64_t t, const SearchAsk& a) {
  if (!a.fromT && !a.toT) return true;
  if (t == 0) return false;   // no clock: its date is unknown, never inside a range
  return (!a.fromT || t >= a.fromT) && (!a.toT || t <= a.toT);
}

void span(uint64_t t, uint64_t& from, uint64_t& to) {
  if (!t) return;
  if (!from || t < from) from = t;
  if (t > to) to = t;
}

// ---- matching and the excerpt ------------------------------------------------------------------
// Where `w` (lower case) first occurs in [from, to) of `text`, case-insensitive; npos when not.
size_t findFolded(const std::string& text, size_t from, size_t to, const std::string& w) {
  for (size_t i = from; i + w.size() <= to; ++i) {
    size_t j = 0;
    while (j < w.size() && lower(text[i + j]) == w[j]) ++j;
    if (j == w.size()) return i;
  }
  return std::string::npos;
}
// The words as one phrase: each after the one before with only non-word bytes between (at least one).
size_t phraseAt(const std::string& text, const std::vector<std::string>& words) {
  for (size_t at = findFolded(text, 0, text.size(), words[0]); at != std::string::npos;
       at = findFolded(text, at + 1, text.size(), words[0])) {
    size_t j = at + words[0].size();
    bool all = true;
    for (size_t w = 1; w < words.size() && all; ++w) {
      const size_t gap = j;
      while (j < text.size() && !wordByte((unsigned char)text[j])) ++j;
      all = j > gap && findFolded(text, j, std::min(text.size(), j + words[w].size()), words[w]) == j;
      j += words[w].size();
    }
    if (all) return at;
  }
  return std::string::npos;
}
// The line holding the most words (the first such) and where its first word begins.
size_t bestLine(const std::string& text, const std::vector<std::string>& words, size_t& count) {
  size_t best = 0;
  count = 0;
  for (size_t start = 0; start < text.size() && count < words.size();) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    size_t n = 0, first = std::string::npos;
    for (const std::string& w : words) {
      const size_t at = findFolded(text, start, end, w);
      if (at == std::string::npos) continue;
      ++n;
      first = std::min(first, at);
    }
    if (n > count) {
      count = n;
      best = first;
    }
    start = end + 1;
  }
  return best;
}

size_t charStart(const std::string& s, size_t i) {
  while (i > 0 && i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
  return i;
}

// The excerpt around `spot`, on one line, at most `cap` bytes.
void excerptOf(const std::string& text, size_t spot, size_t cap, SearchHit& h) {
  const size_t from = spot > cap / 4 ? charStart(text, spot - cap / 4) : 0;
  size_t to = std::min(text.size(), from + cap);
  if (to < text.size()) to = charStart(text, to);
  h.excerpt.clear();
  for (size_t i = from; i < to; ++i) {
    const char c = text[i];
    h.excerpt += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
  }
  h.cutBefore = from > 0;
  h.cutAfter = to < text.size();
}

// ---- the ranked hits of one window, in bounded memory ------------------------------------------
// The best `keep` matches by (rank, then the order read: newest first), and the dates of every match
// not kept (ranked after them).
struct Ranked {
  size_t keep = 0;
  std::vector<std::pair<uint32_t, SearchHit>> top;   // (the order read, the hit), best first
  uint32_t matches = 0;
  uint64_t restFrom = 0, restTo = 0;
  static bool better(const std::pair<uint32_t, SearchHit>& x, const std::pair<uint32_t, SearchHit>& y) {
    return x.second.rank != y.second.rank ? x.second.rank > y.second.rank : x.first < y.first;
  }
  // A cheap test before the excerpt is cut: would a match of this rank, read in this order, be kept?
  bool wants(MatchRank r, uint32_t order) const {
    if (top.size() < keep) return true;
    if (!keep) return false;
    const auto& worst = top.back();
    return r != worst.second.rank ? r > worst.second.rank : order < worst.first;
  }
  void offer(uint32_t order, SearchHit&& h) {
    ++matches;
    if (!wants(h.rank, order)) {
      span(h.t, restFrom, restTo);
      return;
    }
    std::pair<uint32_t, SearchHit> e(order, std::move(h));
    top.insert(std::upper_bound(top.begin(), top.end(), e, better), std::move(e));
    if (top.size() > keep) {
      span(top.back().second.t, restFrom, restTo);
      top.pop_back();
    }
  }
  void count(uint64_t t) {   // a match too low to be kept: counted and dated only
    ++matches;
    span(t, restFrom, restTo);
  }
};

// The day rows the stored owner messages mirror (u.m), every segment read forward.
Status mirroredOf(TxFiles& files, const std::string& chatKey, std::vector<uint64_t>& out) {
  out.clear();
  std::vector<uint32_t> segs;
  Status st = segmentsOf(files, chatKey, segs);
  Item it;
  for (size_t i = 0; st.ok() && i < segs.size(); ++i) {
    std::string path;
    if (!segmentPath(chatKey, segs[i], path).ok()) continue;
    st = forwardLines(files, path, SIZE_MAX, [&](const char* p, size_t n) {
      LineHeader h;
      if (checkLine(p, n, h).ok() && h.kind == ItemKind::User && decodeLine(p, n, it).ok() && it.m) out.push_back(it.m);
      return true;
    });
  }
  return st;
}

// ---- one window ------------------------------------------------------------------------------
struct Scan {
  const SearchDeps& d;
  TxFiles& files;   // counted into out.card
  const SearchAsk& a;
  const std::vector<std::string>& words;
  SearchOutcome& out;
  Ranked ranked;           // the transcript's matches
  Ranked oldRanked;        // the earlier engine's (kept apart: a /clear row found later drops them all)
  Item it;                 // reused per line
  uint32_t order = 0;      // the order read (the transcript: newest first)
  uint32_t oldOrder = UINT32_MAX;   // the old rows are read oldest first: a later one is newer
  uint64_t consumed = 0;   // what the window has read (lines, payloads, rows): its budget
  bool haveStart = false;  // the window's start is known (a fresh search sets it past the open turn)
  Pos start;
  bool haveEnd = false;
  Pos end;
  std::vector<uint64_t> mirrored;   // the day rows stored owner messages mirror (u.m): in the transcript
  std::vector<uint32_t> taintedTurns;   // turns holding an outside item
  uint32_t oldestTurn = 0;          // the oldest turn the window read a line of
  uint32_t newestTurn = 0;          // ... the newest (a window that starts inside a turn reads part of it)
  bool wholeTranscript = false;     // this window read every line of the transcript (mirrored is whole)
  bool freshStart = true;           // the window starts at the conversation's newest line

  // Does the window end at `p`? Its known end, else its budget (the first call of a window).
  bool endsAt(const Pos& p) {
    if (haveEnd) return samePos(p, end);
    const bool over = consumed >= d.limits.windowBytes || (d.nowMs && out.card.totalMs >= d.limits.cardMs);
    if (over) {
      end = p;
      haveEnd = true;
    }
    return over;
  }
  void at(const Pos& p) {   // the first position a fresh search considers is its window's start
    if (haveStart) return;
    start = p;
    haveStart = true;
  }

  // One match into `into`: kept with its excerpt when it ranks high enough, else counted.
  void match(Ranked& into, uint32_t ord, const std::string& text, SearchHit&& h) {
    size_t spot = 0;
    h.rank = rankOf(text, words, spot);
    if (h.rank == MatchRank::None) return;
    if (!into.wants(h.rank, ord)) return into.count(h.t);
    excerptOf(text, spot, d.limits.excerptBytes, h);
    into.offer(ord, std::move(h));
  }

  // The text of a decoded line: inline, or its bulk payload. `searched` false: a payload gone after 30
  // days (counted); not ok: the card refused.
  Status textOf(bool bulk, std::string& text, bool& searched) {
    searched = true;
    if (!bulk || it.handle.empty()) {
      text = std::move(it.x);
      return Status::success();
    }
    TxLog log(files, d.chatKey);
    const Status st = log.readBulk(it.handle, 0, kBulkMaxBytes, text);
    consumed += text.size();
    if (st.err == Err::NotFound || st.err == Err::InvalidArgument) {
      ++out.unreadBulk;
      searched = false;
      return Status::success();
    }
    return st;
  }

  bool outsideItem() const {
    return d.itemOutside ? d.itemOutside(it) : (it.kind == ItemKind::Result && it.cls == ResultClass::Outside);
  }

  SearchHit hitOfItem() const {
    SearchHit hit;
    hit.id = "r" + std::to_string(it.q);
    hit.t = it.t;
    hit.turn = it.turn;
    hit.source = sourceOf(it.kind);
    hit.hasTz = it.kind == ItemKind::User;
    if (hit.hasTz) hit.tzMinutes = it.tzMinutes;
    if (it.kind == ItemKind::Result) hit.tool = it.n;
    hit.outside = outsideItem();
    return hit;
  }

  // The facts every line gives, matched or not: its date, its turn, whether it makes its turn outside,
  // the day row it mirrors. `decoded`: `it` holds the line.
  void note(const Line& ln, const LineHeader& h, bool& decoded) {
    span(h.t, out.readFromT, out.readToT);
    if (h.turn && (!oldestTurn || h.turn < oldestTurn)) oldestTurn = h.turn;
    if (h.turn > newestTurn) newestTurn = h.turn;
    decoded = false;
    if (h.kind != ItemKind::Result && h.kind != ItemKind::DeviceNote && h.kind != ItemKind::User) return;
    if (!(decoded = decodeLine(ln.data, ln.len, it).ok())) return;
    if (outsideItem()) taintedTurns.push_back(h.turn);
    if (it.kind == ItemKind::User && it.m) mirrored.push_back(it.m);
  }

  Status line(const Line& ln, const LineHeader& h) {
    bool decoded = false;
    note(ln, h, decoded);
    if (!searchedKind(h.kind) || !inRange(h.t, a)) return Status::success();
    // A result's payload, or the rest of an assistant text over the message cap, lives in the bulk file.
    const bool bulk = (h.kind == ItemKind::Result || h.kind == ItemKind::Assistant) &&
                      containsFolded(ln.data, ln.len, "\"hdl\":");
    if (!bulk && !allIn(ln.data, ln.len, words)) return Status::success();   // a word missing: no decode
    if (!decoded && !decodeLine(ln.data, ln.len, it).ok()) return Status::success();
    if (it.kind == ItemKind::Result && ownResult(it.n)) return Status::success();
    SearchHit hit = hitOfItem();   // before textOf takes the item's text
    std::string text;
    bool searched = false;
    const Status st = textOf(bulk, text, searched);
    if (st.ok() && searched) match(ranked, order++, text, std::move(hit));
    return st;
  }

  // One earlier-engine row, read oldest first. `answered`: the owner row this assistant row answers is in
  // the transcript (the import brought both over).
  void oldRow(DayRow& r, bool& answered) {
    span(r.t, out.readFromT, out.readToT);
    if (r.clearBoundary) return clearedBefore(answered);   // the person cleared everything before it
    if (r.ours || (r.role != RowRole::Owner && r.role != RowRole::Assistant) || imported(r, answered)) return;
    if (!hourInRange(r.t)) return;
    SearchHit hit;
    hit.id = rowIdString(r.id);
    hit.t = r.t;
    hit.hourOnly = true;
    hit.source = r.role == RowRole::Owner ? HitSource::OldPerson : HitSource::OldAssistant;
    hit.outside = r.role == RowRole::Assistant || r.photo;   // its class is not known: fail closed
    match(oldRanked, oldOrder--, r.text, std::move(hit));
  }
  void clearedBefore(bool& answered) {
    const size_t keep = oldRanked.keep;
    oldRanked = Ranked{};
    oldRanked.keep = keep;
    answered = false;
  }
  // In the transcript already: an owner row a stored owner message mirrors, and the answer to it.
  bool imported(const DayRow& r, bool& answered) const {
    if (r.role != RowRole::Owner) return answered;
    answered = std::binary_search(mirrored.begin(), mirrored.end(), r.id);
    return answered;
  }
  // A row's time is known to the hour: it is in range when that hour overlaps the range.
  bool hourInRange(uint64_t t) const {
    if (!a.fromT && !a.toT) return true;
    if (!t || (a.fromT && t + 3599 < a.fromT)) return false;
    return !a.toT || t <= a.toT;
  }

  Status transcript(const Pos& from, bool& finished);
  Status step(const Line& ln, const Pos& here);
  Status oldRows(const Pos& from, bool& finished);
  Status clearedLater(uint64_t after);
};

// A date range on a fresh query: the segments that cannot hold it are not read. Lines are stored in
// time order, so a segment whose first line is stamped after `to` holds nothing at or before it, and
// once a segment whose first line is before `from` has been read, every older one is older still.
// Segment 1 is always read (the history import's own items open it, stamped with their rows' times),
// and a first line with no clock decides nothing.
Status firstStamps(TxFiles& files, const std::string& chatKey, const std::vector<uint32_t>& segs,
                   std::vector<uint64_t>& first) {
  first.assign(segs.size(), 0);
  for (size_t i = 1; i < segs.size(); ++i) {
    std::string path;
    if (!segmentPath(chatKey, segs[i], path).ok()) continue;
    const Status st = forwardLines(files, path, kWindowMax, [&](const char* p, size_t n) {
      LineHeader h;
      if (!checkLine(p, n, h).ok()) return true;
      first[i] = h.t;
      return false;
    });
    if (!st.ok()) return st;
  }
  return Status::success();
}
Status rangeSegments(TxFiles& files, const std::string& chatKey, const std::vector<uint32_t>& segs, const SearchAsk& a,
                     uint32_t& startSeg, uint32_t& stopBelow) {
  startSeg = 0;
  stopBelow = 0;
  if (segs.size() < 2) return Status::success();
  std::vector<uint64_t> first;
  const Status st = firstStamps(files, chatKey, segs, first);
  if (!st.ok()) return st;
  size_t start = segs.size() - 1;
  while (a.toT && start > 0 && first[start] && first[start] > a.toT) --start;
  if (start != segs.size() - 1) startSeg = segs[start];
  for (size_t i = start; a.fromT && i > 0 && !stopBelow; --i)
    if (first[i] && first[i] < a.fromT) stopBelow = segs[i];
  return Status::success();
}

// Where the transcript part starts and where a date range stops it: a fresh search (seg 0) from its
// newest line or the newest segment the range can reach; a range's oldest segment on every call.
struct TranscriptPlan {
  std::vector<uint32_t> segs;
  uint32_t seg = 0;
  uint64_t off = 0;
  uint32_t stopBelow = 0;   // segments below this one are older than the range (0: none)
};
Status planTranscript(TxFiles& files, const std::string& chatKey, const SearchAsk& a, const Pos& from,
                      TranscriptPlan& p) {
  p.seg = from.seg;
  p.off = from.off;
  Status st = segmentsOf(files, chatKey, p.segs);
  if (!st.ok() || (!a.toT && !a.fromT)) return st;
  uint32_t newest = 0;
  st = rangeSegments(files, chatKey, p.segs, a, newest, p.stopBelow);
  if (!from.seg && newest) {
    p.seg = newest;
    p.off = UINT64_MAX;   // from the end of that segment
  }
  return st;
}

// The transcript from `from` (T; seg 0: a fresh search). `finished` when its start (or the range's
// oldest segment) was reached inside the window.
Status Scan::transcript(const Pos& from, bool& finished) {
  finished = false;
  TranscriptPlan plan;
  Status st = planTranscript(files, d.chatKey, a, from, plan);
  Backward rd(files, d.chatKey, plan.segs);
  if (st.ok()) st = rd.start(plan.seg, plan.off);
  bool none = false;
  while (st.ok() && !none) {
    uint32_t seg = 0;
    uint64_t off = 0;
    rd.pos(seg, off);
    if (rd.atStart() || (plan.stopBelow && seg < plan.stopBelow)) break;
    const Pos here = tPos(seg, off);
    if (haveStart && endsAt(here)) return Status::success();
    Line ln;
    st = rd.next(ln, none);
    if (st.ok() && !none) st = step(ln, here);
  }
  finished = st.ok();
  wholeTranscript = finished && !from.seg && !a.fromT && !a.toT;
  return st;
}

// One line of the transcript: the open turn's lines are passed before the window starts.
Status Scan::step(const Line& ln, const Pos& here) {
  LineHeader h;
  if (!checkLine(ln.data, ln.len, h).ok()) return Status::success();   // torn or garbage: never stored
  if (!haveStart && a.openTurn && h.turn == a.openTurn) return Status::success();   // the question's own turn
  at(here);
  consumed += ln.len;
  return line(ln, h);
}

// The earlier engine's rows from `from` (O: rows after its token, "" for every row of the chat),
// oldest first. `finished` when the store's newest row was reached.
Status Scan::oldRows(const Pos& from, bool& finished) {
  finished = false;
  out.oldRead = true;
  Status st = wholeTranscript ? Status::success() : mirroredOf(files, d.chatKey, mirrored);
  if (!st.ok()) return st;
  std::sort(mirrored.begin(), mirrored.end());
  uint64_t after = from.token.empty() ? 0 : rowIdNumber(from.token);
  bool answered = std::binary_search(mirrored.begin(), mirrored.end(), after);   // the row before the window
  std::vector<DayRow> rows;
  while (true) {
    bool end = false;
    rows.clear();
    if (!(st = d.old->read(after, kOldPageRows, rows, end, out.card)).ok()) return st;
    for (DayRow& r : rows) {
      if (r.id <= after) return Status::fail(Err::Io, "tx::search.old-order");   // the store went back
      const Pos here = oPos(after ? rowIdString(after) : std::string(), 0);
      at(here);
      if (endsAt(here)) return clearedLater(after);
      consumed += r.text.size() + 64;
      after = r.id;
      oldRow(r, answered);
    }
    if (end || rows.empty()) break;
  }
  finished = true;
  return Status::success();
}

// A window that ends inside the old rows: a /clear row after its end clears every row it read, so the
// rest of the store is looked through for one (rows only, bounded) before its hits are kept.
Status Scan::clearedLater(uint64_t after) {
  std::vector<DayRow> rows;
  uint64_t read = 0;
  bool end = false;
  while (!end) {
    rows.clear();
    const Status st = d.old->read(after, kOldPageRows, rows, end, out.card);
    if (!st.ok()) return st;
    if (rows.empty()) break;
    for (const DayRow& r : rows) {
      if (r.id <= after) return Status::fail(Err::Io, "tx::search.old-order");
      read += r.text.size() + 64;
      after = r.id;
      if (r.clearBoundary) {
        bool answered = false;
        clearedBefore(answered);
        return Status::success();
      }
    }
    if (read > 4 * kSearchWindowBytes) return Status::fail(Err::TooLarge, "tx::search.clear-scan");
  }
  return Status::success();
}

// The window from `from` to its end: the transcript, then the old rows.
Status scanWindow(Scan& scan, const Pos& from) {
  Pos p = from;
  bool finished = false;
  Status st;
  if (p.kind == Pos::T) {
    st = scan.transcript(p, finished);
    if (!st.ok() || !finished) return st;
    p = scan.d.old ? oPos(std::string(), 0) : Pos{};
  }
  if (p.kind == Pos::O && scan.d.old) {
    st = scan.oldRows(p, finished);
    if (!st.ok() || !finished) return st;
  }
  scan.at(Pos{});
  scan.end = Pos{};   // the start of the range: the window is complete
  scan.haveEnd = true;
  return Status::success();
}

// Both parts' matches as one ranking: by rank, the transcript before the old rows among equals (it is
// newer), each part in its own order.
void merge(Ranked& t, Ranked& o, Ranked& into) {
  into.matches = t.matches + o.matches;
  into.restFrom = t.restFrom;
  into.restTo = t.restTo;
  span(o.restFrom, into.restFrom, into.restTo);
  span(o.restTo, into.restFrom, into.restTo);
  size_t i = 0, j = 0;
  while (i < t.top.size() || j < o.top.size()) {
    const bool takeT = j >= o.top.size() || (i < t.top.size() && t.top[i].second.rank >= o.top[j].second.rank);
    auto& e = takeT ? t.top[i++] : o.top[j++];
    if (into.top.size() < into.keep) into.top.push_back(std::move(e));
    else span(e.second.t, into.restFrom, into.restTo);
  }
}

// An assistant text of a turn that holds outside content is outside; a turn the window did not read
// whole (its oldest, when the window stopped inside the range) counts as one.
void markTaintedReplies(Scan& scan, std::vector<SearchHit>& hits) {
  std::sort(scan.taintedTurns.begin(), scan.taintedTurns.end());
  for (SearchHit& h : hits) {
    if (h.source != HitSource::Assistant || h.outside) continue;
    const bool partial = (scan.end.kind != Pos::Z && h.turn == scan.oldestTurn) ||   // its older lines unread
                         (!scan.freshStart && h.turn == scan.newestTurn);              // its newer lines unread
    h.outside = std::binary_search(scan.taintedTurns.begin(), scan.taintedTurns.end(), h.turn) || partial;
  }
}

// The returned page of a scanned window: ranks [rank, rank + maxHits), the count and dates of the
// rest, and the next cursor.
void finish(Scan& scan, uint32_t rank) {
  SearchOutcome& out = scan.out;
  Ranked r;
  r.keep = scan.ranked.keep;
  merge(scan.ranked, scan.oldRanked, r);
  out.matches = r.matches;
  out.rankFrom = rank;
  for (size_t i = rank; i < r.top.size(); ++i) out.hits.push_back(std::move(r.top[i].second));
  markTaintedReplies(scan, out.hits);
  const uint32_t through = rank + uint32_t(out.hits.size());
  out.further = r.matches > through ? r.matches - through : 0;
  out.furtherFromT = r.restFrom;
  out.furtherToT = r.restTo;
  out.complete = scan.end.kind == Pos::Z;
  if (out.further && through < scan.d.limits.rankedMax) out.next = cursorText(scan.start, &scan.end, through);
  else if (!out.complete) out.next = cursorText(scan.end, nullptr, 0);
}

}  // namespace

Status RowSourceRows::read(uint64_t after, size_t max, std::vector<DayRow>& out, bool& end, ReadStats& stats) {
  const size_t badBefore = unreadable_ ? unreadable_() : 0;
  const uint32_t t0 = now_ ? now_() : 0;
  const Status st = src_.read(after, 0, max, out, end);
  uint64_t bytes = 0;
  for (const DayRow& r : out) bytes += r.text.size();
  stats.add(now_ ? now_() - t0 : 0, bytes);
  if (!st.ok()) return st;
  if (unreadable_ && unreadable_() != badBefore) return Status::fail(Err::Io, "tx::RowSourceRows.unreadable");
  return Status::success();
}

bool ReadOnlyCounted::readAt(const std::string& path, size_t offset, char* buf, size_t want, size_t& got) {
  const uint32_t t0 = now_ ? now_() : 0;
  const bool ok = in_.readAt(path, offset, buf, want, got);
  st_.add(now_ ? now_() - t0 : 0, ok ? got : 0);
  return ok;
}

std::vector<std::string> queryWords(const std::string& query) {
  std::vector<std::string> out;
  std::string w;
  auto flush = [&] {
    if (!w.empty() && std::find(out.begin(), out.end(), w) == out.end()) out.push_back(w);
    w.clear();
  };
  for (char c : query) {
    if (wordByte((unsigned char)c)) w += lower(c);
    else flush();
  }
  flush();
  return out;
}

bool matchesAll(const std::string& text, const std::vector<std::string>& words) {
  return allIn(text.data(), text.size(), words);
}

bool matchesNear(const std::string& text, const std::vector<std::string>& words) {
  if (words.empty()) return false;
  size_t prev = 0;   // the start of the line before the current one
  for (size_t start = 0; start <= text.size();) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    if (allIn(text.data() + prev, end - prev, words)) return true;   // this line with the one before it
    if (end == text.size()) return false;
    prev = start;
    start = end + 1;
  }
  return false;
}

bool cursorOk(const std::string& cursor) {
  Cursor c;
  return parseCursor(cursor, c);
}

MatchRank rankOf(const std::string& text, const std::vector<std::string>& words, size_t& spot) {
  spot = 0;
  if (words.empty() || !allIn(text.data(), text.size(), words)) return MatchRank::None;
  const size_t phrase = phraseAt(text, words);
  if (phrase != std::string::npos) {
    spot = phrase;
    return MatchRank::Phrase;
  }
  size_t count = 0;
  spot = bestLine(text, words, count);
  if (count == words.size()) return MatchRank::Line;
  return matchesNear(text, words) ? MatchRank::Near : MatchRank::None;
}

Status search(const SearchDeps& d, const SearchAsk& a, SearchOutcome& out) {
  out = SearchOutcome{};
  Cursor c;
  if (!d.files || !chatKeyOk(d.chatKey) || !parseCursor(a.before, c) || (!c.fresh && c.start.kind == Pos::Z))
    return Status::fail(Err::InvalidArgument, "tx::search");
  const std::vector<std::string> words = queryWords(a.query);
  if (words.empty()) return Status::success();
  try {
    ReadOnlyCounted files(*d.files, d.nowMs, out.card);
    Scan scan{d, files, a, words, out, Ranked{}, Ranked{}, Item{}};
    scan.ranked.keep = std::min<size_t>(c.rank + d.limits.maxHits, d.limits.rankedMax + d.limits.maxHits);
    scan.oldRanked.keep = scan.ranked.keep;
    if (!c.fresh) {
      scan.freshStart = false;
      scan.start = c.start;
      scan.haveStart = true;
      scan.haveEnd = c.haveEnd;
      scan.end = c.end;
    }
    const Pos from = c.fresh ? tPos(0, 0) : c.start;
    const Status st = (from.kind == Pos::O && !d.old) ? Status::success() : scanWindow(scan, from);
    if (!st.ok()) {
      out.next = a.before;   // the same call again: nothing of a window is returned half-ranked
      return st;
    }
    finish(scan, c.rank);
    return Status::success();
  } catch (const std::bad_alloc&) {
    return Status::fail(Err::TooLarge, "tx::search.memory");
  }
}

Status verbatimFloor(TxFiles& files, const std::string& chatKey, uint64_t& floor) {
  floor = 0;
  std::vector<uint32_t> segs;
  Status s = segmentsOf(files, chatKey, segs);
  if (!s.ok() || segs.empty()) return s;
  std::string path;
  if (!segmentPath(chatKey, segs.front(), path).ok()) return Status::fail(Err::InvalidArgument, "tx::verbatimFloor");
  Item it;
  return forwardLines(files, path, kVerbatimFloorScan, [&](const char* p, size_t n) {
    LineHeader h;
    if (!checkLine(p, n, h).ok() || h.kind != ItemKind::User) return true;
    if (!decodeLine(p, n, it).ok() || it.m == 0) return true;
    floor = it.m;
    return false;
  });
}

namespace {

// The q of a segment's first whole line (0: none).
Status firstQ(TxFiles& files, const std::string& path, uint64_t& q) {
  q = 0;
  return forwardLines(files, path, kWindowMax, [&](const char* p, size_t n) {
    LineHeader h;
    if (!checkLine(p, n, h).ok()) return true;
    q = h.q;
    return false;
  });
}

// The index of the last segment whose first line is at or below q (segments roll in q order); segs.size()
// when none is.
Status segmentOf(TxFiles& files, const std::string& chatKey, const std::vector<uint32_t>& segs, uint64_t q,
                 size_t& pick) {
  pick = segs.size();
  for (size_t i = 0; i < segs.size(); ++i) {
    std::string path;
    uint64_t first = 0;
    if (!segmentPath(chatKey, segs[i], path).ok()) continue;
    const Status st = firstQ(files, path, first);
    if (!st.ok()) return st;
    if (first > q) break;
    if (first) pick = i;
  }
  return Status::success();
}

// "b/<turn>.txb@<off>+<len>": the payload's length (0 when the handle has none).
uint64_t handleLen(const std::string& h) {
  const size_t plus = h.rfind('+');
  uint64_t v = 0;
  return plus != std::string::npos && parseU64(h, plus + 1, h.size(), v) ? v : 0;
}

// The owner message `ln` carries the photo `ref` (decoded into `owner`).
bool carriesPhoto(const Line& ln, const std::string& quoted, const std::string& ref, Item& owner) {
  LineHeader h;
  if (!checkLine(ln.data, ln.len, h).ok() || h.kind != ItemKind::User) return false;
  if (!containsFolded(ln.data, ln.len, quoted) || !decodeLine(ln.data, ln.len, owner).ok()) return false;
  return std::any_of(owner.parts.begin(), owner.parts.end(),
                     [&](const Part& p) { return p.kind == PartKind::Image && p.ref == ref; });
}

}  // namespace

Status findItem(TxFiles& files, const std::string& chatKey, uint64_t q, Item& out) {
  if (!chatKeyOk(chatKey) || q == 0) return Status::fail(Err::InvalidArgument, "tx::findItem");
  std::vector<uint32_t> segs;
  size_t pick = 0;
  Status st = segmentsOf(files, chatKey, segs);
  if (st.ok()) st = segmentOf(files, chatKey, segs, q, pick);
  if (!st.ok()) return st;
  bool found = false;
  for (size_t i = pick; i < segs.size() && !found && st.ok(); ++i) {
    std::string path;
    if (!segmentPath(chatKey, segs[i], path).ok()) continue;
    st = forwardLines(files, path, SIZE_MAX, [&](const char* p, size_t n) {
      LineHeader h;
      if (!checkLine(p, n, h).ok() || h.q != q) return true;
      found = decodeLine(p, n, out).ok();
      return !found;
    });
  }
  if (!st.ok()) return st;
  return found ? Status::success() : Status::fail(Err::NotFound, "tx::findItem");
}

Status itemText(TxFiles& files, const std::string& chatKey, const Item& it, const TextPage& page, std::string& out,
                size_t& total) {
  out.clear();
  total = 0;
  if (!it.handle.empty() && (it.kind == ItemKind::Result || it.kind == ItemKind::Assistant)) {
    total = size_t(handleLen(it.handle));
    TxLog log(files, chatKey);
    return log.readBulk(it.handle, page.offset, page.limit, out);
  }
  const std::string& whole = it.kind == ItemKind::Call ? it.args : it.x;
  total = whole.size();
  if (page.offset < whole.size()) out = whole.substr(page.offset, page.limit);
  return Status::success();
}

Status findPhoto(TxFiles& files, const std::string& chatKey, const std::string& ref, Item& owner, bool& found,
                 PhotoScan& scan) {
  found = false;
  if (!chatKeyOk(chatKey) || ref.size() < 2 || ref[0] != 'p') return Status::fail(Err::InvalidArgument, "tx::findPhoto");
  try {
    ReadStats stats;
    ReadOnlyCounted counted(files, scan.nowMs, stats);
    std::vector<uint32_t> segs;
    Status st = segmentsOf(counted, chatKey, segs);
    Backward rd(counted, chatKey, segs);
    if (st.ok()) st = rd.start(0, 0);
    const std::string quoted = "\"" + ref + "\"";
    bool none = false;
    uint64_t newest = 0;
    while (st.ok() && !found) {
      if (stats.bytes > kPhotoScanBytes || (scan.nowMs && stats.totalMs > scan.cardMs))
        return Status::fail(Err::TooLarge, "tx::findPhoto.budget");
      Line ln;
      st = rd.next(ln, none);
      if (!st.ok() || none) break;
      LineHeader h;
      if (checkLine(ln.data, ln.len, h).ok()) span(h.t, scan.readFromT, newest);
      found = carriesPhoto(ln, quoted, ref, owner);
    }
    return st;
  } catch (const std::bad_alloc&) {
    return Status::fail(Err::TooLarge, "tx::findPhoto.memory");
  }
}

namespace {
// One line read back while looking for turn `turn`: `past` once a line older than the turn follows it.
bool lineOfTurnOutside(const Line& ln, uint32_t turn, const std::function<bool(const Item& it)>& outside, Item& it,
                       bool& seen, bool& past) {
  LineHeader h;
  if (!checkLine(ln.data, ln.len, h).ok() || (h.turn != turn && !seen)) return false;
  past = h.turn != turn;
  seen = true;
  if (past || (h.kind != ItemKind::Result && h.kind != ItemKind::DeviceNote && h.kind != ItemKind::User)) return false;
  return decodeLine(ln.data, ln.len, it).ok() && outside(it);
}
}  // namespace

Status turnHoldsOutside(TxFiles& files, const std::string& chatKey, uint32_t turn,
                        const std::function<bool(const Item& it)>& outside, bool& holds) {
  holds = false;
  if (!chatKeyOk(chatKey) || !turn || !outside) return Status::fail(Err::InvalidArgument, "tx::turnHoldsOutside");
  try {
    std::vector<uint32_t> segs;
    Status st = segmentsOf(files, chatKey, segs);
    Backward rd(files, chatKey, segs);
    if (st.ok()) st = rd.start(0, 0);
    Item it;
    bool none = false, seen = false, past = false;
    while (st.ok() && !holds && !past) {
      Line ln;
      st = rd.next(ln, none);
      if (!st.ok() || none) break;
      holds = lineOfTurnOutside(ln, turn, outside, it, seen, past);
    }
    return st;
  } catch (const std::bad_alloc&) {
    return Status::fail(Err::TooLarge, "tx::turnHoldsOutside.memory");
  }
}

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
