#include "nimbus/nsn_proto.h"

namespace nimbus::nsn {

uint8_t crc8(const uint8_t* data, size_t len) {
  uint8_t crc = 0;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b)
      crc = (crc & 0x01) ? uint8_t((crc >> 1) ^ 0x8C) : uint8_t(crc >> 1);
  }
  return crc;
}

const char* harnessName(uint8_t h) {
  switch (h) {
    case kHarnessClaude: return "claude";
    case kHarnessCodex:  return "codex";
    case kHarnessVibe:   return "vibe";
  }
  return "";
}

static size_t titleLen(const char* t) {
  size_t n = 0;
  while (n < size_t(kMaxTitle) && t[n]) ++n;
  return n;
}

size_t encode(const Frame& f, uint8_t* out, size_t cap) {
  const uint8_t n = f.count > kMaxSegs ? uint8_t(kMaxSegs) : f.count;
  // v2 extension is emitted only when a segment carries harness/title - so a plain
  // v1 frame encodes byte-identically (the byte-locked vectors stay valid).
  bool v2 = false;
  size_t tlvLen = 0;
  for (uint8_t i = 0; i < n; ++i) {
    const size_t tl = titleLen(f.segs[i].title);
    if (f.segs[i].harness != 0 || tl != 0) v2 = true;
    tlvLen += 2 + tl;
  }
  if (!v2) tlvLen = 0;
  const size_t payloadLen = 4 + size_t(n) * 4 + tlvLen;
  const size_t total = 2 + payloadLen + 1;
  if (cap < total || payloadLen > 255) return 0;

  uint8_t* p = out + 2;  // payload starts after SOF+LEN
  p[0] = kMagic;
  p[1] = f.seq;
  p[2] = n;
  p[3] = f.brightness;
  for (uint8_t i = 0; i < n; ++i) {
    uint8_t* s = p + 4 + size_t(i) * 4;
    s[0] = f.segs[i].state;
    s[1] = f.segs[i].hue;
    s[2] = f.segs[i].anim;
    s[3] = f.segs[i].span;
  }
  if (v2) {  // trailing per-segment [harness][titleLen][title]
    uint8_t* t = p + 4 + size_t(n) * 4;
    for (uint8_t i = 0; i < n; ++i) {
      const size_t tl = titleLen(f.segs[i].title);
      *t++ = f.segs[i].harness;
      *t++ = uint8_t(tl);
      for (size_t k = 0; k < tl; ++k) *t++ = uint8_t(f.segs[i].title[k]);
    }
  }
  out[0] = kSof;
  out[1] = uint8_t(payloadLen);
  out[2 + payloadLen] = crc8(p, payloadLen);
  return total;
}

bool decodePacket(const uint8_t* data, size_t len, Frame& out) {
  if (len < 6) return false;                    // SOF+LEN+minimal payload+CRC
  if (data[0] != kSof) return false;
  const size_t payloadLen = data[1];
  if (len < 2 + payloadLen + 1) return false;
  const uint8_t* payload = data + 2;
  if (payloadLen < 4) return false;
  if (crc8(payload, payloadLen) != data[2 + payloadLen]) return false;
  if (payload[0] != kMagic) return false;

  out.seq        = payload[1];
  out.brightness = payload[3];
  const uint8_t n = payload[2];
  uint8_t written = 0;
  for (uint8_t i = 0; i < n && written < kMaxSegs; ++i) {
    const size_t off = 4 + size_t(i) * 4;
    if (off + 4 > payloadLen) break;            // truncated record: stop (as reference)
    const uint8_t state = payload[off + 0];
    const uint8_t anim  = payload[off + 2];
    if (state > kMaxState || anim > kMaxAnim) return false;
    out.segs[written] = Segment{state, payload[off + 1], anim, payload[off + 3]};
    ++written;
  }
  out.count = written;

  // v2 extension: optional per-segment [harness][titleLen][title] after the base
  // block. Absent (v1 frame) or truncated -> leave harness=0/title="" and stop.
  size_t toff = 4 + size_t(n) * 4;
  for (uint8_t i = 0; i < written; ++i) {
    if (toff + 2 > payloadLen) break;
    const uint8_t harness = payload[toff];
    const uint8_t tl = payload[toff + 1];
    toff += 2;
    if (toff + tl > payloadLen) break;            // truncated title
    out.segs[i].harness = harness;
    const size_t c = tl < uint8_t(kMaxTitle) ? tl : size_t(kMaxTitle);
    for (size_t k = 0; k < c; ++k) out.segs[i].title[k] = char(payload[toff + k]);
    out.segs[i].title[c] = 0;
    toff += tl;
  }
  return true;
}

bool Decoder::feed(uint8_t byte, Frame& out) {
  switch (st_) {
    case St::Sof:
      if (byte == kSof) { st_ = St::Len; }
      return false;
    case St::Len:
      if (byte < 4 || size_t(byte) + 3 > kMaxPacket) { st_ = St::Sof; return false; }
      len_ = byte;
      have_ = 0;
      buf_[0] = kSof;
      buf_[1] = len_;
      st_ = St::Body;
      return false;
    case St::Body:
      buf_[2 + have_++] = byte;
      if (have_ < size_t(len_) + 1) return false;  // payload + trailing CRC
      st_ = St::Sof;
      return decodePacket(buf_, 2 + size_t(len_) + 1, out);
  }
  return false;
}

void Decoder::reset() {
  st_ = St::Sof;
  have_ = 0;
}

}  // namespace nimbus::nsn
