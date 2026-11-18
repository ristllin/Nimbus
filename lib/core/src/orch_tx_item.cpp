// The transcript item codec (tx/item.h): one item, one JSON line, crc32 last. Lane RB-T1.
//
// The encoder writes through a LineSink so the arena can encode straight into its PSRAM
// block with no allocation; the crc is computed on the way. The decoder is a cursor over
// the line with no recursion (a stored line cannot grow the tg_poll stack) and no
// allocation for keys, numbers or skipped values.
#include <cstring>

#include "nimbus/orch/tx/item.h"

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
const char kTailHead[] = ",\"z\":\"";   // 6 bytes, then 8 hex, then "}

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
    std::memcpy(tail, kTailHead, 6);
    const uint32_t crc = ~crc_;
    for (int i = 0; i < 8; ++i) tail[6 + i] = kHex[(crc >> (28 - 4 * i)) & 15u];
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
    w.put(']');
  });
  fieldList(w, "ev", e.events, [&](const std::string& s) { putStr(w, s.data(), s.size()); });
  fieldU64(w, "ph", e.promptHash);
  fieldU64(w, "mh", e.manifestHash);
}

void encodeRoundKinds(Writer& w, const Item& it) {
  switch (it.kind) {
    case ItemKind::Assistant:
      fieldStr(w, "x", it.x);
      fieldU64(w, "rd", it.rd);
      fieldFlag(w, "fin", it.fin);
      return;
    case ItemKind::Call:
      fieldStr(w, "id", it.id);
      fieldStr(w, "n", it.n);
      fieldStr(w, "args", it.args);
      fieldFlag(w, "eff", it.eff);
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

struct StringSink final : LineSink {
  std::string& s;
  explicit StringSink(std::string& out) : s(out) {}
  void put(const char* p, size_t n) override { s.append(p, n); }
};

}  // namespace

Status encodeLineTo(const Item& item, LineSink& sink, size_t& lineBytes) {
  lineBytes = 0;
  if (!knownKind(item.kind)) return Status::fail(Err::InvalidArgument, "tx::encodeLine");
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
         *c.p != '"' && *c.p != '[' && *c.p != '{')
    ++c.p;
  return c.p != start;
}

// Skip one value of any shape, nested to any depth, without recursion.
bool skipValue(Cursor& c) {
  size_t depth = 0;
  do {
    const char ch = c.peek();
    if (ch == '"') {
      if (!skipString(c)) return false;
    } else if (ch == '{' || ch == '[') {
      ++depth;
      ++c.p;
      continue;
    } else if (ch == '}' || ch == ']') {
      if (!depth) return false;
      --depth;
      ++c.p;
    } else if (ch == ',' || ch == ':') {
      if (!depth) return false;
      ++c.p;
      continue;
    } else if (!skipScalar(c)) {
      return false;
    }
  } while (depth && c.more());
  return depth == 0;
}

// A short key into `buf`; `fits` is false when it is longer than the buffer (an unknown
// key, skipped by the caller).
bool readKey(Cursor& c, char* buf, size_t cap, bool& fits) {
  size_t n = 0;
  fits = true;
  const bool ok = readString(c, [&](char ch) {
    if (n + 1 < cap)
      buf[n++] = ch;
    else
      fits = false;
  });
  buf[n] = '\0';
  return ok;
}

// '[' each (',' each)* ']' with `each(cursor)` reading one element.
template <class Each>
bool readArray(Cursor& c, Each each) {
  if (!c.eat('[')) return false;
  if (c.eat(']')) return true;
  do {
    if (!each(c)) return false;
  } while (c.eat(','));
  return c.eat(']');
}

// '{' "key":value (',' ...)* '}' with `field(key, fits, cursor)` reading one value.
template <class Field>
bool readObject(Cursor& c, Field field) {
  if (!c.eat('{')) return false;
  if (c.eat('}')) return true;
  do {
    char key[8];
    bool fits = true;
    if (!readKey(c, key, sizeof key, fits) || !c.eat(':')) return false;
    if (!(fits ? field(key, c) : skipValue(c))) return false;
  } while (c.eat(','));
  return c.eat('}');
}

// ---- the item's fields ----------------------------------------------------------------

struct Decoding {
  Item& it;
  bool haveKind = false;
  size_t fields = 0;
};

using FieldFn = bool (*)(Cursor&, Decoding&);
struct FieldRule {
  const char* key;
  FieldFn read;
};

bool readKind(Cursor& c, Decoding& d) {
  char letter[3];
  bool fits = true;
  if (d.fields != 1 || !readKey(c, letter, sizeof letter, fits) || !fits || std::strlen(letter) != 1) return false;
  d.haveKind = kindFromLetter(letter[0], d.it.kind);
  return d.haveKind;
}

bool readResultClass(Cursor& c, ResultClass& out) {
  char slug[8];
  bool fits = true;
  if (!readKey(c, slug, sizeof slug, fits)) return false;
  // An unknown class reads as outside content: never shown whole after its turn.
  out = ResultClass::Outside;
  for (ResultClass k : {ResultClass::Receipt, ResultClass::Bulk, ResultClass::Outside})
    if (fits && std::strcmp(slug, resultClassSlug(k)) == 0) out = k;
  return true;
}

bool readNoteClass(Cursor& c, NoteClass& out) {
  char slug[12];
  bool fits = true;
  if (!readKey(c, slug, sizeof slug, fits)) return false;
  out = NoteClass::Unknown;
  for (uint8_t i = 0; i <= static_cast<uint8_t>(NoteClass::Gap); ++i)
    if (fits && std::strcmp(slug, noteClassSlug(static_cast<NoteClass>(i))) == 0) out = static_cast<NoteClass>(i);
  return true;
}

bool readCls(Cursor& c, Decoding& d) {
  if (d.it.kind == ItemKind::DeviceNote) return readNoteClass(c, d.it.note);
  return readResultClass(c, d.it.cls);
}

bool readPartKind(Cursor& c, PartKind& out) {
  char slug[8];
  bool fits = true;
  if (!readKey(c, slug, sizeof slug, fits)) return false;
  for (PartKind k : {PartKind::Image, PartKind::File, PartKind::Voice})
    if (fits && std::strcmp(slug, partKindSlug(k)) == 0) {
      out = k;
      return true;
    }
  return false;
}

bool readPart(Cursor& c, Part& p) {
  return readObject(c, [&](const char* key, Cursor& cc) {
    if (!std::strcmp(key, "k")) return readPartKind(cc, p.kind);
    if (!std::strcmp(key, "ref")) return readStr(cc, p.ref);
    if (!std::strcmp(key, "mime")) return readStr(cc, p.mime);
    if (!std::strcmp(key, "b")) return readUnsigned(cc, p.bytes);
    return skipValue(cc);
  });
}

bool readExchange(Cursor& c, ExchangeFacts& x) {
  int n = 0;
  return readArray(c, [&](Cursor& cc) {
    switch (n++) {
      case 0: return readUnsigned(cc, x.requestBytes);
      case 1: return readUnsigned(cc, x.ms);
      case 2: return readUnsigned(cc, x.httpStatus);
      default: return skipValue(cc);
    }
  }) && n >= 3;
}

bool readStrings(Cursor& c, std::vector<std::string>& v) {
  v.clear();
  return readArray(c, [&](Cursor& cc) {
    v.emplace_back();
    return readStr(cc, v.back());
  });
}

bool readU64s(Cursor& c, std::vector<uint64_t>& v) {
  v.clear();
  return readArray(c, [&](Cursor& cc) {
    v.push_back(0);
    return readU64(cc, v.back());
  });
}

bool readPhotosOrHash(Cursor& c, Decoding& d) {
  if (d.it.kind == ItemKind::TurnEnd) return readUnsigned(c, d.it.end.promptHash);
  return readStrings(c, d.it.photos);
}

bool readTz(Cursor& c, Decoding& d) {
  int64_t v = 0;
  if (!readI64(c, v) || v < INT16_MIN || v > INT16_MAX) return false;
  d.it.tzMinutes = static_cast<int16_t>(v);
  return true;
}

#define TX_STR(k, f) {k, [](Cursor& c, Decoding& d) { return readStr(c, d.it.f); }}
#define TX_NUM(k, f) {k, [](Cursor& c, Decoding& d) { return readUnsigned(c, d.it.f); }}
#define TX_BOOL(k, f) {k, [](Cursor& c, Decoding& d) { return readBool(c, d.it.f); }}

const FieldRule kFields[] = {
    {"k", readKind},
    TX_NUM("q", q),
    TX_NUM("turn", turn),
    TX_NUM("t", t),
    TX_STR("x", x),
    {"tz", readTz},
    TX_STR("ch", ch),
    {"src", [](Cursor& c, Decoding& d) { return readU64s(c, d.it.src); }},
    TX_NUM("m", m),
    {"parts", [](Cursor& c, Decoding& d) {
       d.it.parts.clear();
       return readArray(c, [&](Cursor& cc) {
         d.it.parts.emplace_back();
         return readPart(cc, d.it.parts.back());
       });
     }},
    TX_STR("tr", trigger),
    TX_STR("hd", head),
    TX_STR("pf", profile),
    TX_NUM("at", attempt),
    {"ph", readPhotosOrHash},
    TX_NUM("rd", rd),
    TX_BOOL("fin", fin),
    TX_STR("id", id),
    TX_STR("n", n),
    TX_STR("args", args),
    TX_BOOL("eff", eff),
    TX_BOOL("ok", ok),
    {"cls", readCls},
    TX_STR("hdl", handle),
    TX_NUM("b", bytes),
    TX_NUM("l", lines),
    TX_STR("st", st),
    TX_STR("day", day),
    TX_NUM("thru", thru),
    TX_BOOL("ext", ext),
    TX_NUM("stub", stub),
    TX_STR("why", why),
    {"sup", [](Cursor& c, Decoding& d) { return readU64s(c, d.it.sup); }},
    {"ids", [](Cursor& c, Decoding& d) {
       d.it.msgIds.clear();
       return readArray(c, [&](Cursor& cc) {
         d.it.msgIds.push_back(0);
         return readI64(cc, d.it.msgIds.back());
       });
     }},
    TX_STR("cause", end.cause),
    TX_NUM("rds", end.rounds),
    TX_NUM("tools", end.tools),
    TX_NUM("hr", end.headRequests),
    TX_NUM("pr", end.probeRequests),
    TX_NUM("pt", end.promptTokens),
    TX_NUM("ot", end.outputTokens),
    TX_NUM("cr", end.cacheReadTokens),
    TX_NUM("cw", end.cacheWriteTokens),
    TX_NUM("usd", end.microUsd),
    TX_NUM("ms", end.ms),
    {"ex", [](Cursor& c, Decoding& d) {
       d.it.end.exchanges.clear();
       return readArray(c, [&](Cursor& cc) {
         d.it.end.exchanges.emplace_back();
         return readExchange(cc, d.it.end.exchanges.back());
       });
     }},
    {"ev", [](Cursor& c, Decoding& d) { return readStrings(c, d.it.end.events); }},
    TX_NUM("mh", end.manifestHash),
};

#undef TX_STR
#undef TX_NUM
#undef TX_BOOL

bool readItemField(const char* key, Cursor& c, Decoding& d) {
  ++d.fields;
  if (d.fields == 1 && std::strcmp(key, "k") != 0) return false;   // k comes first
  for (const FieldRule& r : kFields)
    if (!std::strcmp(key, r.key)) return r.read(c, d);
  return skipValue(c);
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
  if (len + 1 > kMaxLineBytes || len < kLineTailBytes + 2) return Status::fail(Err::Torn, where);
  const char* tail = data + len - kLineTailBytes;
  uint32_t want = 0;
  const bool shaped = std::memcmp(tail, kTailHead, 6) == 0 && tail[14] == '"' && tail[15] == '}';
  if (!shaped || !readCrc(tail + 6, want) || crc32(data, len - kLineTailBytes) != want)
    return Status::fail(Err::Torn, where);
  return Status::success();
}

}  // namespace

Status decodeLine(const char* data, size_t len, Item& out) {
  out = Item{};
  Status st = frame(data, len, "tx::decodeLine");
  if (!st.ok()) return st;
  Cursor c{data, data + len};
  Decoding d{out};
  const bool parsed = readObject(c, [&](const char* key, Cursor& cc) { return readItemField(key, cc, d); });
  if (!parsed || c.more() || !d.haveKind) return Status::fail(Err::InvalidArgument, "tx::decodeLine");
  return Status::success();
}

Status checkLine(const char* data, size_t len, LineHeader& out) {
  out = LineHeader{};
  Status st = frame(data, len, "tx::checkLine");
  if (!st.ok()) return st;
  Cursor c{data, data + len};
  size_t fields = 0;
  bool haveKind = false;
  const bool parsed = readObject(c, [&](const char* key, Cursor& cc) {
    if (++fields == 1) {
      Item none;   // readKind writes only the kind; no string of it is touched
      Decoding d{none};
      d.fields = 1;
      haveKind = std::strcmp(key, "k") == 0 && readKind(cc, d);
      out.kind = none.kind;
      return haveKind;
    }
    if (!std::strcmp(key, "q")) return readU64(cc, out.q);
    if (!std::strcmp(key, "turn")) return readUnsigned(cc, out.turn);
    if (!std::strcmp(key, "t")) return readU64(cc, out.t);
    return skipValue(cc);
  });
  if (!parsed || c.more() || !haveKind) return Status::fail(Err::InvalidArgument, "tx::checkLine");
  return Status::success();
}

// ==== the message stamp (Ruling 11) ======================================================

namespace {

// Days since 1970-01-01 to a civil date (H. Hinnant's algorithm, proleptic Gregorian).
void civilFromDays(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = static_cast<int64_t>(yoe) + era * 400 + (m <= 2);
}

char* putNum(char* p, unsigned v, int width) {
  char buf[10];
  int n = 0;
  do {
    buf[n++] = static_cast<char>('0' + v % 10);
    v /= 10;
  } while (v);
  while (n < width) buf[n++] = '0';
  while (n) *p++ = buf[--n];
  return p;
}

}  // namespace

Status messageStamp(uint64_t epoch, int16_t tzMinutes, char* out, size_t cap) {
  static const char kDays[] = "SunMonTueWedThuFriSat";
  static const char kMonths[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const uint64_t kLastSecondOf9999 = 253402300799ULL;
  if (!out || cap < kStampMaxBytes || !epoch || epoch > kLastSecondOf9999) return Status::fail(Err::InvalidArgument, "tx::messageStamp");
  const int64_t local = static_cast<int64_t>(epoch) + int64_t(tzMinutes) * 60;
  const int64_t days = (local >= 0 ? local : local - 86399) / 86400;
  const unsigned secs = static_cast<unsigned>(local - days * 86400);
  int64_t y = 0;
  unsigned m = 0, d = 0;
  civilFromDays(days, y, m, d);
  if (y < 1 || y > 9999) return Status::fail(Err::InvalidArgument, "tx::messageStamp");
  const unsigned wd = static_cast<unsigned>(((days % 7) + 11) % 7);   // 1970-01-01 was a Thursday
  char* p = out;
  *p++ = '[';
  std::memcpy(p, kDays + 3 * wd, 3);
  p += 3;
  *p++ = ' ';
  p = putNum(p, d, 1);
  *p++ = ' ';
  std::memcpy(p, kMonths + 3 * (m - 1), 3);
  p += 3;
  *p++ = ' ';
  p = putNum(p, static_cast<unsigned>(y), 1);
  *p++ = ' ';
  p = putNum(p, secs / 3600, 2);
  *p++ = ':';
  p = putNum(p, secs / 60 % 60, 2);
  *p++ = ']';
  *p = '\0';
  return Status::success();
}

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
