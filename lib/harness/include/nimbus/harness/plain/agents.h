#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/harness/plain/hooks.h"
#include "nimbus/harness/plain/turn.h"

// plain/agents.h - sub-agents as tools per backend (PRD section 6) over the kept jobs.cpp.
// Briefs carry task text and file paths, never the owner's transcript. Arrival: the
// report is saved whole as agents/<id>-<name>.md, one h item carries it as an outside
// bulk result, then one head turn runs in which ledger, note, schedule and device writes
// are refused. Registered in HookSlot::Agents.
//
// Seam declared by lane RB-B0a; the body (plain_agents.cpp) is lane RB-U2's.
namespace agent {
namespace plain {

struct AgentBrief {
  std::string name;
  std::string task;
  std::vector<std::string> files;   // up to 4 paths
  std::string model;                // "" = the backend's default
};

struct AgentsDeps {
  std::string backend;              // the sub-agent backend host
};

// The report path for agent `id` named `name`: "agents/<id>-<name>.md".
Status agentReportPath(const std::string& id, const std::string& name, std::string& out);
Status installAgents(PlainHooks& hooks, const AgentsDeps& deps);

}  // namespace plain
}  // namespace agent
