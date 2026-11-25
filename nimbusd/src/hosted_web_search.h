#pragma once
#include <ArduinoJson.h>

#include <functional>

#include "nimbus/harness/plain/request.h"   // toolDecl, declSchema
#include "nimbus/harness/plain/texts.h"
#include "nimbus/harness/websearch.h"
#include "nimbus/orch/tool_registry.h"

// hosted_web_search - engine 2's web_search on nimbusd (lane E2-P-HOST1), the hosted twin of the device's
// registration in src/agent/tools_plain.cpp: the old engine's web.search handler (Tavily, else the
// provider's own search) on a private registry `inner`, reached with the engine-2 name and arguments
// ({query}). No search key now (`open` false): one plain line. Its words are catalogued as tool_results
// (tools/release_gate/gen_model_text_catalog.py), as the device file's are.
namespace nimbusd {

inline void registerHostedWebSearch(nimbus::orch::ToolRegistry& reg, nimbus::orch::ToolRegistry& inner,
                                    const agent::websearch::WebSearchToolDeps& deps, std::function<bool()> open) {
  const auto* decl = agent::plain::toolDecl("web_search");
  if (!decl || !deps.search) return;
  agent::websearch::registerWebSearchTool(inner, deps);
  reg.add(
      "web_search", decl->description,
      [&inner, open](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) {
        if (open && !open())
          return nimbus::orch::ToolResult::fail(agent::plain::texts::tool_results::kWebSearchNoKey);
        ArduinoJson::JsonDocument args;
        args["query"] = a["query"];
        return inner.dispatch("web.search", args.as<ArduinoJson::JsonObjectConst>(), who);
      },
      agent::plain::declSchema(*decl, false));
}

}  // namespace nimbusd
