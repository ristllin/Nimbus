// USB serial firmware update - device serial listener. See usb_updater.h for the
// wire protocol and the design rationale. This is the ONLY production Serial
// reader; main.cpp wires the pump out of test / notifier-debug builds (which own
// Serial through the test console / nsn debug channel) so two readers never race.
//
// The framing state machine lives in nimbus::usbfw::FrameReader (portable, host-
// tested). This file is the thin glue: it feeds the reader bytes from the main-
// loop pump (bounded per pass so the RX path never starves the loop), wires the
// reader's sink to otaupd::local* plus the "NFWU " replies, aborts a wedged
// transfer on a stall timeout, and stages the post-install reboot on the main task
// (never inline on AsyncTCP, matching the CUM-270 rule) with a short delay so the
// "ok" line flushes first.
#include <Arduino.h>

#include "usb_updater.h"

#include <esp_system.h>
#include <esp_task_wdt.h>

#include "ota_update.h"

namespace usbupd {
namespace {

using namespace nimbus::usbfw;

FrameReader s_reader;
uint32_t    s_lastByteMs = 0;      // for the mid-transfer stall timeout
bool        s_rebootPending = false;
uint32_t    s_rebootAtMs = 0;
uint32_t    s_pumpWork = 0;        // sink events (chunk ack/resend) this pump pass

constexpr int      kByteBudget    = kMaxChunk;   // bytes drained per pump pass
constexpr uint32_t kStallMs       = 20 * 1000;   // abort a wedged transfer
constexpr uint32_t kRebootDelayMs = 1500;        // let "ok" flush before restart
constexpr uint32_t kMaxWorkPerPump = 32;         // chunk events before yielding the loop
constexpr uint32_t kMaxPumpMs      = 8;          // wall-clock ceiling per pump pass

// ack/resend replies flush: the HWCDC TX task does NOT drain a small buffered line
// promptly, so a lock-step host (push_firmware.py sends a chunk, waits for its ack)
// would time out if the ack sat unflushed. For the honest host the flush returns at
// once (it is draining TX); a malicious flood that never drains is bounded instead
// by the per-pass work cap (kMaxWorkPerPump) + the esp_task_wdt_reset in pump(), so
// the flush can no longer starve the loop into a watchdog reset.
void replyNum(const char* verb, uint32_t n) {
  Serial.print(kReplyPrefix);
  Serial.print(verb);
  Serial.print(' ');
  Serial.println(n);
  Serial.flush();
}
void reply(const char* tail) {
  Serial.print(kReplyPrefix);
  Serial.println(tail);
  Serial.flush();
}
void replyWhy(const char* verb, const char* why) {
  Serial.print(kReplyPrefix);
  Serial.print(verb);
  Serial.print(' ');
  Serial.println(why && why[0] ? why : verb);
  Serial.flush();
}

// ---- reader sink: framing events -> the OTA engine + serial replies ---------

bool onStart(void*, uint32_t size, const uint8_t*) {
  const char* why = "";
  if (otaupd::localBegin(size, &why)) {
    reply("ready");
    return true;
  }
  replyWhy("err", why);   // "size" / "confirm" / "busy" / "slot"
  return false;
}

bool onChunkOk(void*, uint32_t seq, const uint8_t* data, uint16_t len) {
  ++s_pumpWork;
  if (!otaupd::localWrite(data, len)) {
    reply("err flash-write");   // localWrite already aborted the session
    return false;
  }
  replyNum("ack", seq);
  return true;
}

void onResend(void*, uint32_t seq) { ++s_pumpWork; replyNum("resend", seq); }

bool onDone(void*, const uint8_t* sha256) {
  char hex[kSha256Len * 2 + 1];
  bytesToHex(sha256, kSha256Len, hex);
  const char* why = "";
  if (otaupd::localFinish(hex, &why)) {
    reply("ok");
    return true;
  }
  replyWhy("err", why[0] ? why : "finish");
  return false;
}

void onError(void*, const char* reason) {
  replyWhy("err", reason);
  if (otaupd::localActive()) otaupd::localAbort();   // discard the half-open slot
}

void wireSink() {
  s_reader.sink.onStart   = onStart;
  s_reader.sink.onChunkOk = onChunkOk;
  s_reader.sink.onResend  = onResend;
  s_reader.sink.onDone    = onDone;
  s_reader.sink.onError   = onError;
}

}  // namespace

void begin() {
  wireSink();
  s_reader.reset();
  s_rebootPending = false;
}

void pump() {
  esp_task_wdt_reset();   // this pass may do bounded flash work; keep the WDT fed

  // Fire a staged reboot once the "ok" reply has had time to flush.
  if (s_rebootPending && (int32_t)(millis() - s_rebootAtMs) >= 0) {
    Serial.flush();
    esp_restart();
  }

  // Abort a transfer that stalled mid-stream so a wedged host never holds the OTA
  // single-flight guard forever (which would block background cloud checks).
  if (!s_reader.scanning() && s_lastByteMs &&
      (int32_t)(millis() - s_lastByteMs) >= (int32_t)kStallMs) {
    reply("err timeout");
    if (otaupd::localActive()) otaupd::localAbort();
    s_reader.reset();
  }

  // Bound BOTH bytes read AND work done (chunk ack/resend events) per pass, and cap
  // wall-clock time, so a chunk flood or a bad-crc resend flood can never monopolize
  // loop() / starve the watchdog. Remaining bytes wait for the next pump.
  const uint32_t entry = millis();
  s_pumpWork = 0;
  int budget = kByteBudget;
  while (budget-- > 0 && Serial.available() > 0) {
    const int c = Serial.read();
    if (c < 0) break;
    s_lastByteMs = millis();
    s_reader.feed((uint8_t)c);
    if (s_reader.done() && !s_rebootPending) {
      s_rebootPending = true;
      s_rebootAtMs = millis() + kRebootDelayMs;
      break;   // image committed; stop reading and let the reboot fire
    }
    if (s_pumpWork >= kMaxWorkPerPump ||
        (uint32_t)(millis() - entry) >= kMaxPumpMs)
      break;   // yield the loop; the rest drains next pass
  }
}

bool active() { return otaupd::localActive(); }

}  // namespace usbupd
