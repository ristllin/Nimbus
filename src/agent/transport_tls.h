#pragma once
#include "nimbus/harness/http.h"
#include "nimbus/harness/providers.h"

// transport_tls - the DEVICE half of the Stage-H provider split.
//
//   deviceTransport()    - the one agent::HttpTransport over WiFiClientSecure /
//                          WiFiClient: tlsSetup (CA bundle / setInsecure) +
//                          tlsClose (RST, no TIME_WAIT) + the tls_arbiter
//                          work-slot + HTTP/1.0 request + ONE body write +
//                          full-body response read.
//   deviceProviderDeps() - the providers::ProviderDeps bundle over store:: +
//                          connectors:: + PsramJsonAllocator + millis/heap.
//
// Scope: the four provider adapters ONLY. audio_stt/audio_tts/tavily/
// embeddings/http_multipart/provider_verify stay on their own TLS paths this
// stage (they stream multipart bodies / need custom handling).
namespace agent {

HttpTransport& deviceTransport();
providers::ProviderDeps deviceProviderDeps();

}  // namespace agent
