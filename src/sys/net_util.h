#pragma once
#include <WiFiClientSecure.h>
#include <lwip/sockets.h>

// agent::store::tlsVerify() - the outbound-TLS security switch. Forward-declared
// (not #include "agent/store.h") so this sys-level header carries no dependency
// on the agent subsystem; the symbol links from src/agent/store.cpp.
namespace agent { namespace store { bool tlsVerify(); } }

// Outbound-TLS setup: the ONE seam every WiFiClientSecure passes through before
// connect(). When store::tlsVerify() is true (default), the server certificate is
// validated against the ~200-root CA bundle the IDF embeds in flash
// (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) - so an evil-twin AP / rogue router on a
// public network can NO LONGER MITM the provider connections and harvest the
// Authorization: Bearer <key> headers + the Telegram bot token. When false, it
// falls back to the historical setInsecure() (encrypted but unvalidated) - the
// escape hatch for a self-signed custom orchHost or a provider whose root isn't
// bundled (a connection ERROR, not a security hole). See docs/security.md.
//
// The bundle is linked in by the framework; these are its binary blob boundary
// symbols (nm libmbedtls.a: _binary_x509_crt_bundle_{start,end}). The asm() label
// pins the exact objcopy-generated symbol name regardless of C++ linkage - the
// idiom the arduino-esp32 CA-bundle examples use. One shared decl across every TU
// that includes this header; the linker resolves to the single embedded blob.
extern const uint8_t _nimbusCrtBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t _nimbusCrtBundleEnd[]   asm("_binary_x509_crt_bundle_end");

inline void tlsSetup(WiFiClientSecure& c) {
  if (agent::store::tlsVerify()) {
    c.setCACertBundle(_nimbusCrtBundleStart,
                      (size_t)(_nimbusCrtBundleEnd - _nimbusCrtBundleStart));
  } else {
    c.setInsecure();
  }
}

// Ported verbatim from Nuage-Solide src/net_util.h (Head Orchestrator v2).
//
// Abortive TLS close: set SO_LINGER with l_linger=0 so close() sends a RST instead
// of a FIN - which leaves NO TIME_WAIT TCP control block behind.
//
// Why this survives the S3/PSRAM relaxation (plan §3.7): the problem it solves is
// an lwIP PCB-POOL issue, not a heap issue. We open/close TLS FREQUENTLY - every
// orchestrator turn, every sub-session poll (round-robin), every Telegram
// long-poll. We active-close (stop() right after reading the HTTP/1.0 response
// body, before the server's FIN), so each normal close parks a ~1.5-2 KB
// TIME_WAIT PCB for ~1-2 minutes. Under load these accumulate faster than they
// drain and starve the fixed lwIP PCB pool (independent of PSRAM) until even DNS
// fails. RST-close skips TIME_WAIT entirely. Safe here: the response is fully read
// before we close, so the RST discards nothing we need.
//
// Depends on lwip/sockets.h + WiFiClientSecure::fd() - both in the arduino-esp32
// core (no new lib_deps).
inline void tlsClose(WiFiClientSecure& c) {
  int fd = c.fd();
  if (fd >= 0) {
    struct linger so;
    so.l_onoff  = 1;
    so.l_linger = 0;
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &so, sizeof(so));
  }
  c.stop();
}
