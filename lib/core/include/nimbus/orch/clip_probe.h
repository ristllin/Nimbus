#pragma once
// clip_probe - read a speech clip's own framing as it streams in, so a clip cut short can be
// told from a whole one when the server gives no length.
//
// OpenAI and the router answer a speech request with the raw audio body, and the request
// is HTTP/1.0, so the body ends when the server closes the connection. Without a
// Content-Length a clean close and a reset or proxy cut look the same: the device kept the
// short clip as a whole one and sent it on as a complete voice reply (CODEREVIEW
// voice-model-seams). The audio itself says where it ends. An MP3 is a run of frames, each
// declaring its own length: a cut stream stops inside one. A WAV declares the size of its
// data, and a sample is a fixed number of bytes. The probe walks that framing byte by byte,
// in any chunking, and reports whether the stream ended on a boundary and how much audio the
// complete frames carry.
//
// It never guesses: a stream whose framing it cannot follow (not MP3 layer III or WAV, an
// unknown tag, a lost sync) is "not readable" and gets no verdict, so an odd but whole clip is
// never refused. Mistral's base64 body has its own end marker (the closing quote) and is not
// probed. Portable and host-tested (test_clip_probe).

#include <cstddef>
#include <cstdint>

namespace nimbus {
namespace orch {

enum class ClipFormat : uint8_t { Mp3, Wav };

class ClipProbe {
 public:
  explicit ClipProbe(ClipFormat f) : fmt_(f), phase_(f == ClipFormat::Mp3 ? Phase::Head : Phase::RiffHead) {}

  // Feed the next bytes of the body, in order, in any chunking.
  void feed(const uint8_t* p, size_t n);

  // The framing parsed cleanly from the first byte (no lost sync, no unknown structure).
  // False means "no verdict", never "cut".
  bool readable() const { return phase_ != Phase::Lost; }
  // The stream so far ends exactly on a frame boundary (MP3), or on a whole sample at or
  // after the declared data size (WAV), and carries audio.
  bool endsOnBoundary() const;
  // How many complete frames (MP3) were seen, or data bytes (WAV).
  uint64_t units() const { return fmt_ == ClipFormat::Mp3 ? frames_ : dataBytes_; }
  // The audio the complete frames (MP3) or the data bytes (WAV) carry, in ms; 0 when unknown.
  uint32_t durationMs() const;

 private:
  enum class Phase : uint8_t {
    Head,       // MP3: collecting a 4-byte frame header, an ID3v2 header or an ID3v1 tag marker
    Skip,       // MP3: inside the ID3v2 tag body
    Body,       // MP3: inside a frame's payload
    Trailer,    // MP3: an ID3v1 tag (the audio is complete)
    RiffHead,   // WAV: the 12-byte RIFF header
    ChunkHead,  // WAV: an 8-byte chunk header
    ChunkBody,  // WAV: skipping (or reading, for "fmt ") a non-data chunk
    Data,       // WAV: the sample data
    Lost,
  };

  void stepMp3(uint8_t b);
  void mp3HeadByte(uint8_t b);
  void endId3Header();
  void parseMp3Header();
  void stepWav(uint8_t b);
  void wavChunkHeader();
  void wavDataByte();
  void endChunkBody();

  ClipFormat fmt_;
  Phase phase_;
  // MP3
  uint8_t  head_[12] = {};
  uint8_t  fill_ = 0;
  bool     id3_ = false;
  uint32_t left_ = 0;          // bytes left in the Skip / Body / ChunkBody phase
  uint64_t frames_ = 0;
  uint64_t samples_ = 0;
  uint32_t rate_ = 0;          // the first frame's sample rate
  uint16_t spf_ = 0;           // samples in the frame being read
  // WAV
  uint32_t dataDeclared_ = 0;  // 0 = unknown (streamed size)
  uint64_t dataBytes_ = 0;
  uint32_t byteRate_ = 0;
  uint16_t blockAlign_ = 0;
  uint8_t  fmtFill_ = 0;
  uint8_t  fmtBuf_[16] = {};
  bool     isFmt_ = false;
  bool     sawData_ = false;
};

}  // namespace orch
}  // namespace nimbus
