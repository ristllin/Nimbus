#include "nimbus/cloud/relay_ws.h"

#include <cctype>
#include <cstring>

#include "nimbus/cloud/relay_codec.h"  // b64Encode

namespace nimbus {
namespace cloud {
namespace ws {

namespace {

// --- SHA1 (RFC 3174), just enough for Sec-WebSocket-Accept --------------------
struct Sha1 {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  uint64_t total = 0;
  uint8_t block[64];
  size_t blen = 0;

  static uint32_t rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

  void process(const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
      w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
             (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6; }
      uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }

  void update(const uint8_t* p, size_t n) {
    total += n;
    while (n) {
      size_t take = 64 - blen;
      if (take > n) take = n;
      std::memcpy(block + blen, p, take);
      blen += take; p += take; n -= take;
      if (blen == 64) { process(block); blen = 0; }
    }
  }

  void finish(uint8_t out[20]) {
    uint64_t bits = total * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (blen != 56) update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - i * 8));
    update(len, 8);
    for (int i = 0; i < 5; i++) {
      out[i * 4] = (uint8_t)(h[i] >> 24);
      out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
      out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
      out[i * 4 + 3] = (uint8_t)(h[i]);
    }
  }
};

std::string lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

}  // namespace

std::string computeAccept(const std::string& clientKeyB64) {
  static const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  Sha1 s;
  s.update(reinterpret_cast<const uint8_t*>(clientKeyB64.data()), clientKeyB64.size());
  s.update(reinterpret_cast<const uint8_t*>(kGuid), sizeof(kGuid) - 1);
  uint8_t digest[20];
  s.finish(digest);
  std::string out;
  b64Encode(digest, 20, out);
  return out;
}

std::string buildUpgradeRequest(const std::string& host, const std::string& path,
                                const std::string& clientKeyB64) {
  std::string r;
  r.reserve(256);
  r += "GET ";
  r += path;
  r += " HTTP/1.1\r\n";
  r += "Host: ";
  r += host;
  r += "\r\n";
  r += "Upgrade: websocket\r\n";
  r += "Connection: Upgrade\r\n";
  r += "Sec-WebSocket-Key: ";
  r += clientKeyB64;
  r += "\r\n";
  r += "Sec-WebSocket-Version: 13\r\n";
  r += "\r\n";
  return r;
}

bool validateUpgradeResponse(const std::string& responseHead, const std::string& clientKeyB64) {
  // Status line must be 101.
  size_t eol = responseHead.find("\r\n");
  if (eol == std::string::npos) return false;
  std::string status = responseHead.substr(0, eol);
  if (status.find(" 101") == std::string::npos) return false;

  const std::string want = lower("Sec-WebSocket-Accept:");
  const std::string expect = computeAccept(clientKeyB64);
  size_t pos = eol + 2;
  bool sawAccept = false;
  while (pos < responseHead.size()) {
    size_t next = responseHead.find("\r\n", pos);
    if (next == std::string::npos) next = responseHead.size();
    std::string line = responseHead.substr(pos, next - pos);
    pos = next + 2;
    if (line.empty()) break;
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    if (lower(line.substr(0, colon + 1)) == want) {
      std::string val = line.substr(colon + 1);
      size_t a = val.find_first_not_of(" \t");
      size_t b = val.find_last_not_of(" \t\r");
      if (a == std::string::npos) return false;
      val = val.substr(a, b - a + 1);
      if (val != expect) return false;
      sawAccept = true;
    }
  }
  return sawAccept;
}

size_t writeFrameHeader(Opcode op, size_t payloadLen, const uint8_t mask[4], uint8_t* hdr) {
  size_t n = 0;
  hdr[n++] = 0x80 | (uint8_t)op;  // FIN=1
  if (payloadLen < 126) {
    hdr[n++] = 0x80 | (uint8_t)payloadLen;  // MASK=1
  } else if (payloadLen <= 0xFFFF) {
    hdr[n++] = 0x80 | 126;
    hdr[n++] = (uint8_t)((payloadLen >> 8) & 0xFF);
    hdr[n++] = (uint8_t)(payloadLen & 0xFF);
  } else {
    hdr[n++] = 0x80 | 127;
    for (int i = 7; i >= 0; i--) hdr[n++] = (uint8_t)((uint64_t(payloadLen) >> (i * 8)) & 0xFF);
  }
  hdr[n++] = mask[0];
  hdr[n++] = mask[1];
  hdr[n++] = mask[2];
  hdr[n++] = mask[3];
  return n;
}

void encodeClientFrame(Opcode op, const uint8_t* payload, size_t len, const uint8_t mask[4],
                       std::string& out) {
  uint8_t hdr[14];
  size_t hn = writeFrameHeader(op, len, mask, hdr);
  out.append(reinterpret_cast<const char*>(hdr), hn);
  for (size_t i = 0; i < len; i++) out.push_back((char)(payload[i] ^ mask[i & 3]));
}

void encodeClose(uint16_t code, const uint8_t mask[4], std::string& out) {
  uint8_t body[2] = {(uint8_t)(code >> 8), (uint8_t)(code & 0xFF)};
  encodeClientFrame(Opcode::Close, body, 2, mask, out);
}

void Parser::feed(const uint8_t* data, size_t len) {
  if (error_) return;
  buf_.insert(buf_.end(), data, data + len);
  parse_();
}

// Decode the frame header at `off`: FIN, opcode, and payload length (incl. the 126/127
// extended-length forms). Enforces "reserved bits zero", "server frames unmasked", and
// the message cap. Returns NeedMore when more bytes are required, Error on a violation.
Parser::Scan Parser::scanHeader_(size_t off, size_t& hdr, uint64_t& plen, Opcode& op,
                                 bool& fin) const {
  if (buf_.size() - off < 2) return Scan::NeedMore;
  uint8_t b0 = buf_[off];
  uint8_t b1 = buf_[off + 1];
  fin = b0 & 0x80;
  if (b0 & 0x70) return Scan::Error;  // reserved bits must be 0
  op = (Opcode)(b0 & 0x0F);
  if (b1 & 0x80) return Scan::Error;  // server frames MUST be unmasked
  plen = b1 & 0x7F;
  hdr = 2;
  if (plen == 126) {
    if (buf_.size() - off < 4) return Scan::NeedMore;
    plen = (uint64_t(buf_[off + 2]) << 8) | buf_[off + 3];
    hdr = 4;
  } else if (plen == 127) {
    if (buf_.size() - off < 10) return Scan::NeedMore;
    plen = 0;
    for (int i = 0; i < 8; i++) plen = (plen << 8) | buf_[off + 2 + i];
    hdr = 10;
  }
  if (plen > maxMessageBytes_) return Scan::Error;
  return Scan::Ok;
}

// Route one fully-buffered frame: queue control + complete data messages, or accumulate
// a fragmented data message. Returns false on a protocol violation.
bool Parser::emitFrame_(Opcode op, bool fin, const uint8_t* pl, uint64_t plen) {
  const bool control = (uint8_t)op & 0x08;
  if (control) {
    if (!fin || plen > 125) return false;  // control frames: FIN, <=125
    Message m;
    m.op = op;
    m.payload.assign(pl, pl + plen);
    if (op == Opcode::Close && plen >= 2) m.closeCode = (uint16_t(pl[0]) << 8) | pl[1];
    ready_.push_back(std::move(m));
    return true;
  }
  if (op == Opcode::Text || op == Opcode::Binary) {
    if (inFragment_) return false;  // new data message before finishing one
    if (fin) {
      Message m;
      m.op = op;
      m.payload.assign(pl, pl + plen);
      ready_.push_back(std::move(m));
    } else {
      inFragment_ = true;
      fragOp_ = op;
      frag_.assign(pl, pl + plen);
    }
    return true;
  }
  if (op == Opcode::Continuation) {
    if (!inFragment_) return false;
    if (frag_.size() + plen > maxMessageBytes_) return false;
    frag_.insert(frag_.end(), pl, pl + plen);
    if (fin) {
      Message m;
      m.op = fragOp_;
      m.payload = std::move(frag_);
      frag_.clear();
      inFragment_ = false;
      ready_.push_back(std::move(m));
    }
    return true;
  }
  return false;  // unknown opcode
}

void Parser::parse_() {
  size_t off = 0;
  while (!error_) {
    size_t hdr = 0;
    uint64_t plen = 0;
    Opcode op = Opcode::Text;
    bool fin = false;
    Scan s = scanHeader_(off, hdr, plen, op, fin);
    if (s == Scan::NeedMore) break;
    if (s == Scan::Error) { error_ = true; break; }
    if (buf_.size() - off < hdr + plen) break;  // wait for the full payload
    if (!emitFrame_(op, fin, buf_.data() + off + hdr, plen)) { error_ = true; break; }
    off += hdr + plen;
  }
  if (off) buf_.erase(buf_.begin(), buf_.begin() + off);
}

bool Parser::next(Message& out) {
  if (ready_.empty()) return false;
  out = std::move(ready_.front());
  ready_.erase(ready_.begin());
  return true;
}

}  // namespace ws
}  // namespace cloud
}  // namespace nimbus
