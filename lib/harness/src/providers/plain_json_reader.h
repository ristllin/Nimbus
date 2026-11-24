#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

// plain_json_reader.h - the strict JSON reader of engine 2's wires (lane RB-W1b, moved out of
// plain_wire_anthropic.cpp by lane E2-R2b so every codec reads answers the same way). INTERNAL
// to lib/harness/src/providers/plain_wire_*.cpp. No JSON document: no 65,535 B string limit, no
// second copy of the body, every byte of a field kept as served (at() gives byte ranges). It
// knows no provider field. The class has internal linkage (an unnamed namespace): each codec gets
// its own copy, which the compiler keeps in line with its parse (the image grows by no call layer).

namespace agent {
namespace plain {

// The index of `key` in `fields` (n when none), its bit set in `seen`; false when a known field
// comes twice (a field given twice is not one answer). For a codec's object of known fields.
bool fieldOnce(const std::string& key, const char* const* fields, uint32_t n, uint32_t& seen, uint32_t& k);

namespace {

void appendUtf8(std::string& o, long c) {
  if (c < 0x80) {
    o.push_back((char)c);
  } else if (c < 0x800) {
    o.push_back((char)(0xC0 | (c >> 6)));
    o.push_back((char)(0x80 | (c & 0x3F)));
  } else if (c < 0x10000) {
    o.push_back((char)(0xE0 | (c >> 12)));
    o.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
    o.push_back((char)(0x80 | (c & 0x3F)));
  } else {
    o.push_back((char)(0xF0 | (c >> 18)));
    o.push_back((char)(0x80 | ((c >> 12) & 0x3F)));
    o.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
    o.push_back((char)(0x80 | (c & 0x3F)));
  }
}

constexpr uint8_t kParseDepth = 64;

class JsonReader {
 public:
  explicit JsonReader(const std::string& s) : s_(s) {}
  size_t at() const { return i_; }
  const std::string& body() const { return s_; }
  void ws() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) ++i_;
  }
  bool peek(char c) {
    ws();
    return i_ < s_.size() && s_[i_] == c;
  }
  bool eat(char c) {
    if (!peek(c)) return false;
    ++i_;
    return true;
  }
  bool end() {
    ws();
    return i_ == s_.size();
  }

  // One string; its decoded text into `out` when given (escapes and surrogate pairs to UTF-8).
  bool str(std::string* out);
  // A non-negative integer (saturating at UINT32_MAX) or null (0).
  bool count(uint32_t& out);
  // Any one value, read without recursion (the device's stack never grows with the answer's
  // nesting): one bit per open container, object or array. peak() is the deepest nesting it met,
  // counted from where it started.
  bool skip();
  uint8_t peak() const { return peak_; }

  // An object: `onKey(key)` reads each value.
  template <typename F>
  bool object(F onKey) {
    if (!eat('{') || ++depth_ > kParseDepth) return false;
    std::string key;
    for (bool first = true; !eat('}'); first = false) {
      if (!first && !eat(',')) return false;
      if (!peek('"') || !str(&key) || !eat(':') || !onKey(key)) return false;
    }
    --depth_;
    return true;
  }
  // An array: `onItem()` reads each element.
  template <typename F>
  bool array(F onItem) {
    if (!eat('[') || ++depth_ > kParseDepth) return false;
    for (bool first = true; !eat(']'); first = false) {
      if (!first && !eat(',')) return false;
      if (!onItem()) return false;
    }
    --depth_;
    return true;
  }

 private:
  struct Open {
    uint64_t objects = 0;   // bit k: open container k is an object
    uint8_t n = 0;          // open containers
  };
  bool skipOne(Open& o, bool& closed);
  bool afterValue(Open& o, bool& closed);
  static void close(Open& o) {
    --o.n;
    o.objects >>= 1;
  }
  bool keyAndColon() { return peek('"') && str(nullptr) && eat(':'); }
  bool anyLiteral() { return literal("true") || literal("false") || literal("null"); }
  bool escape(std::string* out);
  bool unicodeEscape(std::string* out);
  bool literal(const char* w);
  bool digits();
  bool at(char c) const { return i_ < s_.size() && s_[i_] == c; }
  bool skipNumber();
  bool exponent();
  bool hex4(long& code);

  const std::string& s_;
  size_t i_ = 0;
  uint8_t depth_ = 0;
  uint8_t peak_ = 0;
};

bool JsonReader::str(std::string* out) {
  if (!eat('"')) return false;
  if (out) out->clear();
  for (;;) {
    if (i_ >= s_.size()) return false;
    const unsigned char c = (unsigned char)s_[i_];
    if (c == '"') {
      ++i_;
      return true;
    }
    if (c < 0x20) return false;   // a raw control byte: not JSON
    if (c == '\\') {
      if (!escape(out)) return false;
      continue;
    }
    const size_t from = i_;
    while (i_ < s_.size() && s_[i_] != '"' && s_[i_] != '\\' && (unsigned char)s_[i_] >= 0x20) ++i_;
    if (out) out->append(s_, from, i_ - from);
  }
}

bool JsonReader::count(uint32_t& out) {
  ws();
  out = 0;
  if (literal("null")) return true;
  const size_t from = i_;
  if (!skipNumber()) return false;
  uint64_t v = 0;
  for (size_t k = from; k < i_; ++k) {
    const char c = s_[k];
    if (c < '0' || c > '9') return false;   // negative, a fraction or an exponent: not a count
    v = v * 10 + (uint64_t)(c - '0');
    if (v > UINT32_MAX) v = UINT32_MAX;
  }
  out = (uint32_t)v;
  return true;
}

bool JsonReader::skip() {
  Open o;
  peak_ = 0;
  for (;;) {
    bool closed = false;   // a value just ended
    if (!skipOne(o, closed)) return false;
    while (closed) {   // after a value: the end, a comma and the next value, or a close
      if (!o.n) return true;
      if (!afterValue(o, closed)) return false;
    }
  }
}

// One value or the start of a container (its first key read for an object).
bool JsonReader::skipOne(Open& o, bool& closed) {
  ws();
  if (i_ >= s_.size()) return false;
  const char c = s_[i_];
  if (c != '{' && c != '[') {
    closed = c == '"' ? str(nullptr) : (c == '-' || (c >= '0' && c <= '9')) ? skipNumber() : anyLiteral();
    return closed;
  }
  if (depth_ + o.n + 1 > kParseDepth) return false;
  ++i_;
  o.objects = (o.objects << 1) | (c == '{' ? 1u : 0u);
  if (++o.n > peak_) peak_ = o.n;
  if (eat(c == '{' ? '}' : ']')) {
    close(o);
    closed = true;
    return true;
  }
  return c != '{' || keyAndColon();
}

// After a value inside a container: a comma (and the next key) or the container's close.
bool JsonReader::afterValue(Open& o, bool& closed) {
  const bool obj = o.objects & 1u;
  if (eat(',')) {
    closed = false;
    return !obj || keyAndColon();
  }
  if (!eat(obj ? '}' : ']')) return false;
  close(o);
  return true;
}

// One escape, its backslash at i_: the JSON set, or \u with four hex digits (a surrogate pair).
bool JsonReader::escape(std::string* out) {
  if (++i_ >= s_.size()) return false;
  const char e = s_[i_++];
  if (e == 'u') return unicodeEscape(out);
  const char* kEsc = "\"\\/bfnrt";
  const char* kOut = "\"\\/\b\f\n\r\t";
  const char* hit = e ? std::strchr(kEsc, e) : nullptr;
  if (!hit) return false;
  if (out) out->push_back(kOut[hit - kEsc]);
  return true;
}

bool JsonReader::unicodeEscape(std::string* out) {
  long code = 0;
  if (!hex4(code)) return false;
  if (code >= 0xDC00 && code <= 0xDFFF) return false;   // a lone low surrogate
  if (code >= 0xD800 && code <= 0xDBFF) {   // a high surrogate needs its low half
    long low = 0;
    if (!(i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u')) return false;
    i_ += 2;
    if (!hex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
  }
  if (out) appendUtf8(*out, code);
  return true;
}

bool JsonReader::literal(const char* w) {
  const size_t n = std::strlen(w);
  if (s_.compare(i_, n, w) != 0) return false;
  i_ += n;
  return true;
}

bool JsonReader::digits() {
  const size_t d = i_;
  while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
  return i_ > d;
}

bool JsonReader::skipNumber() {
  if (at('-')) ++i_;
  const size_t intAt = i_;
  if (!digits()) return false;
  if (s_[intAt] == '0' && i_ - intAt > 1) return false;   // no leading zero
  if (at('.') && (++i_, !digits())) return false;
  return !(at('e') || at('E')) || exponent();
}

bool JsonReader::exponent() {
  ++i_;
  if (at('+') || at('-')) ++i_;
  return digits();
}

bool JsonReader::hex4(long& code) {
  if (i_ + 4 > s_.size()) return false;
  code = 0;
  for (int k = 0; k < 4; ++k) {
    const char h = s_[i_++];
    const int v = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
    if (v < 0) return false;
    code = code * 16 + v;
  }
  return true;
}

}  // namespace

}  // namespace plain
}  // namespace agent
