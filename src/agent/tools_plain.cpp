// tools_plain.cpp - the plain engine's (engine 2) tool families on the device's shared registry.
// An engine-2 image links this file instead of tools_e1.cpp (tools/engine/engine1_only.txt);
// memory::begin and model_refresh::begin call these hooks where the engine-1 families register.
// Every other old-engine registration sits inside the engine-1 guard
// (tools/engine/list_tool_registrations.py --check). The engine-2 binding lanes register their own
// tools here or in their *_device.cpp files.
#include "engine_tools.h"
#include "adapters/tavily.h"                // tavily::available, tavily::search - the old engine's search
#include "file_tools_device.h"              // lanes RB-N2b, E2-TOOLS: the plain file tools
#include "history_tools_device.h"           // lane E2-SRCH: search and view
#include "nimbus/harness/plain/request.h"   // toolDecl, declSchema
#include "nimbus/harness/plain/texts.h"     // tool_results::kWebSearchNoKey
#include "nimbus/harness/providers.h"       // hostedWebSearch(Available) - the provider's own search
#include "nimbus/harness/turn_memory.h"     // PsramFirstScope
#include "nimbus/harness/websearch.h"       // registerWebSearchTool - the old engine's web.search handler
#include "nimbus/orch/calc_tool.h"          // registerCalcTool (RB-REC8)
#include "nimbus/orch/tool_groups.h"        // registerMore - delete_file is declared behind more
#include "notes_device.h"                   // lane RB-N1: note
#include "orchestrator.h"                   // turnInFlight, turnCount - the note tool's origin (lane E2-DEV)
#include "schedule_device.h"                // lane RB-U1b, E2-SCHED: schedule
#include "tools_plain_skills.h"             // lane E2-P-SKILL: skill_list, skill_save, skill_delete
#include "store.h"                          // providerOverBudget, recordProviderCall
#include "tools_plain_send.h"               // lane E2-P-OUT1: send and speak
#include "tools_plain_files.h"              // lane E2-P-OUT2: the files_admin and media tools
#include "transport_tls.h"                  // deviceProviderDeps

namespace agent {
namespace tools {

namespace {

namespace orch = nimbus::orch;

constexpr char kOldSearch[] = "web.search";   // the old engine's handler, reached on a private registry
constexpr char kQuery[] = "query";
constexpr char kTavily[] = "tavily";          // the Tavily budget's provider slug

// turnCount() + 1 of the turn that read outside content (tg_poll only), and the mark of the call that
// read it (lane E2-FSAFE: web_search, and the file reads file_tools_device.cpp marks).
uint32_t s_outsideTurn = 0;
bool s_outsideCall = false;

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
        markOutsideCall();   // marked even on its own refusal (conservative, as E2-TOOLS chose)
        if (!searchKey()) return orch::ToolResult::fail(agent::plain::texts::tool_results::kWebSearchNoKey);
        const PsramFirstScope psram;
        ArduinoJson::JsonDocument args;
        args[kQuery] = a[kQuery];
        return inner.dispatch(kOldSearch, args.as<ArduinoJson::JsonObjectConst>(), who);
      },
      agent::plain::declSchema(*decl, false));
}

// The origin of a call: attended inside a person's turn, tainted when that turn ran web_search,
// unattended outside a turn.
nimbus::orch::notes::Origin turnOrigin() {
  if (!orchestrator::turnInFlight()) return nimbus::orch::notes::Origin::Unattended;
  return outsideThisTurn() ? nimbus::orch::notes::Origin::Tainted : nimbus::orch::notes::Origin::Attended;
}

// The chat of the running turn (where an entry it makes delivers); "" outside a turn (the owner default).
std::string turnChat() {
  if (!orchestrator::turnInFlight()) return std::string();
  const String c = orchestrator::currentChat();
  return std::string(c.c_str(), c.length());
}

}  // namespace

void markOutsideCall() {
  s_outsideCall = true;
  if (orchestrator::turnInFlight()) s_outsideTurn = orchestrator::turnCount() + 1;
}

bool takeOutsideCall() {
  const bool was = s_outsideCall;
  s_outsideCall = false;
  return was;
}

// note (lane RB-N1, notes_device.cpp). Lane E2-DEV binds its origin: on engine 2 only a person's
// message runs a turn (no routine, agent or resume is bound yet), so a call inside a turn is attended,
// unless that turn read outside content (web_search, a file from inbox/, agents/ or another person: tainted,
// kept for approval; the rails send such a turn's note to the pending shelf first); outside a turn it is
// kept for approval. Lane E2-TOOLS: the file tools over the old engine's store, delete_file behind more,
// calc, web_search.
// schedule (lane RB-U1b, schedule_device.cpp; lane E2-SCHED binds it): the same origin as the note tool,
// and an entry delivers to the chat of the turn that made it.
void registerMemoryFamilies(nimbus::orch::ToolRegistry& reg, const nimbus::orch::MemoryContext& ctx) {
  notesdev::registerNote(reg, turnOrigin);
  scheduledev::registerSchedule(reg, turnOrigin, turnChat);
  filetoolsdev::registerFileTools(reg);
  nimbus::orch::registerCalcTool(reg);
  registerWebSearch(reg);
  historydev::registerHistory(reg, ctx);   // E2-SRCH: answered through the turn; E2-P-MEM: scope memories reads ctx
  registerSend(reg);                         // E2-P-OUT1: send and speak (tools_plain_send.cpp)
  plainfiles::registerDevice(reg);         // E2-P-OUT2: image, PDF, share, stat, fetch, music (behind more)
  skillsplain::registerSkills(reg, turnOrigin);   // E2-P-SKILL: the skills group behind more
  nimbus::orch::registerMore(reg);
}

void registerModelsFamily(nimbus::orch::ToolRegistry&, const nimbus::orch::ModelsToolHandlers&) {}

}  // namespace tools
}  // namespace agent
