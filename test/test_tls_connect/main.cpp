#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "nimbus/net/tls_connect.h"

// The dial seam (nimbus/net/tls_connect.h; src/sys/net_util.h connectHost instantiates it for
// every outbound connection the firmware makes) against a fake client with the framework's
// connect overloads and the framework's rule for the TLS name. Lane SNI.
//
// arduino-esp32 3.3.9, libraries/NetworkClientSecure/src/NetworkClientSecure.cpp:
//   :110-115 connect(ip, port)            -> connect(ip, port, _CA_cert, _cert, _private_key),
//                                            or the PSK form when _pskIdent and _psKey are set
//   :117-120 connect(ip, port, timeout)   -> _timeout = timeout; connect(ip, port)
//   :134-136 connect(ip, port, CA, cert, key) -> connect(ip, port, NULL, CA, cert, key)
//   :147-148 connect(ip, port, host, CA, cert, key) -> start_ssl_client(ssl, ip, port, host,
//            _timeout, CA, _use_ca_bundle, cert, key, NULL, NULL, _use_insecure, _alpn_protos)
//   the const char* host overloads resolve with Network.hostByName inside connect()
// ssl_client.cpp:309 sets the TLS name to `host` when it is not NULL, else ip.toString().
// ssl_client.cpp:203-227 picks the verification mode: the insecure member first, then a CA
// buffer argument, then the bundle member; :275 loads a client certificate only when both the
// cert and key arguments are set.
//
// From e697fe87 (lane CRASH2) the seam dialed connect(ip, port[, timeout]), so the name was the
// address string, the ClientHello carried no SNI and the name check ran against an address: the
// CDN front ends refused every handshake with a fatal alert (-30592, bench B03 on bf40a3ed), and
// the K01 soak on c168797b recorded every provider exchange fail=connect.

void setUp() {}
void tearDown() {}

namespace {

struct Addr {
  uint32_t v = 0;
  std::string toString() const {
    char b[20];
    std::snprintf(b, sizeof b, "%u.%u.%u.%u", (unsigned)(v >> 24) & 255u, (unsigned)(v >> 16) & 255u,
                  (unsigned)(v >> 8) & 255u, (unsigned)v & 255u);
    return b;
  }
};

// What one start_ssl_client call got.
struct Dial {
  std::string tlsName;
  bool caArg = false, certArg = false, keyArg = false;
  int32_t timeout = 0;
  uint16_t port = 0;
  uint32_t addr = 0;
};

// NetworkClientSecure's overload set. The members are what the firmware leaves them: tlsSetup()
// and relayTlsSetup() call only setCACertBundle or setInsecure, so the CA, certificate, key and
// PSK members stay NULL. The peer is a CDN front end with a certificate for `peerHost`: it
// completes a handshake whose name is that host and refuses any other (an address string, the
// wrong name).
struct FakeSecure {
  const char* caMember = nullptr;
  const char* certMember = nullptr;
  const char* keyMember = nullptr;
  const char* pskIdent = nullptr;
  const char* psKey = nullptr;
  int32_t timeout = 30000;   // NetworkClientSecure.cpp:33
  std::string peerHost;
  std::vector<Dial> dials;
  int dnsInside = 0;         // a host overload ran: Network.hostByName inside connect()
  int pskDials = 0;

  void setConnectionTimeout(uint32_t ms) { timeout = (int32_t)ms; }   // NetworkClient.cpp:351

  int connect(Addr ip, uint16_t port) {
    if (pskIdent && psKey) return connect(ip, port, pskIdent, psKey);
    return connect(ip, port, caMember, certMember, keyMember);
  }
  int connect(Addr ip, uint16_t port, int32_t t) {
    timeout = t;
    return connect(ip, port);
  }
  int connect(const char*, uint16_t) { return ++dnsInside, 0; }
  int connect(const char*, uint16_t, int32_t) { return ++dnsInside, 0; }
  int connect(Addr ip, uint16_t port, const char* ca, const char* cert, const char* key) {
    return connect(ip, port, (const char*)nullptr, ca, cert, key);
  }
  int connect(const char*, uint16_t, const char*, const char*, const char*) { return ++dnsInside, 0; }
  int connect(Addr, uint16_t, const char*, const char*) { return ++pskDials, ++dnsInside, 0; }   // :185-187
  int connect(const char*, uint16_t, const char*, const char*) { return ++pskDials, ++dnsInside, 0; }
  int connect(Addr ip, uint16_t port, const char* host, const char* ca, const char* cert, const char* key) {
    Dial d;
    d.tlsName = host != nullptr ? std::string(host) : ip.toString();
    d.caArg = ca != nullptr;
    d.certArg = cert != nullptr;
    d.keyArg = key != nullptr;
    d.timeout = timeout;
    d.port = port;
    d.addr = ip.v;
    dials.push_back(d);
    return d.tlsName == peerHost ? 1 : 0;
  }
};

// NetworkClient's overload set (NetworkClient.cpp:206-212, :306-307).
struct FakePlain {
  int32_t timeout = 3000;
  std::vector<uint32_t> addrs;
  std::vector<int32_t> timeouts;
  int dnsInside = 0;
  int connect(Addr ip, uint16_t port) { return connect(ip, port, timeout); }
  int connect(Addr ip, uint16_t, int32_t t) {
    timeout = t;
    addrs.push_back(ip.v);
    timeouts.push_back(t);
    return 1;
  }
  int connect(const char*, uint16_t) { return ++dnsInside, 0; }
  int connect(const char*, uint16_t, int32_t) { return ++dnsInside, 0; }
};

struct Lookups {
  std::vector<std::string> asked;
  bool ok = true;
  uint32_t answer = 0xA04F680Au;   // 160.79.104.10
};

// The resolver the device passes (resolveHostLocked), as a value with a pointer to the record.
struct Resolve {
  Lookups* l;
  bool operator()(const char* host, Addr& out) const {
    l->asked.push_back(host ? host : "(null)");
    if (!l->ok) return false;
    out.v = l->answer;
    return true;
  }
};

// Every host the firmware dials over TLS (providers, Telegram, the relay and router, OTA and its
// redirect target, sfx sync, web search), plus one custom provider host given as an address.
const char* const kHosts[] = {
    "api.mistral.ai",        "api.anthropic.com",         "api.openai.com",
    "api.z.ai",              "api.telegram.org",          "relay.cumulo-nimbus.ai",
    "app.cumulo-nimbus.ai",  "github.com",                "objects.githubusercontent.com",
    "raw.githubusercontent.com", "api.tavily.com",        "192.168.50.20",
};

int dialSecure(FakeSecure& c, const char* host, int32_t timeoutMs, Lookups& l) {
  return nimbus::net::connectSecure<Addr>(c, host, 443, timeoutMs, Resolve{&l});
}

}  // namespace

// The fake reproduces the defect: the framework's address overloads give the TLS layer the
// address string as the name, and the peer refuses it. Without this, a fake that ignored names
// could let the tests below pass on the broken seam.
void test_an_address_dial_names_the_address_and_the_peer_refuses_it() {
  FakeSecure c;
  c.peerHost = "api.mistral.ai";
  TEST_ASSERT_EQUAL_INT(0, c.connect(Addr{0xA04F680Au}, 443));
  TEST_ASSERT_EQUAL_INT(0, c.connect(Addr{0xA04F680Au}, 443, 12000));
  TEST_ASSERT_EQUAL_size_t(2, c.dials.size());
  TEST_ASSERT_EQUAL_STRING("160.79.104.10", c.dials[0].tlsName.c_str());
  TEST_ASSERT_EQUAL_STRING("160.79.104.10", c.dials[1].tlsName.c_str());
}

// The class: every host, both forms (with a timeout and with the client's own), and the handshake
// completes because the name is the host's.
void test_every_secure_dial_names_its_host_for_sni_and_the_name_check() {
  for (const char* host : kHosts) {
    for (int32_t t : {nimbus::net::kKeepTimeout, (int32_t)10000}) {
      FakeSecure c;
      c.peerHost = host;
      Lookups l;
      TEST_ASSERT_EQUAL_INT_MESSAGE(1, dialSecure(c, host, t, l), host);
      TEST_ASSERT_EQUAL_size_t_MESSAGE(1, c.dials.size(), host);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(host, c.dials[0].tlsName.c_str(), host);
      TEST_ASSERT_EQUAL_UINT16(443, c.dials[0].port);
    }
  }
}

// The verification mode stays the client's: no CA buffer, certificate or key argument (each would
// override or add to the bundle or insecure member start_ssl_client reads), and never the PSK form.
void test_a_secure_dial_keeps_the_clients_verification_mode() {
  for (const char* host : kHosts) {
    FakeSecure c;
    c.peerHost = host;
    Lookups l;
    dialSecure(c, host, 15000, l);
    TEST_ASSERT_EQUAL_size_t(1, c.dials.size());
    TEST_ASSERT_FALSE_MESSAGE(c.dials[0].caArg, host);
    TEST_ASSERT_FALSE_MESSAGE(c.dials[0].certArg, host);
    TEST_ASSERT_FALSE_MESSAGE(c.dials[0].keyArg, host);
    TEST_ASSERT_EQUAL_INT(0, c.pskDials);
  }
}

// A timeout given to the seam is the one the socket and the reads run with, as
// connect(ip, port, timeout) set it. With none, the client's own setting stands (the callers set
// setConnectionTimeout before they dial).
void test_the_timeout_reaches_the_socket_and_none_keeps_the_clients() {
  FakeSecure a;
  a.peerHost = "api.telegram.org";
  Lookups l;
  dialSecure(a, "api.telegram.org", 8000, l);
  TEST_ASSERT_EQUAL_INT32(8000, a.dials.at(0).timeout);

  FakeSecure b;
  b.peerHost = "api.openai.com";
  b.setConnectionTimeout(15000);
  dialSecure(b, "api.openai.com", nimbus::net::kKeepTimeout, l);
  TEST_ASSERT_EQUAL_INT32(15000, b.dials.at(0).timeout);
}

// CRASH2's guarantee stays: the name is resolved once, by the device's resolver (under the core
// lock on the device), and connect() gets the address, so no DNS runs inside it.
void test_the_name_is_resolved_once_outside_connect() {
  FakeSecure c;
  c.peerHost = "api.anthropic.com";
  Lookups l;
  l.answer = 0x0A000005u;
  TEST_ASSERT_EQUAL_INT(1, dialSecure(c, "api.anthropic.com", 12000, l));
  TEST_ASSERT_EQUAL_size_t(1, l.asked.size());
  TEST_ASSERT_EQUAL_STRING("api.anthropic.com", l.asked[0].c_str());
  TEST_ASSERT_EQUAL_INT(0, c.dnsInside);
  TEST_ASSERT_EQUAL_UINT32(0x0A000005u, c.dials.at(0).addr);
}

void test_a_failed_lookup_never_dials() {
  FakeSecure c;
  c.peerHost = "api.mistral.ai";
  Lookups l;
  l.ok = false;
  TEST_ASSERT_EQUAL_INT(0, dialSecure(c, "api.mistral.ai", 12000, l));
  TEST_ASSERT_TRUE(c.dials.empty());
  TEST_ASSERT_EQUAL_INT(0, c.dnsInside);
}

// A secure dial with no name is refused before any lookup: it could only reach the peer by address.
void test_a_secure_dial_without_a_name_is_refused() {
  for (const char* host : {(const char*)nullptr, ""}) {
    FakeSecure c;
    c.peerHost = "api.mistral.ai";
    Lookups l;
    TEST_ASSERT_EQUAL_INT(0, dialSecure(c, host, 12000, l));
    TEST_ASSERT_TRUE(c.dials.empty());
    TEST_ASSERT_TRUE(l.asked.empty());
  }
}

// A plain TCP dial (a custom http:// provider, an http MCP server) goes by address with its timeout
// and no DNS inside; there is no TLS layer to name.
void test_a_plain_dial_goes_by_address_with_its_timeout() {
  Lookups l;
  FakePlain p;
  TEST_ASSERT_EQUAL_INT(1, nimbus::net::connectPlain<Addr>(p, "192.168.50.20", 8080, nimbus::net::kKeepTimeout, Resolve{&l}));
  TEST_ASSERT_EQUAL_INT(1, nimbus::net::connectPlain<Addr>(p, "mcp.local", 80, 5000, Resolve{&l}));
  TEST_ASSERT_EQUAL_size_t(2, p.addrs.size());
  TEST_ASSERT_EQUAL_INT32(3000, p.timeouts[0]);
  TEST_ASSERT_EQUAL_INT32(5000, p.timeouts[1]);
  TEST_ASSERT_EQUAL_INT(0, p.dnsInside);
  TEST_ASSERT_EQUAL_size_t(2, l.asked.size());

  FakePlain q;
  l.ok = false;
  TEST_ASSERT_EQUAL_INT(0, nimbus::net::connectPlain<Addr>(q, "mcp.local", 80, 5000, Resolve{&l}));
  TEST_ASSERT_TRUE(q.addrs.empty());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_an_address_dial_names_the_address_and_the_peer_refuses_it);
  RUN_TEST(test_every_secure_dial_names_its_host_for_sni_and_the_name_check);
  RUN_TEST(test_a_secure_dial_keeps_the_clients_verification_mode);
  RUN_TEST(test_the_timeout_reaches_the_socket_and_none_keeps_the_clients);
  RUN_TEST(test_the_name_is_resolved_once_outside_connect);
  RUN_TEST(test_a_failed_lookup_never_dials);
  RUN_TEST(test_a_secure_dial_without_a_name_is_refused);
  RUN_TEST(test_a_plain_dial_goes_by_address_with_its_timeout);
  return UNITY_END();
}
