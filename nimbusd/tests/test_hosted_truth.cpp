// test_hosted_truth - the truth check on a Virtual Nimbus (lane RB-G0):
//   (a) the probe goes out on the instance's transport: Anthropic Messages, the head host's key,
//       claude-haiku-5-5, the measured prompt, one text message, an 8 s bound, never an image;
//   (b) the probe sees the chat's own ring and never another chat's;
//   (c) a true statement about an effect in the ring is delivered as written; an unbacked
//       success is held once and the retry's reply is delivered unchanged;
//   (d) the probe fails open past 8 s and on a transport failure, each counted;
//   (e) no key, a head without a passing measured row, or a tainted turn: no request at all,
//       and a failed write still gets its receipt line;
//   (f) the modes and counters live in <data>/truth_state.json, survive a restart, and a
//       damaged file starts again from the table.
// Offline: a scripted transport, no provider traffic.
#include <sys/stat.h>

#include <ArduinoJson.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../../test/support/fake_http.h"
#include "hosted_effects.h"
#include "hosted_truth.h"
#include "test_util.h"

using namespace nimbusd;
namespace truth = nimbus::orch::truth;
using harness_test::Exchange;
using harness_test::FakeHttpTransport;

namespace {

constexpr int64_t kNow = 1763470920;   // Tue 18 Nov 2025 13:02 UTC

std::string freshDir(const std::string& tag) {
  ndtest::rmTree(ndtest::scratchDir(tag));
  return ndtest::scratchDir(tag) + "/data";
}

Exchange answer(bool yes, uint32_t takesMs = 600) {
  Exchange e;
  e.expectHost = "api.anthropic.com";
  e.expectPathContains = "/v1/messages";
  e.body = std::string("{\"content\":[{\"type\":\"text\",\"text\":\"{\\\"claims_unbacked_action\\\": ") +
           (yes ? "true" : "false") + "}\"}]}";
  e.takesMs = takesMs;
  return e;
}

struct Rig {
  std::string dir;
  FakeHttpTransport http;
  uint32_t clock = 0;
  std::string key = "test-key-not-real";
  HostedEffects effects;
  HostedTruth truth;
  explicit Rig(const std::string& d)
      : dir(d), effects(d), truth(d, deps(), &effects) {
    http.clock = &clock;
  }
  HostedTruth::Deps deps() {
    HostedTruth::Deps x;
    x.http = &http;
    x.key = [this](const std::string& host) { return host == "anthropic" ? key : std::string(); };
    x.nowMs = [this]() { return clock; };
    return x;
  }
};

HostedTruth::Input input(const std::string& chat, const std::string& reply) {
  HostedTruth::Input in;
  in.chatId = chat;
  in.host = "anthropic";
  in.model = "claude-sonnet-5-5";
  in.owner = "Did you log my skyr?";
  in.reply = reply;
  in.nowEpoch = kNow;
  return in;
}

std::string userText(const agent::HttpRequest& r) {
  JsonDocument d;
  if (deserializeJson(d, r.body)) return "";
  return d["messages"][0]["content"].as<std::string>();
}

std::string header(const agent::HttpRequest& r, const std::string& name) {
  for (const auto& h : r.headers)
    if (h.first == name) return h.second;
  return "";
}

truth::RetryFn retryWith(const std::string& text, std::vector<std::string>* items) {
  return [text, items](const std::string& item, std::string& reply, std::vector<truth::TurnEffect>&) {
    items->push_back(item);
    reply = text;
    return true;
  };
}

bool exists(const std::string& p) {
  struct stat st;
  return ::stat(p.c_str(), &st) == 0;
}

void testTheRequest(ndtest::Ctx& c) {
  std::printf("\n-- the request --\n");
  Rig rig(freshDir("truth-a"));
  rig.http.script = {answer(false)};
  std::vector<std::string> items;
  const truth::Outcome o = rig.truth.check(input("111", "Nothing logged today yet."), retryWith("x", &items));
  c.eqi((long)rig.http.seen.size(), 1, "one probe request");
  const agent::HttpRequest& r = rig.http.seen[0];
  c.eq(r.method, "POST", "POST");
  c.eq(r.host, "api.anthropic.com", "to Anthropic");
  c.eq(r.path, "/v1/messages", "the Messages wire");
  c.eqi((long)r.timeoutMs, 8000, "bounded at 8 s");
  c.eq(header(r, "x-api-key"), rig.key, "with the head host's key");
  c.eq(header(r, "anthropic-version"), "2023-06-01", "the API version");
  JsonDocument d;
  c.ok(!deserializeJson(d, r.body), "the body is JSON");
  c.eq(d["model"] | "", "claude-haiku-5-5", "the measured probe model");
  c.eq(d["system"] | "", std::string(truth::kProbePrompt), "the measured prompt");
  c.ok(d["messages"][0]["content"].is<const char*>(), "one text message, no content blocks");
  c.ok(r.body.find("\"image\"") == std::string::npos && r.body.find("base64") == std::string::npos, "no image");
  c.ok(r.splices.empty() && rig.http.spliced[0].empty(), "no spliced bytes");
  c.ok(!o.held && o.reply == "Nothing logged today yet." && o.status.empty(), "a no delivers the reply as written");
  c.ok(items.empty(), "no retry");
  ndtest::rmTree(ndtest::scratchDir("truth-a"));
}

void testTheChatsOwnRing(ndtest::Ctx& c) {
  std::printf("\n-- the chat's own ring --\n");
  Rig rig(freshDir("truth-b"));
  rig.effects.record("111", kNow - 3600, "ledger_write", "added L00A3 breakfast: skyr 150 g");
  rig.effects.record("222", kNow - 600, "files_write", "saved secret/plan.txt (90 bytes)");
  rig.http.script = {answer(false)};
  std::vector<std::string> items;
  const truth::Outcome o =
      rig.truth.check(input("111", "Yes: skyr 150 g is logged for breakfast at 12:02."), retryWith("x", &items));
  const std::string u = userText(rig.http.seen[0]);
  c.ok(u.find("- 12:02 ledger_write: added L00A3 breakfast: skyr 150 g") != std::string::npos,
       "the chat's ring entry reaches the probe with its local time");
  c.ok(u.find("secret/plan.txt") == std::string::npos, "another chat's entry never does");
  c.ok(!o.held && o.reply.find("skyr 150 g is logged") != std::string::npos, "the true statement is delivered");
  ndtest::rmTree(ndtest::scratchDir("truth-b"));
}

void testTheHold(ndtest::Ctx& c) {
  std::printf("\n-- the hold --\n");
  Rig rig(freshDir("truth-c"));
  rig.http.script = {answer(true)};
  std::vector<std::string> items;
  const std::string retry = "I have not logged it: nothing was written. Shall I add it now?";
  const truth::Outcome o = rig.truth.check(input("111", "Logged: 150 g skyr."), retryWith(retry, &items));
  c.ok(o.held && o.retried, "held once");
  c.eqi((long)items.size(), 1, "one facts-only round");
  c.eq(items.empty() ? "" : items[0], "[device] effects this turn: none. Recent: none.", "the facts item");
  c.eq(o.reply, retry, "the retry's reply, unchanged");
  c.ok(o.status.empty(), "no device line");
  c.eqi((long)rig.http.seen.size(), 1, "no second probe without a failed write");
  ndtest::rmTree(ndtest::scratchDir("truth-c"));
}

void testFailOpen(ndtest::Ctx& c) {
  std::printf("\n-- fail-open --\n");
  Rig rig(freshDir("truth-d"));
  rig.http.script = {answer(true, 8500), harness_test::transportCut(9000), harness_test::transportCut(20)};
  std::vector<std::string> items;
  truth::Outcome o = rig.truth.check(input("111", "Logged."), retryWith("x", &items));
  c.ok(!o.held && o.reply == "Logged.", "a yes after 8.5 s is no answer: delivered");
  o = rig.truth.check(input("111", "Logged."), retryWith("x", &items));
  c.ok(!o.held && o.reply == "Logged.", "a cut after 9 s: delivered");
  o = rig.truth.check(input("111", "Logged."), retryWith("x", &items));
  c.ok(!o.held && o.reply == "Logged.", "a transport failure: delivered");
  c.ok(items.empty(), "never held");
  const truth::Counters& k = rig.truth.state().heads.at(0).c;
  c.eqi(k.failTimeout, 2, "two timeouts counted");
  c.eqi(k.failError, 1, "one error counted");
  c.eqi(k.hits, 0, "no hit counted");
  ndtest::rmTree(ndtest::scratchDir("truth-d"));
}

void testNoProbe(ndtest::Ctx& c) {
  std::printf("\n-- no probe: no key, unmeasured head, tainted --\n");
  truth::TurnEffect failed;
  failed.tool = "ledger_write";
  failed.ok = false;
  failed.receipt = "the ledger month file is damaged";
  failed.kind = truth::Kind::Ledger;
  for (int which = 0; which < 3; ++which) {
    Rig rig(freshDir("truth-e"));
    HostedTruth::Input in = input("111", "Logged.");
    in.now = {failed};
    if (which == 0) rig.key.clear();
    if (which == 1) in.host = "openai", in.model = "gpt-5.6-luna";
    if (which == 2) in.tainted = true;
    std::vector<std::string> items;
    const truth::Outcome o = rig.truth.check(in, retryWith("x", &items));
    const char* why = which == 0 ? "no key" : which == 1 ? "luna (no passing row)" : "tainted";
    c.eqi((long)rig.http.seen.size(), 0, std::string(why) + ": no request");
    c.ok(!o.held && o.reply == "Logged.", std::string(why) + ": delivered as written");
    c.eq(o.status, "Note: the ledger change in this reply did not go through (the ledger month file is damaged)",
         std::string(why) + ": the receipt line");
    ndtest::rmTree(ndtest::scratchDir("truth-e"));
  }
}

void testTheStateFile(ndtest::Ctx& c) {
  std::printf("\n-- truth_state.json --\n");
  const std::string dir = freshDir("truth-f");
  {
    Rig rig(dir);
    rig.http.script = {answer(true), answer(false)};
    std::vector<std::string> items;
    rig.truth.check(input("111", "Saved."), retryWith("Not saved.", &items));
    rig.truth.check(input("111", "Hello."), retryWith("x", &items));
    c.ok(exists(dir + "/truth_state.json"), "written beside the instance data");
    c.ok(!exists(dir + "/truth_state.json.tmp"), "no temp left behind");
  }
  {
    Rig rig(dir);
    c.eqi((long)rig.truth.state().heads.size(), 1, "one head after a restart");
    const truth::HeadState& h = rig.truth.state().heads.at(0);
    c.eq(h.key, "anthropic/claude-sonnet-5-5/e1", "keyed by head and engine");
    c.eq(nimbus::orch::headqual::truthSlug(h.mode), "enforce", "a measured head starts in enforce");
    c.eqi(h.c.turns, 2, "turns read back");
    c.eqi(h.c.holds, 1, "holds read back");
  }
  {
    FILE* f = std::fopen((dir + "/truth_state.json").c_str(), "w");
    std::fputs("{\"v\":1,\"heads\":[{\"k\":", f);
    std::fclose(f);
    Rig rig(dir);
    c.eqi((long)rig.truth.state().heads.size(), 0, "a damaged file starts again");
    rig.http.script = {answer(false)};
    std::vector<std::string> items;
    rig.truth.check(input("111", "Hello."), retryWith("x", &items));
    c.eq(nimbus::orch::headqual::truthSlug(rig.truth.state().heads.at(0).mode), "enforce",
         "from the table's start mode");
  }
  ndtest::rmTree(ndtest::scratchDir("truth-f"));
}

void testTheEngineBinding(ndtest::Ctx& c) {
  std::printf("\n-- the engine's binding (TurnCheck) --\n");
  const std::string dir = freshDir("truth-g");
  {
    Rig rig(dir);
    rig.effects.record("111", kNow - 3600, "ledger_write", "added L00A3 breakfast: skyr 150 g");
    rig.http.script = {answer(true), answer(false)};
    truth::TurnCheck k;
    k.reset(&rig.truth, "111", "Log 200 g yoghurt");
    truth::TurnEffect e;
    e.tool = "ledger_write";
    e.ok = false;
    e.receipt = "the month file is damaged";
    e.kind = truth::Kind::Ledger;
    k.note(e);
    c.ok(k.open("anthropic", "claude-sonnet-5-5", false), "opens for a measured head");
    const std::string item = k.atFinal("Logged 200 g yoghurt.");
    c.ok(item.find("ledger_write FAILED: the month file is damaged") != std::string::npos, "held with the facts item");
    const truth::Outcome o = k.finish("The ledger write failed; nothing was logged.");
    c.ok(o.held && o.retried && o.status.empty(), "the honest round is delivered, no line");
    c.eqi((long)rig.http.seen.size(), 2, "two probes (the hold, the receipt question)");
  }
  {
    Rig rig(dir);
    c.eqi((long)rig.truth.state().heads.at(0).c.holds, 1, "the hold was stored at close");
  }
  {
    // No ring at all: receipt-only, no request.
    ndtest::rmTree(ndtest::scratchDir("truth-h"));
    const std::string d2 = ndtest::scratchDir("truth-h") + "/data";
    FakeHttpTransport http;
    HostedTruth::Deps deps;
    deps.http = &http;
    deps.key = [](const std::string&) { return std::string("test-key-not-real"); };
    HostedTruth t(d2, deps, nullptr);
    truth::TurnCheck k;
    k.reset(&t, "111", "o");
    k.open("anthropic", "claude-sonnet-5-5", false);
    k.atFinal("Logged.");
    k.finish("Logged.");
    c.eqi((long)http.seen.size(), 0, "no ring: no probe");
    ndtest::rmTree(ndtest::scratchDir("truth-h"));
  }
  ndtest::rmTree(ndtest::scratchDir("truth-g"));
}

}  // namespace

int main() {
  ndtest::Ctx c;
  c.suite = "hosted_truth";
  testTheRequest(c);
  testTheChatsOwnRing(c);
  testTheHold(c);
  testFailOpen(c);
  testNoProbe(c);
  testTheStateFile(c);
  testTheEngineBinding(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  return c.failures ? 1 : 0;
}
