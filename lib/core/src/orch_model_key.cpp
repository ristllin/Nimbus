#include "nimbus/orch/model_key.h"

#include <cstring>

namespace nimbus {
namespace orch {

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }

bool allDigits(const char* s, size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (!isDigit(s[i])) return false;
  return true;
}

bool startsWith(const char* s, const char* prefix) {
  return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

// Copy `n` bytes of `src` into `out` (kModelKeyCap) when they fit; else "".
size_t copyKey(const char* src, size_t n, char* out) {
  if (n == 0 || n >= kModelKeyCap) { out[0] = 0; return 0; }
  std::memcpy(out, src, n);
  out[n] = 0;
  return n;
}

// The selector split resolveRouterRoute uses, read in place: the first '/' at an
// index > 0 separates the upstream from the model.
struct Split {
  const char* up = nullptr;
  size_t      upLen = 0;
  const char* bare = "";
};
Split splitSelector(const char* sel) {
  Split s;
  s.bare = sel;
  const char* sl = std::strchr(sel, '/');
  if (sl && sl > sel) {
    s.up = sel;
    s.upLen = (size_t)(sl - sel);
    s.bare = sl + 1;
  }
  return s;
}

bool isLower(char c) { return c >= 'a' && c <= 'z'; }
bool isAlnum(char c) { return isLower(c) || (c >= 'A' && c <= 'Z') || isDigit(c); }
bool keyChar(char c) { return isAlnum(c) || std::strchr("._:/+-", c) != nullptr; }
bool provChar(char c) { return isLower(c) || isDigit(c) || c == '_' || c == '-'; }

// 1..maxLen characters, each accepted by `ok`.
bool charsetOk(const char* s, size_t maxLen, bool (*ok)(char)) {
  if (!s || !*s) return false;
  size_t n = 0;
  for (const char* p = s; *p; ++p)
    if (!ok(*p) || ++n > maxLen) return false;
  return true;
}

// The version-suffix shapes (each includes its leading '-', length checked by the caller).
bool dateCompact(const char* s) { return allDigits(s + 1, 8); }                 // -YYYYMMDD
bool dateDashed(const char* s) {                                                // -YYYY-MM-DD
  return allDigits(s + 1, 4) && s[5] == '-' && allDigits(s + 6, 2) && s[8] == '-' &&
         allDigits(s + 9, 2);
}
bool yearMonth(const char* s) {                                                 // -20YY-MM
  return s[1] == '2' && s[2] == '0' && allDigits(s + 3, 2) && s[5] == '-' && allDigits(s + 6, 2);
}

}  // namespace

bool modelKeySafe(const char* s) { return charsetOk(s, kModelKeyCap - 1, keyChar); }

bool provKeySafe(const char* s) { return charsetOk(s, 15, provChar); }

bool isVersionSuffix(const char* s) {
  if (!s || s[0] != '-') return false;
  const size_t n = std::strlen(s);
  switch (n) {
    case 5:  if (allDigits(s + 1, 4)) return true; break;   // -NNNN build
    case 7:  if (std::strcmp(s, "-latest") == 0) return true; break;
    case 8:  if (yearMonth(s)) return true; break;
    case 9:  if (dateCompact(s)) return true; break;
    case 11: if (dateDashed(s)) return true; break;
    default: break;
  }
  return n >= 3 && s[1] == 'v' && allDigits(s + 2, n - 2);   // -vN
}

size_t normalizeModelId(const char* raw, char* out) {
  out[0] = 0;
  if (!raw) return 0;
  size_t n = std::strlen(raw);
  if (n == 0 || n >= kModelKeyCap) return 0;
  // Fixed-length suffixes, longest first, so a full date is never read as a
  // shorter form. One suffix only: "foo-2411-latest" keeps its "-2411".
  static const size_t kLens[] = {11, 9, 8, 7, 5};
  size_t keep = n;
  for (size_t L : kLens) {
    if (n > L && raw[n - L] == '-' && isVersionSuffix(raw + n - L)) { keep = n - L; break; }
  }
  if (keep == n) {   // -vN has no fixed length
    const char* dash = std::strrchr(raw, '-');
    if (dash && dash > raw && isVersionSuffix(dash)) keep = (size_t)(dash - raw);
  }
  return copyKey(raw, keep, out);
}

const char* inferUpstream(const char* bare) {
  if (!bare) return "openai";
  if (startsWith(bare, "claude")) return "anthropic";
  if (startsWith(bare, "mistral") || startsWith(bare, "pixtral") ||
      startsWith(bare, "codestral") || startsWith(bare, "ministral"))
    return "mistral";
  if (startsWith(bare, "glm")) return "zai";
  return "openai";
}

namespace {

// A finished key, or "" when it is not one the ledger can carry.
size_t safeKey(const char* pick, char* out) {
  if (!copyKey(pick, std::strlen(pick), out) || !modelKeySafe(out)) { out[0] = 0; return 0; }
  return std::strlen(out);
}

// "custom": the raw id, never stripped; the echo only when it differs.
size_t customKey(const char* configured, const char* served, char* out) {
  return safeKey((*served && std::strcmp(served, configured) != 0) ? served : configured, out);
}

// Direct providers: the configured id, or the served echo on a real substitution.
size_t directKey(const char* configured, const char* served, char* out) {
  char nc[kModelKeyCap], ns[kModelKeyCap];
  normalizeModelId(configured, nc);
  normalizeModelId(served, ns);
  return safeKey((ns[0] && std::strcmp(ns, nc) != 0) ? ns : nc, out);
}

// "<up>/<bare>" into out, or "" when it does not fit or holds a further '/'.
size_t joinUpstream(const char* up, size_t upLen, const char* bare, char* out) {
  const size_t bl = std::strlen(bare);
  if (!bl || std::strchr(bare, '/') || upLen + 1 + bl >= kModelKeyCap) return 0;
  char upBuf[kModelKeyCap];
  std::memcpy(upBuf, up, upLen);
  upBuf[upLen] = 0;
  if (!provKeySafe(upBuf)) return 0;
  std::memcpy(out, up, upLen);
  out[upLen] = '/';
  std::memcpy(out + upLen + 1, bare, bl + 1);
  if (!modelKeySafe(out)) { out[0] = 0; return 0; }
  return std::strlen(out);
}

// "cumulo": "<upstream>/<bare>". The configured selector's upstream (a bare
// selector routes to the router default, openai); on a substitution, the echo's own
// prefix or the upstream its bare id implies.
size_t cumuloKey(const char* configured, const char* served, char* out) {
  const Split c = splitSelector(configured);
  const Split s = splitSelector(served);
  char nc[kModelKeyCap], ns[kModelKeyCap];
  normalizeModelId(c.bare, nc);
  normalizeModelId(s.bare, ns);
  if (ns[0] && std::strcmp(ns, nc) != 0) {
    const char* up = s.up ? s.up : inferUpstream(s.bare);
    return joinUpstream(up, s.up ? s.upLen : std::strlen(up), ns, out);
  }
  return joinUpstream(c.up ? c.up : "openai", c.up ? c.upLen : 6, nc, out);
}

}  // namespace

size_t usageModelKey(const char* host, const char* configured, const char* served, char* out) {
  out[0] = 0;
  if (!host || !*host) return 0;
  if (!configured) configured = "";
  if (!served) served = "";
  if (std::strcmp(host, "custom") == 0) return customKey(configured, served, out);
  if (std::strcmp(host, "cumulo") == 0) return cumuloKey(configured, served, out);
  return directKey(configured, served, out);
}

}  // namespace orch
}  // namespace nimbus
