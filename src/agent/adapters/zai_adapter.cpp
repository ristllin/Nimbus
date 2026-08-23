#include "zai_adapter.h"

#include "../agent_config.h"
#include "../store.h"
#include "../transport_tls.h"
#include "nimbus/harness/providers.h"

namespace agent {

Capabilities ZaiAdapter::capabilities() const {
  Capabilities c;
  c.typicalLatencySec = 30;
  return c;
}

FabricErr ZaiAdapter::dispatch(const Directive& d, char outJobId[72]) {
  String host = store::zaiBase();
  if (!host.length()) host = ZAI_HOST_PRIMARY;
  String model = store::subModel("zai");
  if (!model.length()) model = store::orchModel("zai");
  providers::CompatEndpoint ep;
  ep.host = host.c_str();
  ep.basePath = ZAI_BASE_PATH;
  ep.key = std::string(store::zaiKey().c_str());
  ep.model = std::string(model.c_str());
  ep.backendTag = "zai";
  return providers::openaiCompatDispatch(deviceProviderDeps(), ep, d, outJobId);
}

FabricErr ZaiAdapter::poll(const char* jobId, ResultEnvelope& env) {
  return providers::openaiCompatPoll(deviceProviderDeps(), "zai", jobId, env);
}

FabricErr ZaiAdapter::cancel(const char* jobId) {
  return providers::openaiCompatCancel(deviceProviderDeps(), jobId);
}

}  // namespace agent
