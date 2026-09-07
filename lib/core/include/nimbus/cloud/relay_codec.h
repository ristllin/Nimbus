#pragma once
// relay_codec - the cumulo-nimbus tunnel wire contract, mirrored from the single
// source of truth: cumulo-nimbus/packages/shared/src/protocol.ts. JSON text frames
// over one persistent WSS the device dials OUT to the relay. Portable (no Arduino):
// host-tested via test/test_relay_codec with vectors byte-locked to protocol.ts
// (tools/gen_relay_vectors.mjs). Changing a field here is a wire change - bump
// kProtocolVersion AND protocol.ts together.
//
// The caller owns the JsonDocument (on-device it passes the PSRAM allocator, so the
// node pool never touches the scarce internal SRAM). Parsed string fields point INTO
// that document and are valid only while it stays alive.
#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nimbus {
namespace cloud {

// Protocol 2 (2026-09) adds the chunked tunnel-upload frames (ubegin/uchunk/uend/
// uabort relay->device, uack device->relay). A large body streams as ordered chunks
// the device pipes into its local server, never a full-body buffer. The device
// advertises this version in its Hello; a cloud that sees v1 refuses a large upload
// with an honest error instead of hanging. Keep in lockstep with protocol.ts.
constexpr int kProtocolVersion = 2;
constexpr uint32_t kDefaultHeartbeatMs = 30000;
// Max control-frame size the relay accepts from a device (protocol.ts MAX_FRAME_BYTES).
constexpr size_t kMaxFrameBytes = 512u * 1024u;

// WebSocket close codes the relay uses (protocol.ts CloseCode; 4000-4999 app range).
enum class CloseCode : uint16_t {
  BadToken = 4001,
  Unpaired = 4002,
  EntitlementRevoked = 4003,
  Superseded = 4004,       // a newer connection for the same deviceId replaced this one
  ProtocolError = 4005,
  PayloadTooLarge = 4006,
};

enum class FrameType {
  Unknown,
  Welcome,
  Req,
  Pong,
  Bye,
  UploadBegin,   // ubegin: start a streamed-body request
  UploadChunk,   // uchunk: one ordered body slice
  UploadEnd,     // uend: body complete, finalize + answer with a `res`
  UploadAbort,   // uabort: abandon the in-flight upload
};

// A relay -> device "req": forward this HTTP request to the local web server.
// Pointers reference the parsed JsonDocument (valid while it is alive). `headers`
// is the raw object; iterate it to rebuild the request line + headers.
struct ReqFrame {
  const char* id = "";
  const char* method = "";
  const char* path = "";
  JsonObjectConst headers;   // may be null if absent
  const char* bodyB64 = nullptr;  // nullptr when the request has no body
};

// A relay -> device chunked-upload frame. Which fields are meaningful depends on the
// FrameType: UploadBegin uses id/method/path/headers/totalLen; UploadChunk uses
// id/seq/off/bodyB64; UploadEnd uses id; UploadAbort uses id/reason. Pointers reference
// the parsed JsonDocument (valid only while it is alive).
struct UploadFrame {
  const char* id = "";
  const char* method = "";        // UploadBegin
  const char* path = "";          // UploadBegin
  JsonObjectConst headers;         // UploadBegin (may be null)
  uint32_t totalLen = 0;          // UploadBegin: declared total body length
  uint32_t seq = 0;               // UploadChunk: 0-based, strictly increasing
  uint32_t off = 0;               // UploadChunk: byte offset of this slice
  const char* bodyB64 = nullptr;  // UploadChunk: base64 body slice
  const char* reason = "";        // UploadAbort
};

struct RelayFrame {
  FrameType type = FrameType::Unknown;
  uint32_t heartbeatMs = 0;       // Welcome
  const char* deviceId = "";      // Welcome: the device id the relay echoes back
                                   // (identity-bound hello-ack; "" on a legacy relay)
  ReqFrame req;                    // Req
  UploadFrame upload;              // UploadBegin/Chunk/End/Abort
  const char* byeReason = "";     // Bye
  int64_t pongTs = 0;              // Pong
};

// Classify + extract a parsed relay->device frame. `doc` must already hold the
// deserialized JSON. Returns false (type Unknown) on anything unrecognized so the
// caller never trusts a malformed frame.
bool parseRelayFrame(const JsonDocument& doc, RelayFrame& out);

// --- device -> relay frame builders (fill `doc`; caller serializes) -----------

void buildHello(JsonDocument& doc, const char* deviceId, const char* connectToken,
                const char* fw);
void buildPing(JsonDocument& doc, int64_t ts);
// device -> relay: acknowledge that `off` total body bytes have been durably accepted
// (written into the local server). Drives the cloud's real progress + backpressure.
void buildUploadAck(JsonDocument& doc, const char* id, uint32_t off);

// Start a "res" frame in `doc` and return its (empty) headers object for the caller
// to populate. Call setResBody() afterwards if there is a body.
JsonObject startRes(JsonDocument& doc, const char* id, int status);
void setResBody(JsonDocument& doc, const char* bodyB64);

// --- base64 (portable; the wire carries bodies base64-encoded) ----------------

// Exact encoded length (with '=' padding) for `rawLen` input bytes.
size_t b64EncodedLen(size_t rawLen);
// Encode into a caller buffer (writes exactly b64EncodedLen(len) bytes, no NUL). Lets
// the device encode a large body straight into a PSRAM buffer, no internal-heap string.
void b64EncodeRaw(const uint8_t* data, size_t len, char* out);
// Standard base64 (RFC 4648, '+'/'/', '=' padding). Appends to `out`.
void b64Encode(const uint8_t* data, size_t len, std::string& out);
// Returns false on invalid input. Tolerates missing padding and internal whitespace.
bool b64Decode(const char* b64, size_t len, std::vector<uint8_t>& out);

}  // namespace cloud
}  // namespace nimbus
