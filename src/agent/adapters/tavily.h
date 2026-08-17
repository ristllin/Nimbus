#pragma once
#include "nimbus/harness/websearch.h"

// tavily - the DEVICE shim for the web-search capability. Everything except the
// key lookup and the transport choice lives in the portable agent::websearch
// (lib/harness), which is host-tested in test/test_websearch and runs unchanged
// in tools/harness-lab.
//
// The orchestrator advertises this as the "web.search" MCP tool (memory_subsystem
// registers it when a key is set) so external MCP clients and the on-device tool
// surface can search the live web. One blocking HTTPS round-trip via the shared
// TLS arbiter (coexists with the Telegram poll socket). The key is human-set
// (agent::store::tavilyKey), never model-writable.
namespace agent {
namespace tavily {

bool available();   // a Tavily key is configured

// One search round-trip. `maxResults` is clamped to [1,10]. The Result NAMES its
// failure: callers must surface `err` rather than substituting a generic
// message, and must not read a failure as "the web had no results" - a
// genuinely empty result set comes back ok, with a digest that says so.
websearch::Result search(const std::string& query, int maxResults = 5);

}  // namespace tavily
}  // namespace agent
