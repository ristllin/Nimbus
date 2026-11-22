// tools_plain.cpp - the plain engine's (engine 2) tool families on the device's shared registry.
// An engine-2 image links this file instead of tools_e1.cpp (tools/engine/engine1_only.txt);
// memory::begin and model_refresh::begin call these hooks where the engine-1 families register.
// Every other old-engine registration sits inside the engine-1 guard
// (tools/engine/list_tool_registrations.py --check). The engine-2 binding lanes register their own
// tools here or in their *_device.cpp files.
#include "engine_tools.h"
#include "adapters/tavily.h"                // tavily::available, tavily::search - the old engine's search
#include "file_tools_device.h"              // lanes RB-N2b, E2-TOOLS: the plain file tools
#include "nimbus/harness/plain/request.h"   // toolDecl, declSchema
#include "nimbus/harness/plain/texts.h"     // tool_results::kWebSearchNoKey
#include "nimbus/harness/providers.h"       // hostedWebSearch(Available) - the provider's own search
#include "nimbus/harness/turn_memory.h"     // PsramFirstScope
#include "nimbus/harness/websearch.h"       // registerWebSearchTool - the old engine's web.search handler
#include "nimbus/orch/calc_tool.h"          // registerCalcTool (RB-REC8)
#include "nimbus/orch/tool_groups.h"        // registerMore - delete_file is declared behind more
#include "notes_device.h"                   // lane RB-N1: note
#include "orchestrator.h"                   // turnInFlight, turnCount - the note tool's origin (lane E2-DEV)
#include "store.h"                          // providerOverBudget, recordProviderCall
#include "transport_tls.h"                  // deviceProviderDeps

namespace agent {
namespace tools {

namespace {

namespace orch = nimbus::orch;

constexpr char kOldSearch[] = "web.search";   // the old engine's handler, reached on a private registry
constexpr char kQuery[] = "query";
constexpr char kTavily[] = "tavily";          // the Tavily budget's provider slug

// turnCount() + 1 of the turn whose web_search ran (tg_poll only): that turn read outside content.
uint32_t s_outsideTurn = 0;

bool outsideThisTurn() { return orchestrator::turnInFlight() && s_outsideTurn == orchestrator::turnCount() + 1; }

bool searchKey() {
  return agent::tavily::available() || agent::providers::hostedWebSearchAvailable(agent::deviceProviderDeps());
}

// web_search (lane E2-TOOLS): the old engine's web.search handler, unchanged (Tavily, else the provider's
// own hosted search; its budget, spend and result lines), on a private registry, reached with the
// engine-2 name and arguments ({query}). No search key: one plain line.
void registerWebSearch(orch::ToolRegistry& reg) {
  const auto* decl = agent::plain::toolDecl("web_search");
  if (!decl) return;
  static orch::ToolRegistry inner;
  agent::websearch::WebSearchToolDeps wd;
  wd.search = [](const std::string& q, int k) {
    if (agent::tavily::available() && !agent::store::providerOverBudget(kTavily)) return agent::tavily::search(q, k);
    return agent::providers::hostedWebSearch(agent::deviceProviderDeps(), q, k);
  };
  wd.overBudget = [] {
    return agent::store::providerOverBudget(kTavily) &&
           !agent::providers::hostedWebSearchAvailable(agent::deviceProviderDeps());
  };
  wd.recordCall = [] { agent::store::recordProviderCall(kTavily); };
  agent::websearch::registerWebSearchTool(inner, wd);
  reg.add(
      "web_search", decl->description,
      [](ArduinoJson::JsonObjectConst a, const orch::Principal& who) {
        if (!searchKey()) return orch::ToolResult::fail(agent::plain::texts::tool_results::kWebSearchNoKey);
        const PsramFirstScope psram;
        ArduinoJson::JsonDocument args;
        args[kQuery] = a[kQuery];
        const orch::ToolResult r = inner.dispatch(kOldSearch, args.as<ArduinoJson::JsonObjectConst>(), who);
        if (orchestrator::turnInFlight()) s_outsideTurn = orchestrator::turnCount() + 1;
        return r;
      },
      agent::plain::declSchema(*decl, false));
}

}  // namespace

// note (lane RB-N1, notes_device.cpp). Lane E2-DEV binds its origin: on engine 2 only a person's
// message runs a turn (no routine, agent or resume is bound yet), so a call inside a turn is attended,
// unless that turn ran web_search (outside content: tainted, kept for approval); outside a turn it is
// kept for approval. Lane E2-TOOLS: the file tools over the old engine's store, delete_file behind more,
// calc, web_search.
void registerMemoryFamilies(nimbus::orch::ToolRegistry& reg, const nimbus::orch::MemoryContext&) {
  notesdev::registerNote(reg, [] {
    if (!orchestrator::turnInFlight()) return nimbus::orch::notes::Origin::Unattended;
    return outsideThisTurn() ? nimbus::orch::notes::Origin::Tainted : nimbus::orch::notes::Origin::Attended;
  });
  filetoolsdev::registerFileTools(reg);
  nimbus::orch::registerCalcTool(reg);
  registerWebSearch(reg);
  nimbus::orch::registerMore(reg);
}

void registerModelsFamily(nimbus::orch::ToolRegistry&, const nimbus::orch::ModelsToolHandlers&) {}

}  // namespace tools
}  // namespace agent
