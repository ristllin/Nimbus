#pragma once
#include <ArduinoJson.h>

#include <functional>
#include <string>

#include "nimbus/docs_pack.h"
#include "nimbus/harness/plain/request.h"   // toolDecl, declSchema
#include "nimbus/orch/tool_registry.h"

// hosted_e2_device - the docs group and device_status on a Virtual Nimbus's engine 2 (lane E2-REP3; the
// device binds the same declarations in src/agent/tools_plain_device.h). Each is registered under its engine-1
// registry name with engine 2's declaration (plain/texts.h), so the binding offers it behind more as the
// device does:
//   - docs.search and docs.read: the instance's own engine-1 handlers (NimbusdRig::registerDocsTools, the
//     docs pack riding the daemon), called through engine 1's registry;
//   - docs.list: the pack's file index, or one file's sections (an unknown file answers the index);
//   - device.status: what is true for an instance, as JSON: its name, version line and engine, the clock and
//     time zone, the head it runs on, the instance disk, the monthly ceiling and engine 2's turn count. It
//     names no battery, Wi-Fi or card: an instance has none.
namespace nimbusd {

namespace e2device {

inline bool bind(nimbus::orch::ToolRegistry& reg, const char* name, nimbus::orch::ToolHandler h) {
  const auto* d = agent::plain::toolDecl(name);
  if (!d || !h) return false;
  reg.add(d->name, d->description, std::move(h), agent::plain::declSchema(*d, false));
  return true;
}

inline nimbus::orch::ToolResult docsList(ArduinoJson::JsonObjectConst a) {
  const std::string want = a["file"].is<const char*>() ? std::string(a["file"].as<const char*>()) : std::string();
  const nimbus::docs::DocFile* f = want.empty() ? nullptr : nimbus::docs::findFile(want);
  JsonDocument d;
  JsonArray arr = d.to<JsonArray>();
  if (f) {
    for (size_t i = f->first; i < (size_t)(f->first + f->count); i++) {
      JsonObject o = arr.add<JsonObject>();
      o["id"] = nimbus::docs::section(i).id;
      o["title"] = nimbus::docs::section(i).title;
    }
  } else {
    for (size_t i = 0; i < nimbus::docs::fileCount(); i++) {
      JsonObject o = arr.add<JsonObject>();
      o["file"] = nimbus::docs::file(i).slug;
      o["title"] = nimbus::docs::file(i).title;
      o["sections"] = nimbus::docs::file(i).count;
    }
  }
  std::string s;
  serializeJson(d, s);
  return nimbus::orch::ToolResult::ok(s);
}

}  // namespace e2device

// The docs group over engine 1's registry `e1` (null: none) and device.status over `status` (null: none).
inline void registerHostedDeviceTools(nimbus::orch::ToolRegistry& reg, const nimbus::orch::ToolRegistry* e1,
                                      std::function<std::string()> status) {
  if (e1) {
    for (const char* name : {"docs.search", "docs.read"})
      if (e1->has(name))
        e2device::bind(reg, name, [e1, n = std::string(name)](ArduinoJson::JsonObjectConst a,
                                                               const nimbus::orch::Principal& who) {
          return e1->dispatch(n, a, who);
        });
    e2device::bind(reg, "docs.list", [](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) {
      return e2device::docsList(a);
    });
  }
  if (status)
    e2device::bind(reg, "device.status", [status](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal&) {
      return nimbus::orch::ToolResult::ok(status());
    });
}

}  // namespace nimbusd
