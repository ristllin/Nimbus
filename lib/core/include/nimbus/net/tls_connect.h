#pragma once
#include <cstdint>
#include <type_traits>

// tls_connect - the device's one dial seam (src/sys/net_util.h connectHost), as templates
// over the client and address types so the host tests run the code the device runs
// (test_tls_connect drives them with a fake that has the framework's connect overloads;
// the device instantiates them with WiFiClientSecure, WiFiClient and IPAddress).
//
// The device resolves a name itself, under the lwIP core lock (lane CRASH2:
// resolveHostLocked), and then dials the address, so no DNS runs inside connect().
//
// A TLS dial by address must still name the host (lane SNI). In arduino-esp32 3.3.9 every
// address overload of NetworkClientSecure::connect but one passes host = NULL
// (NetworkClientSecure.cpp:110-136), and start_ssl_client then names the session after the
// address string (ssl_client.cpp:309): no SNI, and the certificate is checked against an
// address. From e697fe87 to this fix every TLS dial on main went that way and the CDN front
// ends refused every handshake (-30592): bench B03 on bf40a3ed, and the K01 soak on c168797b
// where every provider exchange failed at connect.
//
// connectSecure dials connect(ip, port, host, nullptr, nullptr, nullptr), the one overload
// that carries the name. The three null pointers are the CA certificate, the client
// certificate and its key, which is what the address overloads passed: they forward the
// members _CA_cert, _cert and _private_key, and the firmware sets none of them. It calls
// only setCACertBundle or setInsecure, and start_ssl_client reads those two modes from the
// members _use_ca_bundle and _use_insecure (ssl_client.cpp:203-227), so both modes stay as
// the caller set them. The PSK form (:110-113) needs setPreSharedKey, which the firmware
// never calls. A timeout goes through setConnectionTimeout, which is all
// connect(ip, port, timeout) did (:117-120 write the same _timeout member).
//
// Ops for both templates: a resolver, bool(const char* host, Addr& out).
namespace nimbus::net {

constexpr int32_t kKeepTimeout = -1;   // dial with the timeout the client already has

// A client type that can carry TLS. The device specializes this for its Client base and its
// secure class (WiFiClientSecure, a typedef of NetworkClientSecure) in src/sys/net_util.h;
// the host tests specialize it for their fakes. connectPlain refuses such a type at compile
// time: a plain dial on a client that can carry TLS would send the TLS session out by
// address with no host name (no SNI, the certificate checked against an address) - the lane
// SNI defect.
template <class T>
struct can_carry_tls : std::false_type {};

// A TLS dial by name. A dial with no name is refused before any lookup: it could only
// reach the peer by address.
template <class Addr, class Secure, class Resolve>
int connectSecure(Secure& c, const char* host, uint16_t port, int32_t timeoutMs, Resolve resolve) {
  if (host == nullptr || *host == '\0') return 0;
  Addr ip;
  if (!resolve(host, ip)) return 0;
  if (timeoutMs >= 0) c.setConnectionTimeout((uint32_t)timeoutMs);
  return c.connect(ip, port, host, nullptr, nullptr, nullptr);
}

// A plain TCP dial by name (no TLS, so no name rides the connection). Only a client that
// cannot carry TLS may take this path: the secure class derives from the plain one, so a
// base-class reference can hold a secure client, and dialing it here would name the TLS
// session after the address.
template <class Addr, class Plain, class Resolve>
int connectPlain(Plain& c, const char* host, uint16_t port, int32_t timeoutMs, Resolve resolve) {
  static_assert(!can_carry_tls<Plain>::value,
                "connectPlain on a client that can carry TLS would dial it by address with no host name");
  Addr ip;
  if (!resolve(host, ip)) return 0;
  return timeoutMs >= 0 ? c.connect(ip, port, timeoutMs) : c.connect(ip, port);
}

}  // namespace nimbus::net
