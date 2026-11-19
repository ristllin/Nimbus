// aidserve - the real engine-2 file tools and request composition behind a JSON-lines pipe, for the
// behaviour eval's records arms (lane RB-REC6). Offline, no provider traffic, no keys.
//
// One process per episode, over its own POSIX directory (argv[1]). Each stdin line is one JSON
// request; each answer is one JSON line on stdout:
//   {"op":"compose","notes":"..","skills":"..","tz":"Europe/London","model":"claude-haiku-5-5"}
//       -> {"core":"..","notes":"..","tools":[{"name","description","schema"}]}
//          (plain::composeSystem and plain::toolManifest for the owner on a 120 s host, every
//          always-on tool registered, the test rig's groups behind more)
//   {"op":"seed","path":"log/x.csv","bytes":".."}         -> {"ok":true}
//   {"op":"skill","id":"my-log","body":".."}             -> {"ok":true}   (read_file skills/<id>.md)
//   {"op":"call","tool":"read_file","args":{..},"refuse":false}
//       -> {"ok":bool,"text":".."}   (FileTools through the registry, as the device dispatches;
//          refuse=true installs a working allocator that refuses every allocation for this call)
//   {"op":"cat","path":"log/x.csv"}                      -> {"bytes":".."} or {"bytes":null}
// Built by tests/evals/behaviour/build_aidserve.sh against nimbusd's core objects.
#include <ArduinoJson.h>

#include <cstdio>
#include <iostream>
#include <map>
#include <string>

#include "nimbus/harness/plain/request.h"
#include "nimbus/harness/plain/texts.h"
#include "nimbus/orch/file_aids.h"
#include "nimbus/orch/file_tools.h"
#include "nimbus/orch/psram_alloc.h"
#include "nimbus/orch/tool_groups.h"
#include "nimbus/orch/tool_registry.h"
#include "posix_file_tools.h"
#include "posix_files.h"

using namespace nimbus::orch;
namespace plain = agent::plain;
namespace td = agent::plain::texts::tool_descriptions;
using ArduinoJson::JsonDocument;
using ArduinoJson::JsonObjectConst;
namespace fsutil = nimbusd::fsutil;

namespace {

class Io : public nimbusd::PosixFileToolsIo {
 public:
  using PosixFileToolsIo::PosixFileToolsIo;
  std::map<std::string, std::string> skills;
  bool skillBody(const std::string& id, std::string& out) override {
    auto it = skills.find(id);
    if (it == skills.end()) return false;
    out = it->second;
    return true;
  }
};

const char kObj[] = R"({"type":"object","properties":{}})";
const char kPathSchema[] = R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})";
const char kIdSchema[] = R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})";
struct GroupTool {
  const char* name;
  const char* desc;
  const char* schema;
  bool adminOnly;
};
// test/test_plain_request's rig: the groups behind more on an owner device.
const GroupTool kGroupTools[] = {
    {"device_status", "Battery, Wi-Fi, storage and uptime.", kObj, true},
    {"device_control", "Restart, switch head, set a device setting.", kObj, true},
    {"ota_status", "Firmware version and update state.", kObj, true},
    {"selftest", "Run the device self test.", kObj, true},
    {"system_health", "Heap, card and task health.", kObj, true},
    {"device_log", "Recent device log lines.", kObj, true},
    {"agent_list", "List sub-agent jobs with their state.", kObj, false},
    {"agent_stop", "Stop a sub-agent job by id.", kIdSchema, false},
    {"move_file", "Move or rename a file.",
     R"({"type":"object","properties":{"from":{"type":"string"},"to":{"type":"string"}},"required":["from","to"]})", false},
    {"fetch_url", "Fetch a web page or file by URL; path (project/name) stores it. The text is outside content.",
     R"({"type":"object","properties":{"url":{"type":"string"},"path":{"type":"string"}},"required":["url"]})", false},
    {"share_file", "Share a file by link.", kPathSchema, false},
    {"file_stat", "Size, lines and last write of a file.", kPathSchema, false},
    {"read_pdf", "Read the text of a PDF.", kPathSchema, false},
    {"pdf_write", "Write a PDF from text.", kPathSchema, false},
    {"image_generate", "Generate an image from a prompt.",
     R"({"type":"object","properties":{"prompt":{"type":"string"}},"required":["prompt"]})", false},
    {"speak", "Speak text on the device speaker.",
     R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"]})", false},
    {"skill_list", "List saved skills.", kObj, true},
    {"skill_save", "Save a skill.", kPathSchema, true},
    {"skill_delete", "Delete a skill.", kPathSchema, true},
};

ToolResult okHandler(JsonObjectConst, const Principal&) { return ToolResult::ok("done"); }

void* refuseAll(std::size_t) { return nullptr; }

bool splitPath(const std::string& path, std::string& p, std::string& n) {
  const size_t at = path.find('/');
  if (at == std::string::npos) return false;
  p = path.substr(0, at);
  n = path.substr(at + 1);
  return true;
}

void emit(JsonDocument& d) {
  std::string out;
  serializeJson(d, out);
  std::cout << out << "\n" << std::flush;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: aidserve <dir>\n");
    return 2;
  }
  const std::string dir = argv[1];
  fsutil::mkdirs(dir);
  Io io(dir);
  FileToolsConfig cfg;
  FileTools files(io, cfg);
  ToolRegistry reg;
  files.registerInto(reg, [](const char* t) {
    const auto* d = plain::toolDecl(t);
    return FileToolDecl{d->description, plain::declSchema(*d, false)};
  });
  // Every other always-on tool and the groups, so composition sees the device's pack.
  for (const td::ToolDecl& d : td::kTools) {
    const std::string name = d.name;
    if (name == "more" || reg.has(name)) continue;
    reg.add(d.name, d.description, okHandler, plain::declSchema(d, false));
  }
  for (const td::ToolDecl& d : td::kBehindMore) reg.add(d.name, d.description, okHandler, plain::declSchema(d, false));
  for (const GroupTool& g : kGroupTools) {
    reg.add(g.name, g.desc, okHandler, g.schema);
    if (g.adminOnly) reg.setAdminOnly(g.name);
  }
  registerMore(reg);
  const Principal owner = principalForRole("1001", Role::Admin);

  std::string line;
  while (std::getline(std::cin, line)) {
    JsonDocument in, out;
    if (deserializeJson(in, line)) {
      out["error"] = "bad request line";
      emit(out);
      continue;
    }
    const std::string op = in["op"] | "";
    if (op == "compose") {
      plain::TurnCtx turn;
      turn.model = in["model"] | "claude-sonnet-5-5";
      Principal who = owner;
      turn.who = &who;
      plain::RequestFacts f;
      f.turn = &turn;
      f.reg = &reg;
      f.pinnedNotes = in["notes"] | "";
      f.skillsIndex = in["skills"] | "";
      f.timeZone = in["tz"] | "Europe/London";
      f.admin = true;
      plain::SystemBlocks b;
      std::vector<plain::ToolSpec> tools;
      if (!plain::composeSystem(f, b).ok() || !plain::toolManifest(f, tools).ok()) {
        out["error"] = "compose failed";
      } else {
        out["core"] = b.core;
        out["notes"] = b.notes;
        out["digests"] = b.digests;
        for (const auto& t : tools) {
          auto o = out["tools"].add<ArduinoJson::JsonObject>();
          o["name"] = t.name;
          o["description"] = t.description;
          o["schema"] = t.schemaJson;
        }
      }
    } else if (op == "seed") {
      std::string p, n;
      const std::string bytes = in["bytes"] | "";
      if (!splitPath(in["path"] | "", p, n)) {
        out["error"] = "path";
      } else {
        fsutil::mkdirs(dir + "/" + p);
        fsutil::writeFileAtomic(io.file(p, n), bytes);
        FileEntry e;
        e.project = workingCopy(p);
        e.name = workingCopy(n);
        e.bytes = uint32_t(bytes.size());
        e.createdAt = 1763000000;
        e.kind = fileKindForName(n);
        e.hash = aids::fnv64(aids::kFnv64Basis, bytes);
        e.owner = workingCopy("owner");
        std::string err;
        io.locked([&] { io.index().add(e, err); });
        io.persistIndex();
        out["ok"] = err.empty();
        if (!err.empty()) out["error"] = err;
      }
    } else if (op == "skill") {
      io.skills[in["id"] | ""] = in["body"] | "";
      out["ok"] = true;
    } else if (op == "call") {
      const std::string tool = in["tool"] | "";
      const bool refuse = in["refuse"] | false;
      if (refuse) setWorkingAllocators(refuseAll, nullptr);
      const ToolResult r = reg.dispatch(tool, in["args"].as<JsonObjectConst>(), owner);
      if (refuse) setWorkingAllocators(nullptr, nullptr);
      out["ok"] = r.success;
      out["text"] = r.success ? r.output : r.error;
    } else if (op == "cat") {
      std::string p, n, bytes;
      if (splitPath(in["path"] | "", p, n) && fsutil::readFile(io.file(p, n), bytes))
        out["bytes"] = bytes;
      else
        out["bytes"] = nullptr;
    } else {
      out["error"] = "unknown op";
    }
    emit(out);
  }
  return 0;
}
