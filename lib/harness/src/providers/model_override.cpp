#include <string>
#include <utility>

#include "nimbus/harness/log.h"
#include "nimbus/harness/providers.h"

// ScopedModelOverride (providers.h): one pointer, the live pin. The object holds its
// own host and model, so the pin costs no static strings.
namespace agent {
namespace providers {

namespace {
const ScopedModelOverride* g_pin = nullptr;
}  // namespace

ScopedModelOverride::ScopedModelOverride(std::string host, std::string model)
    : host_(std::move(host)), model_(std::move(model)) {
  if (host_.empty() || model_.empty()) return;
  if (g_pin) {
    hlog::logf("model pin: %s on %s not taken, %s on %s is held", model_.c_str(), host_.c_str(),
               g_pin->model().c_str(), g_pin->host().c_str());
    return;
  }
  g_pin = this;
  active_ = true;
}

ScopedModelOverride::~ScopedModelOverride() {
  if (active_ && g_pin == this) g_pin = nullptr;
}

std::string modelOverrideFor(const char* host) {
  if (!g_pin || !host || g_pin->host() != host) return std::string();
  return g_pin->model();
}

}  // namespace providers
}  // namespace agent
