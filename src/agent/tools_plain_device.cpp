// tools_plain_device.cpp - the device side of engine 2's device, people, models and docs groups
// (lane E2-P-TOOLS; the registration is tools_plain_device.h). Binds engine 1's handlers where
// engine 1 registers them (main.cpp, memory_subsystem.cpp, device_log_tool.cpp, models_tool.h)
// and engine 1's device-action executors (orchestrator::fillDeviceApplyDeps over
// device_apply.cpp), never engine 1's turn machinery.
#include "tools_plain_device.h"

#include <memory>

#include "device_log_tool.h"           // devicelog::run - device.log
#include "nimbus/orch/models_tool.h"   // registerModelsTool - models.list on a private registry
#include "orchestrator.h"              // fillDeviceApplyDeps, inScheduledTurn, currentChat, turnInFlight
#include "store.h"                     // ttsEnabled - the voice-replies switch, read at call time

namespace agent {
namespace tools {

namespace {

constexpr char kClaude[] = "anthropic";   // the one head engine 2 answers with (refusalFor)

// The executors as engine 1 builds them, read when the call runs (the voice switch is live). A
// head switch is held to Claude: engine 2 answers no other provider yet, so a switch elsewhere
// would leave every later message refused.
DeviceApplyDeps controlDeps() {
  DeviceApplyDeps d;
  orchestrator::fillDeviceApplyDeps(d);
  d.ttsEnabled = store::ttsEnabled();
  const auto valid = d.modelIsValid;
  d.modelIsValid = [valid](const std::string& p, const std::string& m) { return p == kClaude && valid && valid(p, m); };
  return d;
}

nimbus::orch::ToolResult deviceLog(ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) {
  return devicelog::run(a);
}

}  // namespace

void registerPlainDevice(nimbus::orch::ToolRegistry& reg) {
  plaindevice::Handlers h;
  h.status = bound::deviceStatus;
  h.log = deviceLog;
  h.ota = bound::otaStatus;
  h.selftest = bound::selfTest;
  h.health = memory::bound::systemHealth;
  h.docsList = memory::bound::docsList;
  h.docsSearch = memory::bound::docsSearch;
  h.docsRead = memory::bound::docsRead;
  h.tenantList = memory::bound::tenantList;
  h.tenantSetRole = memory::bound::tenantSetRole;
  h.tenantSetQuota = memory::bound::tenantSetQuota;
  plaindevice::ControlDeps c;
  c.unattended = [] { return orchestrator::inScheduledTurn(); };
  c.chat = [] {
    if (!orchestrator::turnInFlight()) return std::string();
    const String chat = orchestrator::currentChat();
    return std::string(chat.c_str(), chat.length());
  };
  c.apply = controlDeps;
  plaindevice::registerDeviceFamilies(reg, h, c);
}

// models.list: the shared tool on a private registry (its own declaration stays engine 1's), reached
// under the same name with the engine-2 declaration.
void registerPlainModels(nimbus::orch::ToolRegistry& reg, const nimbus::orch::ModelsToolHandlers& mh) {
  static std::unique_ptr<nimbus::orch::ToolRegistry> inner;
  if (!inner) inner.reset(new nimbus::orch::ToolRegistry());
  nimbus::orch::registerModelsTool(*inner, mh);
  nimbus::orch::ToolRegistry* r = inner.get();
  plaindevice::bindDeclared(reg, "models.list", [r](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) {
    return r->dispatch("models.list", a, who);
  });
}

bool switchHead(const std::string& providerModel) {
  const size_t slash = providerModel.find('/');
  if (slash == std::string::npos || slash == 0 || slash + 1 >= providerModel.size()) return false;
  DeviceApplyDeps d;
  orchestrator::fillDeviceApplyDeps(d);
  if (!d.setOrchHostModel) return false;
  d.setOrchHostModel(providerModel.substr(0, slash), providerModel.substr(slash + 1));
  return true;
}

}  // namespace tools
}  // namespace agent
