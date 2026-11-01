#pragma once
#include <cstdint>

// speaker_memory - when the speaker may open, in DMA-capable internal SRAM.
//
// Opening the speaker allocates the I2S driver's buffers of 1024 B each (8 descriptors, 256
// stereo 16-bit frames, solide-drivers audio.cpp). They must be DMA-capable, so only the main
// internal SRAM serves them; the 7.4 KB RTC block counted in "internal free" does not. How
// many depends on the board, and every open pays it again because the channel is torn down
// when idle:
//   - a raw I2S amp (Solide S3): the TX channel only, 8 buffers;
//   - the ES8311 codec (Freenove boards, Lumi): one full-duplex channel, TX and RX, 16 buffers.
//
// On Lumi during photo turns (1 Nov 2026) a sound's open took internal free to about 11 KB:
// 7.4 KB of RTC and 3.4 KB of main SRAM, in pieces under 1 KB. The next buffer failed, the open
// gave up, and for that moment the radio's own buffers had nothing left either. A sound is
// decoration, so the rule is to not try:
//   - a cue opens only when the buffers fit and the profile's cue reserve stays free;
//   - a cue whose open still failed (fragmentation the totals cannot show) holds the cues back
//     for a minute, doubling to kHoldMaxMs, until one plays again;
//   - what the owner asked for (a spoken reply, a track, a test sound) keeps a smaller reserve
//     and no hold: it fails honestly when memory says no.
// Portable and host-tested (test_sfx).
namespace nimbus {
namespace sfx {

struct SpeakerProfile {
  uint32_t buffers;       // I2S DMA buffers one open allocates
  uint32_t channels;      // I2S channels one open creates
  uint32_t cueReserve;    // what stays free after a cue's open
  uint32_t askedReserve;  // what stays free after the open of a sound the owner asked for
};

inline constexpr uint32_t kSpeakerDmaBufferBytes = 1024;
// One buffer needs one block this large.
inline constexpr uint32_t kSpeakerDmaMinLargest = kSpeakerDmaBufferBytes + 16;
// The radio's buffers are 1600 B. The raw board has room for five after a cue and two after an
// asked sound. Two after a cue and one after an asked sound is what the codec board can give
// and still play at all: Lumi rests at 21.8 KB of DMA memory (board freenove_s3, Nov 2026), so
// an 8 KB reserve on top of the 17.7 KB open would silence every cue on every Freenove unit at
// rest. Cues at rest have run there with about 4 KB left since the codec's idle release.
inline constexpr SpeakerProfile kTxOnlySpeaker{8, 1, 8192, 3200};
inline constexpr SpeakerProfile kCodecSpeaker{16, 2, 3200, 1600};

constexpr SpeakerProfile speakerProfile(bool fullDuplexCodec) {
  return fullDuplexCodec ? kCodecSpeaker : kTxOnlySpeaker;
}

// The buffers with their allocator headers, plus each channel's descriptors and control block.
constexpr uint32_t speakerNeedBytes(const SpeakerProfile& p) {
  return p.buffers * kSpeakerDmaMinLargest + p.channels * 512;
}

enum class SoundKind : uint8_t { Cue, Asked };

constexpr bool speakerMemoryOk(const SpeakerProfile& p, SoundKind kind, uint32_t dmaFree, uint32_t dmaLargest) {
  return dmaLargest >= kSpeakerDmaMinLargest &&
         dmaFree >= speakerNeedBytes(p) + (kind == SoundKind::Cue ? p.cueReserve : p.askedReserve);
}

// What one sound's walk through its clip tiers came to.
struct ClipResult {
  bool played;
  bool allocFailed;
};

// Tries the tiers 0..n-1 until one plays. A tier that only has no clip falls through to the next;
// one that failed an allocation ends the walk, because each further tier would be another open
// taking the radio's memory for nothing. `playTier(i)` plays tier i, `allocFails()` reads the
// heap's failure count.
template <class PlayTier, class AllocFails>
ClipResult playFirstTier(int n, PlayTier playTier, AllocFails allocFails) {
  const uint32_t before = allocFails();
  for (int i = 0; i < n; ++i) {
    if (playTier(i)) return {true, false};
    if (allocFails() != before) return {false, true};
  }
  return {false, false};
}

// The cues' hold, on the sfx task only. Time is injected.
class CueGate {
 public:
  static constexpr uint32_t kHoldFirstMs = 60u * 1000u;
  static constexpr uint32_t kHoldMaxMs = 16u * 60u * 1000u;
  static constexpr uint32_t kLogEveryMs = 10u * 60u * 1000u;
  static constexpr uint32_t kAskedLogEveryMs = 10u * 1000u;

  explicit CueGate(const SpeakerProfile& profile) : profile_(profile) {}

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
  // A sound the owner asked for did not play. True when that deserves a log line now: the first
  // at once, then at most once per kAskedLogEveryMs. The owner has no other sign of it.
  bool askedLineDue(uint32_t nowMs);
  // The hold in force (0 = none), for tests and the log line.
  uint32_t holdMs() const { return holdMs_; }

 private:
  SpeakerProfile profile_;
  uint32_t holdMs_ = 0;
  uint32_t holdFrom_ = 0;
  uint32_t lastLog_ = 0;
  uint32_t skipped_ = 0;
  bool     logged_ = false;
  uint32_t lastAskedLine_ = 0;
  bool     askedLogged_ = false;
};

}  // namespace sfx
}  // namespace nimbus
