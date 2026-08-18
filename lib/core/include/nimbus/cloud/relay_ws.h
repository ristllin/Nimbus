#pragma once
// relay_ws - a minimal RFC 6455 WebSocket CLIENT, framing only (no sockets). The
// device dials the relay over WSS, so this is deliberately client-side: outbound
// frames are ALWAYS masked (RFC 6455 5.3), inbound (server) frames must NOT be.
// Portable + host-tested (test/test_relay_ws). The device wires this over a
// WiFiClientSecure in src/net/relay_client.cpp.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nimbus {
namespace cloud {
namespace ws {

enum class Opcode : uint8_t {
  Continuation = 0x0,
  Text = 0x1,
  Binary = 0x2,
  Close = 0x8,
  Ping = 0x9,
  Pong = 0xA,
};

// --- handshake ----------------------------------------------------------------

// base64(SHA1(clientKeyB64 + RFC6455 GUID)) - the value the server must echo in
// Sec-WebSocket-Accept. Exposed for testing; validateUpgradeResponse uses it.
std::string computeAccept(const std::string& clientKeyB64);

// Build the HTTP/1.1 Upgrade request head (ends with the blank line). clientKeyB64
// is base64 of 16 caller-supplied random bytes (the RNG seam lives at the caller).
std::string buildUpgradeRequest(const std::string& host, const std::string& path,
                                const std::string& clientKeyB64);

// True iff `responseHead` (status line + headers, up to and including the blank line)
// is a valid 101 Switching Protocols whose Sec-WebSocket-Accept matches clientKeyB64.
// Case-insensitive header names; tolerant of header order and whitespace.
bool validateUpgradeResponse(const std::string& responseHead, const std::string& clientKeyB64);

// --- outbound (masked) frames -------------------------------------------------

// Write just the client frame header (FIN=1, masked) for a `payloadLen` payload into
// `hdr` (must be >= 14 bytes). Returns the header length written (6, 8, or 14). The
// caller then appends the payload XORed byte-wise with `mask`. This is the seam the
// device uses to assemble a large `res` frame directly in ONE PSRAM buffer (header +
// masked payload) for a single write(), rather than growing an internal-SRAM string.
size_t writeFrameHeader(Opcode op, size_t payloadLen, const uint8_t mask[4], uint8_t* hdr);

// Encode one client frame (FIN=1, masked) and append it to `out`. `mask` is 4 bytes
// the caller supplies from a RNG (esp_random on device, fixed in tests). Payload may
// be null when len==0 (e.g. an empty ping). Convenience for small control frames.
void encodeClientFrame(Opcode op, const uint8_t* payload, size_t len, const uint8_t mask[4],
                       std::string& out);

// Convenience for a Close frame carrying a 2-byte big-endian status code.
void encodeClose(uint16_t code, const uint8_t mask[4], std::string& out);

// --- inbound parser -----------------------------------------------------------

struct Message {
  Opcode op = Opcode::Text;         // Text/Binary (reassembled) or a control frame
  std::vector<uint8_t> payload;     // application data, or control payload
  uint16_t closeCode = 0;           // set when op == Close (0 if none supplied)
};

// Incremental server->device frame parser. Feed raw bytes as they arrive; pull
// complete messages. Reassembles fragmented data messages; surfaces interleaved
// control frames immediately. Enforces server-frames-are-unmasked and the frame cap.
class Parser {
 public:
  explicit Parser(size_t maxMessageBytes) : maxMessageBytes_(maxMessageBytes) {}

  void feed(const uint8_t* data, size_t len);
  // Pull the next ready message. Returns false when more bytes are needed.
  bool next(Message& out);
  bool protocolError() const { return error_; }

 private:
  std::vector<uint8_t> buf_;         // unparsed bytes
  std::vector<uint8_t> frag_;        // reassembly of a fragmented data message
  Opcode fragOp_ = Opcode::Text;     // opcode of the in-progress data message
  bool inFragment_ = false;
  bool error_ = false;
  size_t maxMessageBytes_;
  std::vector<Message> ready_;       // completed messages awaiting next()
  void parse_();
  // parse_ helpers: decode one frame's header at `off`, then route one full frame.
  // Splitting keeps each piece well inside the complexity gate.
  enum class Scan { NeedMore, Error, Ok };
  Scan scanHeader_(size_t off, size_t& hdr, uint64_t& plen, Opcode& op, bool& fin) const;
  bool emitFrame_(Opcode op, bool fin, const uint8_t* pl, uint64_t plen);
};

}  // namespace ws
}  // namespace cloud
}  // namespace nimbus
