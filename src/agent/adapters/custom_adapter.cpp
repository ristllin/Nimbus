#include "custom_adapter.h"

#include "../transport_tls.h"
#include "nimbus/harness/providers.h"

// Custom / proxy adapter - THIN device wrapper since Stage H of the harness
// extraction. The entire wire (base-URL parsing, the keyless-on-http rule, the
// per-convention chat-completion sub-session + cache, and the NEW head-custom
// single-shot turn) lives in lib/harness/src/providers/custom.cpp, host-tested
// by test_harness_wire_custom. This file only binds the device deps; the public
// surface is unchanged (the head-custom ProviderTurnFn is registered in
// orchestrator.cpp, gated on store::hasCustom()).

namespace agent {

Capabilities CustomAdapter::capabilities() const {
  Capabilities c;
  c.selfHosted        = true;
  c.typicalLatencySec = 30;
  return c;
}

FabricErr CustomAdapter::dispatch(const Directive& d, char outJobId[72]) {
  return providers::customDispatch(deviceProviderDeps(), d, outJobId);
}

FabricErr CustomAdapter::poll(const char* jobId, ResultEnvelope& env) {
  return providers::customPoll(deviceProviderDeps(), jobId, env);
}

FabricErr CustomAdapter::cancel(const char* jobId) {
  return providers::customCancel(deviceProviderDeps(), jobId);
}

}  // namespace agent
