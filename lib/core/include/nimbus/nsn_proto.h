#pragma once
#include <cstddef>
#include <cstdint>

// nsn_proto - codec for the nsn wire protocol (nsnotify).
//
// Reference implementation: nsnotify/notify/broker/frame.py
// (https://github.com/ristllin/nsnotify).
// Byte compatibility is proven by test vectors generated from that encoder
// (tools/gen_nsn_vectors.py -> test/test_proto/nsn_vectors.h); regenerate the
// vectors whenever the reference changes.
//
// Wire format:  [SOF 0xAA] [LEN] [payload: LEN bytes] [CRC8-MAXIM over payload]
// Payload (v1): [MAGIC 0x4E] [seq] [count N<=16] [brightness] [N x 4-byte segment]
// Segment:      [state] [hue 0-254, 255=white] [anim] [span, 0=auto-even]
//
// PROTOCOL v2 (backward-compatible, same MAGIC 0x4E): after the base N-segment
// block the payload may carry an OPTIONAL per-segment extension so the e-ink can
// name a session - [harness:1][titleLen:1][title: titleLen bytes] repeated for the
// first N segments. A v1 decoder ignores these trailing bytes (they are still
// covered by the CRC), so a v2 frame decodes cleanly (harness/title empty) on old
// firmware; the broker only SENDS v2 to a device that advertised protoVer>=2.
//
// state/anim values match solide::ring::Status/Anim and notify.state by design.

namespace nimbus::nsn {

constexpr uint8_t kSof     = 0xAA;
constexpr uint8_t kMagic   = 0x4E;
constexpr int     kMaxSegs = 16;
constexpr int     kMaxTitle = 24;   // per-segment v2 title cap (bytes); broker budgets to the MTU
// Cap sized for v2: SOF+LEN(<=255)+CRC. A v2 payload = base (4+4N) + N*(2+title) can
// exceed the v1 71-byte cap, so raise the buffer; the broker keeps each frame within
// the negotiated BLE MTU (<= 255 payload).
constexpr size_t  kMaxPacket = 258;

constexpr uint8_t kMaxState = 6;  // notify.State: Idle..Offline
constexpr uint8_t kMaxAnim  = 5;  // notify.Anim:  Off..Fade

// Harness tag (v2). 0 = unknown/absent (v1 frames + brokers that don't set it).
constexpr uint8_t kHarnessUnknown = 0, kHarnessClaude = 1, kHarnessCodex = 2, kHarnessVibe = 3;
const char* harnessName(uint8_t h);   // "claude"/"codex"/"vibe"/"" (unknown)

struct Segment {
  uint8_t state = 0;
  uint8_t hue   = 0;
  uint8_t anim  = 0;
  uint8_t span  = 0;
  uint8_t harness = 0;                 // v2: kHarness* (0 = unknown)
  char    title[kMaxTitle + 1] = {0};  // v2: NUL-terminated short title ("" = none)
};

struct Frame {
  uint8_t seq        = 0;
  uint8_t brightness = 0;
  uint8_t count      = 0;
  Segment segs[kMaxSegs];
};

// CRC-8/MAXIM (poly 0x31 reflected -> 0x8C, init 0x00), identical to frame.py.
uint8_t crc8(const uint8_t* data, size_t len);

// Encode a complete packet. Frames with count > kMaxSegs are truncated to
// kMaxSegs (mirroring the reference). Returns bytes written, or 0 if cap is
// too small.
size_t encode(const Frame& f, uint8_t* out, size_t cap);

// One-shot decode of a complete packet (SOF/LEN/CRC wrapper included).
// Mirrors frame.py's tolerance: extra bytes after the packet are ignored,
// truncated segment records are dropped. Unlike the Python reference (which
// raises on out-of-range enums), out-of-range state/anim rejects the frame -
// on-device we drop garbage rather than crash. Returns false on any error.
bool decodePacket(const uint8_t* data, size_t len, Frame& out);

// Incremental decoder for a byte stream (serial RX): hunts for SOF, buffers
// one packet, validates. feed() returns true exactly when a complete valid
// frame has been decoded into `out`. Invalid packets are skipped silently
// (resync on next SOF).
class Decoder {
 public:
  bool feed(uint8_t byte, Frame& out);
  void reset();

 private:
  enum class St : uint8_t { Sof, Len, Body };
  St      st_  = St::Sof;
  uint8_t len_ = 0;              // payload length from header
  size_t  have_ = 0;             // bytes accumulated in buf_ (payload + crc)
  uint8_t buf_[kMaxPacket];
};

}  // namespace nimbus::nsn
