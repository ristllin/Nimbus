// plain_json_reader.cpp - the strict JSON reader of engine 2's wires (plain_json_reader.h).
#include "plain_json_reader.h"

#include <cstring>

namespace agent {
namespace plain {

namespace {

void putUtf8(std::string& o, long c) {
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

}  // namespace

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
  if (out) putUtf8(*out, code);
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

}  // namespace plain
}  // namespace agent
