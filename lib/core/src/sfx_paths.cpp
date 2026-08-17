#include "nimbus/sfx_paths.h"

#include <cstring>

namespace nimbus::sfx {

namespace {

// Filesystem-safe AND HTTP-request-line-safe characters only.
bool pathCharOk(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
         (ch >= '0' && ch <= '9') || ch == '/' || ch == '.' ||
         ch == '_' || ch == '-';
}

// Parse the all-digit run [begin, end) -> *out. False on empty / non-digit /
// absurdly large (>9999 - also bounds the digit run length).
bool parseDigits(const char* begin, const char* end, unsigned* out) {
  if (begin >= end) return false;
  unsigned n = 0;
  for (const char* c = begin; c < end; c++) {
    if (*c < '0' || *c > '9') return false;
    n = n * 10 + (unsigned)(*c - '0');
    if (n > 9999) return false;
  }
  *out = n;
  return true;
}

}  // namespace

bool safeRepoPath(const char* p) {
  if (!p) return false;
  if (strncmp(p, "sd/", 3) != 0) return false;          // basic tier is embedded, not synced
  if (strncmp(p, "sd/custom/", 10) == 0) return false;  // owner-local pool: sync must not touch it
  if (strstr(p, "..")) return false;                    // no parent-dir traversal
  for (const char* c = p; *c; c++) {
    if (!pathCharOk(*c)) return false;
  }
  return true;
}

bool parseClipFilename(const char* name, char* slugOut, size_t slugCap, unsigned* nOut) {
  if (!name || !slugOut || slugCap == 0 || !nOut) return false;
  const size_t len = strlen(name);
  if (len < 7) return false;  // shortest valid: "a-0.wav"
  if (strcmp(name + len - 4, ".wav") != 0) return false;
  const char* end = name + len - 4;  // one past the digits
  // Find the LAST dash before the extension.
  const char* dash = nullptr;
  for (const char* c = end - 1; c > name; c--) {
    if (*c == '-') { dash = c; break; }
  }
  if (!dash) return false;
  unsigned n = 0;
  if (!parseDigits(dash + 1, end, &n)) return false;
  const size_t slugLen = (size_t)(dash - name);
  if (slugLen == 0 || slugLen >= slugCap) return false;
  memcpy(slugOut, name, slugLen);
  slugOut[slugLen] = 0;
  *nOut = n;
  return true;
}

}  // namespace nimbus::sfx
