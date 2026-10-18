// reply_markup - see the header for the class and the three seams.
// Portable, Arduino-free; ArduinoJson only for the leak recovery.

#include "nimbus/orch/reply_markup.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

namespace nimbus {
namespace orch {

namespace {

const size_t npos = std::string::npos;

// ---- code spans: the tg_html rules, so the cleaner and the renderer agree on
//      what is code. A fence opens at a line start with ``` and an optional
//      language tag, and closes at the next ```; inline code is a backtick pair
//      on one line with something between. -------------------------------------
struct Span {
  size_t b, e;   // [b, e)
};

bool langByte(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
         c == '_' || c == '+' || c == '-';
}

size_t fenceEnd(const std::string& s, size_t i) {
  const size_t nl = s.find('\n', i + 3);
  if (nl == npos) return npos;
  for (size_t k = i + 3; k < nl; k++)
    if (!langByte(s[k])) return npos;
  const size_t close = s.find("```", nl + 1);
  return close == npos ? npos : close + 3;
}

std::vector<Span> codeSpans(const std::string& s) {
  std::vector<Span> out;
  const size_t n = s.size();
  if (s.find('`') == npos) return out;
  size_t i = 0;
  while (i < n) {
    // Unlike the renderer, a fence opener counts anywhere on a line here: code
    // is protected even when a removed block sat in front of it on that line.
    if (s.compare(i, 3, "```") == 0) {
      const size_t e = fenceEnd(s, i);
      if (e != npos) {
        out.push_back({i, e});
        i = e;
        continue;
      }
    }
    if (s[i] == '`') {
      const size_t close = s.find('`', i + 1);
      const size_t nl = s.find('\n', i + 1);
      if (close != npos && close > i + 1 && (nl == npos || close < nl)) {
        out.push_back({i, close + 1});
        i = close + 1;
        continue;
      }
    }
    i++;
  }
  return out;
}

bool inCode(const std::vector<Span>& sp, size_t pos) {
  // Spans are sorted and disjoint: the candidate is the last one starting <= pos.
  auto it = std::upper_bound(sp.begin(), sp.end(), pos,
                             [](size_t p, const Span& s) { return p < s.b; });
  if (it == sp.begin()) return false;
  --it;
  return pos < it->e;
}

bool blankRange(const std::string& s, size_t a, size_t b) {
  for (size_t i = a; i < b && i < s.size(); i++)
    if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n') return false;
  return true;
}

std::string trimmed(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t\r\n");
  if (b == npos) return std::string();
  const size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

std::string rtrimmed(const std::string& s) {
  const size_t e = s.find_last_not_of(" \t\r\n");
  return e == npos ? std::string() : s.substr(0, e + 1);
}

std::string lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

// ---- tags ---------------------------------------------------------------------
// '<' ['/'] [prefix ':'] name [ws attrs] ['/'] '>' with prefix [a-z]+ (the
// namespace Claude's tool-call format uses), name [A-Za-z_][A-Za-z0-9_-]*, and
// attributes bounded to one tag (no '<' inside, at most kTagMax bytes). Also
// the special token form <|...|>.
constexpr size_t kTagMax = 300;

struct Tag {
  size_t b = 0, e = 0;       // [b, e) covers the whole tag
  bool close = false;
  bool selfClose = false;
  bool special = false;      // <|...|>
  std::string raw;           // local name as written (prefix stripped)
  std::string name;          // the same, lowercased
  std::string nameAttr;      // value of name="..." / name='...' ("" when absent)
};

bool spaceByte(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

void readNameAttr(const std::string& attrs, std::string& out) {
  size_t p = 0;
  while ((p = attrs.find("name", p)) != npos) {
    const bool startOk = p == 0 || spaceByte(attrs[p - 1]);
    size_t q = p + 4;
    while (q < attrs.size() && spaceByte(attrs[q])) q++;
    if (startOk && q < attrs.size() && attrs[q] == '=') {
      q++;
      while (q < attrs.size() && spaceByte(attrs[q])) q++;
      if (q < attrs.size() && (attrs[q] == '"' || attrs[q] == '\'')) {
        const char quote = attrs[q];
        const size_t end = attrs.find(quote, q + 1);
        if (end != npos) {
          out = attrs.substr(q + 1, end - q - 1);
          return;
        }
      }
    }
    p += 4;
  }
}

// <|special|>: up to 64 non-space bytes between the bars.
bool parseSpecialAt(const std::string& s, size_t j, Tag& t) {
  const size_t n = s.size();
  const size_t lim = std::min(n, j + 1 + 64);
  for (size_t k = j + 1; k + 1 < n && k < lim; k++) {
    if (spaceByte(s[k])) return false;
    if (s[k] != '|' || s[k + 1] != '>') continue;
    if (k == j + 1) return false;   // "<||>" is not a token
    t.special = true;
    t.e = k + 2;
    return true;
  }
  return false;
}

bool nameStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool nameByte(char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '-'; }

// [prefix ':'] name from s[j]; returns the index after the name, npos when none.
size_t parseTagName(const std::string& s, size_t j, Tag& t) {
  const size_t n = s.size();
  size_t k = j;
  while (k < n && s[k] >= 'a' && s[k] <= 'z') k++;
  if (k < n && s[k] == ':' && k > j) j = k + 1;   // namespace prefix
  if (j >= n || !nameStart(s[j])) return npos;
  const size_t nb = j;
  while (j < n && nameByte(s[j])) j++;
  t.raw = s.substr(nb, j - nb);
  t.name = lower(t.raw);
  return j;
}

// Attributes from s[j] (whitespace first) to the closing '>' within the tag bound.
bool parseTagAttrs(const std::string& s, size_t j, size_t limit, Tag& t) {
  size_t m = j;
  while (m < limit && s[m] != '>' && s[m] != '<') m++;
  if (m >= limit || s[m] != '>') return false;
  const std::string attrs = s.substr(j, m - j);
  if (t.close && !blankRange(attrs, 0, attrs.size())) return false;   // a close tag has none
  if (!t.close) {
    const size_t last = attrs.find_last_not_of(" \t\r\n");
    t.selfClose = last != npos && attrs[last] == '/';
    readNameAttr(attrs, t.nameAttr);
  }
  t.e = m + 1;
  return true;
}

// What follows the name at s[j]: '>', '/>' or attributes, for the tag at s[i].
bool parseTagEnd(const std::string& s, size_t i, size_t j, Tag& t) {
  const size_t n = s.size();
  if (j < n && s[j] == '>') {
    t.e = j + 1;
    return true;
  }
  if (!t.close && j + 1 < n && s[j] == '/' && s[j + 1] == '>') {
    t.selfClose = true;
    t.e = j + 2;
    return true;
  }
  if (j >= n || !spaceByte(s[j])) return false;
  return parseTagAttrs(s, j, std::min(n, i + kTagMax), t);
}

bool parseTagAt(const std::string& s, size_t i, Tag& t) {
  const size_t n = s.size();
  if (i >= n || s[i] != '<') return false;
  t = Tag{};
  t.b = i;
  size_t j = i + 1;
  if (j < n && s[j] == '|') return parseSpecialAt(s, j, t);
  if (j < n && s[j] == '/') {
    t.close = true;
    j++;
  }
  j = parseTagName(s, j, t);
  return j != npos && parseTagEnd(s, i, j, t);
}

// ---- formatting F1: the leak repair --------------------------------------------
// Markers are matched case-sensitively (the decoder writes them exactly), with
// any namespace prefix: CLOSE </parameter> or </F> for a contract field F; OPEN
// <parameter name="...">; CALL <invoke ...>, </invoke>, <function_calls>,
// </function_calls>.
enum class Mk : uint8_t { None, Close, Open, Call };

bool isFieldName(const std::string& raw) {
  TurnField f;
  return turnFieldFromName(raw.data(), raw.size(), f);
}

Mk markerAt(const std::string& s, size_t i, Tag& t) {
  if (!parseTagAt(s, i, t) || t.special) return Mk::None;
  const bool call = t.raw == "invoke" || t.raw == "function_calls";
  if (t.close) {
    if (t.raw == "parameter" || isFieldName(t.raw)) return Mk::Close;
    return call ? Mk::Call : Mk::None;
  }
  if (t.raw == "parameter" && !t.selfClose && !t.nameAttr.empty()) return Mk::Open;
  return call ? Mk::Call : Mk::None;
}

// The next marker at or after `from`, outside code; npos when none.
size_t nextMarker(const std::string& s, const std::vector<Span>& code, size_t from, Tag& t,
                  Mk& kind) {
  for (size_t p = s.find('<', from); p != npos; p = s.find('<', p + 1)) {
    if (inCode(code, p)) continue;
    kind = markerAt(s, p, t);
    if (kind != Mk::None) return p;
  }
  kind = Mk::None;
  return npos;
}

struct Block {
  std::string name;
  std::string value;
};

// Cut `s` at its first marker when the tail obeys the structural rule (only
// markers, whitespace and parameter value blocks) and holds tool-call structure
// (a parameter block or an invoke tag, never a lone close tag). The blocks are
// returned in order, values trimmed.
bool cutLeakTail(std::string& s, std::vector<Block>* blocks) {
  if (s.find('<') == npos) return false;
  const std::vector<Span> code = codeSpans(s);
  Tag t;
  Mk kind = Mk::None;
  const size_t m = nextMarker(s, code, 0, t, kind);
  if (m == npos) return false;
  std::vector<Block> found;
  bool inBlock = false;
  bool structure = kind != Mk::Close;   // a lone close tag is not tool-call structure
  Block cur;
  size_t i = m;
  for (;;) {
    Tag nt;
    Mk nk = Mk::None;
    const size_t at = nextMarker(s, code, i, nt, nk);
    const size_t segEnd = at == npos ? s.size() : at;
    if (inBlock) cur.value.append(s, i, segEnd - i);
    else if (!blankRange(s, i, segEnd)) return false;   // prose after a marker: not a leak
    if (at == npos) break;
    if (inBlock) {
      cur.value = trimmed(cur.value);
      found.push_back(std::move(cur));
      cur = Block{};
      inBlock = false;
    }
    if (nk == Mk::Open) {
      cur.name = nt.nameAttr;
      inBlock = true;
    }
    structure = structure || nk != Mk::Close;
    i = nt.e;
  }
  // Only real tool-call structure (a parameter block or an invoke tag) is cut
  // here: prose that merely ends on "</parameter>" keeps its words, and the
  // reply seam removes a stray closing wrapper or parameter tag at delivery.
  if (!structure) return false;
  if (inBlock) {
    cur.value = trimmed(cur.value);
    found.push_back(std::move(cur));
  }
  s = rtrimmed(s.substr(0, m));
  if (blocks) *blocks = std::move(found);
  return true;
}

uint16_t bitOf(TurnField f) { return (uint16_t)(1u << (unsigned)f); }

bool kindMatches(FieldKind k, ArduinoJson::JsonVariantConst v) {
  switch (k) {
    case FieldKind::String:         return v.is<const char*>();
    case FieldKind::ArrayOfObjects:
    case FieldKind::ArrayOfStrings: return v.is<ArduinoJson::JsonArrayConst>();
    case FieldKind::Object:         return v.is<ArduinoJson::JsonObjectConst>() || v.isNull();
  }
  return false;
}

void recoverBlocks(ArduinoJson::JsonObject root, const std::vector<Block>& blocks,
                   LeakRepair& rep) {
  for (const Block& b : blocks) {
    TurnField f;
    if (!turnFieldFromName(b.name.data(), b.name.size(), f)) continue;
    const char* key = turnFieldName(f);
    if (!root[key].isNull()) continue;   // the document's own value always wins
    const FieldKind kind = turnFieldKind(f);
    if (kind == FieldKind::String) {
      root[key] = b.value;
      rep.recoveredMask |= bitOf(f);
      continue;
    }
    ArduinoJson::JsonDocument d;
    if (deserializeJson(d, b.value, ArduinoJson::DeserializationOption::NestingLimit(16))) {
      rep.droppedBlocks++;
      continue;
    }
    // A JSON string is kept for the container normalizer to repair or report.
    if (!kindMatches(kind, d.as<ArduinoJson::JsonVariantConst>()) &&
        !d.is<const char*>()) {
      rep.droppedBlocks++;
      continue;
    }
    root[key] = d.as<ArduinoJson::JsonVariantConst>();
    rep.recoveredMask |= bitOf(f);
    rep.containerMask |= bitOf(f);
  }
}

void cutNested(ArduinoJson::JsonVariant v, LeakRepair& rep) {
  if (!v.is<const char*>()) return;
  std::string s = v.as<const char*>();
  if (cutLeakTail(s, nullptr)) {
    v.set(s);
    if (rep.nestedCuts < 255) rep.nestedCuts++;
  }
}

}  // namespace

// Exposed to orch_turn.cpp's normalizer through the public pair below: the top
// strings first (so the recovered fields exist before container coercion), the
// nested strings last (so a stringified session_ops is coerced before its tasks
// are checked).
void repairLeakedTop(ArduinoJson::JsonObject root, LeakRepair& rep) {
  for (TurnField f : {TurnField::Reply, TurnField::Memory, TurnField::Ask}) {
    const char* key = turnFieldName(f);
    ArduinoJson::JsonVariant v = root[key];
    if (!v.is<const char*>()) continue;
    std::string s = v.as<const char*>();
    std::vector<Block> blocks;
    if (!cutLeakTail(s, &blocks)) continue;
    v.set(s);
    rep.repaired = true;
    recoverBlocks(root, blocks, rep);
  }
}

void repairLeakedNested(ArduinoJson::JsonObject root, LeakRepair& rep) {
  for (const char* arr : {"session_ops", "spawn"}) {
    for (ArduinoJson::JsonVariant it : root[arr].as<ArduinoJson::JsonArray>()) {
      if (!it.is<ArduinoJson::JsonObject>()) continue;
      cutNested(it["task"], rep);
      cutNested(it["message"], rep);
    }
  }
  for (ArduinoJson::JsonVariant it : root["mem_write"].as<ArduinoJson::JsonArray>())
    if (it.is<ArduinoJson::JsonObject>()) cutNested(it["content"], rep);
  ArduinoJson::JsonVariant sp = root["scratchpad"];
  if (sp.is<ArduinoJson::JsonObject>()) {
    cutNested(sp["active"], rep);
    for (const char* tier : {"short", "mid", "long"})
      for (ArduinoJson::JsonVariant it : sp[tier].as<ArduinoJson::JsonArray>()) cutNested(it, rep);
  }
}

void repairLeakedParams(ArduinoJson::JsonObject root, LeakRepair& rep) {
  repairLeakedTop(root, rep);
  repairLeakedNested(root, rep);
}

// ---- formatting F3 ---------------------------------------------------------------
namespace {

bool isReasoning(const std::string& n) {
  return n == "think" || n == "thinking" || n == "reasoning";
}
bool isToolMarkup(const std::string& n) {
  return n == "function_calls" || n == "function_results" || n == "invoke" ||
         n == "tool_call" || n == "tool_result" || n == "tool_use" || n == "arg_key" ||
         n == "arg_value";
}
bool isWrapper(const std::string& n) {
  return n == "reply" || n == "answer" || n == "response" || n == "output" || n == "final";
}

// The close tag matching the open tag `open` (same lowercased name, nesting
// counted), searched outside code from open.e; npos when unclosed.
size_t matchingClose(const std::string& s, const std::vector<Span>& code, const Tag& open,
                     Tag& close) {
  int depth = 1;
  for (size_t p = s.find('<', open.e); p != npos; p = s.find('<', p + 1)) {
    if (inCode(code, p)) continue;
    Tag t;
    if (!parseTagAt(s, p, t) || t.special || t.name != open.name) continue;
    if (t.close) {
      if (--depth == 0) {
        close = t;
        return p;
      }
    } else if (!t.selfClose) {
      depth++;
    }
  }
  return npos;
}

}  // namespace

std::string stripReasoningPreamble(const std::string& text, size_t* strippedBytes) {
  if (strippedBytes) *strippedBytes = 0;
  if (text.find('<') == npos) return text;
  std::string s = text;
  bool changed = false;
  for (int guard = 0; guard < 8; guard++) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == npos || s[b] != '<') break;
    Tag open;
    if (!parseTagAt(s, b, open) || open.special || open.close || open.selfClose ||
        !isReasoning(open.name))
      break;
    const std::vector<Span> code = codeSpans(s);
    Tag close;
    if (matchingClose(s, code, open, close) != npos) {
      s.erase(0, close.e);
      changed = true;
      continue;
    }
    s.erase(b, open.e - b);   // unclosed: only the tag goes, never the answer
    changed = true;
    break;
  }
  if (!changed) return text;
  const size_t b = s.find_first_not_of(" \t\r\n");
  s = b == npos ? std::string() : s.substr(b);
  if (strippedBytes) *strippedBytes = text.size() - s.size();
  return s;
}

namespace {
// The wrapper open tag at text[b], or false.
bool wrapperOpenAt(const std::string& text, size_t b, Tag& open) {
  if (!parseTagAt(text, b, open) || open.special || open.close || open.selfClose) return false;
  return isWrapper(open.name) || open.name == "orch_turn";
}
}  // namespace

std::string unwrapWholeReply(const std::string& text) {
  const size_t b = text.find_first_not_of(" \t\r\n");
  if (b == npos || text[b] != '<') return text;
  const size_t e = text.find_last_not_of(" \t\r\n");
  Tag open;
  if (text[e] != '>' || !wrapperOpenAt(text, b, open)) return text;
  const size_t lt = text.rfind('<', e);
  if (lt == npos || lt < open.e) return text;
  Tag close;
  const bool closes = parseTagAt(text, lt, close) && close.close && close.name == open.name &&
                      close.e == e + 1;
  return closes ? trimmed(text.substr(open.e, lt - open.e)) : text;
}

// ---- formatting F4: the reply seam -------------------------------------------------
namespace {

// One removal pass over the deny-list blocks and tags. Returns true when
// anything was removed.
bool removeDenyMarkup(std::string& s) {
  if (s.find('<') == npos) return false;
  const std::vector<Span> code = codeSpans(s);
  std::string out;
  out.reserve(s.size());
  bool removed = false;
  size_t i = 0;
  while (i < s.size()) {
    const size_t lt = s.find('<', i);
    if (lt == npos) {
      out.append(s, i, npos);
      break;
    }
    out.append(s, i, lt - i);
    i = lt;
    Tag t;
    if (inCode(code, lt) || !parseTagAt(s, lt, t)) {
      out += s[lt];
      i = lt + 1;
      continue;
    }
    if (t.special || t.name == "parameter" || t.name == "orch_turn") {
      i = t.e;   // token or tag only; its text (if any) stays
      removed = true;
      continue;
    }
    if (isReasoning(t.name) || isToolMarkup(t.name)) {
      removed = true;
      if (!t.close && !t.selfClose) {
        Tag close;
        if (matchingClose(s, code, t, close) != npos) {
          i = close.e;   // the whole block, content included
          continue;
        }
      }
      i = t.e;   // unclosed, orphan close or self-closing: the tag alone
      continue;
    }
    out.append(s, lt, t.e - lt);   // any other tag is content
    i = t.e;
  }
  if (removed) s.swap(out);
  return removed;
}

// A wrapper tag that opens the text, or closes it. Prose that mentions one in
// the middle keeps it.
bool removeLeadingWrapper(std::string& s) {
  const size_t b = s.find_first_not_of(" \t\r\n");
  Tag t;
  if (b == npos || s[b] != '<' || !parseTagAt(s, b, t)) return false;
  if (t.special || t.close || !isWrapper(t.name)) return false;
  s.erase(b, t.e - b);
  return true;
}

bool removeTrailingWrapper(std::string& s) {
  const size_t e = s.find_last_not_of(" \t\r\n");
  if (e == npos || s[e] != '>') return false;
  const size_t lt = s.rfind('<', e);
  Tag c;
  if (lt == npos || !parseTagAt(s, lt, c) || !c.close || c.e != e + 1 || !isWrapper(c.name))
    return false;
  if (inCode(codeSpans(s), lt)) return false;
  s.erase(lt, c.e - lt);
  return true;
}

bool removeEdgeWrappers(std::string& s) {
  bool removed = false;
  for (int guard = 0; guard < 4; guard++) {
    const bool lead = removeLeadingWrapper(s);
    const bool trail = removeTrailingWrapper(s);
    if (!lead && !trail) break;
    removed = true;
  }
  return removed;
}

// Runs of three or more line breaks (blank lines may hold spaces or tabs)
// become exactly two, outside code. Only run after a removal, so untouched
// text keeps its own spacing.
void collapseBlankLines(std::string& s) {
  const std::vector<Span> code = codeSpans(s);
  std::string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] != '\n' || inCode(code, i)) {
      out += s[i++];
      continue;
    }
    size_t j = i;
    int breaks = 0;
    size_t lastBreak = i;
    while (j < s.size() && !inCode(code, j) &&
           (s[j] == '\n' || s[j] == ' ' || s[j] == '\t' || s[j] == '\r')) {
      if (s[j] == '\n') {
        breaks++;
        lastBreak = j;
      }
      j++;
    }
    if (breaks >= 3) {
      out += "\n\n";
      i = lastBreak + 1;   // keep the indentation of the next line
    } else {
      out.append(s, i, j - i);
      i = j;
    }
  }
  s.swap(out);
}

}  // namespace

std::string cleanReplyMarkup(const std::string& text) {
  if (text.find('<') == npos) return text;
  std::string s = text;
  bool removed = false;
  // Until nothing more goes: a removal can join two halves into a new tag
  // ("<thi" + block + "nk>"), so one pass alone would not be idempotent.
  for (int guard = 0; guard < 4 && removeDenyMarkup(s); guard++) removed = true;
  if (removeEdgeWrappers(s)) removed = true;
  if (!removed) return text;
  collapseBlankLines(s);
  return trimmed(s);
}

namespace {

bool denyTagName(const std::string& n) {
  return isReasoning(n) || isToolMarkup(n) || n == "parameter" || n == "orch_turn";
}

// The first deny-list tag or <|token|> outside code, as text; "" when none.
std::string strayTag(const std::string& s, const std::vector<Span>& code) {
  for (size_t p = s.find('<'); p != npos; p = s.find('<', p + 1)) {
    Tag t;
    if (inCode(code, p) || !parseTagAt(s, p, t)) continue;
    if (t.special) return "special token " + s.substr(p, t.e - p);
    if (denyTagName(t.name)) return "tag <" + std::string(t.close ? "/" : "") + t.name + ">";
  }
  return std::string();
}

// A wrapper that opens or closes the whole text; "" when none.
std::string edgeWrapper(const std::string& s, const std::vector<Span>& code) {
  const size_t b = s.find_first_not_of(" \t\r\n");
  Tag t;
  if (b != npos && s[b] == '<' && parseTagAt(s, b, t) && !t.close && !t.special && isWrapper(t.name))
    return "wrapper <" + t.name + "> opens the text";
  const size_t e = s.find_last_not_of(" \t\r\n");
  if (e == npos || s[e] != '>') return std::string();
  const size_t lt = s.rfind('<', e);
  if (lt != npos && !inCode(code, lt) && parseTagAt(s, lt, t) && t.close && t.e == e + 1 &&
      isWrapper(t.name))
    return "wrapper </" + t.name + "> closes the text";
  return std::string();
}

// Raw orch_turn JSON outside code; "" when none.
std::string rawTurnJson(const std::string& s, const std::vector<Span>& code) {
  for (const char* raw : {"{\"reply\":", "\"session_ops\":[", "\"mem_write\":["})
    for (size_t p = s.find(raw); p != npos; p = s.find(raw, p + 1))
      if (!inCode(code, p)) return std::string("raw turn JSON ") + raw;
  return std::string();
}

}  // namespace

bool hasStrayMarkup(const std::string& delivered, std::string* why) {
  const std::vector<Span> code = codeSpans(delivered);
  std::string hit = strayTag(delivered, code);
  if (hit.empty()) hit = edgeWrapper(delivered, code);
  if (hit.empty()) hit = rawTurnJson(delivered, code);
  if (why) *why = hit;
  return !hit.empty();
}

}  // namespace orch
}  // namespace nimbus
