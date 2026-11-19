#pragma once
#include <ArduinoJson.h>

#include <map>
#include <string>
#include <vector>

#include "items.h"
#include "nimbus/harness/plain/turn.h"

// tx_battery/world.h - the tools a battery turn calls (lane RB-B0b): a fake registry with
// the classes the properties care about. Pure reads, effects (counted per call id and per
// tool plus arguments, so a re-run after a crash is seen whatever id it carries), outside
// content (taints, stubbed after its turn) and large internal reads (tier 2). The world
// outlives a power cut: what an effect did stays done.
namespace txb {

namespace plain = agent::plain;

class World {
 public:
  std::map<std::string, std::string> files;      // path -> text (read_file, write_file)
  std::map<std::string, int> effectRuns;         // "<tool> <args>" -> times run
  std::vector<std::string> dispatched;           // tool names, in order

  static bool isEffect(const std::string& n) {
    return n == "write_file" || n == "ledger_write" || n == "device_control" || n == "append_file";
  }
  // Times an effect tool ran, summed over its arguments (0 when it never ran).
  int runsOf(const std::string& tool) const {
    int n = 0;
    for (const auto& kv : effectRuns) n += kv.first.rfind(tool + " ", 0) == 0 ? kv.second : 0;
    return n;
  }

  plain::ToolOutput dispatch(const plain::ToolCall& c, const plain::TurnCtx&) {
    dispatched.push_back(c.name);
    JsonDocument a;
    deserializeJson(a, c.argsJson);
    plain::ToolOutput o;
    o.id = c.id;
    o.name = c.name;
    if (isEffect(c.name)) ++effectRuns[c.name + " " + c.argsJson];
    if (c.name == "read_file") return readFile(a, o);
    if (c.name == "write_file") {
      files[a["path"] | ""] = a["text"] | "";
      o.text = std::string("wrote ") + (a["path"] | "");
    } else if (c.name == "ledger_write") {
      o.text = std::string("ledger ") + (a["day"] | "") + ": row L00A3 added";
    } else if (c.name == "ledger_day") {
      o.text = std::string("ledger ") + (a["day"] | "") + ": L00A3 skyr 150 g; total 150 g";
    } else if (c.name == "web_search" || c.name == "fetch_url") {
      o.outside = true;
      o.text = payload(std::string("OUTSIDE-") + (a["q"] | "page"), a["bytes"] | 3000);
    } else if (c.name == "big_read") {
      o.text = payload(std::string("BULK-") + (a["tag"] | "x"), a["bytes"] | 20000);
    } else if (c.name == "device_control") {   // restart, install and a head switch run after the turn
      const std::string action = a["action"] | "";
      o.text = "scheduled " + action;
      o.deferred = action == "restart"   ? plain::Deferred::Restart
                   : action == "install" ? plain::Deferred::Install
                   : action == "head"    ? plain::Deferred::HeadSwitch
                                         : plain::Deferred::None;
      o.deferredArg = a["to"] | "";
    } else {
      o.isError = true;
      o.text = "unknown tool " + c.name;
    }
    return o;
  }

 private:
  plain::ToolOutput& readFile(const JsonDocument& a, plain::ToolOutput& o) {
    auto it = files.find(a["path"] | "");
    o.isError = it == files.end();
    o.text = o.isError ? std::string("no such file: ") + (a["path"] | "") : it->second;
    return o;
  }
};

}  // namespace txb
