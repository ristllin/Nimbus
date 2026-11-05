#include "nimbus/orch/clip_probe.h"

#include <cstring>

namespace nimbus {
namespace orch {

namespace {

uint32_t le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// Whether the first n bytes at p are the n characters of s ("ID3", "data", "fmt ").
bool tagIs(const uint8_t* p, const char* s, size_t n) { return std::memcmp(p, s, n) == 0; }

// MPEG audio layer III tables, indexed as the header bits are: version 0 = 2.5, 2 = 2, 3 = 1
// (1 is reserved); the bitrate index 1..14 (0 is free format, 15 is invalid); the sample rate
// index 0..2. Layer III is what the speech providers send; any other layer is "no verdict".
constexpr uint16_t kRates[4][3] = {{11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}, {44100, 48000, 32000}};
constexpr uint16_t kBitrateV1[14] = {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
constexpr uint16_t kBitrateV2[14] = {8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};

// Chunks other than "data" are small; a huge one is not a WAV the probe understands.
constexpr uint32_t kWavChunkMax = 1u << 20;

}  // namespace

void ClipProbe::feed(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n && phase_ != Phase::Lost; i++) {
    if (fmt_ == ClipFormat::Mp3) stepMp3(p[i]);
    else stepWav(p[i]);
  }
}

void ClipProbe::parseMp3Header() {
  const unsigned ver = (head_[1] >> 3) & 3;      // 0: MPEG 2.5, 1: reserved, 2: MPEG 2, 3: MPEG 1
  const unsigned layer = (head_[1] >> 1) & 3;    // 1: layer III (0 is reserved, 2 and 3 are II and I)
  const unsigned br = (head_[2] >> 4) & 15;
  const unsigned sr = (head_[2] >> 2) & 3;
  const unsigned pad = (head_[2] >> 1) & 1;
  if (head_[0] != 0xFF || (head_[1] & 0xE0) != 0xE0 || ver == 1 || layer != 1 || br == 0 || br == 15 || sr == 3) {
    phase_ = Phase::Lost;
    return;
  }
  const uint32_t kbps = ver == 3 ? kBitrateV1[br - 1] : kBitrateV2[br - 1];
  const uint32_t rate = kRates[ver][sr];
  spf_ = ver == 3 ? 1152 : 576;
  const uint32_t bytes = spf_ / 8u * kbps * 1000u / rate + pad;   // 144 (MPEG 1) or 72 bytes per kbps / kHz
  if (bytes < 5) {
    phase_ = Phase::Lost;
    return;
  }
  if (rate_ == 0) rate_ = rate;
  fill_ = 0;
  left_ = bytes - 4;
  phase_ = Phase::Body;
}

// "ID3", version, revision, flags, then a 4-byte synchsafe size (and a 10-byte footer if flagged).
void ClipProbe::endId3Header() {
  for (int i = 6; i < 10; i++)
    if (head_[i] & 0x80) {
      phase_ = Phase::Lost;
      return;
    }
  left_ = ((uint32_t)head_[6] << 21) | ((uint32_t)head_[7] << 14) | ((uint32_t)head_[8] << 7) | head_[9];
  if (head_[5] & 0x10) left_ += 10;
  id3_ = false;
  fill_ = 0;
  phase_ = left_ ? Phase::Skip : Phase::Head;
}

void ClipProbe::mp3HeadByte(uint8_t b) {
  head_[fill_++] = b;
  if (fill_ == 3 && tagIs(head_, "ID3", 3) && frames_ == 0) {
    id3_ = true;
  } else if (fill_ == 3 && tagIs(head_, "TAG", 3) && frames_ > 0) {
    phase_ = Phase::Trailer;   // an ID3v1 tag after the last frame: the audio is complete
  } else if (id3_ && fill_ == 10) {
    endId3Header();
  } else if (!id3_ && fill_ == 4) {
    parseMp3Header();
  }
}

void ClipProbe::stepMp3(uint8_t b) {
  if (phase_ == Phase::Head) {
    mp3HeadByte(b);
  } else if (phase_ == Phase::Skip) {
    if (--left_ == 0) phase_ = Phase::Head;
  } else if (phase_ == Phase::Body && --left_ == 0) {
    frames_++;
    samples_ += spf_;
    phase_ = Phase::Head;
  }   // Trailer, Lost: nothing more to read
}

void ClipProbe::endChunkBody() {
  if (isFmt_ && fmtFill_ >= 16) {
    byteRate_ = le32(fmtBuf_ + 8);
    blockAlign_ = le16(fmtBuf_ + 12);
  }
  isFmt_ = false;
  fill_ = 0;
  phase_ = Phase::ChunkHead;
}

// A complete 8-byte chunk header: the sample data starts, or a chunk to skip (or, for
// "fmt ", to read).
void ClipProbe::wavChunkHeader() {
  const uint32_t size = le32(head_ + 4);
  if (tagIs(head_, "data", 4)) {
    sawData_ = true;
    // A streamed WAV may declare 0 or the maximum: the size is then unknown.
    dataDeclared_ = (size == 0 || size >= 0x7FFFFFF0u) ? 0 : size;
    fill_ = 0;
    phase_ = Phase::Data;
    return;
  }
  if (size > kWavChunkMax) {
    phase_ = Phase::Lost;
    return;
  }
  isFmt_ = tagIs(head_, "fmt ", 4);
  fmtFill_ = 0;
  left_ = size + (size & 1u);   // chunks are padded to an even size
  fill_ = 0;
  if (left_ == 0) endChunkBody();
  else phase_ = Phase::ChunkBody;
}

void ClipProbe::wavDataByte() {
  dataBytes_++;
  if (!dataDeclared_ || dataBytes_ < dataDeclared_) return;
  // The declared data is whole. An odd size is followed by a pad byte, then any trailing chunks.
  isFmt_ = false;
  fill_ = 0;
  left_ = dataDeclared_ & 1u;
  phase_ = left_ ? Phase::ChunkBody : Phase::ChunkHead;
}

void ClipProbe::stepWav(uint8_t b) {
  if (phase_ == Phase::RiffHead) {
    head_[fill_++] = b;
    if (fill_ < 12) return;
    const bool ok = tagIs(head_, "RIFF", 4) && tagIs(head_ + 8, "WAVE", 4);
    fill_ = 0;
    phase_ = ok ? Phase::ChunkHead : Phase::Lost;
  } else if (phase_ == Phase::ChunkHead) {
    head_[fill_++] = b;
    if (fill_ == 8) wavChunkHeader();
  } else if (phase_ == Phase::ChunkBody) {
    if (isFmt_ && fmtFill_ < sizeof(fmtBuf_)) fmtBuf_[fmtFill_++] = b;
    if (--left_ == 0) endChunkBody();
  } else if (phase_ == Phase::Data) {
    wavDataByte();
  }
}

bool ClipProbe::endsOnBoundary() const {
  if (phase_ == Phase::Lost) return false;
  if (fmt_ == ClipFormat::Mp3)
    return frames_ > 0 && ((phase_ == Phase::Head && fill_ == 0) || phase_ == Phase::Trailer);
  if (!sawData_ || dataBytes_ == 0) return false;
  if (blockAlign_ && dataBytes_ % blockAlign_ != 0) return false;
  return dataDeclared_ ? dataBytes_ >= dataDeclared_ : phase_ == Phase::Data;
}

uint32_t ClipProbe::durationMs() const {
  if (fmt_ == ClipFormat::Mp3) return rate_ ? (uint32_t)(samples_ * 1000u / rate_) : 0;
  return byteRate_ ? (uint32_t)(dataBytes_ * 1000u / byteRate_) : 0;
}

}  // namespace orch
}  // namespace nimbus
