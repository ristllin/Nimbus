#include "file_tools_device.h"

#include <Arduino.h>                         // millis - the per-call figures

#include "../sys/agent_log.h"                // alogf - one figures line per call
#include "files_subsystem.h"                 // toolsIo, toolsHolds - the card seam over the device's file store
#include "nimbus/harness/plain/rails.h"      // readsOutsidePath - the rails' rule for device-written folders
#include "nimbus/harness/plain/request.h"    // toolDecl, declSchema, kWriteContentMax
#include "nimbus/harness/turn_memory.h"      // PsramFirstScope
#include "nimbus/orch/file_tools.h"

// file_tools_device - engine 2's plain file tools on the device (lane RB-N2b's registration, bound by lane
// E2-TOOLS over the old engine's store). The library holds the rules and every result text;
// agent::files::toolsIo() is the card (files_subsystem.cpp). One turn task runs tool calls (no on-device
// concurrency, AGENTS.md section 4), so calls on the store never overlap and the one tools part file is
// never shared by two writes. delete_file is declared behind more (texts.h kBehindMore).
namespace agent::filetoolsdev {

namespace orch = nimbus::orch;

namespace {

// The write cap is the schema's: no device host sets RequestFacts::shortHost, so the manifest states
// the 120 s host's 12,000 B and the tools enforce exactly that (decision C).
constexpr bool kShortHost = false;

orch::FileTools& tools() {
  static orch::FileTools t(files::toolsIo(), [] {
    orch::FileToolsConfig c;
    c.contentCap = kShortHost ? agent::plain::kWriteContentMaxShort : agent::plain::kWriteContentMax;
    return c;
  }());
  return t;
}

using Method = orch::ToolResult (orch::FileTools::*)(ArduinoJson::JsonObjectConst, const orch::Principal&);

struct Bound {
  const char* name;
  Method call;
};
const Bound kTools[] = {
    {"read_file", &orch::FileTools::read},   {"write_file", &orch::FileTools::write},
    {"append_file", &orch::FileTools::append}, {"edit_file", &orch::FileTools::edit},
    {"list_files", &orch::FileTools::list},   {"delete_file", &orch::FileTools::remove},
};

// Lane E2-FSAFE: does this read bring in content the owner did not write himself? A path or list
// prefix under inbox/ or agents/ (the rails' own rule, plain::readsOutsidePath, which the engine also
// applies to the stored result), or a file of another person (a member's file the owner reads).
bool readsOutside(const char* name, ArduinoJson::JsonObjectConst args, const orch::Principal& who) {
  const std::string tool = name;
  if (tool != "read_file" && tool != "list_files") return false;
  std::string json;
  serializeJson(args, json);
  if (agent::plain::readsOutsidePath(tool, json)) return true;
  return tool == "read_file" && files::readsOthersFile(who, args["path"].as<std::string>());
}

}  // namespace

void registerFileTools(orch::ToolRegistry& reg) {
  for (const Bound& b : kTools) {
    const auto* decl = agent::plain::toolDecl(b.name);
    if (!decl) continue;
    const Method m = b.call;
    reg.add(
        b.name, decl->description,
        [m, name = b.name](ArduinoJson::JsonObjectConst args, const orch::Principal& who) {
          const PsramFirstScope psram;
          (void)files::toolsHolds(true);
          const uint32_t t0 = millis();
          orch::ToolResult r = (tools().*m)(args, who);
          if (r.success && readsOutside(name, args, who)) tools::markOutsideCall();
          const files::ToolsHolds h = files::toolsHolds(true);
          alogf("e2: tool %s ok=%d ms=%u holds=%u holdMaxUs=%u holdTotalUs=%u", name, r.success ? 1 : 0,
                (unsigned)(millis() - t0), (unsigned)h.n, (unsigned)h.maxUs, (unsigned)h.totalUs);
          return r;
        },
        agent::plain::declSchema(*decl, kShortHost));
  }
}

}  // namespace agent::filetoolsdev
