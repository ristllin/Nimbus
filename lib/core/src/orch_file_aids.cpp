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

using detail::join;
using detail::Num;
using detail::quoteCut;

size_t countNl(std::string_view s) {
  size_t n = 0;
  for (char c : s) n += c == '\n';
  return n;
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

// Where a finder's bytes and matches go.
struct FindSink {
  virtual ~FindSink() = default;
  virtual void raw(const char*, size_t) {}           // each chunk as read
  virtual void bytes(const char*, size_t) {}         // settled bytes (not part of a replaced match)
  virtual bool match(const Hit&) { return false; }   // true: replaced (the sink emitted the new text)
};

// A streaming non-overlapping matcher over a window of carry + one chunk (Horspool).
// run() settles every byte that can no longer start a match and reports each match.
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

  // The line end at e, right after a match: its length (0 at the end of the file, 1 "\n"
  // or a last "\r", 2 "\r\n"), or -1 when e is not at a line end.
  int endAt(size_t e, bool eof) const {
    if (e >= len) return eof ? 0 : -1;
    if (w[e] == '\n') return 1;
    if (w[e] != '\r') return -1;
    if (e + 1 < len) return w[e + 1] == '\n' ? 2 : -1;
    return eof ? 1 : -1;
  }
  Hit describe(size_t q, bool eof) const {
    Hit h{line, (q == 0 ? prev : w[q - 1]) == '\n', false, 0};
    if (!h.lineStart) return h;
    if (pat.back() == '\n') {
      h.whole = true;
      return h;
    }
    const int k = endAt(q + pat.size(), eof);
    h.whole = k >= 0;
    h.lineEnd = k > 0 ? size_t(k) : 0;
    return h;
  }
  // Settle w[a..b): the sink sees them and the line moves on.
  void settle(size_t a, size_t b, FindSink& out) {
    if (b <= a) return;
    out.bytes(w + a, b - a);
    line += countNl(std::string_view(w + a, b - a));
  }
  void run(bool eof, FindSink& out) {
    const size_t m = pat.size();
    const size_t lim = eof ? len : (len >= 2 ? len - 2 : 0);
    size_t p = 0, q = 0;
    while (q + m <= lim) {
      if (std::memcmp(w + q, pat.data(), m) != 0) {
        q += skip[static_cast<unsigned char>(w[q + m - 1])];
        continue;
      }
      settle(p, q, out);
      if (!out.match(describe(q, eof))) out.bytes(w + q, m);
      line += countNl(pat);
      p = q = q + m;
    }
    const size_t keep = eof ? len : std::min(q, len);
    settle(p, keep, out);
    if (keep) prev = w[keep - 1];
    std::memmove(w, w + keep, len - keep);
    len -= keep;
  }
};

// What one pass read: its bytes and their hash.
struct Read {
  uint64_t bytes = 0, hash = kFnv64Basis;
};

// Read the whole file through the finder into the sink. False: a card fault.
bool pass(AidSource& src, Finder& f, Read& r, FindSink& out) {
  f.reset();
  r = Read();
  for (;;) {
    size_t got = 0;
    if (!src.readAt(r.bytes, kAidChunk, f.room(), got)) return false;
    out.raw(f.room(), got);
    r.hash = fnv64(r.hash, std::string_view(f.room(), got));
    f.len += got;
    r.bytes += got;
    const bool eof = got < kAidChunk;
    f.run(eof, out);
    if (eof) return true;
  }
}

bool isHex8(const std::string& s) {
  if (s.size() != 8) return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

// Old in the new file: every match's line.
struct StillAt : FindSink {
  LineList lines;
  bool match(const Hit& h) override {
    listAdd(lines, uint32_t(h.line));
    return false;
  }
};

// The part file writer: kAidChunk writes, each written chunk counted for the result.
struct Out {
  AidSink& sink;
  Finder& count;   // old in the new file
  char* buf = nullptr;
  size_t len = 0;
  bool first = true, failed = false;
  uint64_t bytes = 0, hash = kFnv64Basis, lines = 1;
  char last = '\n';
  StillAt still;

  bool flush(bool eof) {
    if (failed) return false;
    if ((len || first) && !sink.partWrite(std::string_view(buf, len), first)) return !(failed = true);
    first = false;
    std::memcpy(count.room(), buf, len);
    count.len += len;
    len = 0;
    count.run(eof, still);
    return true;
  }
  void put(const char* p, size_t n) {
    while (n && !failed) {
      const size_t k = std::min(n, kAidChunk - len);
      std::memcpy(buf + len, p, k);
      hash = fnv64(hash, std::string_view(p, k));
      lines += countNl(std::string_view(p, k));
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
    return join({path, T::kChangedSince, e.ifSha8, " (now ", sha8Hex(p.hash), "); nothing written"});
  if (!p.count) return join({T::kNotIn, path, " (", Num(p.bytes), " B)"});
  if (e.line && p.pick < 0 && p.onLine)
    return join({T::kPartOf, Num(e.line), ", not the whole line; line ", Num(e.line), " is: ",
                 quoteCut(p.quote, kAidQuoteMax)});
  if (e.line && p.pick < 0) return join({T::kNoMatchOn, Num(e.line), "; matches at ", renderLines(p.lines)});
  if (!e.line && !e.all && p.count > 1)
    return join({T::kManyA, path, " ", Num(p.count), " times, at ", renderLines(p.lines), T::kManyB});
  return "";
}

// One edit: the two passes and the swap. As the finder's sink it is pass 1 (plan) or
// pass 2 (write).
struct EditRun : FindSink {
  AidSource& src;
  AidSink& sink;
  const std::string& path;
  const EditRequest& e;
  Finder in, outCount;
  char* buf = nullptr;
  Out* out = nullptr;   // set during pass 2
  Plan plan;
  EditOutcome o;

  EditRun(AidSource& s, AidSink& k, const std::string& p, const EditRequest& r) : src(s), sink(k), path(p), e(r) {}
  ~EditRun() override { workingFree(buf); }
  bool fail(std::string err) {
    o.error = std::move(err);
    return false;
  }
  bool alloc() {
    if (in.init(e.oldText) && outCount.init(e.oldText)) buf = static_cast<char*>(workingAlloc(kAidChunk));
    return buf || fail(kAidNoMemory);
  }

  // Pass 1: every match, the pick, the picked line's text.
  uint64_t rawLine = 1;
  size_t quoted = 0;
  void raw(const char* p, size_t n) override {
    if (out) return;
    for (size_t i = 0; i < n; ++i) {
      if (rawLine == e.line && p[i] != '\n' && quoted++ < kAidQuoteMax + 4) plan.quote += p[i];
      rawLine += p[i] == '\n';
    }
  }
  bool noteMatch(const Hit& h) {
    const bool here = e.line && h.line == e.line;
    plan.onLine |= here;
    if (here && h.lineStart && h.whole) plan.pick = long(plan.count);
    listAdd(plan.lines, uint32_t(h.line));
    ++plan.count;
    return false;
  }
  bool planPass(uint64_t hash) {
    Read r;
    if (!pass(src, in, r, *this)) return fail(T::kCardRead);
    plan.bytes = r.bytes;
    plan.hash = r.hash;
    if (!plan.quote.empty() && plan.quote.back() == '\r') plan.quote.pop_back();
    plan.fileLines = rawLine - 1 + (plan.bytes && in.prev != '\n' ? 1 : 0);
    o.oldSize = plan.bytes;
    if (hash && hash != plan.hash) return fail(path + T::kChanged);
    o.error = planError(path, e, plan);
    return o.error.empty();
  }

  // Pass 2: the new bytes to the part file, then the swap when nothing moved.
  size_t index = 0, dropNext = 0;
  uint64_t startLine = 0;
  bool tookEnd = false;
  void bytes(const char* p, size_t n) override {
    if (!out) return;
    const size_t d = std::min(n, dropNext);
    dropNext -= d;
    out->put(p + d, n - d);
  }
  bool match(const Hit& h) override {
    if (!out) return noteMatch(h);
    const size_t i = index++;
    const bool pick = e.all || (e.line ? long(i) == plan.pick : i == 0);
    if (!pick) return false;
    if (o.at.size() < kAidListMax) o.at.push_back(uint32_t(e.newText.empty() ? h.line : out->lines));
    ++o.replaced;
    startLine = h.line;
    dropNext = h.whole && takesLineEnd(e) ? h.lineEnd : 0;
    tookEnd = dropNext > 0;
    out->put(e.newText.data(), e.newText.size());
    return true;
  }
  // The error after pass 2, "" when the part may be swapped in.
  std::string writeError(bool read, bool wrote, const Read& r) const {
    if (!read) return T::kCardRead;
    if (!wrote) return T::kCardWrite;
    if (r.bytes != plan.bytes || r.hash != plan.hash || index != plan.count) return path + T::kChanged;
    return "";
  }
  bool writePass() {
    Out w{sink, outCount, buf};
    out = &w;
    Read r;
    const bool read = pass(src, in, r, *this);
    const bool wrote = read && w.flush(true);
    std::string err = writeError(read, wrote, r);
    if (err.empty() && !sink.partSwap()) err = T::kCardWrite;
    if (!err.empty()) {
      sink.partDiscard();
      return fail(err);
    }
    o.newSize = w.bytes;
    o.newHash = w.hash;
    o.newLines = w.lines - 1 + (w.bytes && w.last != '\n' ? 1 : 0);
    o.stillAt = w.still.lines;
    if (o.replaced == 1) shiftAfter();
    return true;
  }

  void shiftAfter() {
    const uint64_t oldNl = countNl(e.oldText) + (tookEnd ? 1 : 0);
    const bool endsNl = tookEnd || e.oldText.back() == '\n';
    o.shift = int64_t(countNl(e.newText)) - int64_t(oldNl);
    const uint64_t from = startLine + oldNl + (endsNl ? 0 : 1);
    if (o.shift != 0 && from <= plan.fileLines) o.shiftFrom = uint32_t(from);
    else o.shift = 0;
  }
};

}  // namespace

EditOutcome editFile(AidSource& src, AidSink& sink, uint64_t size, uint64_t hash, const std::string& path,
                     const EditRequest& e) {
  (void)size;
  EditRun run(src, sink, path, e);
  run.o.error = checkEdit(e);
  if (run.o.error.empty() && run.alloc() && run.planPass(hash) && run.writePass()) run.o.ok = true;
  return std::move(run.o);
}

std::string renderEdit(const std::string& path, const EditOutcome& o) {
  if (!o.ok) return o.error;
  LineList at;
  at.count = o.replaced;
  at.lines = o.at;
  std::string s = join({"replaced ", Num(o.replaced), " in ", path, " at ", renderLines(at), "; file is ", Num(o.newSize),
                        " B (was ", Num(o.oldSize), " B), ", Num(o.newLines), " lines, sha8=", sha8Hex(o.newHash)});
  if (o.shiftFrom)
    s += join({"; lines that were ", Num(o.shiftFrom), " and later are now ", Num(uint64_t(o.shift < 0 ? -o.shift : o.shift)),
               o.shift < 0 ? " lower" : " higher"});
  if (o.stillAt.count) s += join({"; old is still at ", renderLines(o.stillAt)});
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

namespace {

// How many lines above appended line k (starting at text[start]) its nearest identical
// line is: the appended lines before it first, then the file's last lines; 0 for none.
uint64_t nearestIdentical(std::string_view text, size_t start, size_t k, std::string_view row, const Lines& file) {
  size_t pe = start;
  for (size_t j = 1; j <= k; ++j) {
    size_t ps = pe - 1;
    while (ps > 0 && text[ps - 1] != '\n') --ps;
    std::string_view prev = text.substr(ps, pe - 1 - ps);
    if (!prev.empty() && prev.back() == '\r') prev.remove_suffix(1);
    if (prev == row) return j;
    pe = ps;
  }
  std::string_view l;
  for (size_t t = 0; t < kAppendTailLines && file.fromEnd(t, l); ++t)
    if (l == row) return k + 1 + t;
  return 0;
}

// One fact: "identical to line 41", "line 214 is identical to line 41", or with the line
// count unknown "the line 2 above it" and "appended line 2 is ...".
std::string factText(bool known, bool single, uint64_t me, uint64_t back, size_t k) {
  const std::string where = known ? join({"line ", Num(me - back)}) : join({"the line ", Num(back), " above it"});
  if (single) return join({"identical to ", where});
  return join({known ? "line " : "appended line ", Num(known ? me : k + 1), " is identical to ", where});
}

std::string_view noCr(std::string_view s) {
  if (!s.empty() && s.back() == '\r') s.remove_suffix(1);
  return s;
}

// Lines in text as the file will hold them (a last line end starts no line).
size_t lineCount(std::string_view text) {
  if (text.empty()) return 0;
  return countNl(text.substr(0, text.size() - 1)) + 1;
}

}  // namespace

AppendFacts appendFacts(std::string_view tail, bool tailIsWhole, uint64_t fileSize, uint64_t fileLines,
                        std::string_view text) {
  AppendFacts a;
  a.fence = endsTorn(fileSize, tail.empty() ? '\n' : tail.back());
  if (a.fence) a.note = T::kFence;
  const bool known = fileLines > 0 || fileSize == 0;
  const size_t cut = tailIsWhole ? 0 : tail.find('\n');
  const Lines file{tail, cut == std::string_view::npos ? tail.size() : cut + (tailIsWhole ? 0 : 1)};
  const size_t total = lineCount(text);
  std::string facts;
  size_t shown = 0, more = 0;
  for (size_t k = 0, start = 0; start < text.size(); ++k) {
    const size_t nl = text.find('\n', start);
    const size_t end = nl == std::string_view::npos ? text.size() : nl;
    const std::string_view row = noCr(text.substr(start, end - start));
    const uint64_t back = row.empty() ? 0 : nearestIdentical(text, start, k, row, file);
    start = end + 1;
    if (!back) continue;
    if (shown >= kAidListMax) {
      ++more;
      continue;
    }
    facts += (shown++ ? ", " : "") + factText(known, total == 1, fileLines + 1 + k, back, k);
  }
  if (!facts.empty()) a.note += join({"; ", facts});
  if (more) a.note += join({" +", Num(more)});
  return a;
}

}  // namespace nimbus::orch::aids
