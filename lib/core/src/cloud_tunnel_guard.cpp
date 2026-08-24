#include "nimbus/cloud/tunnel_guard.h"

#include <cctype>
#include <cstddef>

namespace nimbus {
namespace cloud {
namespace tunnel {

namespace {

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool asciiCaseContains(const std::string& hay, const char* needleLower) {
  // Case-insensitive substring search (needle is already lowercase). Small inputs.
  for (size_t i = 0; i < hay.size(); ++i) {
    size_t j = 0;
    while (needleLower[j] &&
           i + j < hay.size() &&
           (char)std::tolower((unsigned char)hay[i + j]) == needleLower[j]) {
      ++j;
    }
    if (!needleLower[j]) return true;
  }
  return false;
}

// Advance past JSON insignificant whitespace starting at i.
size_t skipWs(const std::string& s, size_t i) {
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
  return i;
}

// Index of the closing '"' of a JSON string whose first value char is at valStart
// (backslash escapes are honored), or std::string::npos if the string is unterminated.
size_t stringValueEnd(const std::string& s, size_t valStart) {
  size_t j = valStart;
  while (j < s.size() && s[j] != '"') {
    if (s[j] == '\\' && j + 1 < s.size()) ++j;  // skip the escaped char
    ++j;
  }
  return j < s.size() ? j : std::string::npos;
}

// Redact the string value of every `"key":"..."` occurrence to "". Returns true if it
// changed anything. Only treats a match as a key when the next non-space char is ':' and
// the value that follows opens with '"', so a value that merely equals the key name is
// left alone.
bool redactStringField(std::string& body, const char* key) {
  const std::string token = std::string("\"") + key + "\"";
  bool changed = false;
  size_t pos = 0;
  while ((pos = body.find(token, pos)) != std::string::npos) {
    size_t i = skipWs(body, pos + token.size());
    if (i >= body.size() || body[i] != ':') { pos += token.size(); continue; }
    i = skipWs(body, i + 1);
    if (i >= body.size() || body[i] != '"') { pos += token.size(); continue; }
    size_t valStart = i + 1;
    size_t valEnd = stringValueEnd(body, valStart);
    if (valEnd == std::string::npos) break;  // unterminated string; leave the tail untouched
    if (valEnd > valStart) {
      body.erase(valStart, valEnd - valStart);  // shrink the value to empty
      changed = true;
    }
    pos = valStart + 1;  // continue after the (now empty) value
  }
  return changed;
}

}  // namespace

std::string canonicalizePath(const std::string& rawPath) {
  // Drop the query / fragment: the router splits these off before matching.
  size_t end = rawPath.size();
  for (size_t i = 0; i < rawPath.size(); ++i) {
    if (rawPath[i] == '?' || rawPath[i] == '#') { end = i; break; }
  }
  std::string out;
  out.reserve(end);
  for (size_t i = 0; i < end;) {
    if (rawPath[i] == '%' && i + 2 < end) {
      int hi = hexVal(rawPath[i + 1]);
      int lo = hexVal(rawPath[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back((char)((hi << 4) | lo));
        i += 3;
        continue;
      }
      // Malformed escape (not two hex digits): leave the '%' literal.
    }
    out.push_back(rawPath[i]);
    ++i;
  }
  // Strip a single trailing slash (but never reduce "/" to "").
  if (out.size() > 1 && out.back() == '/') out.pop_back();
  return out;
}

bool isDeniedPath(const std::string& canonicalPath) {
  static const char* const kDenied[] = {
      "/api/connect",           // token + AP password
      "/api/token/regen",       // mints a fresh durable token
      "/api/signin/code",       // mints a single-use code that exchanges for the token
      "/api/signin/exchange",   // redeems a code -> returns the durable token
  };
  for (const char* d : kDenied) {
    if (canonicalPath == d) return true;
  }
  return false;
}

bool isTunnelDenied(const std::string& rawPath) {
  return isDeniedPath(canonicalizePath(rawPath));
}

bool scrubJsonSecrets(const std::string& contentType, std::string& body) {
  if (!asciiCaseContains(contentType, "application/json")) return false;
  bool changed = false;
  changed |= redactStringField(body, "token");
  changed |= redactStringField(body, "apPass");
  return changed;
}

}  // namespace tunnel
}  // namespace cloud
}  // namespace nimbus
