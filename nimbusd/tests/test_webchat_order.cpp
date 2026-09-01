// test_webchat_order - offline (T2) proof of the mid-turn chat contract (CUM-218),
// driven over a real loopback socket through the daemon HTTP surface. The owner hit
// this live: messages POSTed to /api/chat while a turn was in flight were either
// silently dropped or answered with a STALE REPLAY of a prior turn's reply ("6").
//
// This is the CLASS test, not one point: it rapid-fires N messages with NO waiting
// between them and asserts every turn gets its OWN reply, in order, none dropped,
// none duplicated, and never a reply belonging to a prior turn. The engine runs the
// turns for real (serialized on its one thread); the delivery hook substitutes a
// distinct, ordered reply per turn ("reply#1", "reply#2", ...) so the web reply-
// matching path (chatPost -> engine mailbox -> ReplyBuffer -> chatGet by turn id) is
// exercised with replies that DIFFER - the exact thing the old single-slot code got
// wrong when they did not.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "daemon_http.h"
#include "engine_thread.h"
#include "http_control.h"
#include "reply_buffer.h"
#include "rig.h"
#include "test_util.h"

using namespace nimbusd;

static bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

struct Out { int status = 0; std::string body; };

static Out req(DaemonHttpTransport& c, int port, const std::string& method,
               const std::string& path, const std::string& tok, const std::string& body) {
  agent::HttpRequest r;
  r.method = method;
  r.host = "127.0.0.1";
  r.port = port;
  r.tls = false;
  r.path = path;
  r.timeoutMs = 8000;
  if (!tok.empty()) r.headers.push_back({"X-Nimbus-Token", tok});
  if (method != "GET") {
    r.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
    r.body = body;
  }
  agent::HttpResponse resp;
  std::string err;
  Out out;
  if (c.exec(r, resp, err)) { out.status = resp.status; out.body = resp.body; }
  return out;
}

// Pull the integer value of "key":<digits> from a JSON body ("" -> -1).
static long jsonInt(const std::string& body, const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  size_t at = body.find(needle);
  if (at == std::string::npos) return -1;
  return std::strtol(body.c_str() + at + needle.size(), nullptr, 10);
}

// Pull the string value of "reply":"..." (no escape handling needed here - the
// substituted replies are plain ASCII).
static std::string jsonStr(const std::string& body, const std::string& key) {
  const std::string needle = "\"" + key + "\":\"";
  size_t at = body.find(needle);
  if (at == std::string::npos) return "";
  size_t start = at + needle.size();
  size_t end = body.find('"', start);
  return end == std::string::npos ? "" : body.substr(start, end - start);
}

int main() {
  ndtest::Ctx c;
  std::printf("=== virtual nimbus mid-turn chat contract (T2, offline, CUM-218) ===\n");
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY", "TAVILY_API_KEY"})
    unsetenv(k);

  const std::string dataDir = ndtest::scratchDir("webchat-order") + "/data";
  ndtest::rmTree(ndtest::scratchDir("webchat-order"));

  Config cfg;
  NimbusdRig::Options opt;
  opt.dataDir = dataDir;
  opt.embeddings = false;
  opt.embedDims = 64;
  opt.devName = "VNOrder";
  NimbusdRig rig(cfg, opt);

  EngineThread eng(&rig);
  eng.start();

  const std::string token = "vn-order-token";
  ReplyBuffer replies;
  // Substitute a distinct, ordered reply per delivered turn. The engine fires the
  // hook once per turn, in completion order (its one thread runs turns FIFO), so
  // "reply#k" is the answer the k-th posted turn must receive - and no other.
  std::atomic<int> nDelivered{0};
  rig.setDeliver([&replies, &nDelivered](const std::string& chat, const std::string&) {
    replies.push("assistant", "reply#" + std::to_string(++nDelivered), chat);
  });
  HttpControl http(&eng, "127.0.0.1", 0, token, &replies, &rig);
  const int port = http.start();
  c.ok(port > 0, "control surface bound a loopback port");

  DaemonHttpTransport cl;

  // ---- rapid-fire N messages with NO waiting between them ----
  const int N = 8;
  std::vector<long> turnIds;
  bool allAccepted = true, allDistinct = true;
  for (int i = 1; i <= N; i++) {
    Out p = req(cl, port, "POST", "/api/chat", token, "text=msg" + std::to_string(i));
    if (p.status != 200 || !has(p.body, "\"pending\":true")) allAccepted = false;
    long t = jsonInt(p.body, "turn");
    for (long prev : turnIds) if (prev == t) allDistinct = false;
    turnIds.push_back(t);
  }
  c.ok(allAccepted, "every rapid POST was accepted and queued (none rejected mid-turn)");
  c.ok((int)turnIds.size() == N && allDistinct && turnIds[0] > 0,
       "each turn got its own id (no coalescing of concurrent turns)");

  // ---- each turn resolves to ITS OWN reply, in order ----
  // Poll each turn id to completion, matching by the id POST handed back. This is
  // the property the old code broke: turn k must see "reply#k", never a neighbour's.
  bool orderOk = true, noneDropped = true;
  std::vector<std::string> got(N);
  for (int i = 0; i < N; i++) {
    std::string reply;
    for (int tries = 0; tries < 80; tries++) {
      Out g = req(cl, port, "GET", "/api/chat?turn=" + std::to_string(turnIds[i]), token, "");
      if (has(g.body, "\"pending\":false")) { reply = jsonStr(g.body, "reply"); break; }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    got[i] = reply;
    if (reply.empty()) noneDropped = false;
    if (reply != "reply#" + std::to_string(i + 1)) orderOk = false;
  }
  c.ok(noneDropped, "every turn produced a reply (no silent drop)");
  c.ok(orderOk, "each turn got its OWN reply in order (no stale replay of a prior turn)");

  // ---- no duplication: the N replies are all distinct ----
  bool allUnique = true;
  for (int i = 0; i < N; i++)
    for (int j = i + 1; j < N; j++)
      if (got[i] == got[j] && !got[i].empty()) allUnique = false;
  c.ok(allUnique, "the N replies are all distinct (no reply handed to two turns)");

  // ---- idempotent pickup: re-polling a settled turn does NOT replay its reply ----
  Out again = req(cl, port, "GET", "/api/chat?turn=" + std::to_string(turnIds[0]), token, "");
  c.ok(has(again.body, "\"pending\":false") && jsonStr(again.body, "reply").empty(),
       "re-polling a delivered turn returns empty, never a replay");

  // ---- exactly N replies were delivered (nothing lost, nothing doubled) ----
  c.ok(nDelivered.load() == N, "the engine delivered exactly N replies for N turns");

  // ---- cross-channel: a Telegram reply landing mid-web-turn is NOT shown in web ----
  // Post a web turn, then inject a Telegram-channel assistant reply into the ring
  // before the web turn's own reply lands. The web bubble must show ITS OWN reply,
  // never the Telegram one (the untagged-ring bleed the channel tag closes).
  {
    Out p = req(cl, port, "POST", "/api/chat", token, "text=web-q");
    long tw = jsonInt(p.body, "turn");
    replies.push("assistant", "telegram-answer", "tg-123");   // a reply on another channel
    std::string reply;
    for (int tries = 0; tries < 80; tries++) {
      Out g = req(cl, port, "GET", "/api/chat?turn=" + std::to_string(tw), token, "");
      if (has(g.body, "\"pending\":false")) { reply = jsonStr(g.body, "reply"); break; }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    c.ok(reply != "telegram-answer" && !reply.empty(),
         "a mid-turn Telegram reply is not claimed by the web turn (channel-matched)");
  }

  http.stop();
  eng.stop();
  ndtest::rmTree(ndtest::scratchDir("webchat-order"));
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
