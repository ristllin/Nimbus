#pragma once
// USB serial firmware update - device serial listener + portable wire protocol.
//
// A third firmware-update path beside signed cloud OTA and a ROM esptool flash:
// the RUNNING production firmware accepts a locally-built .bin over the USB serial
// cable and installs it through the SAME esp_ota A/B engine, rollback guard, and
// deferred reboot (otaupd::localBegin/localWrite/localFinish in ota_update.*). USB
// serial only, no auth: the physical cable is the trust boundary, matching esptool
// and the ROM flash path. A peer on Wi-Fi cannot reach the serial stream.
//
// This header is deliberately Arduino-free. The wire protocol (crc32, frame
// pack/parse) and the commit-order sequencer are portable so the exact code the
// device runs is host-tested under pio test -e native (test/test_usbfw_proto).
// The device-side pump API (namespace usbupd) is declared with plain types; only
// usb_updater.cpp pulls in Arduino / Serial.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nimbus::usbfw {

// ---- wire protocol ----------------------------------------------------------
// The host opens with a magic marker the reader would never see in normal serial
// traffic (production reads no serial today). Once it appears the reader collects
// a fixed START header, then framed data chunks, then a DONE trailer. Every
// multi-byte integer on the wire is little-endian.
//
//   START : "NIMBUSFW1" (9 bytes) + size (u32 LE) + sha256 (32 bytes)
//   CHNK  : "CHNK" (4 bytes) + len (u16 LE) + payload[len] + crc32 (u32 LE)
//   DONE  : "DONE" (4 bytes) + sha256 (32 bytes, over the whole image)
//
// Device replies are single newline-terminated lines prefixed "NFWU " so the host
// filters them out of ordinary firmware log noise on the same TX line:
//   NFWU ready | NFWU ack <n> | NFWU resend <n> | NFWU ok | NFWU err <reason>
// Every refusal or fault is "NFWU err <reason>" (reasons: size / confirm / busy /
// slot / short / badsha / sha-fail / commit / badframe / chunklen / shamismatch /
// flash-write / timeout).

inline constexpr char    kMagic[]     = "NIMBUSFW1";  // sent WITHOUT the trailing NUL
inline constexpr size_t  kMagicLen    = 9;
inline constexpr char    kChunkTag[]  = "CHNK";
inline constexpr char    kDoneTag[]   = "DONE";
inline constexpr size_t  kTagLen      = 4;
inline constexpr size_t  kSha256Len   = 32;
inline constexpr size_t  kStartHdrLen = 4 + kSha256Len;              // size + sha, after magic
inline constexpr size_t  kStartFrameLen = kMagicLen + kStartHdrLen;  // 45
inline constexpr size_t  kMaxChunk    = 4096;                        // host chunk payload ceiling
inline constexpr char    kReplyPrefix[] = "NFWU ";

// ---- little-endian helpers --------------------------------------------------

inline uint32_t rdU32le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
inline uint16_t rdU16le(const uint8_t* p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
inline void wrU32le(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}
inline void wrU16le(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}

// ---- crc32 (IEEE / zlib, seed 0) -------------------------------------------
// Matches Python's binascii.crc32 / zlib.crc32 so the host tool and the device
// agree byte for byte. Bytewise reflected form, poly 0xEDB88320.
inline uint32_t crc32(const uint8_t* d, size_t n, uint32_t seed = 0) {
  uint32_t crc = ~seed;
  for (size_t i = 0; i < n; ++i) {
    crc ^= d[i];
    for (int k = 0; k < 8; ++k)
      crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
  }
  return ~crc;
}

// ---- START header pack / parse ---------------------------------------------

struct StartHeader {
  uint32_t size = 0;
  uint8_t  sha256[kSha256Len] = {0};
};

// Writes kStartFrameLen bytes to out (magic + size + sha).
inline void packStart(uint8_t* out, const StartHeader& h) {
  std::memcpy(out, kMagic, kMagicLen);
  wrU32le(out + kMagicLen, h.size);
  std::memcpy(out + kMagicLen + 4, h.sha256, kSha256Len);
}
// Parses a full kStartFrameLen-byte frame. False if the magic does not match.
inline bool parseStart(const uint8_t* in, StartHeader& h) {
  if (std::memcmp(in, kMagic, kMagicLen) != 0) return false;
  h.size = rdU32le(in + kMagicLen);
  std::memcpy(h.sha256, in + kMagicLen + 4, kSha256Len);
  return true;
}

// ---- CHNK frame pack / crc-check -------------------------------------------

// Writes a full chunk frame to out (must hold kTagLen + 2 + len + 4 bytes) and
// returns the total byte count.
inline size_t packChunk(uint8_t* out, const uint8_t* payload, uint16_t len) {
  std::memcpy(out, kChunkTag, kTagLen);
  wrU16le(out + kTagLen, len);
  std::memcpy(out + kTagLen + 2, payload, len);
  wrU32le(out + kTagLen + 2 + len, crc32(payload, len));
  return kTagLen + 2 + len + 4;
}
// True iff the payload's crc32 matches the frame's trailing crc field.
inline bool checkChunkCrc(const uint8_t* payload, uint16_t len, uint32_t crcField) {
  return crc32(payload, len) == crcField;
}

// ---- hex helpers (sha256 <-> ascii hex) ------------------------------------

inline void bytesToHex(const uint8_t* d, size_t n, char* out) {
  static const char* H = "0123456789abcdef";
  for (size_t i = 0; i < n; ++i) {
    out[i * 2]     = H[d[i] >> 4];
    out[i * 2 + 1] = H[d[i] & 0xF];
  }
  out[n * 2] = '\0';
}
// Parses exactly n bytes of lowercase/uppercase hex. False on any non-hex nibble.
inline bool hexToBytes(const char* hex, uint8_t* out, size_t n) {
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < n; ++i) {
    int hi = nib(hex[i * 2]);
    int lo = nib(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

// ---- confirm gate -----------------------------------------------------------
// Whether the optional on-device confirm gate should refuse a USB update START
// right now. Default OFF (flagOn == false) never blocks. When ON it blocks unless
// an arm window is open: armUntilMs is a millis() deadline (0 = never armed), and
// the gate opens only while nowMs is strictly before it. Portable + host-tested so
// the OFF / on-but-unarmed / armed / armed-then-expired branches are covered
// without a device.
inline bool confirmGateBlocks(bool flagOn, uint32_t nowMs, uint32_t armUntilMs) {
  if (!flagOn) return false;             // gate off: cable == trust
  if (armUntilMs == 0) return true;      // on but never armed
  return (int32_t)(nowMs - armUntilMs) >= 0;   // armed window has expired
}

// ---- local-update decision helpers (pure, host-tested) ----------------------
// The mark-valid trigger must stay suppressed while an install is in flight. A
// local (USB) install arms the NEXT image's pending flag and stays "installing"
// through its deferred reboot; marking the CURRENT image valid in that window would
// clear the fresh flag and defeat rollback (CUM-390 review FIX 1). Factored out so
// the guard is host-tested, not just correct by inspection.
inline bool markValidAllowed(bool alreadyValid, bool installing, bool pending,
                             bool bootHealthy) {
  return !alreadyValid && !installing && pending && bootHealthy;
}

// The refusals localBegin makes BEFORE it claims the single-flight guard, in order.
// Returns nullptr to proceed to the atomic claim, else a machine reason. rebootArmed
// (state == ReadyToReboot) can never reopen the slot while an install is committed
// and a reboot is pending (CUM-390 review FIX 2, defense in depth atop the held
// guard). confirmBlocks is the confirmGateBlocks() verdict.
inline const char* localBeginPrecheck(bool sizeZero, bool rebootArmed,
                                      bool confirmBlocks) {
  if (sizeZero) return "size";
  if (rebootArmed) return "busy";
  if (confirmBlocks) return "confirm";
  return nullptr;
}

// ---- commit-order sequencer -------------------------------------------------
// The load-bearing arm-before-flip order the rollback guard depends on
// (CUM-389 / CUM-270), factored out of ota_update's cloud install so the local
// (USB) path runs the SAME sequence and a host test can assert it without a
// device. Hooks are injected so the test can record the call order.
struct CommitHooks {
  void* ctx = nullptr;
  void (*setPrevSlot)(void*, const char*)     = nullptr;
  void (*setBootCount)(void*, int)            = nullptr;
  void (*setPending)(void*, int)              = nullptr;
  void (*setLastResult)(void*, const char*)   = nullptr;
  void (*setPendingNotes)(void*, const char*) = nullptr;
  bool (*endFlip)(void*)                       = nullptr;  // Update.end(); true = flipped
};

// Arm the NVS rollback guard (prev / boots / pending) while STILL running the old
// image, record the pending result, then flip the boot pointer. Power loss after
// the arm but before the flip is the "aborted-preflip" case bootGuard disarms;
// after the flip the guard is already armed. If the flip fails, disarm the guard
// so a boot that never moved is not counted against the rollback budget. Returns
// true iff the flip succeeded.
inline bool commitInstall(const CommitHooks& h, const char* prevSlot,
                          const char* resultBefore, const char* notes) {
  h.setPrevSlot(h.ctx, prevSlot);
  h.setBootCount(h.ctx, 0);
  h.setPending(h.ctx, 1);
  h.setLastResult(h.ctx, resultBefore);
  if (h.setPendingNotes) h.setPendingNotes(h.ctx, notes ? notes : "");
  if (!h.endFlip(h.ctx)) {
    h.setPending(h.ctx, 0);
    return false;
  }
  return true;
}

// ---- framing reader ---------------------------------------------------------
// The device-side receive state machine, factored out of the Serial glue so the
// framing logic (magic scan, chunk buffering, per-chunk crc -> ack/resend, the
// START-vs-DONE sha cross-check, and every error transition) is host-tested
// without a board. Feed it one byte at a time; it drives the injected sink on
// each protocol event and never blocks. usb_updater.cpp wires the sink to
// otaupd::local* + the "NFWU " serial replies; the host test wires a recorder.
struct ReaderSink {
  void* ctx = nullptr;
  // START parsed. Return true to accept (device claimed the slot) so the reader
  // enters streaming; false to refuse (the sink emits the refusal itself).
  bool (*onStart)(void*, uint32_t size, const uint8_t sha256[32]) = nullptr;
  // A crc-valid chunk at index seq. Return true if the write succeeded (the sink
  // acks seq); false fails the transfer (the sink emits the error, reader resets).
  bool (*onChunkOk)(void*, uint32_t seq, const uint8_t* data, uint16_t len) = nullptr;
  // A chunk whose crc failed: the same seq will be resent by the host.
  void (*onResend)(void*, uint32_t seq) = nullptr;
  // DONE parsed and cross-checked (sha256 == the START sha). Return true if the
  // install finished. Either way the reader returns to scanning afterwards.
  bool (*onDone)(void*, const uint8_t sha256[32]) = nullptr;
  // A framing error the host should hear ("badframe"/"chunklen"/"shamismatch").
  void (*onError)(void*, const char* reason) = nullptr;
};

class FrameReader {
 public:
  ReaderSink sink;

  void reset() {
    phase_ = P::Scan; magic_ = 0; bufN_ = 0; need_ = 0; chunkLen_ = 0;
    seq_ = 0; done_ = false;
  }
  bool scanning() const { return phase_ == P::Scan; }  // no transfer in flight
  bool done() const { return done_; }                  // a DONE finished successfully

  void feed(uint8_t c) {
    if (phase_ == P::Scan) { scan(c); return; }
    buf_[bufN_++] = c;
    if (bufN_ < need_) return;
    switch (phase_) {
      case P::Header:    header();    return;
      case P::FrameTag:  frameTag();  return;
      case P::ChunkLen:  chunkLen();  return;
      case P::ChunkBody: chunkBody(); return;
      case P::DoneHash:  doneHash();  return;
      case P::Scan:      return;      // unreachable (handled above)
    }
  }

 private:
  enum class P : uint8_t { Scan, Header, FrameTag, ChunkLen, ChunkBody, DoneHash };

  void expect(P p, size_t need) { phase_ = p; bufN_ = 0; need_ = need; }
  void fail(const char* reason) {
    if (sink.onError) sink.onError(sink.ctx, reason);
    reset();
  }

  void scan(uint8_t c) {
    // Rolling match; a mismatch that still matches magic[0] keeps the run alive.
    if (c == (uint8_t)kMagic[magic_]) {
      if (++magic_ == kMagicLen) expect(P::Header, kStartHdrLen);
    } else {
      magic_ = (c == (uint8_t)kMagic[0]) ? 1 : 0;
    }
  }
  void header() {
    uint8_t frame[kStartFrameLen];
    std::memcpy(frame, kMagic, kMagicLen);
    std::memcpy(frame + kMagicLen, buf_, kStartHdrLen);
    StartHeader h;
    parseStart(frame, h);
    std::memcpy(startSha_, h.sha256, kSha256Len);
    if (sink.onStart && sink.onStart(sink.ctx, h.size, h.sha256)) {
      seq_ = 0;
      expect(P::FrameTag, kTagLen);
    } else {
      reset();   // the sink emitted the refusal
    }
  }
  void frameTag() {
    if (std::memcmp(buf_, kChunkTag, kTagLen) == 0) expect(P::ChunkLen, 2);
    else if (std::memcmp(buf_, kDoneTag, kTagLen) == 0) expect(P::DoneHash, kSha256Len);
    else fail("badframe");
  }
  void chunkLen() {
    chunkLen_ = rdU16le(buf_);
    if (chunkLen_ == 0 || chunkLen_ > kMaxChunk) { fail("chunklen"); return; }
    expect(P::ChunkBody, (size_t)chunkLen_ + 4);   // payload + crc32
  }
  void chunkBody() {
    const uint32_t crcField = rdU32le(buf_ + chunkLen_);
    if (!checkChunkCrc(buf_, chunkLen_, crcField)) {
      if (sink.onResend) sink.onResend(sink.ctx, seq_);   // host resends this seq
      expect(P::FrameTag, kTagLen);
      return;
    }
    if (!(sink.onChunkOk && sink.onChunkOk(sink.ctx, seq_, buf_, chunkLen_))) {
      reset();   // the sink emitted the write error
      return;
    }
    ++seq_;
    expect(P::FrameTag, kTagLen);
  }
  void doneHash() {
    if (std::memcmp(buf_, startSha_, kSha256Len) != 0) { fail("shamismatch"); return; }
    done_ = sink.onDone && sink.onDone(sink.ctx, buf_);
    // The DONE frame is consumed either way; return to scanning (done_ persists so
    // the caller can act on a success), without clearing done_ via reset().
    phase_ = P::Scan;
    magic_ = 0;
    bufN_ = 0;
  }

  P        phase_ = P::Scan;
  size_t   magic_ = 0;
  size_t   bufN_ = 0;
  size_t   need_ = 0;
  uint16_t chunkLen_ = 0;
  uint32_t seq_ = 0;
  bool     done_ = false;
  uint8_t  startSha_[kSha256Len] = {0};
  uint8_t  buf_[kMaxChunk + 4];      // largest run: a chunk payload + its crc32
};

}  // namespace nimbus::usbfw

// ---- device serial-update listener -----------------------------------------
// Production-only pump (main.cpp wires it out of test / notifier-debug builds so
// it never competes with those Serial readers). Inert until the magic marker
// appears; bounded bytes per pump so the RX path never starves the main loop.
namespace usbupd {
void begin();   // arm the listener (call once from setup, after Serial.begin)
void pump();    // call every main-loop pass; cheap no-op until a push starts
bool active();  // true while a push is mid-flight (holds the OTA single-flight)
}  // namespace usbupd
