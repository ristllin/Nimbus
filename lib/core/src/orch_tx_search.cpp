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

// "t<seg>.<off>" -> the transcript part; "o" or "o<store cursor>[~<k>]" -> the old rows.
struct Cursor {
  bool old = false;
  uint32_t seg = 0;
  uint64_t off = 0;
  std::string token;
  size_t skip = 0;   // rows of the old page at `token` already considered
};
bool oldTokenByte(char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || ch == 'm' || ch == ':'; }
bool parseOldCursor(const std::string& c, Cursor& out) {
  out.old = true;
  out.token = c.substr(1);
  const size_t tilde = out.token.find('~');
  if (tilde != std::string::npos) {
    uint64_t k = 0;
    if (!parseU64(out.token, tilde + 1, out.token.size(), k) || k == 0 || k > 4096) return false;
    out.skip = size_t(k);
    out.token.resize(tilde);
  }
  return out.token.size() <= 32 && std::all_of(out.token.begin(), out.token.end(), oldTokenByte);
}
bool parseTranscriptCursor(const std::string& c, Cursor& out) {
  const size_t dot = c.find('.');
  uint64_t seg = 0, off = 0;
  if (dot == std::string::npos || !parseU64(c, 1, dot, seg) || !parseU64(c, dot + 1, c.size(), off)) return false;
  if (seg < 1 || seg > kMaxSegment) return false;
  out.seg = uint32_t(seg);
  out.off = off;
  return true;
}
bool parseCursor(const std::string& c, Cursor& out) {
  out = Cursor{};
  if (c.empty()) return true;
  if (c[0] == 'o') return parseOldCursor(c, out);
  return c[0] == 't' && parseTranscriptCursor(c, out);
}
std::string tCursor(uint32_t seg, uint64_t off) { return "t" + std::to_string(seg) + "." + std::to_string(off); }

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

void noteOldest(uint64_t t, SearchOutcome& out) {
  if (t && (!out.oldestT || t < out.oldestT)) out.oldestT = t;
}

// One search call: what it reads, what it asks and what it has found so far.
struct Scan {
  const SearchDeps& d;
  TxFiles& files;   // counted into out.card
  const SearchAsk& a;
  const std::vector<std::string>& words;
  SearchOutcome& out;
  Item it;   // reused per line

  bool full() const { return out.hits.size() >= d.limits.maxHits; }
  bool overBudget() const {
    if (out.card.bytes >= d.limits.cardBytes) return true;
    return d.nowMs && out.card.totalMs >= d.limits.cardMs;
  }
  // Stop here: the next call starts at `next`.
  Status pause(std::string next) {
    out.more = true;
    out.next = std::move(next);
    return Status::success();
  }
  Status stopAt(std::string next, const Status& st) {
    out.next = std::move(next);
    return st;
  }

  // Whether the header lets the line be a hit at all (kind, the open turn, the dates).
  bool headerPasses(const LineHeader& h) {
    if (!searchedKind(h.kind) || (a.openTurn && h.turn == a.openTurn)) return false;
    noteOldest(h.t, out);
    return inRange(h.t, a);
  }

  // The text of a decoded line: inline, or its bulk payload. `searched` false: nothing to search (a
  // payload gone after 30 days is counted); not ok: the card refused (the call stops at this line).
  Status textOf(bool bulk, std::string& text, bool& searched) {
    searched = true;
    if (!bulk || it.handle.empty()) {
      text = std::move(it.x);
      return Status::success();
    }
    TxLog log(files, d.chatKey);
    const Status st = log.readBulk(it.handle, 0, kBulkMaxBytes, text);
    if (st.err == Err::NotFound || st.err == Err::InvalidArgument) {
      ++out.unreadBulk;
      searched = false;
      return Status::success();
    }
    return st;
  }

  void addHit(std::string text) {
    SearchHit hit;
    hit.id = "r" + std::to_string(it.q);
    hit.t = it.t;
    hit.source = sourceOf(it.kind);
    hit.hasTz = it.kind == ItemKind::User;
    if (hit.hasTz) hit.tzMinutes = it.tzMinutes;
    if (it.kind == ItemKind::Result) {
      hit.tool = it.n;
      hit.outside = it.cls == ResultClass::Outside;
    }
    hit.text = std::move(text);
    out.hits.push_back(std::move(hit));
  }

  // One line of the transcript considered; a hit is added to out.hits.
  Status line(const Line& ln) {
    LineHeader h;
    if (!checkLine(ln.data, ln.len, h).ok() || !headerPasses(h)) return Status::success();   // torn: never stored
    // A result's payload, or the rest of an assistant text over the message cap, lives in the bulk file.
    const bool bulk = (h.kind == ItemKind::Result || h.kind == ItemKind::Assistant) &&
                      containsFolded(ln.data, ln.len, "\"hdl\":");
    if (!bulk && !allIn(ln.data, ln.len, words)) return Status::success();   // a word missing: no decode
    if (!decodeLine(ln.data, ln.len, it).ok()) return Status::success();
    if (it.kind == ItemKind::Result && ownResult(it.n)) return Status::success();
    std::string text;
    bool searched = false;
    const Status st = textOf(bulk, text, searched);
    if (st.ok() && searched && matchesAll(text, words)) addHit(std::move(text));
    return st;
  }

  Status transcript(const Cursor& c, bool& finished);
  Status oldRows(std::string token, size_t skip);
  Status oldPage(OldPage& page, size_t skip, const std::string& token, bool& stopped);
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

// Where a transcript read starts: the segments, the first line's cursor, and the range's oldest segment.
struct StartPlan {
  std::vector<uint32_t> segs;
  uint32_t seg = 0;         // 0: the newest line
  uint64_t off = 0;
  uint32_t stopBelow = 0;   // segments below this one are older than the range (0: none)
};
Status planStart(TxFiles& files, const std::string& chatKey, const SearchAsk& a, const Cursor& c, StartPlan& p) {
  p.seg = c.seg;
  p.off = c.off;
  Status st = segmentsOf(files, chatKey, p.segs);
  if (!st.ok() || c.seg || (!a.toT && !a.fromT)) return st;
  st = rangeSegments(files, chatKey, p.segs, a, p.seg, p.stopBelow);
  if (p.seg) p.off = UINT64_MAX;   // from the end of the first segment the range can reach
  return st;
}

// The transcript part. `finished` when its start (or the range's oldest segment) was reached.
Status Scan::transcript(const Cursor& c, bool& finished) {
  finished = false;
  StartPlan plan;
  Status st = planStart(files, d.chatKey, a, c, plan);
  Backward rd(files, d.chatKey, plan.segs);
  if (st.ok()) st = rd.start(plan.seg, plan.off);
  if (!st.ok()) return stopAt(c.seg ? tCursor(c.seg, c.off) : std::string(), st);
  while (true) {
    uint32_t seg = 0;
    uint64_t off = 0;
    rd.pos(seg, off);
    if (rd.atStart() || (plan.stopBelow && seg < plan.stopBelow)) break;
    if (full() || overBudget()) return pause(tCursor(seg, off));   // a line's own bulk read counts too
    Line ln;
    bool none = false;
    st = rd.next(ln, none);
    if (st.ok() && none) break;
    if (st.ok()) st = line(ln);
    if (!st.ok()) return stopAt(tCursor(seg, off), st);   // this line is read again by the next call
  }
  finished = true;
  return Status::success();
}

bool oldConversation(const DayRow& r) {
  return !r.ours && (r.role == RowRole::Owner || r.role == RowRole::Assistant);
}

// "<store cursor>~<k>": the page at that cursor with its first k rows already considered. The store
// answers the same page for the same cursor, so a stop inside a page loses no row and repeats none,
// whatever order the store keeps inside it.
std::string oldCursor(const std::string& token, size_t skip) {
  return "o" + token + (skip ? "~" + std::to_string(skip) : std::string());
}

// The rows of one page from `skip`; `stopped` when the hits filled up inside it.
Status Scan::oldPage(OldPage& page, size_t skip, const std::string& token, bool& stopped) {
  stopped = false;
  for (size_t i = skip; i < page.rows.size(); ++i) {
    if (full()) {   // the rest of this page stays for the next call
      stopped = true;
      return pause(oldCursor(token, i));
    }
    DayRow& r = page.rows[i];
    noteOldest(r.t, out);
    if (!oldConversation(r) || !inRange(r.t, a) || !matchesAll(r.text, words)) continue;
    SearchHit hit;
    hit.id = rowIdString(r.id);
    hit.t = r.t;
    hit.source = r.role == RowRole::Owner ? HitSource::OldPerson : HitSource::OldAssistant;
    hit.text = std::move(r.text);
    out.hits.push_back(std::move(hit));
  }
  return Status::success();
}

// The earlier engine's rows, from `token` (the store's cursor: rows strictly older), the first
// `skip` rows of its page already considered.
Status Scan::oldRows(std::string token, size_t skip) {
  out.oldRead = true;
  OldPage page;
  while (true) {
    if (full() || overBudget()) return pause(oldCursor(token, skip));
    OldPageAsk ask;
    ask.before = token;
    ask.fromT = a.fromT;
    ask.toT = a.toT;
    page = OldPage{};
    Status st = d.old->page(ask, page, out.card);
    if (!st.ok()) return stopAt(oldCursor(token, skip), st);
    bool stopped = false;
    st = oldPage(page, skip, token, stopped);
    if (stopped || !page.older) return st;
    if (page.next.empty() || page.next == ask.before) return Status::fail(Err::Io, "tx::search.old-cursor");
    token = page.next;
    skip = 0;
  }
}

// The cursor the old part starts from: the import's verbatim floor (rows below it only).
Status floorCursor(TxFiles& files, const std::string& chatKey, std::string& token) {
  uint64_t floor = 0;
  const Status st = verbatimFloor(files, chatKey, floor);
  token = floor ? rowIdString(floor) : std::string();
  return st;
}

}  // namespace

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

bool cursorOk(const std::string& cursor) {
  Cursor c;
  return parseCursor(cursor, c);
}

Status search(const SearchDeps& d, const SearchAsk& a, SearchOutcome& out) {
  out = SearchOutcome{};
  Cursor c;
  if (!d.files || !chatKeyOk(d.chatKey) || !parseCursor(a.before, c))
    return Status::fail(Err::InvalidArgument, "tx::search");
  const std::vector<std::string> words = queryWords(a.query);
  if (words.empty()) return Status::success();
  try {
    ReadOnlyCounted files(*d.files, d.nowMs, out.card);
    Scan scan{d, files, a, words, out, Item{}};
    if (!c.old) {
      bool finished = false;
      Status st = scan.transcript(c, finished);
      if (!st.ok() || !finished || !d.old) return st;
      st = floorCursor(files, d.chatKey, c.token);
      if (!st.ok()) return scan.stopAt(tCursor(1, 0), st);   // the next call reads the start and looks again
    }
    if (!d.old) return Status::success();   // an old cursor with no old rows: nothing below
    return scan.oldRows(c.token, c.skip);
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

Status findPhoto(TxFiles& files, const std::string& chatKey, const std::string& ref, Item& owner, bool& found) {
  found = false;
  if (!chatKeyOk(chatKey) || ref.size() < 2 || ref[0] != 'p') return Status::fail(Err::InvalidArgument, "tx::findPhoto");
  try {
    ReadStats stats;
    ReadOnlyCounted counted(files, nullptr, stats);
    std::vector<uint32_t> segs;
    Status st = segmentsOf(counted, chatKey, segs);
    Backward rd(counted, chatKey, segs);
    if (st.ok()) st = rd.start(0, 0);
    const std::string quoted = "\"" + ref + "\"";
    bool none = false;
    while (st.ok() && !found) {
      if (stats.bytes > kPhotoScanBytes) return Status::fail(Err::TooLarge, "tx::findPhoto.budget");
      Line ln;
      st = rd.next(ln, none);
      if (!st.ok() || none) break;
      found = carriesPhoto(ln, quoted, ref, owner);
    }
    return st;
  } catch (const std::bad_alloc&) {
    return Status::fail(Err::TooLarge, "tx::findPhoto.memory");
  }
}

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
