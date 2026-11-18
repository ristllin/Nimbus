// Sub-agents (plain/agents.h).
// Stub bodies from lane RB-B0a: each answers Err::NotImplemented until lane RB-U2 writes them.
#include "nimbus/harness/plain/agents.h"

namespace agent {
namespace plain {

Status agentReportPath(const std::string&, const std::string&, std::string&) {
  return Status::pending("plain::agentReportPath");
}
Status installAgents(PlainHooks&, const AgentsDeps&) { return Status::pending("plain::installAgents"); }

}  // namespace plain
}  // namespace agent
