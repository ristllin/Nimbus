#include "cumulo_adapter.h"

#include "../agent_config.h"
#include "../store.h"
#include "../transport_tls.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/router_route.h"   // CUM-369: shared "<upstream>/<model>" split (head parity)

namespace agent {

Capabilities CumuloAdapter::capabilities() const {
  Capabilities c;
  c.typicalLatencySec = 45;   // an extra hop through the router
  return c;
}

// Strip scheme + any path from a stored base, leaving a bare host for connect().
static String bareHost(String h) {
  int sch = h.indexOf("://");
  if (sch >= 0) h = h.substring(sch + 3);
  int sl = h.indexOf('/');
  if (sl >= 0) h = h.substring(0, sl);
  return h;
}

FabricErr CumuloAdapter::dispatch(const Directive& d, char outJobId[72]) {
  String sel = store::subModel("cumulo");   // "<upstream>/<model>"
  if (!sel.length()) sel = store::orchModel("cumulo");
  // Resolve the selector through the ONE shared rule the orchestrator head also
  // uses (CUM-369), so both address the router identically: base
  // /router/<upstream>/v1 and only the bare model (the id the router prices).
  const nimbus::orch::RouterRoute route =
      nimbus::orch::resolveRouterRoute(std::string(sel.c_str()));
  if (route.model.empty()) return FabricErr::BadRequest;
  String host = store::cumuloBase();
  if (!host.length()) host = CUMULO_HOST_DEFAULT;
  host = bareHost(host);
  providers::CompatEndpoint ep;
  ep.host = host.c_str();
  ep.basePath = route.basePath;
  ep.key = std::string(store::cumuloKey().c_str());
  ep.model = route.model;
  ep.backendTag = "cumulo";
  // The router proxies each upstream on its native wire: Anthropic speaks Messages,
  // everyone else chat-completions.
  ep.wire = (route.upstream == "anthropic") ? providers::CompatWire::AnthropicMessages
                                            : providers::CompatWire::OpenAIChat;
  return providers::openaiCompatDispatch(deviceProviderDeps(), ep, d, outJobId);
}

FabricErr CumuloAdapter::poll(const char* jobId, ResultEnvelope& env) {
  return providers::openaiCompatPoll(deviceProviderDeps(), "cumulo", jobId, env);
}

FabricErr CumuloAdapter::cancel(const char* jobId) {
  return providers::openaiCompatCancel(deviceProviderDeps(), jobId);
}

}  // namespace agent
