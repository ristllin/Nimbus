// orch_file_aids - the streamed edit with its line pick, the append facts and the shared
// hash rules (file_aids.h).
//
// The edit reads the file twice in kAidChunk reads: pass 1 finds every match and decides;
// pass 2 finds them again, writes the new bytes to the part file in kAidChunk writes and
// counts old in what it wrote (the "still at" lines). The part is swapped in only when
// pass 2 saw the same file as pass 1. Working memory: two match windows (old + 2 + one
// chunk each) and one output chunk, all from workingAlloc.
#include <cstdio>
#include <cstring>

#include "nimbus/orch/file_aids.h"
#include "nimbus/orch/psram_alloc.h"

namespace nimbus::orch::aids {

namespace {

// Model-facing templates of the edit (tool results; ruling 42's edit_file line).
namespace T {
constexpr const char* kNotIn = "old is not in ";
constexpr const char* kManyA = "old is in ";
constexpr const char* kManyB = "; line picks one, all=true replaces all";
constexpr const char* kNoMatchOn = "no match on line ";
constexpr const char* kPartOf = "old is part of line ";
constexpr const char* kChangedSince = " changed since sha8=";
constexpr const char* kChanged = " changed during the edit; nothing written";
constexpr const char* kCardRead = "card read failed";
constexpr const char* kCardWrite = "card write failed; nothing changed";
constexpr const char* kFence = "; added a newline before the text";
}  // namespace T

std::string num(uint64_t v) { return std::to_string(v); }

size_t countNl(std::string_view s) {
  size_t n = 0;
  for (char c : s) n += c == '\n';
  return n;
}

std::string quoteCut(std::string_view s, size_t n) {
  if (s.size() <= n) return std::string(s);
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  return std::string(s.substr(0, n)) + " ...";
}

void listAdd(LineList& l, uint32_t line) {
  ++l.count;
  if (l.lines.size() < kAidListMax) l.lines.push_back(line);
}

// What a match looks like where it was found.
struct Hit {
  uint64_t line;     // the line it starts on
  bool lineStart;    // it starts at the start of that line
  bool whole;        // and ends at a line end or the end of the file
  size_t lineEnd;    // the line end right after it (0, 1 "\n" or 2 "\r\n") when whole
};

// A streaming non-overlapping matcher over a window of carry + one chunk (Horspool).
// run() settles every byte that can no longer start a match and reports each match;
// onMatch returns true to replace it (the caller emits the replacement).
struct Finder {
  std::string_view pat;
  size_t skip[256];
  char* w = nullptr;
  size_t cap = 0, len = 0;
  char prev = '\n';      // the byte before w[0]; '\n' at the start of the file
  uint64_t line = 1;     // the line w[0] is on

  bool init(std::string_view p) {
    pat = p;
    cap = p.size() + 2 + kAidChunk;
    w = static_cast<char*>(workingAlloc(cap));
    for (size_t& s : skip) s = p.size();
    for (size_t i = 0; i + 1 < p.size(); ++i) skip[static_cast<unsigned char>(p[i])] = p.size() - 1 - i;
    return w != nullptr;
  }
  ~Finder() { workingFree(w); }
  void reset() {
    len = 0;
    prev = '\n';
    line = 1;
  }
  char* room() { return w + len; }
  size_t roomSize() const { return cap - len; }

  Hit describe(size_t q, bool eof) const {
    Hit h{line, (q == 0 ? prev : w[q - 1]) == '\n', false, 0};
    const size_t e = q + pat.size();
    if (!h.lineStart) return h;
    if (pat.back() == '\n') h.whole = true;
    else if (e == len && eof) h.whole = true;
    else if (e < len && w[e] == '\n') h.whole = true, h.lineEnd = 1;
    else if (e < len && w[e] == '\r' && e + 1 < len && w[e + 1] == '\n') h.whole = true, h.lineEnd = 2;
    else if (e < len && w[e] == '\r' && e + 1 == len && eof) h.whole = true, h.lineEnd = 1;
    return h;
  }

  // Settle w[a..b): onBytes sees them and the line moves on.
  template <class Bytes>
  void settle(size_t a, size_t b, Bytes& onBytes) {
    if (b <= a) return;
    onBytes(w + a, b - a);
    for (size_t i = a; i < b; ++i) line += w[i] == '\n';
  }

  template <class Bytes, class Match>
  void run(bool eof, Bytes& onBytes, Match& onMatch) {
    const size_t m = pat.size();
    const size_t lim = eof ? len : (len >= 2 ? len - 2 : 0);
    size_t p = 0, q = 0;
    while (q + m <= lim) {
      if (std::memcmp(w + q, pat.data(), m) != 0) {
        q += skip[static_cast<unsigned char>(w[q + m - 1])];
        continue;
      }
      settle(p, q, onBytes);
      const Hit h = describe(q, eof);
      if (!onMatch(h)) onBytes(w + q, m);
      line += countNl(pat);
      p = q = q + m;
    }
    const size_t keep = eof ? len : std::min(q, len);
    settle(p, keep, onBytes);
    if (keep) prev = w[keep - 1];
    std::memmove(w, w + keep, len - keep);
    len -= keep;
  }
};

// Read the whole file through the finder: feed(chunk) for every chunk read (before
// matching), then run. False: a card fault. bytes and hash cover what was read.
template <class Feed, class Bytes, class Match>
bool pass(AidSource& src, Finder& f, uint64_t& bytes, uint64_t& hash, Feed& feed, Bytes& onBytes, Match& onMatch) {
  f.reset();
  bytes = 0;
  hash = kFnv64Basis;
  for (;;) {
    size_t got = 0;
    if (!src.readAt(bytes, kAidChunk, f.room(), got)) return false;
    feed(f.room(), got);
    hash = fnv64(hash, std::string_view(f.room(), got));
    f.len += got;
    bytes += got;
    const bool eof = got < kAidChunk;
    f.run(eof, onBytes, onMatch);
    if (eof) return true;
  }
}

bool isHex8(const std::string& s) {
  if (s.size() != 8) return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

// The part file writer: kAidChunk writes, each written chunk counted for the result.
struct Out {
  AidSink& sink;
  Finder& count;   // old in the new file
  char* buf = nullptr;
  size_t len = 0;
  bool first = true, failed = false;
  uint64_t bytes = 0, hash = kFnv64Basis, lines = 1;
  char last = '\n';
  LineList stillAt;
  uint64_t outLine() const { return lines; }

  bool flush(bool eof) {
    if (failed) return false;
    if (len || first) {
      if (!sink.partWrite(std::string_view(buf, len), first)) {
        failed = true;
        return false;
      }
      first = false;
    }
    std::memcpy(count.room(), buf, len);
    count.len += len;
    len = 0;
    auto none = [](const char*, size_t) {};
    auto seen = [&](const Hit& h) {
      listAdd(stillAt, uint32_t(h.line));
      return false;
    };
    count.run(eof, none, seen);
    return true;
  }
  void put(const char* p, size_t n) {
    while (n && !failed) {
      const size_t k = std::min(n, kAidChunk - len);
      std::memcpy(buf + len, p, k);
      hash = fnv64(hash, std::string_view(p, k));
      for (size_t i = 0; i < k; ++i) lines += p[i] == '\n';
      bytes += k;
      last = p[k - 1];
      len += k;
      p += k;
      n -= k;
      if (len == kAidChunk) flush(false);
    }
  }
};

}  // namespace

uint64_t fnv64(uint64_t h, std::string_view bytes) {
  for (unsigned char c : bytes) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  return h;
}

std::string sha8Hex(uint64_t hash) {
  if (!hash) return "unknown";
  char b[12];
  std::snprintf(b, sizeof b, "%08llx", static_cast<unsigned long long>(hash >> 32));
  return b;
}

// ---- edit_file ----------------------------------------------------------------------
std::string checkEdit(const EditRequest& e) {
  if (e.oldText.empty()) return "old is empty";
  if (e.oldText == e.newText) return "old and new are the same";
  if (e.all && e.line) return "line picks one match and all=true replaces every match; not both";
  if (!e.ifSha8.empty() && !isHex8(e.ifSha8)) return "if_sha8 is 8 hex digits";
  return "";
}

namespace {

// What pass 1 learned.
struct Plan {
  size_t count = 0;
  LineList lines;          // start lines of the matches
  long pick = -1;          // the match a line pick replaces
  bool onLine = false;     // some match starts on the picked line
  std::string quote;       // the picked line's text (for the part-of error)
  uint64_t bytes = 0, hash = 0, fileLines = 0;
  uint64_t pickLine = 0;
};

bool takesLineEnd(const EditRequest& e) {
  const char c = e.oldText.front();
  return e.newText.empty() && e.oldText.back() != '\n' && c != '\n' && c != '\r';
}

std::string planError(const std::string& path, const EditRequest& e, const Plan& p) {
  if (!e.ifSha8.empty() && sha8Hex(p.hash) != e.ifSha8)
    return path + T::kChangedSince + e.ifSha8 + " (now " + sha8Hex(p.hash) + "); nothing written";
  if (!p.count) return T::kNotIn + path + " (" + num(p.bytes) + " B)";
  if (e.line && p.pick < 0) {
    if (p.onLine)
      return T::kPartOf + num(e.line) + ", not the whole line; line " + num(e.line) + " is: " +
             quoteCut(p.quote, kAidQuoteMax);
    return T::kNoMatchOn + num(e.line) + "; matches at " + renderLines(p.lines);
  }
  if (!e.line && !e.all && p.count > 1) {
    std::string at = renderLines(p.lines);
    return T::kManyA + path + " " + num(p.count) + " times, at " + at + T::kManyB;
  }
  return "";
}

}  // namespace

EditOutcome editFile(AidSource& src, AidSink& sink, uint64_t size, uint64_t hash, const std::string& path,
                     const EditRequest& e) {
  (void)size;
  EditOutcome o;
  o.error = checkEdit(e);
  if (!o.error.empty()) return o;
  Finder in, outCount;
  char* buf = nullptr;
  if (!in.init(e.oldText) || !outCount.init(e.oldText) || !(buf = static_cast<char*>(workingAlloc(kAidChunk)))) {
    o.error = kAidNoMemory;
    return o;
  }
  struct Freer {
    char* p;
    ~Freer() { workingFree(p); }
  } freeBuf{buf};

  // Pass 1: find and decide.
  Plan plan;
  uint64_t rawLine = 1;
  size_t quoteBytes = 0;
  auto feed1 = [&](const char* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      if (rawLine == e.line && p[i] != '\n' && quoteBytes++ < kAidQuoteMax + 4) plan.quote += p[i];
      rawLine += p[i] == '\n';
    }
  };
  auto bytes1 = [](const char*, size_t) {};
  auto match1 = [&](const Hit& h) {
    if (e.line && h.line == e.line) {
      plan.onLine = true;
      if (h.lineStart && h.whole) plan.pick = long(plan.count);
    }
    listAdd(plan.lines, uint32_t(h.line));
    ++plan.count;
    return false;
  };
  if (!pass(src, in, plan.bytes, plan.hash, feed1, bytes1, match1)) {
    o.error = T::kCardRead;
    return o;
  }
  if (!plan.quote.empty() && plan.quote.back() == '\r') plan.quote.pop_back();
  plan.fileLines = rawLine - 1 + (plan.bytes && in.prev != '\n' ? 1 : 0);
  o.oldSize = plan.bytes;
  if (hash && hash != plan.hash) {
    o.error = path + T::kChanged;
    return o;
  }
  o.error = planError(path, e, plan);
  if (!o.error.empty()) return o;

  // Pass 2: write the new bytes.
  Out out{sink, outCount, buf};
  size_t index = 0, dropNext = 0;
  uint64_t startLine = 0;
  bool tookEnd = false;
  auto feed2 = [](const char*, size_t) {};
  auto bytes2 = [&](const char* p, size_t n) {
    const size_t d = std::min(n, dropNext);
    dropNext -= d;
    out.put(p + d, n - d);
  };
  auto match2 = [&](const Hit& h) {
    const size_t i = index++;
    const bool replace = e.all || (e.line ? long(i) == plan.pick : i == 0);
    if (!replace) return false;
    if (o.at.size() < kAidListMax) o.at.push_back(uint32_t(out.outLine()));
    ++o.replaced;
    startLine = h.line;
    if (h.whole && takesLineEnd(e)) {
      dropNext = h.lineEnd;
      tookEnd = h.lineEnd > 0;
    }
    out.put(e.newText.data(), e.newText.size());
    return true;
  };
  uint64_t bytes2n = 0, hash2 = 0;
  const bool read2 = pass(src, in, bytes2n, hash2, feed2, bytes2, match2);
  const bool wrote = read2 && out.flush(true) && !out.failed;
  if (!read2 || !wrote) {
    sink.partDiscard();
    o.error = read2 ? T::kCardWrite : T::kCardRead;
    return o;
  }
  if (bytes2n != plan.bytes || hash2 != plan.hash || index != plan.count) {
    sink.partDiscard();
    o.error = path + T::kChanged;
    return o;
  }
  if (!sink.partSwap()) {
    sink.partDiscard();
    o.error = T::kCardWrite;
    return o;
  }
  o.ok = true;
  o.newSize = out.bytes;
  o.newHash = out.hash;
  o.newLines = out.lines - 1 + (out.bytes && out.last != '\n' ? 1 : 0);
  o.stillAt = out.stillAt;
  if (o.replaced == 1) {
    const uint64_t oldNl = countNl(e.oldText) + (tookEnd ? 1 : 0);
    const bool endsNl = tookEnd || e.oldText.back() == '\n';
    o.shift = int64_t(countNl(e.newText)) - int64_t(oldNl);
    const uint64_t from = startLine + oldNl + (endsNl ? 0 : 1);
    if (o.shift != 0 && from <= plan.fileLines) o.shiftFrom = uint32_t(from);
    else o.shift = 0;
  }
  return o;
}

std::string renderEdit(const std::string& path, const EditOutcome& o) {
  if (!o.ok) return o.error;
  LineList at;
  at.count = o.replaced;
  at.lines = o.at;
  std::string s = "replaced " + num(o.replaced) + " in " + path + " at " + renderLines(at) + "; file is " +
                  num(o.newSize) + " B (was " + num(o.oldSize) + " B), " + num(o.newLines) + " lines, sha8=" +
                  sha8Hex(o.newHash);
  if (o.shiftFrom) {
    const uint64_t d = uint64_t(o.shift < 0 ? -o.shift : o.shift);
    s += "; lines that were " + num(o.shiftFrom) + " and later are now " + num(d) + (o.shift < 0 ? " lower" : " higher");
  }
  if (o.stillAt.count) s += "; old is still at " + renderLines(o.stillAt);
  return s;
}

// ---- append_file facts --------------------------------------------------------------
namespace {

// Lines of s from the end: line(k) is the k-th from the last (0 = last), trailing CR cut.
struct Lines {
  std::string_view s;
  size_t begin = 0;   // whole lines start here
  // The k-th line from the end (0 = the last), false past the first whole line.
  bool fromEnd(size_t k, std::string_view& out) const {
    size_t end = s.size();
    if (end <= begin) return false;
    if (s[end - 1] == '\n') --end;   // the last line's own line end
    for (size_t i = 0;; ++i) {
      size_t start = end;
      while (start > begin && s[start - 1] != '\n') --start;
      if (i == k) {
        out = s.substr(start, end - start);
        if (!out.empty() && out.back() == '\r') out.remove_suffix(1);
        return true;
      }
      if (start == begin) return false;
      end = start - 1;
    }
  }
};

}  // namespace

AppendFacts appendFacts(std::string_view tail, bool tailIsWhole, uint64_t fileSize, uint64_t fileLines,
                        std::string_view text) {
  AppendFacts a;
  a.fence = fileSize > 0 && !tail.empty() && tail.back() != '\n';
  if (a.fence) a.note = T::kFence;
  const bool known = !(fileLines == 0 && fileSize > 0);
  Lines file{tail, 0};
  if (!tailIsWhole) {
    const size_t nl = tail.find('\n');
    file.begin = nl == std::string_view::npos ? tail.size() : nl + 1;
  }
  // The appended lines, split as the file will hold them.
  size_t total = 0;
  for (size_t i = 0; i < text.size(); ++i) total += text[i] == '\n' && i + 1 < text.size();
  if (!text.empty()) ++total;
  std::string facts;
  size_t shown = 0, more = 0;
  size_t start = 0;
  for (size_t k = 0; start < text.size(); ++k) {
    size_t end = text.find('\n', start);
    if (end == std::string_view::npos) end = text.size();
    std::string_view row = text.substr(start, end - start);
    if (!row.empty() && row.back() == '\r') row.remove_suffix(1);
    uint64_t back = 0;   // how many lines above this one the identical line is
    if (!row.empty()) {
      // The appended lines before it, nearest first.
      size_t pe = start;
      for (size_t j = 1; j <= k && !back; ++j) {
        size_t ps = pe - 1;
        while (ps > 0 && text[ps - 1] != '\n') --ps;
        std::string_view prev = text.substr(ps, pe - 1 - ps);
        if (!prev.empty() && prev.back() == '\r') prev.remove_suffix(1);
        if (prev == row) back = j;
        pe = ps;
      }
      // Then the file's last lines.
      std::string_view l;
      for (size_t t = 0; !back && t < kAppendTailLines && file.fromEnd(t, l); ++t)
        if (l == row) back = k + 1 + t;
    }
    if (back) {
      const uint64_t me = fileLines + 1 + k;
      const std::string where = known ? "line " + num(me - back) : "the line " + num(back) + " above it";
      std::string fact = total == 1 ? "identical to " + where
                                    : (known ? "line " + num(me) : "appended line " + num(k + 1)) + " is identical to " + where;
      if (shown < kAidListMax) facts += (shown++ ? ", " : "") + fact;
      else ++more;
    }
    start = end + 1;
  }
  if (!facts.empty()) {
    a.note += "; " + facts;
    if (more) a.note += " +" + num(more);
  }
  return a;
}

}  // namespace nimbus::orch::aids
