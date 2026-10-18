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
    case WavErr::NoData:   return "no data chunk in the clip";
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

// Read `len` bytes at byte `offset` of the clip into `dst`; return the bytes read
// (fewer at end of file). The device passes a seek+read over the card file.
using WavReadFn = size_t (*)(void* ctx, uint64_t offset, uint8_t* dst, size_t len);

// Walk the clip's chunks by OFFSET, reading only each 8-byte chunk header and the
// 16-byte fmt body, so a padding chunk of any size before the samples is skipped
// without buffering it. macOS `say` writes a 4008-byte FLLR chunk, which puts the
// samples at byte 4096 - past any fixed header window. Stops at `maxChunks` so a
// hostile file cannot keep the walker spinning.
inline WavErr parseWavStream(WavReadFn read, void* ctx, WavInfo& out, int maxChunks = 64) {
  using namespace wav_detail;
  out = WavInfo{};
  uint8_t h[24];
  if (!read || read(ctx, 0, h, 12) < 12) return WavErr::TooShort;
  if (!tag(h, "RIFF")) return WavErr::NotRiff;
  if (!tag(h + 8, "WAVE")) return WavErr::NotWave;
  bool haveFmt = false;
  uint64_t pos = 12;
  for (int i = 0; i < maxChunks && read(ctx, pos, h, 8) == 8; i++) {
    const uint32_t size = le32(h + 4);
    if (tag(h, "data")) {
      if (!haveFmt) return WavErr::NoFmt;
      if (pos + 8 > 0xFFFFFFFFull) return WavErr::NoData;
      out.dataOffset = uint32_t(pos + 8);
      out.dataBytes = size;
      return WavErr::Ok;
    }
    if (tag(h, "fmt ")) {
      if (size < 16 || read(ctx, pos + 8, h + 8, 16) < 16) return WavErr::TooShort;
      out.format = le16(h + 8);
      out.channels = le16(h + 10);
      out.sampleRate = le32(h + 12);
      out.bitsPerSample = le16(h + 22);
      if (out.format != 1) return WavErr::NotPcm;
      haveFmt = true;
    }
    pos += 8u + uint64_t(size) + (size & 1u);   // RIFF pads an odd-sized chunk with one byte
  }
  return haveFmt ? WavErr::NoData : WavErr::NoFmt;
}

namespace wav_detail {
struct MemClip { const uint8_t* buf; size_t len; };
inline size_t memRead(void* ctx, uint64_t off, uint8_t* dst, size_t n) {
  const MemClip* m = static_cast<const MemClip*>(ctx);
  if (off >= m->len) return 0;
  const size_t avail = size_t(m->len - off);
  const size_t take = n < avail ? n : avail;
  for (size_t i = 0; i < take; i++) dst[i] = m->buf[off + i];
  return take;
}
}  // namespace wav_detail

// The same walk over a clip already in memory (`len` bytes of it).
inline WavErr parseWavHeader(const uint8_t* buf, size_t len, WavInfo& out) {
  if (!buf) { out = WavInfo{}; return WavErr::TooShort; }
  wav_detail::MemClip m{buf, len};
  return parseWavStream(&wav_detail::memRead, &m, out);
}

// The hold-to-talk path takes exactly what the mic records: 16 kHz mono 16-bit.
inline bool wavMatchesMic(const WavInfo& w) {
  return w.format == 1 && w.channels == 1 && w.sampleRate == 16000 && w.bitsPerSample == 16;
}

}  // namespace nimbus::voice
