#include "cumulo_adapter.h"

#include "../agent_config.h"
#include "../store.h"
#include "../transport_tls.h"
#include "nimbus/harness/providers.h"

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
  int sl = sel.indexOf('/');
  String upstream = sl > 0 ? sel.substring(0, sl) : String("openai");
  String model = sl > 0 ? sel.substring(sl + 1) : sel;
  if (!model.length()) return FabricErr::BadRequest;
  String host = store::cumuloBase();
  if (!host.length()) host = CUMULO_HOST_DEFAULT;
  host = bareHost(host);
  String basePath = String("/router/") + upstream + "/v1";
  providers::CompatEndpoint ep;
  ep.host = host.c_str();
  ep.basePath = std::string(basePath.c_str());
  ep.key = std::string(store::cumuloKey().c_str());
  ep.model = std::string(model.c_str());
  ep.backendTag = "cumulo";
  return providers::openaiCompatDispatch(deviceProviderDeps(), ep, d, outJobId);
}

FabricErr CumuloAdapter::poll(const char* jobId, ResultEnvelope& env) {
  return providers::openaiCompatPoll(deviceProviderDeps(), "cumulo", jobId, env);
}

FabricErr CumuloAdapter::cancel(const char* jobId) {
  return providers::openaiCompatCancel(deviceProviderDeps(), jobId);
}

}  // namespace agent
