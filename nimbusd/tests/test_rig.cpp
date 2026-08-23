// test_rig - offline (T1) proof that the FULL daemon composition (NimbusdRig:
// the real TurnEngine + tool registry + all memory engines, wired to POSIX
// stores) builds, links, and is restart-safe end to end.
//
// No provider keys, no network: every check is deterministic. It writes memory
// through the rig, tears the rig down (== process exit), rebuilds a fresh rig
// pointed at the same data directory (== restart), and asserts the assistant's
// durable state came back and the tool surface is composed as expected.
#include <string>

#include "rig.h"
#include "test_util.h"

using namespace nimbusd;
namespace orch = nimbus::orch;

static std::vector<int8_t> oneHot(int dims, int slot) {
  std::vector<int8_t> v(dims, 0);
  v[slot % dims] = 127;
  return v;
}

static bool regHas(orch::ToolRegistry& reg, const std::string& name) {
  for (const auto& s : reg.toolSpecs())
    if (s.name == name) return true;
  return false;
}

int main() {
  ndtest::Ctx c;
  std::printf("=== nimbusd rig composition (T1, offline) ===\n");

  // Hermetic: this test must not depend on provider keys the dev machine may
  // have exported. Config reads the environment (by design, env wins), so clear
  // the provider/search keys here to exercise the no-key composition.
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY",
                        "TAVILY_API_KEY"})
    unsetenv(k);

  const std::string dataDir = ndtest::scratchDir("rig") + "/data";
  ndtest::rmTree(ndtest::scratchDir("rig"));

  Config cfg;  // no keys loaded -> no providers, embeddings inert
  NimbusdRig::Options opt;
  opt.dataDir = dataDir;
  opt.embeddings = false;   // no embedder without a key
  opt.embedDims = 64;

  // ---- 1. the composition builds, and the tool surface is what hosted expects
  {
    NimbusdRig rig(cfg, opt);
    std::printf("  -- tool surface --\n");
    c.ok(regHas(rig.registry(), "memory.write"), "memory.write is registered");
    c.ok(regHas(rig.registry(), "memory.search"), "memory.search is registered");
    c.ok(regHas(rig.registry(), "files.list"), "files.list is registered");
    c.ok(regHas(rig.registry(), "artifact.save"), "artifact.save is registered");
    c.ok(regHas(rig.registry(), "docs.search"), "docs.search is registered");
    c.ok(regHas(rig.registry(), "device.status"), "device.status is registered");
    // web.search is advertised ONLY when a Tavily key is present - none here.
    c.ok(!regHas(rig.registry(), "web.search"),
         "web.search is absent with no Tavily key (advertised == callable)");

    std::printf("  -- write durable memory through the rig --\n");
    orch::VecEntry e;
    e.id = "k1";
    e.content = "the server room door code is 7788";
    e.importance = 0.9f;
    e.ttlHours = -1;
    e.vec = oneHot(64, 5);
    c.ok(rig.vectors().add(e), "a vector was added");

    // A file artifact through the disk-backed tool.
    rig.files().seed("ops/runbook.txt", "restart the relay with: systemctl restart relay");
    c.ok(rig.files().has("ops/runbook.txt"), "a file artifact was saved");

    // An episodic message (append-log, durable per-write).
    orch::EpisodicMessage m;
    m.sessionId = "owner";
    m.role = "user";
    m.text = "note: the code rotates on the first of the month";
    m.tsHours = 1000;
    rig.episodic().addMessage(m);
    c.eqi(rig.episodic().messageCount(), 1, "one episodic message written");

    rig.flush();  // (the destructor also flushes; explicit here for clarity)
  }

  // ---- 2. RESTART: a fresh rig on the same data dir recovers everything ------
  {
    std::printf("  -- restart: rehydrate from disk --\n");
    NimbusdRig rig(cfg, opt);
    c.eqi(rig.vectors().size(), 1, "vector memory survived the restart");
    auto hits = rig.vectors().search(oneHot(64, 5), 1);
    c.ok(!hits.empty() && hits[0].content.find("7788") != std::string::npos,
         "associative recall of the persisted fact after restart");

    c.ok(rig.files().has("ops/runbook.txt"), "file artifact survived the restart");
    c.ok(rig.files().get("ops/runbook.txt").find("systemctl") != std::string::npos,
         "file contents intact after restart");

    orch::MsgQuery q;
    q.sessionId = "owner";
    q.limit = 10;
    auto rows = rig.episodic().query(q);
    c.ok(!rows.empty() && rows[0].text.find("rotates") != std::string::npos,
         "episodic history survived the restart");
  }

  ndtest::rmTree(ndtest::scratchDir("rig"));
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
