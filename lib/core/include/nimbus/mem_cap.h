#pragma once
// Ported verbatim from Nuage-Solide lib/core/mem_cap.h (Head Orchestrator v2).
//
// Portable, UTF-8-safe length cap. The DEVICE - never the model - enforces the
// memory/result length limits (you cannot trust a model to self-limit its
// "memory" field, and a runaway value would blow the prompt budget). A naive byte
// truncation (String::substring(0, n)) can split a multi-byte UTF-8 sequence,
// leaving invalid trailing bytes that corrupt the next provider prompt or run the
// ASCII glyph lookup past its table. utf8CapLen returns the largest length
// <= maxBytes that ends on a complete-character boundary, so the kept prefix
// [0, returned) is always valid UTF-8 when the input was.
//
// Header-only + Arduino-free so it is unit-tested natively (pio test -e native)
// and shared by orch_memory (directive + model memory caps) and the orchestrator
// fresh-result store / turn task cap.
namespace nimbus {

inline int utf8CapLen(const char* s, int len, int maxBytes) {
  if (maxBytes < 0) maxBytes = 0;
  if (len <= maxBytes) return len;
  int k = maxBytes;
  // A UTF-8 continuation byte matches 10xxxxxx. If the first byte we would drop
  // (s[k]) is a continuation byte, we are cutting mid-sequence - back up k until
  // s[k] starts a new character (lead byte 11xxxxxx or ASCII 0xxxxxxx).
  while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80) k--;
  return k;
}

}  // namespace nimbus
