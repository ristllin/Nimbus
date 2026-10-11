// test_tool_hints - CUM-538 on a hosted instance: a head that calls a name which
// is not a tool gets the same way back the device gives it.
//
// The registry's recovery text has three cases (lib/core tool_registry.h); the two
// that need live facts read them through hooks the device installs and nimbusd
// did not, so on a Virtual Nimbus a connector name fell through to the bare
// "unknown tool" list and a spawn field never said how many sub-agents may start.
// The class rules, over the instance's own connector registry and job queue:
//   (a) an enabled configured connector (by name, type or Mistral id) is named as a
//       connector of its provider's sub-agent jobs, with the spawn to use instead;
//   (b) a disabled connector is not offered as a way back;
//   (c) a spawn-type reply field carries the real remaining spawn room;
//   (d) anything else still lists the callable tools.
// Offline: no provider traffic.
#include <cstdio>
#include <string>

#include "nimbus/orch/caps.h"
#include "rig.h"
#include "test_util.h"

using namespace nimbusd;

static bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

static NimbusdRig::Options opts(const std::string& tag) {
  NimbusdRig::Options o;
  o.dataDir = ndtest::scratchDir(tag) + "/data";
  ndtest::rmTree(ndtest::scratchDir(tag));
  o.embeddings = false;
  o.embedDims = 64;
  return o;
}

int main() {
  ndtest::Ctx c;
  std::printf("=== unknown-tool recovery on a hosted instance (CUM-538, offline) ===\n");
  Config cfg;
  NimbusdRig rig(cfg, opts("hints"));
  c.eq(rig.connectors().replaceBlob(
           "[{\"name\":\"gcal\",\"type\":\"gcal\",\"prov\":\"mistral\",\"kind\":\"connector\","
           "\"cid\":\"connector_googlecalendar\",\"en\":1},"
           "{\"name\":\"notion\",\"type\":\"notion\",\"prov\":\"mistral\",\"kind\":\"connector\",\"cid\":\"notion\",\"en\":0}]"),
       "", "two Studio connectors are configured (notion disabled)");

  std::printf("  -- (a) an enabled connector names its provider's sub-agent jobs --\n");
  for (const char* name : {"gcal", "connector_googlecalendar"}) {
    const std::string out = rig.callTool(name, "{}");
    c.ok(has(out, std::string(name) + " is a connector attached to Mistral sub-agent jobs, not a tool"),
         std::string(name) + ": named as a connector of Mistral sub-agents");
    c.ok(has(out, "\\\"provider\\\":\\\"mistral\\\""), std::string(name) + ": with the spawn to use instead");
    c.ok(has(out, "You can start up to "), std::string(name) + ": and the spawn room");
  }

  std::printf("  -- (b) a disabled connector is not offered --\n");
  c.ok(has(rig.callTool("notion", "{}"), "unknown tool: notion"), "a disabled connector falls to the tool list");

  std::printf("  -- (c) a spawn field carries the real spawn room --\n");
  const std::string room = "You can start up to " + std::to_string(nimbus::orch::spawnRoomFor(0)) + " sub-agents";
  c.ok(has(rig.callTool("session_ops", "{}"), room), "session_ops says how many sub-agents may start");

  std::printf("  -- (d) anything else lists the callable tools --\n");
  const std::string other = rig.callTool("no_such_tool", "{}");
  c.ok(has(other, "unknown tool: no_such_tool") && has(other, "Callable tools:"), "the bounded tool list");

  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
