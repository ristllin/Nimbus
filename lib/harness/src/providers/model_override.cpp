#include <string>
#include <utility>

#include "nimbus/harness/log.h"
#include "nimbus/harness/providers.h"

// ScopedModelOverride (providers.h): one pointer per thread, the live pin. Thread-local
// like the connector grant's scope (orch_connector_grant.cpp): only the task that pinned
// reads it, so another task's request (a sub-agent poll, a web handler, the VN's HTTP
// threads) never sees it, and the object it points at lives on that task's own stack.
// The object holds its own host and model, so the pin costs no static strings.
namespace agent {
namespace providers {

namespace {
thread_local const ScopedModelOverride* t_pin = nullptr;

// The hosts whose requests read their model through ProviderDeps::orchModel, the
// closure that consults the pin. Z.ai, the Cumulo router and the custom endpoint read
// theirs elsewhere (customModel, the router's pick), so a pin there could not take
// effect while its spend was filed under it.
bool pinnable(const std::string& host) {
  return host == "anthropic" || host == "openai" || host == "mistral";
}
}  // namespace

ScopedModelOverride::ScopedModelOverride(std::string host, std::string model)
    : host_(std::move(host)), model_(std::move(model)) {
  if (host_.empty() || model_.empty()) return;
  if (!pinnable(host_)) {
    hlog::logf("model pin: %s on %s not taken, only a direct host's model can be pinned", model_.c_str(),
               host_.c_str());
    return;
  }
  if (t_pin) {
    hlog::logf("model pin: %s on %s not taken, %s on %s is held", model_.c_str(), host_.c_str(),
               t_pin->model().c_str(), t_pin->host().c_str());
    return;
  }
  t_pin = this;
  active_ = true;
}

ScopedModelOverride::~ScopedModelOverride() {
  if (active_ && t_pin == this) t_pin = nullptr;
}

std::string modelOverrideFor(const char* host) {
  if (!t_pin || !host || t_pin->host() != host) return std::string();
  return t_pin->model();
}

}  // namespace providers
}  // namespace agent
