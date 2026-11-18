#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "nimbus/harness/http.h"   // BodySplice: a photo spliced into the body at send time
#include "nimbus/orch/tx/status.h"
#include "nimbus/orch/wire_limits.h"   // kJsonStrGuard: the 60,000 B per-string guard
#include "nimbus/utf8.h"               // utf8Repair, utf8ClipNoted: the repair and the clip

// plain_body.h - the checked body writer of engine 2 (PRD section 1, "Request on each
// wire": renderers write into the PSRAM body through a checked writer, UTF-8 repair, a
// 60,000 B per-string guard). Lane RB-EX. INTERNAL to the plain wires
// (lib/harness/src/providers/plain_wire_*.cpp), like providers/wire.h is to the old ones,
// and built on the same primitives: nimbus/utf8.h, nimbus/orch/wire_limits.h and the
// strict JSON rule of nimbus/json_strict.h. It includes nothing of the old engine.
//
// A renderer writes JSON straight into the caller's body string, with no JSON document in
// between (ArduinoJson stores a string of 65,536 B or more as null and the provider then
// sees a hole; a document also doubles the body's memory):
//
//   std::string body;
//   body::BodyWriter w(body, &req.splices);
//   w.beginObject().key("model").str(model).key("max_tokens").unum(in.maxTokens);
//   w.key("messages").beginArray() ... .endArray().endObject();
//   if (!w.ok()) return w.status();   // a refused string or a misuse: nothing is sent
//
// Every string value passes three rules, whatever its source:
//   1. UTF-8 repair: each invalid sequence becomes U+FFFD (a torn character in one tool
//      result made every provider answer 400 on 18 Oct);
//   2. the guard: at most kJsonStrGuard (60,000) bytes of text per string. Over it, a
//      string written with Over::Refuse (system text, an owner message) fails the writer
//      with Err::TooLarge and nothing is sent; one written with Over::Clip (a tool result,
//      call arguments, replayed prose) is cut on a character boundary with the visible
//      note "[cut: kept K of N bytes]" inside the guard. Repair runs before the clip, so
//      every clip path is UTF-8-safe;
//   3. strict escaping: '"', '\\' and every byte under 0x20 are escaped (\b \f \n \r \t,
//      else \u00XX), so a strict parser accepts the body (json_strict.h).
// A stored JSON fragment (a tool's schema, the model's call arguments, a signed thinking
// block) goes in through raw(): repaired, its strings escaped and guarded, its brackets
// balanced; a fragment that is not balanced JSON or holds an over-guard string is refused.
//
// The writer is sticky: the first error is kept and every later call does nothing, so a
// renderer checks once at the end. Output is a pure function of the calls: the same calls
// give the same bytes (the prefix property).
namespace agent {
namespace plain {
namespace body {

using Status = nimbus::orch::tx::Status;
using Err = nimbus::orch::tx::Err;

inline constexpr size_t kStrGuard = nimbus::orch::kJsonStrGuard;
inline constexpr uint8_t kMaxDepth = 32;

// What a string over the guard does.
enum class Over : uint8_t {
  Refuse = 0,   // the writer fails with TooLarge (text the model must see whole)
  Clip = 1,     // cut on a character boundary with the visible note, inside the guard
};

// What the writer did, for the e item and the tests.
struct BodyStats {
  uint32_t strings = 0;    // string values written (raw fragments' strings included)
  uint32_t repaired = 0;   // strings that held invalid UTF-8 and were repaired
  uint32_t clipped = 0;    // strings cut at the guard
  size_t longest = 0;      // the longest string's text bytes, after repair and clip
};

namespace detail {

// Append the JSON escape of [p, p+n) (already valid UTF-8) to out.
inline void escapeInto(std::string& out, const char* p, size_t n) {
  static constexpr char kHex[] = "0123456789abcdef";
  size_t run = 0;   // bytes copied as they are since the last escape
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(p[i]);
    if (c >= 0x20 && c != '"' && c != '\\') {
      ++run;
      continue;
    }
    out.append(p + i - run, run);
    run = 0;
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: {
        const char esc[6] = {'\\', 'u', '0', '0', kHex[c >> 4], kHex[c & 0x0F]};
        out.append(esc, sizeof(esc));
      }
    }
  }
  out.append(p + n - run, run);
}

// The text bytes a JSON string escape stands for, at most: a \uXXXX is at most 3 bytes of
// UTF-8 (a surrogate pair is two escapes and 4 bytes), any other escape one byte.
inline size_t escapeTextBytes(char after) { return after == 'u' ? 3 : 1; }

}  // namespace detail

class BodyWriter {
 public:
  // `splices`: where splice() records photo payloads (null = this body takes none).
  explicit BodyWriter(std::string& out, BodySplices* splices = nullptr, size_t guard = kStrGuard)
      : out_(out), splices_(splices), guard_(guard) {}

  BodyWriter& beginObject() { return open('{', true); }
  BodyWriter& endObject() { return close('}', true); }
  BodyWriter& beginArray() { return open('[', false); }
  BodyWriter& endArray() { return close(']', false); }

  // An object key: a literal of printable ASCII without '"' or '\\' (the renderers' own
  // field names; never data).
  BodyWriter& key(const char* k) {
    if (!ready() || !inObject() || afterKey_) return fail(Err::InvalidArgument, "plain::body key");
    if (!k || !*k) return fail(Err::InvalidArgument, "plain::body key");
    for (const char* p = k; *p; ++p)
      if (*p < 0x20 || *p > 0x7E || *p == '"' || *p == '\\') return fail(Err::InvalidArgument, "plain::body key");
    if (count_[depth_ - 1]++) out_ += ',';
    out_ += '"';
    out_ += k;
    out_ += "\":";
    afterKey_ = true;
    return *this;
  }

  BodyWriter& str(const std::string& s, Over over = Over::Refuse) { return str(s.data(), s.size(), over); }
  BodyWriter& str(const char* s, size_t n, Over over = Over::Refuse) {
    if (!value()) return *this;
    if (!s && n) return fail(Err::InvalidArgument, "plain::body str");
    if (n <= guard_ && nimbus::utf8Valid(s, n)) {   // the common case: no copy
      out_ += '"';
      detail::escapeInto(out_, s, n);
      out_ += '"';
      count(n, false, false);
      return *this;
    }
    std::string fixed(s, n);
    return text(fixed, over);
  }

  BodyWriter& num(int64_t v) {
    if (value()) out_ += std::to_string(v);
    return *this;
  }
  BodyWriter& unum(uint64_t v) {
    if (value()) out_ += std::to_string(v);
    return *this;
  }
  BodyWriter& boolean(bool v) {
    if (value()) out_ += v ? "true" : "false";
    return *this;
  }
  BodyWriter& null() {
    if (value()) out_ += "null";
    return *this;
  }

  // A stored JSON value written as it is, after the three rules: invalid UTF-8 repaired,
  // raw control bytes inside its strings escaped, every string at most the guard (an
  // over-guard string refuses the fragment: a fragment is never clipped, its JSON would
  // break), one object or array with brackets balanced and every string closed. Whitespace outside strings is
  // dropped, so a fragment stored pretty and one stored compact render the same bytes.
  BodyWriter& raw(const std::string& json) {
    if (!value()) return *this;
    std::string fixed;
    const std::string* src = &json;
    if (!nimbus::utf8Valid(json)) {
      fixed = json;
      nimbus::utf8Repair(fixed);
      src = &fixed;
      ++stats_.repaired;
    }
    const size_t mark = out_.size();
    if (!rawInto(*src)) out_.resize(mark);
    return *this;
  }

  // A string written in pieces (a stamp then the owner's text, a data URL's prefix then
  // its photo): the pieces join into one string value; the three rules apply to the whole
  // text. splice() places a photo's base64 at this point of the string; a string that
  // holds a splice is never clipped (an over-guard text around it is refused).
  BodyWriter& beginStr(Over over = Over::Refuse) {
    if (!value()) return *this;
    inStr_ = true;
    strOver_ = over;
    pending_.clear();
    pendingSplices_ = 0;
    out_ += '"';
    return *this;
  }
  BodyWriter& part(const std::string& s) { return part(s.data(), s.size()); }
  BodyWriter& part(const char* s, size_t n) {
    if (!ok() || !inStr_) return fail(Err::InvalidArgument, "plain::body part");
    if (!s && n) return fail(Err::InvalidArgument, "plain::body part");
    pending_.append(s, n);
    return *this;
  }
  BodyWriter& splice(const uint8_t* bytes, size_t len, bool base64 = true) {
    if (!ok() || !inStr_ || !splices_ || !bytes || !len) return fail(Err::InvalidArgument, "plain::body splice");
    if (!flushSegment(Over::Refuse)) return *this;
    BodySplice sp;
    sp.at = out_.size();
    sp.bytes = bytes;
    sp.len = len;
    sp.base64 = base64;
    splices_->push_back(sp);
    ++pendingSplices_;
    return *this;
  }
  BodyWriter& endStr() {
    if (!ok() || !inStr_) return fail(Err::InvalidArgument, "plain::body endStr");
    if (!flushSegment(pendingSplices_ ? Over::Refuse : strOver_)) return *this;
    out_ += '"';
    inStr_ = false;
    count(strText_, false, false);
    strText_ = 0;
    return *this;
  }

  bool ok() const { return st_.ok(); }
  Status status() const {
    if (!st_.ok()) return st_;
    if (depth_ || inStr_ || afterKey_ || !top_) return Status::fail(Err::InvalidArgument, "plain::body unfinished");
    return Status::success();
  }
  const BodyStats& stats() const { return stats_; }
  size_t guard() const { return guard_; }

 private:
  BodyWriter& fail(Err e, const char* at) {
    if (st_.ok()) st_ = Status::fail(e, at);
    return *this;
  }
  bool ready() const { return st_.ok() && !inStr_; }
  bool inObject() const { return depth_ && obj_[depth_ - 1]; }

  // Before any value: the comma, the key rule, one top-level value.
  bool value() {
    if (!ready()) {
      fail(Err::InvalidArgument, "plain::body value");
      return false;
    }
    if (!depth_) {
      if (top_) {
        fail(Err::InvalidArgument, "plain::body second top-level value");
        return false;
      }
      top_ = true;
      return true;
    }
    if (obj_[depth_ - 1]) {
      if (!afterKey_) {
        fail(Err::InvalidArgument, "plain::body value without key");
        return false;
      }
      afterKey_ = false;
      return true;
    }
    if (count_[depth_ - 1]++) out_ += ',';
    return true;
  }

  BodyWriter& open(char c, bool object) {
    if (depth_ >= kMaxDepth) return fail(Err::TooLarge, "plain::body depth");
    if (!value()) return *this;
    out_ += c;
    obj_[depth_] = object;
    count_[depth_] = 0;
    ++depth_;
    return *this;
  }
  BodyWriter& close(char c, bool object) {
    if (!ready() || !depth_ || obj_[depth_ - 1] != object || afterKey_)
      return fail(Err::InvalidArgument, "plain::body close");
    out_ += c;
    --depth_;
    return *this;
  }

  void count(size_t textBytes, bool repaired, bool clipped) {
    ++stats_.strings;
    stats_.repaired += repaired;
    stats_.clipped += clipped;
    if (textBytes > stats_.longest) stats_.longest = textBytes;
  }

  // A whole string's text that needs the repair or the guard.
  BodyWriter& text(std::string& s, Over over) {
    const bool repaired = nimbus::utf8Repair(s) > 0;
    bool clipped = false;
    if (s.size() > guard_) {
      if (over == Over::Refuse) return fail(Err::TooLarge, "plain::body string over the guard");
      clipped = nimbus::utf8ClipNoted(s, guard_);
    }
    out_ += '"';
    detail::escapeInto(out_, s.data(), s.size());
    out_ += '"';
    count(s.size(), repaired, clipped);
    return *this;
  }

  // The pieces written since the last splice, under the rules; the guard counts the text
  // of the whole string, splices excluded.
  bool flushSegment(Over over) {
    const bool repaired = nimbus::utf8Repair(pending_) > 0;
    stats_.repaired += repaired;
    if (strText_ + pending_.size() > guard_) {
      if (over == Over::Refuse) {
        fail(Err::TooLarge, "plain::body string over the guard");
        return false;
      }
      nimbus::utf8ClipNoted(pending_, guard_ - strText_);
      ++stats_.clipped;
    }
    strText_ += pending_.size();
    detail::escapeInto(out_, pending_.data(), pending_.size());
    pending_.clear();
    return true;
  }

  // Where a raw fragment's walk is.
  struct RawWalk {
    uint8_t depth = 0;
    bool inString = false, closed = false, bad = false;
    size_t strBytes = 0;   // the current string's text bytes, at most
  };

  // One byte inside a fragment's string (an escape takes the byte after it too).
  bool rawStringByte(const std::string& j, size_t& i, RawWalk& w) {
    const char c = j[i];
    if (c == '\\') {
      if (i + 1 >= j.size()) {
        w.bad = true;
        return true;
      }
      out_ += c;
      out_ += j[++i];
      w.strBytes += detail::escapeTextBytes(j[i]);
    } else if (c == '"') {
      out_ += c;
      w.inString = false;
      count(w.strBytes, false, false);
    } else {
      detail::escapeInto(out_, &c, 1);
      ++w.strBytes;
    }
    if (w.strBytes <= guard_) return true;
    fail(Err::TooLarge, "plain::body raw string over the guard");
    return false;
  }

  static bool isJsonSpace(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

  // One byte between a fragment's strings.
  bool rawStructByte(char c, RawWalk& w) {
    if (isJsonSpace(c)) return true;
    if (w.closed || static_cast<unsigned char>(c) < 0x20 || (!w.depth && c != '{' && c != '[')) {
      w.bad = true;   // a second value, a raw control byte, or not an object or array
      return true;
    }
    if (c == '"') {
      w.inString = true;
      w.strBytes = 0;
    } else if (c == '{' || c == '[') {
      if (++w.depth > kMaxDepth) {
        fail(Err::TooLarge, "plain::body raw depth");
        return false;
      }
    } else if (c == '}' || c == ']') {
      w.closed = --w.depth == 0;
    }
    out_ += c;
    return true;
  }

  // A JSON object or array, compacted and escaped into out_. False (with the error kept)
  // when it is not one balanced value or holds a string over the guard.
  bool rawInto(const std::string& j) {
    RawWalk w;
    for (size_t i = 0; i < j.size() && !w.bad; ++i) {
      const bool ok = w.inString ? rawStringByte(j, i, w) : rawStructByte(j[i], w);
      if (!ok) return false;
    }
    if (w.bad || w.inString || w.depth || !w.closed) {
      fail(Err::InvalidArgument, "plain::body raw not one balanced JSON value");
      return false;
    }
    return true;
  }

  std::string& out_;
  BodySplices* splices_;
  size_t guard_;
  Status st_;
  uint8_t depth_ = 0;
  bool obj_[kMaxDepth] = {};
  uint32_t count_[kMaxDepth] = {};
  bool afterKey_ = false;
  bool top_ = false;
  // the open piecewise string
  bool inStr_ = false;
  Over strOver_ = Over::Refuse;
  std::string pending_;
  size_t strText_ = 0;
  uint32_t pendingSplices_ = 0;
  BodyStats stats_;
};

}  // namespace body
}  // namespace plain
}  // namespace agent
