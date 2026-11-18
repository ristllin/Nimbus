#pragma once
// link_time - lane RB-M2's bench timing seam (test builds only).
//
// The latency model of the rebuild PRD (section 2) reads a few constants from the bench:
// the connect time, the upload rate, the provider's wait and the read of one exchange,
// and where the time between a model's result and the Telegram send goes. The serial
// console (src/test_console.cpp, verbs LINKX and LOGMS) reads them from here.
//
// Everything that records lives inside NIMBUS_TEST. In a production image the marks are
// empty statements and no symbol of this file exists: every name carries "linkTime", the
// marker tools/release_gate/check_elf_symbols.py refuses in a shipped ELF.
#include <stdint.h>

#if defined(NIMBUS_TEST)
#include <Arduino.h>

namespace nimbus {
namespace linkTime {

// The phases of the most recent provider exchange (transport_tls.cpp), uptime ms.
// 0 = not reached. One exchange runs at a time (the TLS work slot), and the console
// reads these right after its own exchange returns, on an idle bench.
struct LinkTimePhases {
  uint32_t start = 0;    // took the work slot
  uint32_t conn = 0;     // the TLS handshake finished (all retries)
  uint32_t head = 0;     // the request head was written
  uint32_t body0 = 0;    // the first body slice started
  uint32_t body1 = 0;    // the last body byte was written
  uint32_t paced = 0;    // ms of body1 - body0 spent waiting for DMA headroom
  uint32_t status = 0;   // the status line arrived (the provider's wait ends here)
  uint32_t hdrs = 0;     // the header block was read
  uint32_t done = 0;     // the body was read and the socket closed
  bool live = false;     // this record belongs to the exchange running now (armed by the console)
};

extern LinkTimePhases linkTimeLast;
// Set by the console just before its own exchange. Only the next exchange to take the
// work slot records (it takes the arm), and it stops recording before it gives the slot
// back, so another task's exchange can never write into the console's record.
extern volatile bool linkTimeArmed;
// LOGMS on: every serial log line ("[agent] ...") carries the device's uptime in ms, so a
// bench script reads device-side times off the existing log lines.
extern volatile bool linkTimeLogMs;

inline void linkTimeBegin(uint32_t nowMs) {
  if (!linkTimeArmed) return;
  linkTimeArmed = false;
  linkTimeLast = LinkTimePhases{};
  linkTimeLast.start = nowMs;
  linkTimeLast.live = true;
}

}  // namespace linkTime
}  // namespace nimbus

#define LINKTIME_BEGIN(ms) ::nimbus::linkTime::linkTimeBegin(ms)
#define LINKTIME_MARK(field) \
  (::nimbus::linkTime::linkTimeLast.live ? (void)(::nimbus::linkTime::linkTimeLast.field = (uint32_t)millis()) : (void)0)
#define LINKTIME_SET(field, v) \
  (::nimbus::linkTime::linkTimeLast.live ? (void)(::nimbus::linkTime::linkTimeLast.field = (uint32_t)(v)) : (void)0)
#define LINKTIME_END() (::nimbus::linkTime::linkTimeLast.live = false)
#else
#define LINKTIME_BEGIN(ms) ((void)0)
#define LINKTIME_MARK(field) ((void)0)
#define LINKTIME_SET(field, v) ((void)0)
#define LINKTIME_END() ((void)0)
#endif
