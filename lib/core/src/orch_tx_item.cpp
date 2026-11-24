// The transcript item codec (tx/item.h): one item, one JSON line, crc32 last. Lane RB-T1.
//
// The encoder writes through a LineSink so the arena can encode straight into its PSRAM
// block with no allocation; the crc is computed on the way. The decoder is a cursor over
// the line with no recursion (a stored line cannot grow the tg_poll stack) and no
// allocation for keys, numbers or skipped values.
#include <algorithm>
#include <cstring>

#include "nimbus/orch/tx/item.h"

#include "nimbus/orch/day_start.h"

namespace nimbus {
namespace orch {
namespace tx {

namespace {

// ---- crc32 (IEEE, reflected), nibble table: 64 B of flash, no RAM -------------------

uint32_t crcUpdate(uint32_t c, const char* data, size_t len) {
  static const uint32_t kNibble[16] = {
      0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
      0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
  };
  for (size_t i = 0; i < len; ++i) {
    c ^= static_cast<uint8_t>(data[i]);
    c = (c >> 4) ^ kNibble[c & 15u];
    c = (c >> 4) ^ kNibble[c & 15u];
  }
  return c;
}

uint32_t crc32(const char* data, size_t len) { return ~crcUpdate(0xFFFFFFFFu, data, len); }

const char kHex[] = "0123456789abcdef";
const char kTailHead[] = ",\"z\":\"";   // then 8 hex digits, then "}
constexpr size_t kTailHeadBytes = sizeof kTailHead - 1;
static_assert(kTailHeadBytes + 8 + 2 == kLineTailBytes, "the tail is ,\"z\":\"xxxxxxxx\"}");

// ---- the writer: buffers small pieces, keeps the crc and the length -----------------

class Writer {
 public:
  explicit Writer(LineSink& out) : out_(out) {}
  void put(char c) {
    if (used_ == sizeof buf_) flush();
    buf_[used_++] = c;
  }
  void put(const char* p, size_t n) {
    if (n > sizeof buf_ - used_) {
      flush();
      if (n >= sizeof buf_) return emit(p, n);
    }
    std::memcpy(buf_ + used_, p, n);
    used_ += n;
  }
  // Close the line: the tail carries the crc of every byte before it.
  void finish() {
    flush();
    char tail[kLineTailBytes + 1];
    std::memcpy(tail, kTailHead, kTailHeadBytes);
    const uint32_t crc = ~crc_;
    for (int i = 0; i < 8; ++i) tail[kTailHeadBytes + i] = kHex[(crc >> (28 - 4 * i)) & 15u];
    tail[14] = '"';
    tail[15] = '}';
    tail[16] = '\n';
    out_.put(tail, sizeof tail);
    n_ += sizeof tail;
  }
  size_t length() const { return n_ + used_; }

 private:
  void flush() {
    if (used_) emit(buf_, used_);
    used_ = 0;
  }
  void emit(const char* p, size_t n) {
    crc_ = crcUpdate(crc_, p, n);
    n_ += n;
    out_.put(p, n);
  }
  LineSink& out_;
  uint32_t crc_ = 0xFFFFFFFFu;
  size_t n_ = 0;
  size_t used_ = 0;
  char buf_[64];
};

void putU64(Writer& w, uint64_t v) {
  char buf[20];
  size_t n = 0;
  do {
    buf[n++] = static_cast<char>('0' + v % 10);
    v /= 10;
  } while (v);
  while (n) w.put(buf[--n]);
}

void putI64(Writer& w, int64_t v) {
  if (v >= 0) return putU64(w, static_cast<uint64_t>(v));
  w.put('-');
  putU64(w, static_cast<uint64_t>(-(v + 1)) + 1);   // INT64_MIN safe
}

// The short escape letter for one byte, or 0 when it has none.
char shortEscape(uint8_t c) {
  switch (c) {
    case '"': return '"';
    case '\\': return '\\';
    case '\n': return 'n';
    case '\r': return 'r';
    case '\t': return 't';
    case '\b': return 'b';
    case '\f': return 'f';
  }
  return 0;
}

void putStr(Writer& w, const char* s, size_t n) {
  w.put('"');
  for (size_t i = 0; i < n; ++i) {
    const uint8_t c = static_cast<uint8_t>(s[i]);
    const char e = shortEscape(c);
    if (e) {
      w.put('\\');
      w.put(e);
    } else if (c < 0x20 || c == 0x7f) {
      w.put("\\u00", 4);
      w.put(kHex[c >> 4]);
      w.put(kHex[c & 15]);
    } else {
      w.put(static_cast<char>(c));
    }
  }
  w.put('"');
}

void putKey(Writer& w, const char* key) {
  w.put(',');
  w.put('"');
  w.put(key, std::strlen(key));
  w.put("\":", 2);
}

void fieldStr(Writer& w, const char* key, const std::string& v) {
  if (v.empty()) return;
  putKey(w, key);
  putStr(w, v.data(), v.size());
}
void fieldU64(Writer& w, const char* key, uint64_t v) {
  if (!v) return;
  putKey(w, key);
  putU64(w, v);
}
void fieldI64(Writer& w, const char* key, int64_t v) {
  if (!v) return;
  putKey(w, key);
  putI64(w, v);
}
void fieldFlag(Writer& w, const char* key, bool v) {
  if (!v) return;
  putKey(w, key);
  w.put("true", 4);
}
void fieldSlug(Writer& w, const char* key, const char* slug) {
  putKey(w, key);
  putStr(w, slug, std::strlen(slug));
}

template <class T, class Each>
void fieldList(Writer& w, const char* key, const std::vector<T>& v, Each each) {
  if (v.empty()) return;
  putKey(w, key);
  w.put('[');
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) w.put(',');
    each(v[i]);
  }
  w.put(']');
}

const char* partKindSlug(PartKind k) {
  switch (k) {
    case PartKind::Image: return "img";
    case PartKind::File: return "file";
    case PartKind::Voice: return "voice";
  }
  return "img";
}

// ---- per kind -----------------------------------------------------------------------

void encodeUser(Writer& w, const Item& it) {
  fieldStr(w, "x", it.x);
  fieldI64(w, "tz", it.tzMinutes);
  fieldStr(w, "ch", it.ch);
  fieldList(w, "src", it.src, [&](uint64_t v) { putU64(w, v); });
  fieldU64(w, "m", it.m);
  fieldList(w, "parts", it.parts, [&](const Part& p) {
    w.put("{\"k\":", 5);
    const char* slug = partKindSlug(p.kind);
    putStr(w, slug, std::strlen(slug));
    fieldStr(w, "ref", p.ref);
    fieldStr(w, "mime", p.mime);
    fieldU64(w, "b", p.bytes);
    fieldStr(w, "n", p.note);   // a photo's note (RB-H81)
    w.put('}');
  });
}

void encodeTurnOpen(Writer& w, const Item& it) {
  fieldStr(w, "tr", it.trigger);
  fieldStr(w, "hd", it.head);
  fieldStr(w, "pf", it.profile);
  fieldU64(w, "at", it.attempt);
  fieldList(w, "ph", it.photos, [&](const std::string& s) { putStr(w, s.data(), s.size()); });
}

void encodeResult(Writer& w, const Item& it) {
  fieldStr(w, "id", it.id);
  fieldStr(w, "n", it.n);
  if (!it.ok) {
    putKey(w, "ok");
    w.put("false", 5);
  }
  if (it.cls != ResultClass::Receipt) fieldSlug(w, "cls", resultClassSlug(it.cls));
  fieldStr(w, "x", it.x);
  fieldStr(w, "hdl", it.handle);
  fieldU64(w, "b", it.bytes);
  fieldU64(w, "l", it.lines);
  fieldStr(w, "st", it.st);
}

void encodeTurnEnd(Writer& w, const Item& it) {
  const TurnEndFacts& e = it.end;
  fieldStr(w, "cause", e.cause);
  fieldU64(w, "rds", e.rounds);
  fieldU64(w, "tools", e.tools);
  fieldU64(w, "hr", e.headRequests);
  fieldU64(w, "pr", e.probeRequests);
  fieldU64(w, "pt", e.promptTokens);
  fieldU64(w, "ot", e.outputTokens);
  fieldU64(w, "cr", e.cacheReadTokens);
  fieldU64(w, "cw", e.cacheWriteTokens);
  fieldU64(w, "usd", e.microUsd);
  fieldU64(w, "ms", e.ms);
  fieldList(w, "ex", e.exchanges, [&](const ExchangeFacts& x) {
    w.put('[');
    putU64(w, x.requestBytes);
    w.put(',');
    putU64(w, x.ms);
    w.put(',');
    putU64(w, x.httpStatus);
    if (x.bodyHash) {   // the request by reference, by position; trailing defaults are left out
      const uint64_t nums[] = {x.fromQ, x.thruQ, x.items, x.bodyHash, x.maxTokens, x.stub, x.toolChoiceNone ? 1u : 0u,
                               x.closedThrough, x.stubThrough, x.omitThrough};
      constexpr size_t kNums = sizeof(nums) / sizeof(nums[0]);
      const bool strs = !x.omission.empty() || !x.effort.empty() || x.dropped;   // the dropped mask rides last
      size_t n = strs ? kNums : 4;
      while (n < kNums && std::any_of(nums + n, nums + kNums, [](uint64_t v) { return v != 0; })) ++n;
      for (size_t i = 0; i < n; ++i) {
        w.put(',');
        putU64(w, nums[i]);
      }
      if (strs) {
        w.put(',');
        putStr(w, x.effort.data(), x.effort.size());
      }
      if (!x.omission.empty() || x.dropped) {
        w.put(',');
        putStr(w, x.omission.data(), x.omission.size());
      }
      if (x.dropped) {
        w.put(',');
        putU64(w, x.dropped);
      }
    }
    w.put(']');
  });
  fieldList(w, "ev", e.events, [&](const std::string& s) { putStr(w, s.data(), s.size()); });
  fieldU64(w, "ph", e.promptHash);
  fieldU64(w, "mh", e.manifestHash);
  fieldU64(w, "lp", e.lastPromptTokens);
}

void encodeRoundKinds(Writer& w, const Item& it) {
  switch (it.kind) {
    case ItemKind::Assistant:
      fieldStr(w, "x", it.x);
      fieldU64(w, "rd", it.rd);
      fieldFlag(w, "fin", it.fin);
      fieldStr(w, "hdl", it.handle);   // a text over the message cap: where its whole is (RB-H81)
      return;
    case ItemKind::Call:
      fieldStr(w, "id", it.id);
      fieldStr(w, "n", it.n);
      fieldStr(w, "args", it.args);
      fieldFlag(w, "eff", it.eff);
      fieldStr(w, "st", it.st);   // long arguments: their stub form, shown once the turn closed (RB-H81)
      fieldU64(w, "rd", it.rd);
      return;
    case ItemKind::Intent:
      fieldStr(w, "id", it.id);
      fieldStr(w, "n", it.n);
      return;
    case ItemKind::RoundMeta:
      fieldStr(w, "x", it.x);
      fieldU64(w, "rd", it.rd);
      return;
    default:
      return;
  }
}

void encodeFields(Writer& w, const Item& it) {
  switch (it.kind) {
    case ItemKind::User: return encodeUser(w, it);
    case ItemKind::TurnOpen: return encodeTurnOpen(w, it);
    case ItemKind::Result: return encodeResult(w, it);
    case ItemKind::TurnEnd: return encodeTurnEnd(w, it);
    case ItemKind::DeviceNote:
      fieldStr(w, "x", it.x);
      fieldSlug(w, "cls", noteClassSlug(it.note));
      return;
    case ItemKind::Digest:
      fieldStr(w, "x", it.x);
      fieldStr(w, "day", it.day);
      fieldU64(w, "thru", it.thru);
      fieldFlag(w, "ext", it.ext);
      return;
    case ItemKind::PointerMove:
      fieldU64(w, "stub", it.stub);
      fieldStr(w, "why", it.why);
      fieldList(w, "sup", it.sup, [&](uint64_t v) { putU64(w, v); });
      return;
    case ItemKind::Delivery:
      fieldList(w, "ids", it.msgIds, [&](int64_t v) { putI64(w, v); });
      return;
    default: return encodeRoundKinds(w, it);
  }
}

bool knownKind(ItemKind k) {
  ItemKind back;
  return kindFromLetter(kindLetter(k), back);
}

// What a decoder accepts: a known kind, q from 1, every list within kMaxListItems.
bool encodable(const Item& it) {
  const size_t lists[] = {it.src.size(),    it.parts.size(),         it.photos.size(),    it.sup.size(),
                          it.msgIds.size(), it.end.exchanges.size(), it.end.events.size()};
  for (size_t n : lists)
    if (n > kMaxListItems) return false;
  return knownKind(it.kind) && it.q != 0;
}

// Keeps at most kMaxLineBytes: a far too long item costs no more than one line of memory.
struct StringSink final : LineSink {
  std::string& s;
  explicit StringSink(std::string& out) : s(out) {}
  void put(const char* p, size_t n) override {
    const size_t room = kMaxLineBytes - (s.size() < kMaxLineBytes ? s.size() : kMaxLineBytes);
    s.append(p, n < room ? n : room);
  }
};

}  // namespace

Status encodeLineTo(const Item& item, LineSink& sink, size_t& lineBytes) {
  lineBytes = 0;
  if (!encodable(item)) return Status::fail(Err::InvalidArgument, "tx::encodeLine");
  Writer w(sink);
  w.put("{\"k\":\"", 6);
  w.put(kindLetter(item.kind));
  w.put("\",\"q\":", 6);
  putU64(w, item.q);
  fieldU64(w, "turn", item.turn);
  fieldU64(w, "t", item.t);
  encodeFields(w, item);
  w.finish();
  lineBytes = w.length();
  return lineBytes <= kMaxLineBytes ? Status::success() : Status::fail(Err::TooLarge, "tx::encodeLine");
}

Status encodeLine(const Item& item, std::string& out) {
  out.clear();
  StringSink sink(out);
  size_t n = 0;
  const Status st = encodeLineTo(item, sink, n);
  if (!st.ok()) out.clear();
  return st;
}

// ==== decoding ==========================================================================

namespace {

struct Cursor {
  const char* p;
  const char* end;
  bool more() const { return p < end; }
  char peek() const { return p < end ? *p : '\0'; }
  bool eat(char c) {
    if (p >= end || *p != c) return false;
    ++p;
    return true;
  }
};

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool read4(Cursor& c, uint32_t& v) {
  if (c.end - c.p < 4) return false;
  v = 0;
  for (int i = 0; i < 4; ++i) {
    const int h = hexVal(*c.p++);
    if (h < 0) return false;
    v = (v << 4) | static_cast<uint32_t>(h);
  }
  return true;
}

// A \u escape (the 'u' already eaten) as one code point; surrogate pairs joined.
bool readCodePoint(Cursor& c, uint32_t& cp) {
  if (!read4(c, cp)) return false;
  if (cp >= 0xDC00 && cp <= 0xDFFF) return false;
  if (cp < 0xD800 || cp > 0xDBFF) return true;
  uint32_t lo = 0;
  if (!c.eat('\\') || !c.eat('u') || !read4(c, lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
  cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
  return true;
}

template <class Out>
void putUtf8(Out& out, uint32_t cp) {
  if (cp < 0x80) return out(static_cast<char>(cp));
  if (cp < 0x800) {
    out(static_cast<char>(0xC0 | (cp >> 6)));
  } else if (cp < 0x10000) {
    out(static_cast<char>(0xE0 | (cp >> 12)));
    out(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
  } else {
    out(static_cast<char>(0xF0 | (cp >> 18)));
    out(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
  }
  out(static_cast<char>(0x80 | (cp & 0x3F)));
}

char unescapeShort(char e) {
  switch (e) {
    case '"': return '"';
    case '\\': return '\\';
    case '/': return '/';
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    case 'b': return '\b';
    case 'f': return '\f';
  }
  return 0;
}

bool readEscape(Cursor& c, uint32_t& cp) {
  if (!c.more()) return false;
  const char e = *c.p++;
  if (e == 'u') return readCodePoint(c, cp);
  cp = static_cast<uint8_t>(unescapeShort(e));
  return cp != 0;
}

// A JSON string; every byte goes to `out(char)`. Raw control bytes are refused.
template <class Out>
bool readString(Cursor& c, Out out) {
  if (!c.eat('"')) return false;
  while (c.more()) {
    const char ch = *c.p++;
    if (ch == '"') return true;
    if (static_cast<uint8_t>(ch) < 0x20) return false;
    if (ch != '\\') {
      out(ch);
      continue;
    }
    uint32_t cp = 0;
    if (!readEscape(c, cp)) return false;
    putUtf8(out, cp);
  }
  return false;
}

bool readStr(Cursor& c, std::string& s) {
  s.clear();
  return readString(c, [&](char ch) { s.push_back(ch); });
}

bool skipString(Cursor& c) {
  return readString(c, [](char) {});
}

bool readU64(Cursor& c, uint64_t& v) {
  v = 0;
  const char* start = c.p;
  while (c.more() && *c.p >= '0' && *c.p <= '9') {
    const uint64_t d = static_cast<uint64_t>(*c.p - '0');
    if (v > (UINT64_MAX - d) / 10) return false;
    v = v * 10 + d;
    ++c.p;
  }
  return c.p != start && !(c.p - start > 1 && *start == '0');
}

bool readI64(Cursor& c, int64_t& v) {
  const bool neg = c.eat('-');
  uint64_t m = 0;
  if (!readU64(c, m)) return false;
  const uint64_t lim = neg ? static_cast<uint64_t>(INT64_MAX) + 1 : static_cast<uint64_t>(INT64_MAX);
  if (m > lim) return false;
  v = neg ? static_cast<int64_t>(0 - m) : static_cast<int64_t>(m);
  return true;
}

template <class T>
bool readUnsigned(Cursor& c, T& out) {
  uint64_t v = 0;
  if (!readU64(c, v) || v > static_cast<uint64_t>(static_cast<T>(~T(0)))) return false;
  out = static_cast<T>(v);
  return true;
}

bool readBool(Cursor& c, bool& b) {
  if (c.end - c.p >= 4 && std::memcmp(c.p, "true", 4) == 0) {
    c.p += 4;
    b = true;
    return true;
  }
  if (c.end - c.p >= 5 && std::memcmp(c.p, "false", 5) == 0) {
    c.p += 5;
    b = false;
    return true;
  }
  return false;
}

// Skip one scalar (number, true, false, null): its bytes up to the next , ] or }.
bool skipScalar(Cursor& c) {
  const char* start = c.p;
  while (c.more() && *c.p != ',' && *c.p != ']' && *c.p != '}' && static_cast<uint8_t>(*c.p) > 0x20 &&
         *c.p != '"' && *c.p != '[' && *c.p != '{' && *c.p != ':')
    ++c.p;
  return c.p != start;
}

// Skip one value of any shape without recursion. Strings and matching brackets are
// checked, not the full grammar; nesting deeper than kSkipDepth is refused (no line of
// ours nests more than 3).
constexpr size_t kSkipDepth = 64;

bool skipClose(Cursor& c, uint64_t& opened, size_t& depth) {
  if (!depth) return false;
  const bool wantObject = (opened >> (depth - 1)) & 1u;
  if ((*c.p == '}') != wantObject) return false;
  --depth;
  ++c.p;
  return true;
}

bool skipValue(Cursor& c) {
  uint64_t opened = 0;   // bit d: the bracket at depth d is an object
  size_t depth = 0;
  do {
    const char ch = c.peek();
    if (ch == '{' || ch == '[') {
      if (depth == kSkipDepth) return false;
      opened = (opened & ~(uint64_t(1) << depth)) | (uint64_t(ch == '{') << depth);
      ++depth;
      ++c.p;
      continue;
    }
    if (ch == '}' || ch == ']') {
      if (!skipClose(c, opened, depth)) return false;
    } else if (ch == ',' || ch == ':') {
      if (!depth) return false;
      ++c.p;
      continue;
    } else if (!(ch == '"' ? skipString(c) : skipScalar(c))) {
      return false;
    }
  } while (depth && c.more());
  return depth == 0;
}

// A short key into `buf`; `fits` is false when it is longer than the buffer or holds a
// NUL (a key no rule names: the caller skips its value).
bool readKey(Cursor& c, char* buf, size_t cap, bool& fits) {
  size_t n = 0;
  fits = true;
  const bool ok = readString(c, [&](char ch) {
    if (ch && n + 1 < cap)
      buf[n++] = ch;
    else
      fits = false;
  });
  buf[n] = '\0';
  return ok;
}

// '[' each (',' each)* ']' with `each(cursor)` reading one element; at most kMaxListItems.
template <class Each>
bool readArray(Cursor& c, Each each) {
  if (!c.eat('[')) return false;
  if (c.eat(']')) return true;
  size_t n = 0;
  do {
    if (++n > kMaxListItems || !each(c)) return false;
  } while (c.eat(','));
  return c.eat(']');
}

// '{' "key":value (',' ...)* '}' with `field(key, fits, cursor)` reading one value; every
// key reaches `field`, so it can count them.
template <class Field>
bool readObject(Cursor& c, Field field) {
  if (!c.eat('{')) return false;
  if (c.eat('}')) return true;
  do {
    char key[8];
    bool fits = true;
    if (!readKey(c, key, sizeof key, fits) || !c.eat(':') || !field(key, fits, c)) return false;
  } while (c.eat(','));
  return c.eat('}');
}

// A slug among `n` values of an enum whose slug function is `slugOf`; `fallback` when the
// slug is unknown (a newer writer), or false to refuse it.
template <class E>
bool readSlug(Cursor& c, E& out, const char* (*slugOf)(E), uint8_t n, const E* fallback) {
  char slug[12];
  bool fits = true;
  if (!readKey(c, slug, sizeof slug, fits)) return false;
  for (uint8_t i = 0; fits && i < n; ++i)
    if (std::strcmp(slug, slugOf(static_cast<E>(i))) == 0) {
      out = static_cast<E>(i);
      return true;
    }
  if (fallback) out = *fallback;
  return fallback != nullptr;
}

bool readKindLetter(Cursor& c, ItemKind& kind) {
  char letter[3];
  bool fits = true;
  if (!readKey(c, letter, sizeof letter, fits) || !fits || std::strlen(letter) != 1) return false;
  return kindFromLetter(letter[0], kind);
}

// ---- the item's fields ----------------------------------------------------------------

struct Decoding {
  Item& it;
  size_t fields = 0;
  bool haveKind = false;
  bool haveQ = false;
};

// Kind masks: a key is read only for the kinds that write it (tx/item.h key table).
constexpr uint16_t bit(ItemKind k) {
  for (size_t i = 0; i < kItemKindCount; ++i)
    if (kAllItemKinds[i] == k) return static_cast<uint16_t>(1u << i);
  return 0;
}
constexpr uint16_t U = bit(ItemKind::User), T = bit(ItemKind::TurnOpen), A = bit(ItemKind::Assistant),
                   C = bit(ItemKind::Call), I = bit(ItemKind::Intent), R = bit(ItemKind::Result),
                   K = bit(ItemKind::RoundMeta), H = bit(ItemKind::DeviceNote), S = bit(ItemKind::Digest),
                   X = bit(ItemKind::PointerMove), D = bit(ItemKind::Delivery), E = bit(ItemKind::TurnEnd);

using FieldFn = bool (*)(Cursor&, Decoding&);
// The member is not named `read`: the tg_poll stack gate ties a call through a pointer member to
// the virtual methods of that name at the same offset (tx::RowSource::read), an edge that cannot run.
struct FieldRule {
  const char* key;
  uint16_t kinds;
  FieldFn decodeField;
};

bool readPart(Cursor& c, Part& p) {
  bool haveKind = false;
  return readObject(c, [&](const char* key, bool fits, Cursor& cc) {
           if (!fits) return skipValue(cc);
           if (!std::strcmp(key, "k"))
             return haveKind = readSlug<PartKind>(cc, p.kind, partKindSlug, 3, nullptr);
           if (!std::strcmp(key, "ref")) return readStr(cc, p.ref);
           if (!std::strcmp(key, "mime")) return readStr(cc, p.mime);
           if (!std::strcmp(key, "b")) return readUnsigned(cc, p.bytes);
           if (!std::strcmp(key, "n")) return readStr(cc, p.note);
           return skipValue(cc);
         }) &&
         haveKind;
}

// The request by reference, by position after the three figures (RB-E3).
bool readExchangeRef(Cursor& cc, int n, ExchangeFacts& x) {
  switch (n) {
    case 3: return readUnsigned(cc, x.fromQ);
    case 4: return readUnsigned(cc, x.thruQ);
    case 5: return readUnsigned(cc, x.items);
    case 6: return readUnsigned(cc, x.bodyHash);
    case 7: return readUnsigned(cc, x.maxTokens);
    case 8: return readUnsigned(cc, x.stub);
    case 9: {
      uint8_t none = 0;
      if (!readUnsigned(cc, none) || none > 1) return false;
      x.toolChoiceNone = none == 1;
      return true;
    }
    case 10: return readUnsigned(cc, x.closedThrough);
    case 11: return readUnsigned(cc, x.stubThrough);
    case 12: return readUnsigned(cc, x.omitThrough);
    case 13: return readStr(cc, x.effort);
    case 14: return readStr(cc, x.omission);
    case 15: return readUnsigned(cc, x.dropped);
    default: return skipValue(cc);
  }
}

bool readExchange(Cursor& c, ExchangeFacts& x) {
  int n = 0;
  const bool ok = readArray(c, [&](Cursor& cc) {
                    switch (n++) {
                      case 0: return readUnsigned(cc, x.requestBytes);
                      case 1: return readUnsigned(cc, x.ms);
                      case 2: return readUnsigned(cc, x.httpStatus);
                      default: return readExchangeRef(cc, n - 1, x);
                    }
                  }) &&
                  n >= 3;
  // bodyHash 0 means no reference was recorded (the encoder writes none): a line that says
  // otherwise reads as the three figures alone, so it re-encodes to what it decodes to.
  if (ok && !x.bodyHash) x = ExchangeFacts{x.requestBytes, x.ms, x.httpStatus};
  return ok;
}

template <class T, class One>
bool readList(Cursor& c, std::vector<T>& v, One one) {
  v.clear();
  return readArray(c, [&](Cursor& cc) {
    v.emplace_back();
    return one(cc, v.back());
  });
}

bool readTz(Cursor& c, Decoding& d) {
  int64_t v = 0;
  if (!readI64(c, v) || v < INT16_MIN || v > INT16_MAX) return false;
  d.it.tzMinutes = static_cast<int16_t>(v);
  return true;
}

bool readQ(Cursor& c, Decoding& d) {
  d.haveQ = readU64(c, d.it.q) && d.it.q != 0;
  return d.haveQ;
}

const ResultClass kOutside = ResultClass::Outside;   // an unknown class is never shown whole
const NoteClass kUnknownNote = NoteClass::Unknown;
constexpr uint8_t kNoteClasses = static_cast<uint8_t>(NoteClass::Gap) + 1;

#define TX_STR(k, m, f) {k, m, [](Cursor& c, Decoding& d) { return readStr(c, d.it.f); }}
#define TX_NUM(k, m, f) {k, m, [](Cursor& c, Decoding& d) { return readUnsigned(c, d.it.f); }}
#define TX_BOOL(k, m, f) {k, m, [](Cursor& c, Decoding& d) { return readBool(c, d.it.f); }}
#define TX_LIST(k, m, f, one) {k, m, [](Cursor& c, Decoding& d) { return readList(c, d.it.f, one); }}

constexpr uint16_t kAll = (1u << kItemKindCount) - 1;

const FieldRule kFields[] = {
    {"q", kAll, readQ},
    TX_NUM("turn", kAll, turn),
    TX_NUM("t", kAll, t),
    TX_STR("x", U | A | R | K | H | S, x),
    {"tz", U, readTz},
    TX_STR("ch", U, ch),
    TX_LIST("src", U, src, readU64),
    TX_NUM("m", U, m),
    TX_LIST("parts", U, parts, readPart),
    TX_STR("tr", T, trigger),
    TX_STR("hd", T, head),
    TX_STR("pf", T, profile),
    TX_NUM("at", T, attempt),
    TX_LIST("ph", T, photos, readStr),
    TX_NUM("rd", A | C | K, rd),
    TX_BOOL("fin", A, fin),
    TX_STR("id", C | I | R, id),
    TX_STR("n", C | I | R, n),
    TX_STR("args", C, args),
    TX_BOOL("eff", C, eff),
    TX_BOOL("ok", R, ok),
    {"cls", R, [](Cursor& c, Decoding& d) { return readSlug<ResultClass>(c, d.it.cls, resultClassSlug, 3, &kOutside); }},
    {"cls", H, [](Cursor& c, Decoding& d) { return readSlug<NoteClass>(c, d.it.note, noteClassSlug, kNoteClasses, &kUnknownNote); }},
    TX_STR("hdl", A | R, handle),
    TX_NUM("b", R, bytes),
    TX_NUM("l", R, lines),
    TX_STR("st", C | R, st),
    TX_STR("day", S, day),
    TX_NUM("thru", S, thru),
    TX_BOOL("ext", S, ext),
    TX_NUM("stub", X, stub),
    TX_STR("why", X, why),
    TX_LIST("sup", X, sup, readU64),
    TX_LIST("ids", D, msgIds, readI64),
    TX_STR("cause", E, end.cause),
    TX_NUM("rds", E, end.rounds),
    TX_NUM("tools", E, end.tools),
    TX_NUM("hr", E, end.headRequests),
    TX_NUM("pr", E, end.probeRequests),
    TX_NUM("pt", E, end.promptTokens),
    TX_NUM("ot", E, end.outputTokens),
    TX_NUM("cr", E, end.cacheReadTokens),
    TX_NUM("cw", E, end.cacheWriteTokens),
    TX_NUM("usd", E, end.microUsd),
    TX_NUM("ms", E, end.ms),
    TX_LIST("ex", E, end.exchanges, readExchange),
    TX_LIST("ev", E, end.events, readStr),
    TX_NUM("ph", E, end.promptHash),
    TX_NUM("mh", E, end.manifestHash),
    TX_NUM("lp", E, end.lastPromptTokens),
};

#undef TX_STR
#undef TX_NUM
#undef TX_BOOL
#undef TX_LIST

// k first, then any key; a key the item's kind does not write is skipped like an unknown one.
bool readItemField(const char* key, bool fits, Cursor& c, Decoding& d) {
  if (++d.fields == 1) return fits && !std::strcmp(key, "k") && (d.haveKind = readKindLetter(c, d.it.kind));
  if (fits && !std::strcmp(key, "k")) return false;   // one k
  const uint16_t mine = bit(d.it.kind);
  for (const FieldRule& r : kFields)
    if (fits && (r.kinds & mine) && !std::strcmp(key, r.key)) return r.decodeField(c, d);
  return skipValue(c);
}

// Every field back to its default, in place: no Item temporary on the stack (about 480 B
// on the device) and the strings keep their capacity for the next decode.
void clearItem(Item& it) {
  it.q = 0, it.kind = ItemKind::User, it.turn = 0, it.t = 0;
  it.x.clear(), it.ch.clear(), it.src.clear(), it.parts.clear();
  it.tzMinutes = 0, it.m = 0;
  it.trigger.clear(), it.head.clear(), it.profile.clear(), it.photos.clear();
  it.attempt = 0, it.rd = 0, it.fin = false;
  it.id.clear(), it.n.clear(), it.args.clear(), it.handle.clear(), it.st.clear();
  it.eff = false, it.ok = true, it.cls = ResultClass::Receipt, it.bytes = 0, it.lines = 0;
  it.note = NoteClass::Unknown;
  it.day.clear(), it.why.clear(), it.sup.clear(), it.msgIds.clear();
  it.thru = 0, it.ext = false, it.stub = 0;
  TurnEndFacts& e = it.end;
  e.cause.clear(), e.exchanges.clear(), e.events.clear();
  e.rounds = e.tools = e.headRequests = e.probeRequests = 0;
  e.promptTokens = e.outputTokens = e.cacheReadTokens = e.cacheWriteTokens = e.cacheWrite1hTokens = 0;
  e.microUsd = e.ms = e.promptHash = e.manifestHash = e.lastPromptTokens = 0;
}

// The 8 lowercase hex digits the encoder writes (an upper-case digit is not ours: torn).
bool readCrc(const char* hex, uint32_t& v) {
  v = 0;
  for (int i = 0; i < 8; ++i) {
    const char h = hex[i];
    if (h >= 'A' && h <= 'F') return false;
    const int d = hexVal(h);
    if (d < 0) return false;
    v = (v << 4) | static_cast<uint32_t>(d);
  }
  return true;
}

// The framing: one optional newline, the fixed tail, the crc. Sets `len` to the line
// without its newline.
Status frame(const char* data, size_t& len, const char* where) {
  if (!data) return Status::fail(Err::InvalidArgument, where);
  if (len && data[len - 1] == '\n') --len;
  if (len >= kMaxLineBytes || len < kLineTailBytes + 2) return Status::fail(Err::Torn, where);
  const char* tail = data + len - kLineTailBytes;
  uint32_t want = 0;
  const bool shaped = std::memcmp(tail, kTailHead, kTailHeadBytes) == 0 && tail[14] == '"' && tail[15] == '}';
  if (!shaped || !readCrc(tail + kTailHeadBytes, want) || crc32(data, len - kLineTailBytes) != want)
    return Status::fail(Err::Torn, where);
  return Status::success();
}

}  // namespace

Status decodeLine(const char* data, size_t len, Item& out) {
  clearItem(out);
  Status st = frame(data, len, "tx::decodeLine");
  if (!st.ok()) return st;
  Cursor c{data, data + len};
  Decoding d{out};
  const bool parsed =
      readObject(c, [&](const char* key, bool fits, Cursor& cc) { return readItemField(key, fits, cc, d); });
  if (!parsed || c.more() || !d.haveKind || !d.haveQ) return Status::fail(Err::InvalidArgument, "tx::decodeLine");
  return Status::success();
}

Status checkLine(const char* data, size_t len, LineHeader& out) {
  out = LineHeader{};
  Status st = frame(data, len, "tx::checkLine");
  if (!st.ok()) return st;
  Cursor c{data, data + len};
  size_t fields = 0;
  bool haveKind = false;
  const bool parsed = readObject(c, [&](const char* key, bool fits, Cursor& cc) {
    if (++fields == 1) return haveKind = fits && !std::strcmp(key, "k") && readKindLetter(cc, out.kind);
    if (!fits) return skipValue(cc);
    if (!std::strcmp(key, "k")) return false;
    if (!std::strcmp(key, "q")) return readU64(cc, out.q);
    if (!std::strcmp(key, "turn")) return readUnsigned(cc, out.turn);
    if (!std::strcmp(key, "t")) return readU64(cc, out.t);
    return skipValue(cc);
  });
  if (!parsed || c.more() || !haveKind || !out.q) return Status::fail(Err::InvalidArgument, "tx::checkLine");
  return Status::success();
}

// ==== the message stamp (Ruling 11, RB-REC5) ==============================================

Status messageStamp(uint64_t epoch, int16_t tzMinutes, uint16_t dayStartMin, char* out, size_t cap) {
  if (!out || cap < kStampMaxBytes) return Status::fail(Err::InvalidArgument, "tx::messageStamp");
  out[0] = '[';
  const day::StampForm f = day::stamp(epoch, tzMinutes, dayStartMin, out + 1, cap - 2);
  if (f == day::StampForm::None) {
    out[0] = '\0';
    return Status::fail(Err::InvalidArgument, "tx::messageStamp");
  }
  const size_t n = std::strlen(out);
  out[n] = ']';
  out[n + 1] = '\0';
  return Status::success();
}

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
