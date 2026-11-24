#pragma once
// tools_plain_device.h - engine 2's device, people, models and docs groups behind more (lane
// E2-P-TOOLS, CUM-1003). Engine 1's handlers are bound as they are: each registers under its
// engine-1 registry name (device.status, tenant.set_role, ...; the model spells them with '_'),
// so the shared taint, action-rule, admin-only and group tables apply unchanged, with the
// engine-2 declaration of the same name (plain/texts.h kBehindMore). device_control runs the
// shared applier (nimbus/harness/device_apply.h); a restart or a head switch it accepts is asked
// of the turn as a deferred effect (plain/turn.h deferredCode) and runs once the reply is sent.
//
// This header is portable (test_plain_device_tools registers it over fakes); the device's
// handlers and deps are bound in tools_plain_device.cpp.
#include <ArduinoJson.h>

#include <functional>
#include <string>
#include <utility>

#include "nimbus/harness/device_apply.h"
#include "nimbus/harness/plain/request.h"   // toolDecl, declSchema
#include "nimbus/harness/plain/texts.h"     // tool_results::kControl*
#include "nimbus/harness/plain/turn.h"      // Deferred, deferredCode
#include "nimbus/orch/action_rules.h"       // actionRefusal, actionCaller
#include "nimbus/orch/tool_registry.h"

namespace nimbus {
namespace orch {
struct ModelsToolHandlers;
}  // namespace orch
}  // namespace nimbus

namespace agent {

// The engine-1 handlers this file binds, defined where engine 1 registers them.
namespace tools {
namespace bound {   // src/main.cpp
nimbus::orch::ToolResult deviceStatus(ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who);
nimbus::orch::ToolResult otaStatus(ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who);
nimbus::orch::ToolResult selfTest(ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who);
}  // namespace bound

// The device's registrations (tools_plain_device.cpp): the groups from registerMemoryFamilies,
// models.list from registerModelsFamily (tools_plain.cpp, one line each).
void registerPlainDevice(nimbus::orch::ToolRegistry& reg);
void registerPlainModels(nimbus::orch::ToolRegistry& reg, const nimbus::orch::ModelsToolHandlers& h);
// A head switch device_control deferred ("provider/model"), after the turn: the head setting is
// written by engine 1's executor. False for an argument of another shape or a refused write.
bool switchHead(const std::string& providerModel);
}  // namespace tools
namespace memory {
namespace bound {   // src/agent/memory_subsystem.cpp
nimbus::orch::ToolResult docsList(ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who);
nimbus::orch::ToolResult docsSearch(ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who);
nimbus::orch::ToolResult docsRead(ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who);
nimbus::orch::ToolResult systemHealth(ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who);
nimbus::orch::ToolResult tenantList(ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who);
nimbus::orch::ToolResult tenantSetRole(ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who);
nimbus::orch::ToolResult tenantSetQuota(ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who);
}  // namespace bound
}  // namespace memory

namespace plaindevice {

using Handler = nimbus::orch::ToolHandler;

// The handlers of the bound groups; an empty one leaves its tool unregistered.
struct Handlers {
  Handler status, log, ota, selftest, health;
  Handler docsList, docsSearch, docsRead;
  Handler tenantList, tenantSetRole, tenantSetQuota;
  Handler modelsList;
};

// What device_control reads at call time: the turn's unattended flag and chat, and the executors.
struct ControlDeps {
  std::function<bool()> unattended;
  std::function<std::string()> chat;
  std::function<DeviceApplyDeps()> apply;
};

// `name` with its engine-2 declaration over `h`; false when either is missing.
inline bool bindDeclared(nimbus::orch::ToolRegistry& reg, const char* name, Handler h) {
  const auto* d = plain::toolDecl(name);
  if (!d || !h) return false;
  reg.add(d->name, d->description, std::move(h), plain::declSchema(*d, false));
  return true;
}

// The keys device_control's action takes: its declaration's own words for the action argument
// (texts.h kBehindMore), so an unknown key's answer names them (E2-BFIX4, bench 24 Nov: Haiku
// sent {"speaker_volume":1} and read "applied: unsupported").
inline constexpr char kControlName[] = "device.control";
inline std::string controlKeys() {
  const auto* d = plain::toolDecl(kControlName);
  ArduinoJson::JsonDocument s;
  if (!d || deserializeJson(s, d->schema)) return std::string();
  return s["properties"]["action"]["description"] | "";
}

// E2-CFIX2 (bench 24 Nov): a config object as its knobs read it. {"key":K,"value":V} names the knob K (the field
// "key" alone reads as a credential and refuses the action), and "volume" is the speaker volume, sfxVol, the
// knob engine 1's own volume command sets (test_console "volume", the menu's Volume row).
inline constexpr char kKnobKey[] = "key", kKnobValue[] = "value", kVolume[] = "volume", kSfxVol[] = "sfxVol",
                      kConfig[] = "config";
inline void knobForm(ArduinoJson::JsonObject o) {
  if (o[kKnobKey].is<const char*>() && !o[kKnobValue].isNull()) {
    const std::string k = o[kKnobKey].as<const char*>();
    ArduinoJson::JsonDocument v;
    v.set(o[kKnobValue]);
    o.remove(kKnobKey);
    o.remove(kKnobValue);
    o[k] = v.as<ArduinoJson::JsonVariantConst>();
  }
  if (!o[kVolume].isNull() && o[kSfxVol].isNull()) {
    o[kSfxVol] = o[kVolume];
    o.remove(kVolume);
  }
}

// One device action now, by the shared applier (engine 1's device.control, the same caller rules
// and summary); a restart or a model switch it accepts is recorded and asked of the turn instead.
inline nimbus::orch::ToolResult deviceControl(ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who,
                                              bool unattended, const std::string& chat, DeviceApplyDeps d) {
  namespace tr = plain::texts::tool_results;
  using nimbus::orch::ToolResult;
  const std::string refused =
      nimbus::orch::actionRefusal(kControlName, nimbus::orch::actionCaller(who, unattended));
  if (!refused.empty()) return ToolResult::fail(refused);
  const ArduinoJson::JsonVariantConst given = a["action"];
  if (!given.is<ArduinoJson::JsonObjectConst>()) return ToolResult::fail(tr::kControlNoAction);
  ArduinoJson::JsonDocument doc;
  doc.set(given);
  if (doc[kConfig].is<ArduinoJson::JsonObject>()) knobForm(doc[kConfig].as<ArduinoJson::JsonObject>());
  else if (doc["type"] == kConfig) knobForm(doc.as<ArduinoJson::JsonObject>());
  const ArduinoJson::JsonVariantConst action = doc.as<ArduinoJson::JsonVariantConst>();
  // A key no action reads, or one only the owner may set, is never "applied": the answer names what the
  // tool takes (E2-BFIX4; E2-CFIX2 for the refused key, which was answered "applied: protected-BLOCKED").
  const nimbus::orch::ValidatedAction va = nimbus::orch::validateAction(action);
  if (va.kind == nimbus::orch::ActionKind::Unknown || (va.kind == nimbus::orch::ActionKind::Config && !va.allowed))
    return ToolResult::fail(tr::kControlNotApplied + va.reason + tr::kControlKeys + controlKeys());
  const std::string unknown =
      va.kind == nimbus::orch::ActionKind::Config && va.allowed ? nimbus::orch::unknownConfigKeys(action) : std::string();
  bool restart = false;
  std::string head;   // provider/model of an accepted switch
  const auto stage = d.stageDevice;
  d.stageDevice = [&restart, stage](const nimbus::orch::ValidatedAction& va) {
    if (va.kind == nimbus::orch::ActionKind::Reboot) restart = true;
    else if (stage) stage(va);
  };
  d.setOrchHostModel = [&head](const std::string& p, const std::string& m) {
    head = p + "/" + m;
    return true;   // deferred: written after the reply (switchHead)
  };
  std::string json, risk;
  serializeJson(action, json);
  const DeviceOutcome dev = applyDeviceAction(json, who, unattended, chat, d, &risk);
  if (dev.refusedWhole) return ToolResult::fail(tr::kControlNotApplied + dev.refusals);
  std::string out = tr::kControlApplied + dev.summary;
  if (!dev.refusals.empty()) out += "\n" + std::string(tr::kControlNotApplied) + dev.refusals;
  if (!unknown.empty()) out += "\n" + std::string(tr::kControlNotApplied) + unknown + tr::kControlKeys + controlKeys();
  out += risk + dev.replyNote;
  ToolResult r = ToolResult::ok(out);
  if (restart || !head.empty()) {
    r.output += tr::kControlAfterReply;
    r.code = restart ? plain::deferredCode(plain::Deferred::Restart)
                     : plain::deferredCode(plain::Deferred::HeadSwitch, head);
  }
  return r;
}

// Every bound tool on `reg` (the people group admin only, as on engine 1), device_control over `c`.
inline void registerDeviceFamilies(nimbus::orch::ToolRegistry& reg, const Handlers& h, const ControlDeps& c) {
  bindDeclared(reg, "device.status", h.status);
  bindDeclared(reg, "device.log", h.log);
  bindDeclared(reg, "ota.status", h.ota);
  bindDeclared(reg, "device.selftest", h.selftest);
  bindDeclared(reg, "system.health", h.health);
  bindDeclared(reg, "docs.list", h.docsList);
  bindDeclared(reg, "docs.search", h.docsSearch);
  bindDeclared(reg, "docs.read", h.docsRead);
  for (const auto& t : {std::make_pair("tenant.list", h.tenantList), std::make_pair("tenant.set_role", h.tenantSetRole),
                        std::make_pair("tenant.set_quota", h.tenantSetQuota)})
    if (bindDeclared(reg, t.first, t.second)) reg.setAdminOnly(t.first);
  bindDeclared(reg, "models.list", h.modelsList);
  if (!c.apply) return;
  bindDeclared(reg, kControlName, [c](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) {
    return deviceControl(a, who, c.unattended && c.unattended(), c.chat ? c.chat() : std::string(), c.apply());
  });
}

// A head switch device_control deferred ("provider/model"), written through `d` after the turn. False
// for an argument of another shape, a device with no writer, or a store that refused the write
// (E2-REP1: the chat is told the truth).
inline bool switchHeadWith(const std::string& providerModel, const DeviceApplyDeps& d) {
  const size_t slash = providerModel.find('/');
  if (slash == std::string::npos || slash == 0 || slash + 1 >= providerModel.size()) return false;
  return d.setOrchHostModel && d.setOrchHostModel(providerModel.substr(0, slash), providerModel.substr(slash + 1));
}

}  // namespace plaindevice
}  // namespace agent
