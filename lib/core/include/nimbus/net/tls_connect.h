#pragma once
#include <cstdint>

// tls_connect - the device's one dial seam (src/sys/net_util.h connectHost), as templates
// over the client and address types so the host tests run the code the device runs
// (test_tls_connect drives them with a fake that has the framework's connect overloads;
// the device instantiates them with WiFiClientSecure, WiFiClient and IPAddress).
//
// The device resolves a name itself, under the lwIP core lock (lane CRASH2:
// resolveHostLocked), and then dials the address, so no DNS runs inside connect().
//
// Ops for both templates: a resolver, bool(const char* host, Addr& out).
namespace nimbus::net {

constexpr int32_t kKeepTimeout = -1;   // dial with the timeout the client already has

// A TLS dial by name.
template <class Addr, class Secure, class Resolve>
int connectSecure(Secure& c, const char* host, uint16_t port, int32_t timeoutMs, Resolve resolve) {
  Addr ip;
  if (!resolve(host, ip)) return 0;
  return timeoutMs >= 0 ? c.connect(ip, port, timeoutMs) : c.connect(ip, port);
}

// A plain TCP dial by name (no TLS, so no name rides the connection).
template <class Addr, class Plain, class Resolve>
int connectPlain(Plain& c, const char* host, uint16_t port, int32_t timeoutMs, Resolve resolve) {
  Addr ip;
  if (!resolve(host, ip)) return 0;
  return timeoutMs >= 0 ? c.connect(ip, port, timeoutMs) : c.connect(ip, port);
}

}  // namespace nimbus::net
