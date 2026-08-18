// test_relay_ws - RFC 6455 client framing (handshake accept, masked outbound frames,
// incremental inbound parser with fragmentation + interleaved control frames + close
// codes + the frame cap). Deterministic: a fixed mask stands in for the RNG seam.
#include <unity.h>

#include <cstring>
#include <string>
#include <vector>

#include "nimbus/cloud/relay_ws.h"

using namespace nimbus::cloud::ws;

void setUp() {}
void tearDown() {}

// The RFC 6455 canonical example: key "dGhlIHNhbXBsZSBub25jZQ==" -> accept
// "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=".
static void test_accept_known_answer() {
  TEST_ASSERT_EQUAL_STRING("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
                           computeAccept("dGhlIHNhbXBsZSBub25jZQ==").c_str());
}

static void test_upgrade_request_and_validate() {
  std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
  std::string req = buildUpgradeRequest("app.cumulo-nimbus.ai", "/device", key);
  TEST_ASSERT_TRUE(req.find("GET /device HTTP/1.1\r\n") == 0);
  TEST_ASSERT_TRUE(req.find("Host: app.cumulo-nimbus.ai\r\n") != std::string::npos);
  TEST_ASSERT_TRUE(req.find("Upgrade: websocket\r\n") != std::string::npos);
  TEST_ASSERT_TRUE(req.find("Sec-WebSocket-Version: 13\r\n") != std::string::npos);
  TEST_ASSERT_TRUE(req.find("Sec-WebSocket-Key: " + key + "\r\n") != std::string::npos);

  std::string good =
      "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
  TEST_ASSERT_TRUE(validateUpgradeResponse(good, key));
  // Wrong accept, wrong status, missing accept -> all reject.
  TEST_ASSERT_FALSE(validateUpgradeResponse(
      "HTTP/1.1 101\r\nSec-WebSocket-Accept: wrong\r\n\r\n", key));
  TEST_ASSERT_FALSE(validateUpgradeResponse(
      "HTTP/1.1 200 OK\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n", key));
  TEST_ASSERT_FALSE(validateUpgradeResponse("HTTP/1.1 101\r\nUpgrade: websocket\r\n\r\n", key));
}

// A client text frame is masked, FIN set, and decodes back to the payload.
static void test_client_frame_masked() {
  const uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
  std::string out;
  const char* p = "Hello";
  encodeClientFrame(Opcode::Text, reinterpret_cast<const uint8_t*>(p), 5, mask, out);
  TEST_ASSERT_EQUAL_UINT(2 + 4 + 5, out.size());
  TEST_ASSERT_EQUAL_HEX8(0x81, (uint8_t)out[0]);          // FIN + text
  TEST_ASSERT_EQUAL_HEX8(0x80 | 5, (uint8_t)out[1]);      // MASK + len 5
  for (int i = 0; i < 5; i++)
    TEST_ASSERT_EQUAL_HEX8((uint8_t)p[i] ^ mask[i & 3], (uint8_t)out[6 + i]);
}

// Extended 16-bit length header is used for payloads >= 126.
static void test_client_frame_extended_len() {
  const uint8_t mask[4] = {1, 2, 3, 4};
  std::vector<uint8_t> big(200, 'x');
  std::string out;
  encodeClientFrame(Opcode::Text, big.data(), big.size(), mask, out);
  TEST_ASSERT_EQUAL_HEX8(0x80 | 126, (uint8_t)out[1]);
  TEST_ASSERT_EQUAL_HEX8(0x00, (uint8_t)out[2]);
  TEST_ASSERT_EQUAL_HEX8(0xC8, (uint8_t)out[3]);  // 200
}

// Build an unmasked server frame (as the relay would send).
static std::string serverFrame(Opcode op, const std::string& payload, bool fin = true) {
  std::string f;
  f.push_back((char)((fin ? 0x80 : 0x00) | (uint8_t)op));
  size_t n = payload.size();
  if (n < 126) {
    f.push_back((char)n);
  } else {
    f.push_back((char)126);
    f.push_back((char)((n >> 8) & 0xFF));
    f.push_back((char)(n & 0xFF));
  }
  f += payload;
  return f;
}

static void test_parse_single_text() {
  Parser p(1024);
  std::string f = serverFrame(Opcode::Text, "{\"t\":\"welcome\"}");
  p.feed(reinterpret_cast<const uint8_t*>(f.data()), f.size());
  Message m;
  TEST_ASSERT_TRUE(p.next(m));
  TEST_ASSERT_EQUAL_INT((int)Opcode::Text, (int)m.op);
  TEST_ASSERT_EQUAL_STRING("{\"t\":\"welcome\"}",
                           std::string(m.payload.begin(), m.payload.end()).c_str());
  TEST_ASSERT_FALSE(p.next(m));
}

// Byte-at-a-time feeding must reassemble the same message (incremental robustness).
static void test_parse_byte_dribble() {
  Parser p(1024);
  std::string f = serverFrame(Opcode::Text, "abcdefghij");
  for (char c : f) p.feed(reinterpret_cast<const uint8_t*>(&c), 1);
  Message m;
  TEST_ASSERT_TRUE(p.next(m));
  TEST_ASSERT_EQUAL_STRING("abcdefghij",
                           std::string(m.payload.begin(), m.payload.end()).c_str());
}

// A fragmented data message with a control frame interleaved: control surfaces first,
// then the reassembled data message.
static void test_parse_fragmented_with_control() {
  Parser p(1024);
  std::string a = serverFrame(Opcode::Text, "Hel", /*fin=*/false);
  std::string ping = serverFrame(Opcode::Ping, "hb");
  std::string b = serverFrame(Opcode::Continuation, "lo", /*fin=*/true);
  std::string all = a + ping + b;
  p.feed(reinterpret_cast<const uint8_t*>(all.data()), all.size());
  Message m;
  TEST_ASSERT_TRUE(p.next(m));
  TEST_ASSERT_EQUAL_INT((int)Opcode::Ping, (int)m.op);
  TEST_ASSERT_TRUE(p.next(m));
  TEST_ASSERT_EQUAL_INT((int)Opcode::Text, (int)m.op);
  TEST_ASSERT_EQUAL_STRING("Hello", std::string(m.payload.begin(), m.payload.end()).c_str());
}

static void test_parse_close_code() {
  Parser p(1024);
  std::string body;
  body.push_back((char)(4002 >> 8));
  body.push_back((char)(4002 & 0xFF));
  std::string f = serverFrame(Opcode::Close, body);
  p.feed(reinterpret_cast<const uint8_t*>(f.data()), f.size());
  Message m;
  TEST_ASSERT_TRUE(p.next(m));
  TEST_ASSERT_EQUAL_INT((int)Opcode::Close, (int)m.op);
  TEST_ASSERT_EQUAL_UINT16(4002, m.closeCode);
}

// A masked server frame is a protocol violation; the cap and reserved bits too.
static void test_parse_protocol_errors() {
  {
    Parser p(1024);
    uint8_t masked[] = {0x81, 0x85, 1, 2, 3, 4, 'a', 'b', 'c', 'd', 'e'};
    p.feed(masked, sizeof(masked));
    TEST_ASSERT_TRUE(p.protocolError());
  }
  {
    Parser p(4);  // tiny cap
    std::string f = serverFrame(Opcode::Text, "toolong");
    p.feed(reinterpret_cast<const uint8_t*>(f.data()), f.size());
    TEST_ASSERT_TRUE(p.protocolError());
  }
  {
    Parser p(1024);
    uint8_t reserved[] = {0xC1, 0x00};  // RSV1 set
    p.feed(reserved, sizeof(reserved));
    TEST_ASSERT_TRUE(p.protocolError());
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_accept_known_answer);
  RUN_TEST(test_upgrade_request_and_validate);
  RUN_TEST(test_client_frame_masked);
  RUN_TEST(test_client_frame_extended_len);
  RUN_TEST(test_parse_single_text);
  RUN_TEST(test_parse_byte_dribble);
  RUN_TEST(test_parse_fragmented_with_control);
  RUN_TEST(test_parse_close_code);
  RUN_TEST(test_parse_protocol_errors);
  return UNITY_END();
}
