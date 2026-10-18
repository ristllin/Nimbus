#include "nimbus/orch/failure_chain.h"

#include <cstring>

#include "nimbus/mem_cap.h"

namespace nimbus {
namespace orch {

namespace {

// Length of the valid UTF-8 sequence at s[i] (1-4), or 0 when it is not one
// (a stray continuation byte, an overlong form, a surrogate, past U+10FFFF, or a
// sequence cut short by the end of the input).
size_t utf8SeqLen(const unsigned char* s, size_t i, size_t n) {
  const unsigned char c = s[i];
  if (c < 0x80) return 1;
  auto cont = [&](size_t k) { return i + k < n && (s[i + k] & 0xC0) == 0x80; };
  if (c >= 0xC2 && c <= 0xDF) return cont(1) ? 2 : 0;
  if (c >= 0xE0 && c <= 0xEF) {
    if (!cont(1) || !cont(2)) return 0;
    if (c == 0xE0 && s[i + 1] < 0xA0) return 0;    // overlong
    if (c == 0xED && s[i + 1] > 0x9F) return 0;    // UTF-16 surrogate
    return 3;
  }
  if (c >= 0xF0 && c <= 0xF4) {
    if (!cont(1) || !cont(2) || !cont(3)) return 0;
    if (c == 0xF0 && s[i + 1] < 0x90) return 0;    // overlong
    if (c == 0xF4 && s[i + 1] > 0x8F) return 0;    // past U+10FFFF
    return 4;
  }
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

std::string sanitizeFailText(const char* s, size_t len, size_t maxBytes) {
  std::string out;
  if (!s || !len || !maxBytes) return out;
  const unsigned char* u = (const unsigned char*)s;
  for (size_t i = 0; i < len;) {
    size_t sl = utf8SeqLen(u, i, len);
    const bool bad = sl == 0;
    if (bad) sl = 1;
    const bool space = !bad && isControl(u, i, sl);
    if (space || (sl == 1 && u[i] == ' ')) {
      // One space for any run of spaces and controls; never a leading one.
      if (!out.empty() && out.back() != ' ') {
        if (out.size() + 1 > maxBytes) break;
        out.push_back(' ');
      }
    } else if (bad) {
      if (out.size() + 1 > maxBytes) break;
      out.push_back('?');
    } else {
      if (out.size() + sl > maxBytes) break;
      out.append(s + i, sl);
    }
    i += sl;
  }
  trimTrailingSpace(out);
  return out;
}

std::string shortFailCause(const std::string& text) {
  if (text.empty()) return std::string();
  for (size_t p = text.find("HTTP "); p != std::string::npos; p = text.find("HTTP ", p + 1)) {
    const size_t d = p + 5;
    if (d + 3 <= text.size() && text[d] >= '1' && text[d] <= '5' && text[d + 1] >= '0' &&
        text[d + 1] <= '9' && text[d + 2] >= '0' && text[d + 2] <= '9')
      return text.substr(d, 3);
  }
  if (containsNoCase(text, "timeout") || containsNoCase(text, "timed out")) return "timeout";
  if (containsNoCase(text, "low memory") || containsNoCase(text, "out of memory"))
    return "low memory";
  if (containsNoCase(text, "budget")) return "budget";
  if (containsNoCase(text, "parse")) return "parse";
  if (containsNoCase(text, "network")) return "network";
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

std::string FailureChain::render(size_t cap) const {
  if (!n_ || !cap) return std::string();
  const FailStep& head = s_[0];
  const std::string pre = head.host + attemptsTag(head.attempts) + ": ";
  const std::string text = head.text.empty() ? std::string("failed") : head.text;
  std::string tails[kMaxSteps];
  for (size_t i = 1; i < n_; ++i) tails[i] = " > " + brief(i);
  auto more = [&](size_t keep) -> std::string {   // the marker for dropped entries
    const size_t dropped = (n_ - 1) - keep;
    return dropped ? " > +" + std::to_string(dropped) + " more" : std::string();
  };
  // Keep as many later entries as leave the first cause kMinHeadText (or all of it).
  size_t keep = n_ - 1, fixed = 0;
  for (;; --keep) {
    fixed = pre.size() + more(keep).size();
    for (size_t i = 1; i <= keep; ++i) fixed += tails[i].size();
    const size_t want = text.size() < kMinHeadText ? text.size() : kMinHeadText;
    if (fixed + want <= cap || keep == 0) break;
  }
  std::string out = pre;
  if (cap > fixed) {
    std::string t = capUtf8(text, cap - fixed);
    if (t.size() < text.size() && t.size() > 3 && cap - fixed >= t.size()) {
      // Mark the cut when there is room for it ("..." replaces the last bytes).
      t = capUtf8(text, cap - fixed - 3);
      trimTrailingSpace(t);
      t += "...";
    }
    out += t;
  } else {
    trimTrailingSpace(out);   // no room for any text: "anthropic:"
  }
  for (size_t i = 1; i <= keep; ++i) out += tails[i];
  out += more(keep);
  if (out.size() > cap) out = capUtf8(out, cap);   // only when even the host does not fit
  return out;
}

}  // namespace orch
}  // namespace nimbus
