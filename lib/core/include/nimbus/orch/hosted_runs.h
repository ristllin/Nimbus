#pragma once
// hosted_runs - the provider-hosted tools a turn really ran (CUM-835).
//
// A head request may carry tools that run on the provider's side within the one
// response (an OpenAI hosted MCP connector or web search, a Mistral Studio connector),
// and a sub-agent's connectors run on lab compute: the device never dispatches either
// and sees only the provider's record of each run. The claim check lets a reply's
// "sent", "changed", "checked" or website claim stand unchecked only when such a tool
// ran this turn; it used to assume so whenever any connector was enabled, so on a
// device with one connector these classes went unchecked on every turn.
//
// Each head adapter calls noteHostedRun for every server-side tool run its response
// lists; the claim check resets the record when a turn begins and reads it at its
// checks. One record per thread: a turn runs on one task (one engine thread on the
// hosted instance). A sub-agent result's runs come from its provenance record
// (runsInRecord), never from its text. Portable, Arduino-free, host-tested
// (test_orch_claims).

#include <cstdint>
#include <string>

namespace nimbus {
namespace orch {

struct HostedRuns {
  uint16_t total = 0;       // every server-side tool run
  uint16_t web = 0;         // ...that searched or fetched the web
  uint16_t connector = 0;   // ...that ran a connector (mail, notes, calendar, files):
                            // not the web, not a provider built-in like code_interpreter
  bool any() const { return total > 0; }
  void add(const HostedRuns& o);
};

// A tool name that searches or fetches the web: one holding web_search, web_fetch or
// news_search (web_search_preview, a connector-prefixed spelling).
bool isWebToolName(const std::string& name);

// One run of `toolName` on `into` (web, connector or neither; total always).
void countRun(const std::string& toolName, uint16_t runs, HostedRuns& into);

// This thread's head turn record.
void noteHostedRun(const std::string& toolName);
HostedRuns hostedRuns();
void resetHostedRuns();

// The runs a sub-agent result's provenance record lists: the " [ran: gmail x1,
// web_search x2]" names with a count above zero, else the " [tools: n, web: m]" counts
// (a run with no name reads as a connector: precision first, as the exemption is).
HostedRuns runsInRecord(const std::string& record);

}  // namespace orch
}  // namespace nimbus
