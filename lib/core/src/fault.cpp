#include "nimbus/fault.h"

#include <cstddef>
#include <cctype>

namespace nimbus::fault {

namespace {
// bit i == Cap i faulted. A single aligned 16-bit store is atomic on the ESP32
// and the host, so readers (capability seams) and the writer (the test console /
// endpoint on another task) never need a lock for the flag itself.
volatile uint16_t g_mask = 0;

const char* const kNames[COUNT] = {"sd",     "memory", "mic",   "speaker",
                                   "led",    "screen", "sd_io", "provider"};
}  // namespace

bool active(Cap c) { return c < COUNT && (g_mask & (uint16_t(1) << c)) != 0; }

void set(Cap c, bool faulted) {
  if (c >= COUNT) return;
  const uint16_t bit = uint16_t(1) << c;
  if (faulted)
    g_mask = uint16_t(g_mask | bit);
  else
    g_mask = uint16_t(g_mask & ~bit);
}

void clearAll() { g_mask = 0; }

uint16_t mask() { return g_mask; }

const char* name(Cap c) { return c < COUNT ? kNames[c] : "?"; }

bool parse(const char* s, Cap& out) {
  if (!s) return false;
  for (uint8_t i = 0; i < COUNT; i++) {
    const char* n = kNames[i];
    size_t j = 0;
    for (; n[j] && s[j]; j++)
      if ((char)tolower((unsigned char)s[j]) != n[j]) break;
    if (!n[j] && !s[j]) {
      out = Cap(i);
      return true;
    }
  }
  return false;
}

}  // namespace nimbus::fault
