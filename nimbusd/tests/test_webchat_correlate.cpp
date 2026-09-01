// test_webchat_correlate - the LIVE-shape residual of the mid-turn chat contract
// (CUM-218 -> CUM-293): a burst of messages stacked server-side while turns are in
// flight, where a turn's reply count is NOT the 1-per-turn the rapid-fire N=8 test
// (test_webchat_order.cpp) always feeds. The owner hit this live: a burst of three
// messages ~1s apart answered the first two, then the THIRD (the second stacked
// entry) never got a reply.
//
// This test drives the web_api reply-matching path DIRECTLY (WebApi::handle over
// POST/GET /api/chat), pushing the assistant deliveries into the reply ring by hand
// so the per-turn delivery COUNT is controlled exactly. The engine is not started -
// its queued turns never run - so the ring holds precisely the deliveries this test
// stages, reproducing the two real shapes the single-slot-per-turn / positional
// matcher gets wrong:
//   A) an in-flight turn that delivers MORE than one message (an interim line then
//      the final answer) shifts every later turn's reply forward and drops the last.
//   B) a turn that delivers NOTHING (a silent/errored turn) steals the NEXT turn's
//      reply, dropping that later turn entirely.
// The property is the class: each web turn resolves to EXACTLY its own turn's reply
// (its final delivery, or empty if it produced none), never a neighbour's, and no
// later turn is ever dropped because a neighbour's delivery count was not one.
#include <string>

#include "engine_thread.h"
#include "reply_buffer.h"
#include "rig.h"
#include "test_util.h"
#include "web_api.h"

using namespace nimbusd;

// Pull the integer value of "key":<digits> from a JSON body ("" -> -1).
static long jsonInt(const std::string& body, const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  size_t at = body.find(needle);
  if (at == std::string::npos) return -1;
  return std::strtol(body.c_str() + at + needle.size(), nullptr, 10);
}

// Pull the string value of "reply":"..." (plain-ASCII test replies, no escapes).
static std::string jsonStr(const std::string& body, const std::string& key) {
  const std::string needle = "\"" + key + "\":\"";
  size_t at = body.find(needle);
  if (at == std::string::npos) return "";
  size_t start = at + needle.size();
  size_t end = body.find('"', start);
  return end == std::string::npos ? "" : body.substr(start, end - start);
}

// POST /api/chat text=<t>, return the turn id the server handed back.
static uint64_t post(WebApi& api, const std::string& text) {
  ApiResp r;
  api.handle("POST", "/api/chat", "text=" + text, r);
  return (uint64_t)jsonInt(r.body, "turn");
}

// GET /api/chat?turn=<id>, return the reply text (empty if none yet).
static std::string get(WebApi& api, uint64_t turn) {
  ApiResp r;
  api.handle("GET", "/api/chat?turn=" + std::to_string(turn), "", r);
  return jsonStr(r.body, "reply");
}

int main() {
  ndtest::Ctx c;
  std::printf("=== virtual nimbus mid-turn chat, live burst shape (T2, CUM-293) ===\n");
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY"})
    unsetenv(k);

  const std::string dataDir = ndtest::scratchDir("webchat-corr") + "/data";
  ndtest::rmTree(ndtest::scratchDir("webchat-corr"));

  Config cfg;
  NimbusdRig::Options opt;
  opt.dataDir = dataDir;
  opt.embeddings = false;
  opt.embedDims = 64;
  opt.devName = "VNCorr";
  NimbusdRig rig(cfg, opt);
  EngineThread eng(&rig);   // deliberately NOT started: this test owns the ring
  ReplyBuffer replies;
  WebApi api(&rig, &eng, &replies);

  // The delivery the daemon's reply hook performs, tagged with the web turn it
  // belongs to (main.cpp reads eng.currentWebTurn()). The test stands in for the
  // engine, so it tags each staged delivery with the turn it answers.
  auto deliver = [&replies](const std::string& text, uint64_t turn) {
    replies.push("assistant", text, "owner", turn);
  };

  // ---- Scenario A: an in-flight turn delivers TWO messages -------------------
  // Three messages arrive stacked (all posted before any reply lands - the real
  // burst). Turn 1 answers with an interim line then its final answer (two
  // deliveries); turns 2 and 3 answer once each. Every turn must still show ITS
  // OWN reply; turn 3 (the second stacked entry) must not be lost.
  {
    const uint64_t t1 = post(api, "q1");
    const uint64_t t2 = post(api, "q2");
    const uint64_t t3 = post(api, "q3");
    c.ok(t1 && t2 > t1 && t3 > t2, "A: three stacked turns each got a distinct id");

    deliver("one moment", t1);   // turn 1, interim
    deliver("ANSWER-1", t1);     // turn 1, final
    deliver("ANSWER-2", t2);     // turn 2
    deliver("ANSWER-3", t3);     // turn 3

    const std::string r1 = get(api, t1), r2 = get(api, t2), r3 = get(api, t3);
    c.eq(r1, "ANSWER-1", "A: turn 1 shows its own final reply (not the interim, not empty)");
    c.eq(r2, "ANSWER-2", "A: turn 2 shows its own reply (not turn 1's spillover interim)");
    c.eq(r3, "ANSWER-3", "A: turn 3 (second stacked entry) shows its own reply, not dropped");
  }

  // ---- Scenario B: a stacked turn delivers NOTHING ---------------------------
  // Turn B2 is silent (an errored / no-output turn). It must resolve to an honest
  // empty, and must NOT swallow turn B3's reply. On the positional matcher B2
  // steals B3's answer and B3 is dropped - the exact "second stacked entry lost".
  {
    const uint64_t b1 = post(api, "b1");
    const uint64_t b2 = post(api, "b2");
    const uint64_t b3 = post(api, "b3");

    deliver("B-ONE", b1);        // turn B1
    // turn B2 delivers nothing at all
    deliver("B-THREE", b3);      // turn B3

    const std::string r1 = get(api, b1), r2 = get(api, b2), r3 = get(api, b3);
    c.eq(r1, "B-ONE", "B: turn B1 shows its own reply");
    c.eq(r2, "", "B: silent turn B2 resolves empty, does NOT steal B3's reply");
    c.eq(r3, "B-THREE", "B: turn B3 (behind the silent one) still shows its own reply");
  }

  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
