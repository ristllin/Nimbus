#pragma once
#include <cstdint>

// speaker_memory - when the speaker may open, in DMA-capable internal SRAM.
//
// Opening the speaker allocates the I2S channel's 8 buffers of 1024 B each (solide-drivers
// audio.cpp: 8 descriptors, 256 stereo 16-bit frames). They must be DMA-capable, so only the
// main internal SRAM serves them; the 7.4 KB RTC block counted in "internal free" does not.
// On Lumi during photo turns (1 Nov 2026) a sound's open took internal free to about 11 KB:
// 7.4 KB of RTC and 3.4 KB of main SRAM, in pieces under 1 KB. The ninth block failed, the open
// gave up, and for that moment the radio's own buffers had nothing left either. A sound is
// decoration, so the rule is to not try:
//   - a cue opens only when the buffers fit and kCueDmaReserve stays free for the radio;
//   - a cue whose open still failed (fragmentation the totals cannot show) holds the cues back
//     for a minute, doubling to kHoldMaxMs, until one plays again;
//   - what the owner asked for (a spoken reply, a track, a test sound) keeps a smaller reserve
//     and no hold: it fails honestly when memory says no.
// Portable and host-tested (test_sfx).
namespace nimbus {
namespace sfx {

inline constexpr uint32_t kSpeakerDmaBuffers = 8;
inline constexpr uint32_t kSpeakerDmaBufferBytes = 1024;
// The buffers with their allocator headers, plus the channel's descriptors and control block.
inline constexpr uint32_t kSpeakerDmaNeedBytes = kSpeakerDmaBuffers * (kSpeakerDmaBufferBytes + 16) + 512;
// One buffer needs one block this large.
inline constexpr uint32_t kSpeakerDmaMinLargest = kSpeakerDmaBufferBytes + 16;
// What stays free after the open: five of the radio's 1600 B buffers for a cue, two for a
// sound the owner asked for.
inline constexpr uint32_t kCueDmaReserve = 8192;
inline constexpr uint32_t kAskedDmaReserve = 3200;

enum class SoundKind : uint8_t { Cue, Asked };

constexpr bool speakerMemoryOk(SoundKind kind, uint32_t dmaFree, uint32_t dmaLargest) {
  return dmaLargest >= kSpeakerDmaMinLargest &&
         dmaFree >= kSpeakerDmaNeedBytes + (kind == SoundKind::Cue ? kCueDmaReserve : kAskedDmaReserve);
}

// The cues' hold, on the sfx task only. Time is injected.
class CueGate {
 public:
  static constexpr uint32_t kHoldFirstMs = 60u * 1000u;
  static constexpr uint32_t kHoldMaxMs = 16u * 60u * 1000u;
  static constexpr uint32_t kLogEveryMs = 10u * 60u * 1000u;

  // Whether this sound may open now. A refused cue is counted; no heap call was spent on it.
  bool allow(SoundKind kind, uint32_t dmaFree, uint32_t dmaLargest, uint32_t nowMs);
  // The sound ended. `played` = it reached the speaker; `allocFailed` = an allocation failed
  // while it tried (a missing clip is neither a success nor a memory failure).
  void finished(bool played, bool allocFailed, uint32_t nowMs);
  // True at most once per kLogEveryMs while cues are being refused; `skipped()` then says how
  // many since the last line and is cleared by clearSkipped().
  bool logDue(uint32_t nowMs);
  uint32_t skipped() const { return skipped_; }
  void clearSkipped() { skipped_ = 0; }
  // The hold in force (0 = none), for tests and the log line.
  uint32_t holdMs() const { return holdMs_; }

 private:
  uint32_t holdMs_ = 0;
  uint32_t holdFrom_ = 0;
  uint32_t lastLog_ = 0;
  uint32_t skipped_ = 0;
  bool     logged_ = false;
};

}  // namespace sfx
}  // namespace nimbus
