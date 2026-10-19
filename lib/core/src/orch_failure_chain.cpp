#include "nimbus/orch/failure_chain.h"

#include <cstring>

#include "nimbus/mem_cap.h"

namespace nimbus {
namespace orch {

namespace {

// Length of the valid UTF-8 sequence at s[i] (1-4), or 0 when it is not one
// (a stray continuation byte, an overlong form, a surrogate, past U+10FFFF, or a
// sequence cut short by the end of the input).
bool isCont(const unsigned char* s, size_t i, size_t n) { return i < n && (s[i] & 0xC0) == 0x80; }

size_t utf8Seq3(const unsigned char* s, size_t i, size_t n) {
  if (!isCont(s, i + 1, n) || !isCont(s, i + 2, n)) return 0;
  if (s[i] == 0xE0 && s[i + 1] < 0xA0) return 0;    // overlong
  if (s[i] == 0xED && s[i + 1] > 0x9F) return 0;    // UTF-16 surrogate
  return 3;
}

size_t utf8Seq4(const unsigned char* s, size_t i, size_t n) {
  if (!isCont(s, i + 1, n) || !isCont(s, i + 2, n) || !isCont(s, i + 3, n)) return 0;
  if (s[i] == 0xF0 && s[i + 1] < 0x90) return 0;    // overlong
  if (s[i] == 0xF4 && s[i + 1] > 0x8F) return 0;    // past U+10FFFF
  return 4;
}

size_t utf8SeqLen(const unsigned char* s, size_t i, size_t n) {
  const unsigned char c = s[i];
  if (c < 0x80) return 1;
  if (c >= 0xC2 && c <= 0xDF) return isCont(s, i + 1, n) ? 2 : 0;
  if (c >= 0xE0 && c <= 0xEF) return utf8Seq3(s, i, n);
  if (c >= 0xF0 && c <= 0xF4) return utf8Seq4(s, i, n);
  return 0;
}

bool isControl(const unsigned char* s, size_t i, size_t len) {
  if (len == 1) return s[i] < 0x20 || s[i] == 0x7F;
  return len == 2 && s[i] == 0xC2 && s[i + 1] < 0xA0;   // C1 controls U+0080..U+009F
}

void trimTrailingSpace(std::string& s) {
  while (!s.empty() && s.back() == ' ') s.pop_back();
}

// `s` cut to at most `cap` bytes on a character boundary (s is valid UTF-8).
std::string capUtf8(const std::string& s, size_t cap) {
  const int keep = utf8CapLen(s.c_str(), (int)s.size(), (int)cap);
  return s.substr(0, (size_t)keep);
}

bool startsWithNoCase(const std::string& s, size_t at, const char* w) {
  for (size_t k = 0; w[k]; ++k) {
    if (at + k >= s.size()) return false;
    char a = s[at + k];
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (a != w[k]) return false;
  }
  return true;
}

bool containsNoCase(const std::string& s, const char* w) {
  for (size_t i = 0; i < s.size(); ++i)
    if (startsWithNoCase(s, i, w)) return true;
  return false;
}

std::string attemptsTag(uint8_t attempts) {
  if (attempts <= 1) return std::string();
  return " (x" + std::to_string((unsigned)attempts) + ")";
}

}  // namespace

namespace {

// Append `n` bytes when they fit in `maxBytes`; false when the output is full.
bool appendCapped(std::string& out, const char* p, size_t n, size_t maxBytes) {
  if (out.size() + n > maxBytes) return false;
  out.append(p, n);
  return true;
}

// One input character into `out`: a run of spaces and controls becomes one space
// (never a leading one), an invalid byte becomes '?'. False when the output is full.
bool emitChar(std::string& out, const unsigned char* u, size_t i, size_t sl, bool bad,
              size_t maxBytes) {
  if (bad) return appendCapped(out, "?", 1, maxBytes);
  if (isControl(u, i, sl) || (sl == 1 && u[i] == ' ')) {
    if (out.empty() || out.back() == ' ') return true;
    return appendCapped(out, " ", 1, maxBytes);
  }
  return appendCapped(out, (const char*)u + i, sl, maxBytes);
}

}  // namespace

std::string sanitizeFailText(const char* s, size_t len, size_t maxBytes) {
  std::string out;
  if (!s || !len || !maxBytes) return out;
  const unsigned char* u = (const unsigned char*)s;
  for (size_t i = 0; i < len;) {
    const size_t sl = utf8SeqLen(u, i, len);
    if (!emitChar(out, u, i, sl ? sl : 1, sl == 0, maxBytes)) break;
    i += sl ? sl : 1;
  }
  trimTrailingSpace(out);
  return out;
}

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// The first "HTTP <3-digit status>" in `text`, or "".
std::string httpStatusIn(const std::string& text) {
  for (size_t p = text.find("HTTP "); p != std::string::npos; p = text.find("HTTP ", p + 1)) {
    const size_t d = p + 5;
    if (d + 3 <= text.size() && text[d] >= '1' && text[d] <= '5' && isDigit(text[d + 1]) &&
        isDigit(text[d + 2]))
      return text.substr(d, 3);
  }
  return std::string();
}

struct CauseWord { const char* word; const char* slug; };
constexpr CauseWord kCauseWords[] = {
    {"timeout", "timeout"},       {"timed out", "timeout"}, {"low memory", "low memory"},
    {"out of memory", "low memory"}, {"budget", "budget"},  {"parse", "parse"},
    {"network", "network"},
};

}  // namespace

std::string shortFailCause(const std::string& text) {
  if (text.empty()) return std::string();
  const std::string status = httpStatusIn(text);
  if (!status.empty()) return status;
  for (const CauseWord& w : kCauseWords)
    if (containsNoCase(text, w.word)) return w.slug;
  std::string head = text.substr(0, text.find(':'));
  trimTrailingSpace(head);
  return capUtf8(head, 24);
}

void FailureChain::clear() {
  for (FailStep& st : s_) st = FailStep();
  n_ = 0;
}

void FailureChain::note(const std::string& host, const std::string& err) {
  const std::string h = sanitizeFailText(host.data(), host.size(), 15);
  // A retry on the host that failed last is another attempt of its run: the run
  // keeps its FIRST error (the one that explains it), only an empty one is filled.
  if (n_ && s_[n_ - 1].host == h) {
    FailStep& run = s_[n_ - 1];
    if (run.attempts < 255) run.attempts++;
    if (run.text.empty()) run.text = sanitizeFailText(err.data(), err.size(), kTextMax);
    return;
  }
  // Full: the new run replaces the last entry, so the chain still ends where the
  // turn did and still starts where it first went wrong.
  FailStep& dst = n_ < kMaxSteps ? s_[n_++] : s_[kMaxSteps - 1];
  dst.host = h;
  dst.text = sanitizeFailText(err.data(), err.size(), kTextMax);
  dst.attempts = 1;
}

bool FailureChain::anyTextHas(const char* needle) const {
  if (!needle || !*needle) return false;
  for (size_t i = 0; i < n_; ++i)
    if (s_[i].text.find(needle) != std::string::npos) return true;
  return false;
}

std::string FailureChain::brief(size_t i) const {
  if (i >= n_) return std::string();
  const FailStep& st = s_[i];
  std::string c = shortFailCause(st.text);
  if (c.empty()) c = "failed";
  return st.host + " " + c + attemptsTag(st.attempts);
}

std::string FailureChain::path() const {
  std::string p;
  for (size_t i = 0; i < n_; ++i) {
    if (i) p += " -> ";
    p += s_[i].host;
  }
  return p;
}

namespace {

// The first cause's text in `room` bytes, marked "..." when it was cut and there is room.
std::string fitHeadText(const std::string& text, size_t room) {
  std::string t = capUtf8(text, room);
  if (t.size() >= text.size() || t.size() <= 3) return t;
  t = capUtf8(text, room - 3);
  trimTrailingSpace(t);
  return t + "...";
}

std::string moreMarker(size_t dropped) {
  return dropped ? " > +" + std::to_string(dropped) + " more" : std::string();
}

}  // namespace

std::string FailureChain::render(size_t cap) const {
  if (!n_ || !cap) return std::string();
  const FailStep& head = s_[0];
  const std::string pre = head.host + attemptsTag(head.attempts) + ": ";
  const std::string text = head.text.empty() ? std::string("failed") : head.text;
  const size_t want = text.size() < kMinHeadText ? text.size() : kMinHeadText;
  std::string tails[kMaxSteps];
  for (size_t i = 1; i < n_; ++i) tails[i] = " > " + brief(i);
  // Keep as many later entries as leave the first cause kMinHeadText (or all of it).
  size_t keep = n_ - 1, fixed = 0;
  for (;; --keep) {
    fixed = pre.size() + moreMarker((n_ - 1) - keep).size();
    for (size_t i = 1; i <= keep; ++i) fixed += tails[i].size();
    if (fixed + want <= cap || keep == 0) break;
  }
  std::string out = pre;
  if (cap > fixed) out += fitHeadText(text, cap - fixed);
  else trimTrailingSpace(out);   // no room for any text: "anthropic:"
  for (size_t i = 1; i <= keep; ++i) out += tails[i];
  out += moreMarker((n_ - 1) - keep);
  if (out.size() > cap) out = capUtf8(out, cap);   // only when even the host does not fit
  return out;
}

}  // namespace orch
}  // namespace nimbus
