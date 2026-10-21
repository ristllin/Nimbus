// test_pending_embed - memory writes survive an embedding outage on a hosted
// instance (device parity, lane P).
//
//   (a) with the embedder refusing for billing, memory.write answers "queued"
//       (never "stored"), the vector store is unchanged and the queue file is on
//       disk; the provider's machine code reaches the classifier, so the words
//       say "out of embedding credit";
//   (b) while parked, further writes and searches make NO embeddings request;
//   (c) restart while queued: a fresh rig on the same data dir still holds it;
//   (d) recovery: one drain step stores it (the vectors are persisted first),
//       memory.search finds it, and the next turn's MEMORY RESULTS can say so;
//   (e) fresh data dir: no file, an empty queue, no drain work.
// Offline: an injected FakeHttpTransport; no network, no real keys.
#include <cstdio>
#include <string>
#include <sys/stat.h>

#include "../../test/support/fake_http.h"
#include "rig.h"
#include "test_util.h"

using namespace nimbusd;
using harness_test::Exchange;
using harness_test::FakeHttpTransport;
namespace orch = nimbus::orch;

static bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

static bool fileExists(const std::string& p) {
  struct stat st;
  return ::stat(p.c_str(), &st) == 0;
}

static void clearEnv() {
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY", "TAVILY_API_KEY",
                        "CUMULO_API_KEY", "Z_AI_TOKEN"})
    unsetenv(k);
}

static NimbusdRig::Options opts(const std::string& dataDir) {
  NimbusdRig::Options o;
  o.dataDir = dataDir;
  o.embeddings = true;
  o.embedHost = "mistral";
  o.embedModel = "mistral-embed";
  o.embedDims = 8;
  o.toolLoop = false;
  return o;
}

// An embeddings response pointing along axis `slot` (distinct facts get distinct
// directions, so they are never merged as near-duplicates).
static Exchange embedOk(int slot = 0) {
  std::string v;
  for (int i = 0; i < 8; i++) v += (i ? "," : "") + std::string(i == slot % 8 ? "0.9" : "0.05");
  Exchange e;
  e.expectHost = "api.mistral.ai";
  e.expectPathContains = "/v1/embeddings";
  e.body = "{\"data\":[{\"embedding\":[" + v + "]}]}";
  return e;
}

// The recorded billing refusal shape (OpenAI's, which Lumi met on 13 Oct; the
// classifier is provider-blind).
static Exchange embedBilling() {
  Exchange e;
  e.expectHost = "api.mistral.ai";
  e.status = 429;
  e.body = "{\"error\":{\"message\":\"You have no credits remaining.\",\"type\":\"insufficient_quota\","
           "\"code\":\"credit_balance_exhausted\"}}";
  return e;
}

int main() {
  ndtest::Ctx c;
  std::printf("=== nimbusd pending-embed queue (T1, offline) ===\n");
  clearEnv();
  setenv("MISTRAL_API_KEY", "mk_test_PENDING", 1);
  const std::string root = ndtest::scratchDir("pending");
  ndtest::rmTree(root);
  const std::string dataDir = root + "/data";
  Config cfg;

  {
    std::printf("  -- (a) a billing refusal queues the write --\n");
    FakeHttpTransport tx;
    tx.script.push_back(embedBilling());
    NimbusdRig rig(cfg, opts(dataDir), &tx);
    c.eqi((long)rig.pendingCount(), 0, "fresh data dir: an empty queue");
    const std::string w = rig.callTool("memory.write", R"({"content":"my pool locker code is 4417"})");
    c.ok(has(w, "queued memory"), "memory.write answers queued");
    c.ok(has(w, "not stored yet"), "and says it is not stored yet");
    c.ok(!has(w, "stored memory"), "never claims stored");
    c.ok(has(w, "out of embedding credit at mistral"), "the billing cause, with the provider");
    c.eqi(rig.vectors().size(), 0, "the vector store is unchanged");
    c.eqi((long)rig.pendingCount(), 1, "one entry is pending");
    c.ok(fileExists(rig.pendingPath()), "the queue is on disk before the answer");
    c.ok(rig.embedPark().parked(), "embeddings are parked");

    std::printf("  -- (b) parked: no embeddings request per write or search --\n");
    const size_t calls = tx.seen.size();
    rig.callTool("memory.write", R"({"content":"the gym opens at six"})");
    const std::string s = rig.callTool("memory.search", R"({"query":"locker code"})");
    c.eqi((long)tx.seen.size(), (long)calls, "no request while parked");
    c.ok(has(s, "4417") && has(s, "pending, not stored yet"), "keyword search finds the queued fact");
    c.eqi((long)rig.pendingCount(), 2, "two entries pending");
    c.eqi((long)rig.drainPendingOnce().step, (long)orch::EmbedDrainStep::Parked, "the drain is parked too");
    c.eqi((long)tx.seen.size(), (long)calls, "and makes no request");
  }

  {
    std::printf("  -- (c) restart while queued --\n");
    FakeHttpTransport tx;
    tx.script.push_back(embedOk(1));
    tx.script.push_back(embedOk(2));
    tx.script.push_back(embedOk(1));
    NimbusdRig rig(cfg, opts(dataDir), &tx);
    c.eqi((long)rig.pendingCount(), 2, "the queue survived the restart");
    c.ok(!rig.embedPark().parked(), "the park memo is RAM only: a restart tries again");

    std::printf("  -- (d) recovery: the drain stores it --\n");
    c.eqi((long)rig.drainPendingOnce().step, (long)orch::EmbedDrainStep::Stored, "one drain step stores");
    c.eqi(rig.vectors().size(), 1, "the store grew by one");
    c.eqi((long)rig.pendingCount(), 1, "one left");
    c.eqi((long)rig.drainPendingOnce().step, (long)orch::EmbedDrainStep::Stored, "the next step stores the other");
    c.eqi((long)rig.pendingCount(), 0, "the queue is empty");
    const std::string q = rig.callTool("memory.search", R"({"query":"locker"})");
    c.ok(has(q, "4417") && !has(q, "keyword match"), "semantic search finds it");
  }

  {
    std::printf("  -- restart after the drain: stored once, nothing pending --\n");
    FakeHttpTransport tx;
    NimbusdRig rig(cfg, opts(dataDir), &tx);
    c.eqi(rig.vectors().size(), 2, "both facts are durable");
    c.eqi((long)rig.pendingCount(), 0, "nothing pending after the restart");
    c.eqi((long)rig.drainPendingOnce().step, (long)orch::EmbedDrainStep::Idle, "no drain work");
    c.eqi((long)tx.seen.size(), 0, "and no request");
  }

  {
    std::printf("  -- (e) the embed shim (the live scenario's outage) --\n");
    const std::string d2 = root + "/data2";
    FakeHttpTransport tx;
    tx.script.push_back(embedOk());
    NimbusdRig rig(cfg, opts(d2), &tx);
    rig.setEmbedShim("HTTP 429 credit_balance_exhausted");
    const std::string w = rig.callTool("memory.write", R"({"content":"the shim fact"})");
    c.ok(has(w, "queued memory"), "the shim queues the write");
    c.eqi((long)tx.seen.size(), 0, "the shim sends nothing");
    rig.setEmbedShim("");
    c.eqi((long)rig.drainPendingOnce().step, (long)orch::EmbedDrainStep::Stored, "shim off: the drain stores");
  }

  ndtest::rmTree(root);
  clearEnv();
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
