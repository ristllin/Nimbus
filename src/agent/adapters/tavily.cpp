#include "tavily.h"

#include "../../sys/agent_log.h"
#include "../../sys/ps_json.h"      // PsramJsonAllocator - keep the response off internal heap
#include "../store.h"
#include "../transport_tls.h"       // the shared device HttpTransport

namespace agent {
namespace tavily {

bool available() { return store::hasTavilyKey(); }

websearch::Result search(const std::string& query, int maxResults) {
  // Reuse the ONE device transport: TLS setup + the work-slot arbiter + the 3x
  // connect retry + a single wall-clock deadline + the streaming filter-parse.
  // Before this, web search had its own hand-rolled socket loop whose response
  // buffer was capped at 6000 bytes - under every real Tavily reply, so the JSON
  // truncated and no search ever succeeded on the device.
  websearch::Result r =
      websearch::search(deviceTransport(), std::string(store::tavilyKey().c_str()),
                        query, maxResults, 20000,
                        &agent::PsramJsonAllocator::instance());
  if (!r.ok) alogf("tavily: %.120s", r.err.c_str());
  return r;
}

}  // namespace tavily
}  // namespace agent
