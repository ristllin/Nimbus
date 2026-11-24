#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// plain_json_reader.h - the strict JSON reader of engine 2's wires (lane RB-W1b, moved out of
// plain_wire_anthropic.cpp by lane E2-R2b so every codec reads answers the same way). INTERNAL
// to lib/harness/src/providers/plain_wire_*.cpp. No JSON document: no 65,535 B string limit, no
// second copy of the body, every byte of a field kept as served (at() gives byte ranges). It
// knows no provider field.

namespace agent {
namespace plain {

inline constexpr uint8_t kParseDepth = 64;

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

}  // namespace plain
}  // namespace agent
