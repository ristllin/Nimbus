#pragma once
#include <cstddef>
#include <cstdint>

// wav_clip - read a recorded clip's WAV header so the test console's AUDIOINJECT
// can feed it through the hold-to-talk path in place of the microphone.
//
// The mic records 16 kHz, mono, 16-bit little-endian PCM into /voice.pcm (no
// header); the speech-to-text step reads exactly that. A clip injected for the
// online voice leg must therefore be the same format, and the injector copies only
// its sample data. This parser finds that data and refuses anything else, so a
// wrong clip fails with a reason instead of sending noise to a paid STT call.
//
// Portable and header-only (no Arduino): host-tested in test/test_wav_clip over
// valid clips, extra chunks before the data, odd-sized chunk padding, and every
// refusal. Little-endian reads only; RIFF is little-endian by definition.
namespace nimbus::voice {

struct WavInfo {
  uint16_t format = 0;          // 1 = PCM
  uint16_t channels = 0;
  uint32_t sampleRate = 0;
  uint16_t bitsPerSample = 0;
  uint32_t dataOffset = 0;      // byte offset of the first sample
  uint32_t dataBytes = 0;       // size of the data chunk as declared
};

enum class WavErr : uint8_t { Ok, TooShort, NotRiff, NotWave, NoFmt, NotPcm, NoData };

inline const char* wavErrName(WavErr e) {
  switch (e) {
    case WavErr::Ok:       return "ok";
    case WavErr::TooShort: return "too short for a WAV header";
    case WavErr::NotRiff:  return "not a RIFF file";
    case WavErr::NotWave:  return "RIFF but not WAVE";
    case WavErr::NoFmt:    return "no fmt chunk before the data";
    case WavErr::NotPcm:   return "not uncompressed PCM";
    case WavErr::NoData:   return "no data chunk in the header window";
  }
  return "unknown";
}

namespace wav_detail {
inline uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline bool tag(const uint8_t* p, const char* t) {
  return p[0] == t[0] && p[1] == t[1] && p[2] == t[2] && p[3] == t[3];
}
}  // namespace wav_detail

// Parse the header from the first `len` bytes of a clip. The data chunk must
// start inside that window (a few hundred bytes covers any real header).
inline WavErr parseWavHeader(const uint8_t* buf, size_t len, WavInfo& out) {
  using namespace wav_detail;
  out = WavInfo{};
  if (!buf || len < 12) return WavErr::TooShort;
  if (!tag(buf, "RIFF")) return WavErr::NotRiff;
  if (!tag(buf + 8, "WAVE")) return WavErr::NotWave;
  bool haveFmt = false;
  size_t pos = 12;
  while (pos + 8 <= len) {
    const uint8_t* ch = buf + pos;
    const uint32_t size = le32(ch + 4);
    if (tag(ch, "fmt ")) {
      if (size < 16 || pos + 8 + 16 > len) return WavErr::TooShort;
      out.format = le16(ch + 8);
      out.channels = le16(ch + 10);
      out.sampleRate = le32(ch + 12);
      out.bitsPerSample = le16(ch + 22);
      if (out.format != 1) return WavErr::NotPcm;
      haveFmt = true;
    } else if (tag(ch, "data")) {
      if (!haveFmt) return WavErr::NoFmt;
      out.dataOffset = uint32_t(pos + 8);
      out.dataBytes = size;
      return WavErr::Ok;
    }
    // Next chunk; RIFF pads an odd-sized chunk with one byte.
    const uint64_t next = uint64_t(pos) + 8u + size + (size & 1u);
    if (next > len) break;
    pos = size_t(next);
  }
  return haveFmt ? WavErr::NoData : WavErr::NoFmt;
}

// The hold-to-talk path takes exactly what the mic records: 16 kHz mono 16-bit.
inline bool wavMatchesMic(const WavInfo& w) {
  return w.format == 1 && w.channels == 1 && w.sampleRate == 16000 && w.bitsPerSample == 16;
}

}  // namespace nimbus::voice
