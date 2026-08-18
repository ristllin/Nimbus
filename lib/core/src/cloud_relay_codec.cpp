#include "nimbus/cloud/relay_codec.h"

namespace nimbus {
namespace cloud {

bool parseRelayFrame(const JsonDocument& doc, RelayFrame& out) {
  out = RelayFrame{};
  if (!doc.is<JsonObjectConst>()) return false;
  const char* t = doc["t"].is<const char*>() ? doc["t"].as<const char*>() : nullptr;
  if (!t) return false;

  if (std::string(t) == "welcome") {
    out.type = FrameType::Welcome;
    out.heartbeatMs = doc["heartbeatMs"].is<uint32_t>() ? doc["heartbeatMs"].as<uint32_t>()
                                                        : kDefaultHeartbeatMs;
    if (out.heartbeatMs == 0) out.heartbeatMs = kDefaultHeartbeatMs;
    return true;
  }
  if (std::string(t) == "req") {
    // id/method/path are required; a malformed req is rejected so the caller never
    // replays garbage into the local server.
    if (!doc["id"].is<const char*>() || !doc["method"].is<const char*>() ||
        !doc["path"].is<const char*>()) {
      return false;
    }
    out.type = FrameType::Req;
    out.req.id = doc["id"].as<const char*>();
    out.req.method = doc["method"].as<const char*>();
    out.req.path = doc["path"].as<const char*>();
    if (doc["headers"].is<JsonObjectConst>()) out.req.headers = doc["headers"].as<JsonObjectConst>();
    out.req.bodyB64 = doc["bodyB64"].is<const char*>() ? doc["bodyB64"].as<const char*>() : nullptr;
    return true;
  }
  if (std::string(t) == "pong") {
    out.type = FrameType::Pong;
    out.pongTs = doc["ts"].is<int64_t>() ? doc["ts"].as<int64_t>() : 0;
    return true;
  }
  if (std::string(t) == "bye") {
    out.type = FrameType::Bye;
    out.byeReason = doc["reason"].is<const char*>() ? doc["reason"].as<const char*>() : "";
    return true;
  }
  return false;
}

void buildHello(JsonDocument& doc, const char* deviceId, const char* connectToken,
                const char* fw) {
  doc.clear();
  doc["t"] = "hello";
  doc["v"] = kProtocolVersion;
  doc["deviceId"] = deviceId;
  doc["connectToken"] = connectToken;
  doc["fw"] = fw ? fw : "";
}

void buildPing(JsonDocument& doc, int64_t ts) {
  doc.clear();
  doc["t"] = "ping";
  doc["ts"] = ts;
}

JsonObject startRes(JsonDocument& doc, const char* id, int status) {
  doc.clear();
  doc["t"] = "res";
  doc["id"] = id;
  doc["status"] = status;
  return doc["headers"].to<JsonObject>();
}

void setResBody(JsonDocument& doc, const char* bodyB64) {
  if (bodyB64 && bodyB64[0]) doc["bodyB64"] = bodyB64;
}

// --- base64 -----------------------------------------------------------------

static const char kB64Enc[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t b64EncodedLen(size_t rawLen) { return ((rawLen + 2) / 3) * 4; }

void b64EncodeRaw(const uint8_t* data, size_t len, char* out) {
  size_t i = 0, o = 0;
  while (i + 3 <= len) {
    uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
    out[o++] = kB64Enc[(n >> 18) & 63];
    out[o++] = kB64Enc[(n >> 12) & 63];
    out[o++] = kB64Enc[(n >> 6) & 63];
    out[o++] = kB64Enc[n & 63];
    i += 3;
  }
  const size_t rem = len - i;
  if (rem == 1) {
    uint32_t n = uint32_t(data[i]) << 16;
    out[o++] = kB64Enc[(n >> 18) & 63];
    out[o++] = kB64Enc[(n >> 12) & 63];
    out[o++] = '=';
    out[o++] = '=';
  } else if (rem == 2) {
    uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
    out[o++] = kB64Enc[(n >> 18) & 63];
    out[o++] = kB64Enc[(n >> 12) & 63];
    out[o++] = kB64Enc[(n >> 6) & 63];
    out[o++] = '=';
  }
}

void b64Encode(const uint8_t* data, size_t len, std::string& out) {
  size_t start = out.size();
  out.resize(start + b64EncodedLen(len));
  b64EncodeRaw(data, len, &out[start]);
}

static inline int b64Val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;  // '=' and whitespace handled by the caller
}

bool b64Decode(const char* b64, size_t len, std::vector<uint8_t>& out) {
  out.clear();
  out.reserve((len / 4) * 3 + 3);
  int quad[4];
  int q = 0;
  for (size_t i = 0; i < len; i++) {
    char c = b64[i];
    if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
    int v = b64Val(c);
    if (v < 0) return false;
    quad[q++] = v;
    if (q == 4) {
      out.push_back((uint8_t)((quad[0] << 2) | (quad[1] >> 4)));
      out.push_back((uint8_t)((quad[1] << 4) | (quad[2] >> 2)));
      out.push_back((uint8_t)((quad[2] << 6) | quad[3]));
      q = 0;
    }
  }
  if (q == 1) return false;  // a single leftover sextet is impossible
  if (q == 2) {
    out.push_back((uint8_t)((quad[0] << 2) | (quad[1] >> 4)));
  } else if (q == 3) {
    out.push_back((uint8_t)((quad[0] << 2) | (quad[1] >> 4)));
    out.push_back((uint8_t)((quad[1] << 4) | (quad[2] >> 2)));
  }
  return true;
}

}  // namespace cloud
}  // namespace nimbus
