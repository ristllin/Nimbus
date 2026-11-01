#include "nimbus/orch/router_refusals.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace nimbus {
namespace orch {

const char* routerActionSlug(RouterAction a) {
  switch (a) {
    case RouterAction::None:        return "none";
    case RouterAction::FixRequest:  return "fix_request";
    case RouterAction::RetryLater:  return "retry_later";
    case RouterAction::RetryCapped: return "retry_capped";
    case RouterAction::FailOver:    return "fail_over";
    case RouterAction::ParkBilling: return "park_billing";
    case RouterAction::ParkAuth:    return "park_auth";
  }
  return "none";
}

const RouterReasonRow* routerReasonRow(const std::string& reason) {
  for (const RouterReasonRow& r : kRouterReasons)
    if (reason == r.reason) return &r;
  return nullptr;
}

namespace {

// ---- a small top-level JSON walk (no document: bodies reach 85 KB on the device) ------
void skipWs(const std::string& s, size_t& i) {
  while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
}

// At an opening quote: past the closing one. False when the string never closes.
bool skipString(const std::string& s, size_t& i) {
  for (++i; i < s.size(); ++i) {
    if (s[i] == '\\') {
      ++i;
      continue;
    }
    if (s[i] == '"') {
      ++i;
      return true;
    }
  }
  return false;
}

// Past one JSON value. False when it never ends.
bool skipValue(const std::string& s, size_t& i) {
  skipWs(s, i);
  if (i >= s.size()) return false;
  if (s[i] == '"') return skipString(s, i);
  if (s[i] != '{' && s[i] != '[') {
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') ++i;
    return i < s.size();
  }
  int depth = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (c == '"') {
      if (!skipString(s, i)) return false;
      continue;
    }
    if (c == '{' || c == '[') ++depth;
    if (c == '}' || c == ']') --depth;
    ++i;
    if (depth == 0) return true;
  }
  return false;
}

// Calls `fn(key, valueStart, valueEnd)` for each member of the object at `i`; stops when
// fn returns false. False when the object is malformed.
template <class Fn>
bool walkObject(const std::string& s, size_t i, Fn&& fn) {
  skipWs(s, i);
  if (i >= s.size() || s[i] != '{') return false;
  ++i;
  for (;;) {
    skipWs(s, i);
    if (i >= s.size()) return false;
    if (s[i] == '}') return true;
    if (s[i] == ',') {
      ++i;
      continue;
    }
    if (s[i] != '"') return false;
    const size_t keyFrom = i + 1;
    if (!skipString(s, i)) return false;
    const std::string key = s.substr(keyFrom, i - 1 - keyFrom);
    skipWs(s, i);
    if (i >= s.size() || s[i] != ':') return false;
    ++i;
    skipWs(s, i);
    const size_t valFrom = i;
    if (!skipValue(s, i)) return false;
    if (!fn(key, valFrom, i)) return true;
  }
}

std::string plainString(const std::string& s, size_t from, size_t to) {
  if (to < from + 2 || s[from] != '"') return std::string();
  return s.substr(from + 1, to - from - 2);
}

bool wordChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

// A device error line ("HTTP 402 insufficient_credits_for_call"): a whole word of the
// table, the longest one that fits at each position.
const RouterReasonRow* reasonWordIn(const std::string& line) {
  for (size_t i = 0; i < line.size(); ++i) {
    if (!wordChar(line[i]) || (i > 0 && wordChar(line[i - 1]))) continue;
    size_t end = i;
    while (end < line.size() && wordChar(line[end])) ++end;
    if (const RouterReasonRow* r = routerReasonRow(line.substr(i, end - i))) return r;
    i = end;
  }
  return nullptr;
}

size_t bodyStart(const std::string& text) {
  const size_t crlf = text.find("\r\n\r\n");
  size_t i = crlf == std::string::npos ? 0 : crlf + 4;
  skipWs(text, i);
  return i;
}

}  // namespace

RouterAnswer readRouterAnswer(const std::string& text) {
  RouterAnswer a;
  const size_t at = bodyStart(text);
  if (at >= text.size() || text[at] != '{') {
    a.row = reasonWordIn(text);
    if (a.row) a.reason = a.row->reason;
    return a;
  }
  std::string errorWord;
  walkObject(text, at, [&](const std::string& key, size_t from, size_t to) {
    if (key == "reason") a.reason = plainString(text, from, to);
    else if (key == "error") errorWord = plainString(text, from, to);
    else if (key == "retryable") a.retryable = text.compare(from, 4, "true") == 0;
    else if (key == "retryAfterMs") a.retryAfterMs = (uint32_t)std::strtoul(text.c_str() + from, nullptr, 10);
    else if (key == "maxAffordableOutputTokens")
      a.maxAffordableOutputTokens = std::strtoll(text.c_str() + from, nullptr, 10);
    return true;
  });
  // A top-level string `error` is a reason only as a machine word: {"error":"Payment
  // required"} is a provider's sentence, not the router's table.
  if (a.reason.empty() && errorWord.find_first_of(" \t.:") == std::string::npos) a.reason = errorWord;
  a.row = a.reason.empty() ? nullptr : routerReasonRow(a.reason);
  return a;
}

bool routerAnswerProvesAccount(const RouterAnswer& a) {
  static const char* const kPast[] = {
      "insufficient_credits_for_call", "credits_held_in_flight", "upstream_rate_limited",
      "upstream_quota_exhausted",      "upstream_auth_failed",   "upstream_error",
      "fallback_exhausted",            "provider_timeout",       "provider_unreachable",
  };
  if (!a.row) return false;
  for (const char* r : kPast)
    if (std::strcmp(a.row->reason, r) == 0) return true;
  return false;
}

namespace {

// The spelling a body with no declared cap takes on `path` ("" = this path has no output
// cap). OpenAI's chat refuses max_tokens on a reasoning model, so it gets the newer name.
const char* capSpellingFor(const std::string& path) {
  const size_t q = path.find('?');
  const std::string p = q == std::string::npos ? path : path.substr(0, q);
  auto ends = [&p](const char* tail) {
    const size_t n = std::strlen(tail);
    return p.size() >= n && p.compare(p.size() - n, n, tail) == 0;
  };
  if (ends("/responses")) return "max_output_tokens";
  if (ends("/messages")) return "max_tokens";
  if (ends("/chat/completions"))
    return p.find("/router/openai/") != std::string::npos ? "max_completion_tokens" : "max_tokens";
  return "";
}

bool isCapKey(const std::string& k) {
  return k == "max_tokens" || k == "max_completion_tokens" || k == "max_output_tokens";
}

struct ValueSpan {
  size_t from = 0, to = 0;
};

// Lowers one numeric value in place. The edit is recorded for the caller's splices.
void lowerValue(std::string& body, const ValueSpan& v, uint32_t cap, CapEdit edits[], size_t& n, size_t editCap) {
  const unsigned long now = std::strtoul(body.c_str() + v.from, nullptr, 10);
  if (now != 0 && now <= cap) return;
  const std::string val = std::to_string(cap);
  body.replace(v.from, v.to - v.from, val);
  if (n < editCap) edits[n++] = CapEdit{v.to, (long)val.size() - (long)(v.to - v.from)};
}

void insertAt(std::string& body, size_t at, const std::string& text, CapEdit edits[], size_t& n, size_t editCap) {
  body.insert(at, text);
  if (n < editCap) edits[n++] = CapEdit{at, (long)text.size()};
}

// The index of the `}` that closes the object starting at `open`.
size_t closingBrace(const std::string& s, size_t open) {
  size_t i = open;
  return skipValue(s, i) && i > open ? i - 1 : std::string::npos;
}

// A member appended before the object's closing brace, comma included.
void appendMember(std::string& body, size_t open, const std::string& member, CapEdit edits[], size_t& n,
                  size_t editCap) {
  const size_t close = closingBrace(body, open);
  if (close == std::string::npos) return;
  size_t last = close;
  while (last > open && std::isspace((unsigned char)body[last - 1])) --last;
  const bool empty = last == open + 1;
  insertAt(body, close, (empty ? "" : ",") + member, edits, n, editCap);
}

}  // namespace

bool capOutputTokens(std::string& body, const std::string& path, uint32_t cap, CapEdit edits[], size_t& editCount,
                     size_t editCap) {
  editCount = 0;
  size_t open = 0;
  skipWs(body, open);
  if (open >= body.size() || body[open] != '{') return false;
  const bool conversation = path.find("/conversations") != std::string::npos;
  const char* spelling = capSpellingFor(path);
  if (!conversation && !*spelling) return false;

  // The largest offsets first, so an edit never moves a span still to do.
  ValueSpan caps[3];
  size_t capCount = 0;
  ValueSpan args;
  const bool ok = walkObject(body, open, [&](const std::string& key, size_t from, size_t to) {
    if (isCapKey(key) && capCount < 3) caps[capCount++] = ValueSpan{from, to};
    if (key == "completion_args") args = ValueSpan{from, to};
    return true;
  });
  if (!ok) return false;
  if (conversation) {
    if (args.to == 0) {
      appendMember(body, open, "\"completion_args\":{\"max_tokens\":" + std::to_string(cap) + "}", edits, editCount,
                   editCap);
      return true;
    }
    ValueSpan inner;
    bool has = false;
    walkObject(body, args.from, [&](const std::string& key, size_t from, size_t to) {
      if (key != "max_tokens") return true;
      inner = ValueSpan{from, to};
      has = true;
      return false;
    });
    if (has) lowerValue(body, inner, cap, edits, editCount, editCap);
    else appendMember(body, args.from, "\"max_tokens\":" + std::to_string(cap), edits, editCount, editCap);
    return true;
  }
  if (capCount == 0) {
    appendMember(body, open, std::string("\"") + spelling + "\":" + std::to_string(cap), edits, editCount, editCap);
    return true;
  }
  for (size_t i = capCount; i-- > 0;) lowerValue(body, caps[i], cap, edits, editCount, editCap);
  return true;
}

size_t applyCapEdits(size_t at, const CapEdit edits[], size_t editCount) {
  long moved = 0;
  for (size_t i = 0; i < editCount; ++i)
    if (at >= edits[i].at) moved += edits[i].delta;
  return (size_t)((long)at + moved);
}

}  // namespace orch
}  // namespace nimbus
