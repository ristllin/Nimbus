#include "nimbus/harness/fabric.h"

#include <cstring>

#include "nimbus/harness/log.h"

// Ported from Nuage-Solide src/agent/heavy_fabric.cpp (Head Orchestrator v2).
// Registry + jobId->adapter routing; unchanged logic. Lifted verbatim from
// src/agent/heavy_fabric.cpp into the portable harness (2026-07); logging now
// rides the injected hlog sink instead of the device agent_log.

namespace agent {

void HeavyFabric::registerAdapter(ManagedAgentAdapter* a) {
  if (adapterCount_ < MAX_ADAPTERS) adapters_[adapterCount_++] = a;
}

void HeavyFabric::bindCategory(const char* category, const char* backendId) {
  if (bindingCount_ >= MAX_BINDINGS) return;
  Binding& b = bindings_[bindingCount_++];
  strncpy(b.category,  category,  sizeof(b.category)  - 1);
  b.category[sizeof(b.category) - 1] = 0;
  strncpy(b.backendId, backendId, sizeof(b.backendId) - 1);
  b.backendId[sizeof(b.backendId) - 1] = 0;
}

ManagedAgentAdapter* HeavyFabric::adapterFor(const char* jobIdOrBackend) {
  // jobId format: "backend:...", so the prefix before ':' is the backend id.
  // Sized for the longest real backend id ("anthropic" = 9) plus margin. A longer
  // prefix means an unknown backend; log and bail rather than match a truncation.
  char backend[24] = {};
  const char* colon = strchr(jobIdOrBackend, ':');
  if (colon) {
    size_t len = (size_t)(colon - jobIdOrBackend);
    if (len >= sizeof(backend)) {
      hlog::logf("fabric: backend prefix too long (%u), unknown backend", (unsigned)len);
      return nullptr;
    }
    memcpy(backend, jobIdOrBackend, len);
  } else {
    if (strlen(jobIdOrBackend) >= sizeof(backend)) {
      hlog::log("fabric: backend id too long, unknown backend");
      return nullptr;
    }
    strncpy(backend, jobIdOrBackend, sizeof(backend) - 1);
  }
  for (int i = 0; i < adapterCount_; i++)
    if (strcmp(adapters_[i]->backendId(), backend) == 0) return adapters_[i];
  return nullptr;
}

static const char* backendForCategory(
    const HeavyFabric::Binding* bindings, int count, const char* category) {
  for (int i = 0; i < count; i++)
    if (strcmp(bindings[i].category, category) == 0) return bindings[i].backendId;
  return nullptr;
}

FabricErr HeavyFabric::dispatch(const Directive& d, char outJobId[72]) {
  const char* bid = backendForCategory(bindings_, bindingCount_, d.category);
  if (!bid) { hlog::logf("fabric: no backend for category %s", d.category ? d.category : "?"); return FabricErr::NotFound; }
  ManagedAgentAdapter* a = adapterFor(bid);
  if (!a) { hlog::logf("fabric: adapter not found: %s", bid); return FabricErr::NotFound; }
  return a->dispatch(d, outJobId);
}

FabricErr HeavyFabric::poll(const char* jobId, ResultEnvelope& env) {
  ManagedAgentAdapter* a = adapterFor(jobId);
  if (!a) return FabricErr::NotFound;
  return a->poll(jobId, env);
}

FabricErr HeavyFabric::cancel(const char* jobId) {
  ManagedAgentAdapter* a = adapterFor(jobId);
  if (!a) return FabricErr::NotFound;
  return a->cancel(jobId);
}

FabricErr HeavyFabric::answer(const char* jobId, const char* userText) {
  ManagedAgentAdapter* a = adapterFor(jobId);
  if (!a) return FabricErr::NotFound;
  return a->answer(jobId, userText);
}

}  // namespace agent
