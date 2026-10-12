// test_router_embed - CUM-540: long-term memory works on a one-key (Cumulo-only)
// hosted instance, and the hosted page's embedding settings really apply.
//
// The class rules (device parity, src/agent/adapters/embeddings.cpp CUM-302):
//   (a) with NO direct key for the embed provider and a Cumulo key, every
//       embedding call goes through the router on the SAME upstream and model
//       (<router>/router/mistral/v1/embeddings, Cumulo key as the bearer, Mistral's
//       body dialect), so memory.write stores a vector and memory.search finds it;
//       /api/mem/stats reports embedAvailable true (route "router");
//   (b) a direct key wins over the router;
//   (c) with no key at all nothing is sent and stats says so;
//   (d) the device's "cumulo" embed provider is the /router/openai/v1 route;
//   (e) the page's verify-then-save: embedverify runs a real call and saves
//       nothing; embedcfg refuses a model switch while memories exist unless the
//       owner erases them, then applies and persists across a restart;
//   (f) a slow embeddings host never parks the single HTTP thread: verify and save
//       answer within a short bound ("still checking", nothing saved), GET
//       /api/mem/embedcfg shows the check settle, and the next verify and save
//       answer from that result without a second call; a save still queued past
//       the bound is never answered as saved;
//   (g) a width the model does not produce is never saved (Mistral ignores a
//       requested width), and a refused save leaves memory working;
//   (h) on load, vectors of another width than the saved config are copied aside
//       and memory starts empty (said in the log), an empty store takes the saved
//       width, so new vectors are never compared with incompatible ones, and when no
//       copy can be written nothing is erased.
// Offline: an injected FakeHttpTransport; no network, no real keys.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <thread>
#include <vector>

#include "../../test/support/fake_http.h"
#include "engine_thread.h"
#include "reply_buffer.h"
#include "rig.h"
#include "test_util.h"
#include "web_api.h"

using namespace nimbusd;
using harness_test::Exchange;
using harness_test::FakeHttpTransport;

static const char* kKey = "cumulo_sk_test_EMBED";

static bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

static void clearEnv() {
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY", "TAVILY_API_KEY",
                        "CUMULO_API_KEY", "Z_AI_TOKEN"})
    unsetenv(k);
}

static NimbusdRig::Options opts(const std::string& tag) {
  NimbusdRig::Options o;
  o.dataDir = ndtest::scratchDir(tag) + "/data";
  ndtest::rmTree(ndtest::scratchDir(tag));
  o.embeddings = true;
  o.embedHost = "mistral";
  o.embedModel = "mistral-embed";
  o.embedDims = 8;
  o.toolLoop = false;
  return o;
}

// An embeddings response with `n` floats (a fixed, non-zero direction).
static Exchange embedOk(const std::string& host, const std::string& path, int n) {
  std::string v;
  for (int i = 0; i < n; i++) v += (i ? "," : "") + std::string(i % 2 ? "0.25" : "0.5");
  Exchange e;
  e.expectHost = host;
  e.expectPathContains = path;
  e.body = "{\"data\":[{\"embedding\":[" + v + "]}],\"usage\":{\"prompt_tokens\":3}}";
  return e;
}

// A slow embeddings host: every request is held until the test releases it (or a
// 30 s backstop, the real embed timeout), so the bound is tested without racing a
// sleep against it.
struct HeldTransport : agent::HttpTransport {
  explicit HeldTransport(agent::HttpTransport* in) : inner(in) {}
  bool exec(const agent::HttpRequest& req, agent::HttpResponse& out, std::string& err) override {
    for (int i = 0; i < 3000 && !released.load(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    calls++;
    return inner->exec(req, out, err);
  }
  agent::HttpTransport* inner;
  std::atomic<bool> released{false};
  std::atomic<int> calls{0};
};

// Every task queued on the engine thread so far has run (FIFO); false on a stall.
static bool drained(EngineThread& eng) {
  auto done = std::make_shared<std::atomic<bool>>(false);
  eng.postWork([done] { done->store(true); });
  for (int i = 0; i < 3000 && !done->load(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  return done->load();
}

// The vectors.bin.mismatch-<time> copies in `dir`.
static std::vector<std::string> mismatchCopies(const std::string& dir) {
  std::vector<std::string> out;
  if (DIR* d = ::opendir(dir.c_str())) {
    while (const dirent* e = ::readdir(d))
      if (std::string(e->d_name).rfind("vectors.bin.mismatch-", 0) == 0) out.push_back(e->d_name);
    ::closedir(d);
  }
  return out;
}

static double secondsSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

static std::string header(const agent::HttpRequest& r, const std::string& name) {
  for (const auto& h : r.headers)
    if (h.first == name) return h.second;
  return std::string();
}

// One /api/* call through the real WebApi with a running engine thread.
struct Web {
  explicit Web(NimbusdRig& rig) : eng(&rig), api(&rig, &eng, &replies) { eng.start(); }
  ~Web() { eng.stop(); }
  ApiResp call(const std::string& m, const std::string& path, const std::string& body = "") {
    ApiResp out;
    api.handle(m, path, body, out);
    return out;
  }
  EngineThread eng;
  ReplyBuffer replies;
  WebApi api;
};

static void testRouterCarriesMemory(ndtest::Ctx& c) {
  std::printf("  -- (a) Cumulo-only: memory.write and memory.search embed through the router --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport tx;
  tx.script.push_back(embedOk(kCumuloHost, "/router/mistral/v1/embeddings", 8));
  tx.script.push_back(embedOk(kCumuloHost, "/router/mistral/v1/embeddings", 8));
  Config cfg;
  NimbusdRig rig(cfg, opts("emb-a"), &tx);
  c.ok(rig.embedAvailable(), "embeddings are available with only a Cumulo key");
  const std::string w = rig.callTool("memory.write", R"({"content":"Priya is vegetarian"})");
  c.eqi(rig.vectors().size(), 1, "memory.write stored a vector");
  if (!tx.seen.empty()) {
    const agent::HttpRequest& r = tx.seen[0];
    c.eq(r.host, kCumuloHost, "the embedding goes to the router host");
    c.eq(r.path, "/router/mistral/v1/embeddings", "on the configured upstream (same vector space)");
    c.eq(header(r, "Authorization"), std::string("Bearer ") + kKey, "the Cumulo key is the bearer");
    c.ok(has(r.body, "\"model\":\"mistral-embed\""), "the configured model rides the body");
    c.ok(!has(r.body, "dimensions"), "Mistral's dialect: no OpenAI dimensions field (CUM-469)");
  }
  const std::string q = rig.callTool("memory.search", R"({"query":"what does Priya eat"})");
  c.ok(has(q, "vegetarian"), "memory.search finds the stored fact");
  {
    Web web(rig);
    const ApiResp st = web.call("GET", "/api/mem/stats");
    c.ok(has(st.body, "\"embedAvailable\":true"), "/api/mem/stats reports embedAvailable true");
    c.ok(has(st.body, "\"route\":\"router\""), "/api/mem/stats names the router route");
  }
  clearEnv();
}

static void testDirectKeyWins(ndtest::Ctx& c) {
  std::printf("  -- (b) a direct key wins over the router --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  setenv("MISTRAL_API_KEY", "mk_test_EMBED", 1);
  FakeHttpTransport tx;
  tx.script.push_back(embedOk("api.mistral.ai", "/v1/embeddings", 8));
  Config cfg;
  NimbusdRig rig(cfg, opts("emb-b"), &tx);
  rig.callTool("memory.write", R"({"content":"direct"})");
  c.ok(!tx.seen.empty() && tx.seen[0].host == "api.mistral.ai", "the direct Mistral key carries the call");
  c.ok(!tx.seen.empty() && tx.seen[0].path == "/v1/embeddings", "on the direct path");
  clearEnv();
}

static void testNoKeyNoRequest(ndtest::Ctx& c) {
  std::printf("  -- (c) no key: nothing is sent and stats says so --\n");
  clearEnv();
  FakeHttpTransport tx;
  Config cfg;
  NimbusdRig rig(cfg, opts("emb-c"), &tx);
  c.ok(!rig.embedAvailable(), "no embeddings without any key");
  const std::string w = rig.callTool("memory.write", R"({"content":"x"})");
  c.eqi((long)tx.seen.size(), 0, "no request is sent");
  c.eqi(rig.vectors().size(), 0, "nothing stored");
  Web web(rig);
  c.ok(has(web.call("GET", "/api/mem/stats").body, "\"embedAvailable\":false"), "stats reports false");
  clearEnv();
}

static void testCumuloProviderRoute(ndtest::Ctx& c) {
  std::printf("  -- (d) the cumulo embed provider rides /router/openai/v1 --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport tx;
  tx.script.push_back(embedOk(kCumuloHost, "/router/openai/v1/embeddings", 8));
  Config cfg;
  NimbusdRig::Options o = opts("emb-d");
  o.embedHost = "cumulo";
  o.embedModel = "text-embedding-3-small";
  NimbusdRig rig(cfg, o, &tx);
  rig.callTool("memory.write", R"({"content":"openai space"})");
  c.ok(!tx.seen.empty() && tx.seen[0].path == "/router/openai/v1/embeddings", "the device's router route");
  c.ok(!tx.seen.empty() && has(tx.seen[0].body, "\"dimensions\":8"), "OpenAI's dialect carries dimensions");
  c.eqi(rig.vectors().size(), 1, "stored");
  clearEnv();
}

static void testVerifyThenSave(ndtest::Ctx& c) {
  std::printf("  -- (e) embedverify saves nothing; embedcfg is locked by memories, reset applies --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  NimbusdRig::Options o = opts("emb-e");
  {
    FakeHttpTransport tx;
    tx.script.push_back(embedOk(kCumuloHost, "/router/mistral/v1/embeddings", 8));    // a memory
    tx.script.push_back(embedOk(kCumuloHost, "/router/openai/v1/embeddings", 16));    // verify
    Config cfg;
    NimbusdRig rig(cfg, o, &tx);
    rig.callTool("memory.write", R"({"content":"keep me"})");
    Web web(rig);
    const std::string cand = "provider=openai&model=text-embedding-3-small&dims=16";
    const ApiResp v = web.call("POST", "/api/mem/embedverify", cand);
    c.eqi(v.status, 200, "embedverify answers 200 for a working candidate");
    c.ok(has(v.body, "\"ok\":true") && has(v.body, "\"dims\":16"), "the live call's width is reported");
    const ApiResp locked = web.call("POST", "/api/mem/embedcfg", cand);
    c.eqi(locked.status, 409, "a switch is refused while memories exist");
    c.ok(has(locked.body, "erase"), "the refusal says how to switch");
    c.eqi(rig.vectors().size(), 1, "the memory is untouched");
    const ApiResp ok = web.call("POST", "/api/mem/embedcfg", cand + "&reset=1");
    c.eqi(ok.status, 200, "reset=1 applies the switch");
    c.ok(has(ok.body, "\"provider\":\"openai\"") && has(ok.body, "\"dims\":16"), "the new config reads back");
    c.eqi(rig.vectors().size(), 0, "the old-model memories were erased");
    const ApiResp bad = web.call("POST", "/api/mem/embedcfg", "provider=openai&model=x%0Aprovider%3Dmistral&dims=16");
    c.eqi(bad.status, 400, "a model name with a line break is refused (it cannot inject a line into embed.txt)");
  }
  {
    Config cfg;   // a restart on the same volume
    NimbusdRig rig(cfg, o);
    c.eq(rig.embedProvider(), "openai", "the embed provider survives a restart");
    c.eqi(rig.embedWidth(), 16, "the width survives a restart");
  }
  clearEnv();
}

static void testSlowCheckNeverParksTheHttpThread(ndtest::Ctx& c) {
  std::printf("  -- (f) a slow check answers within the bound; its result is kept for the next try --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport inner;
  inner.script.push_back(embedOk(kCumuloHost, "/router/openai/v1/embeddings", 16));
  HeldTransport tx(&inner);   // answers only once the test releases it: longer than any bound
  Config cfg;
  NimbusdRig rig(cfg, opts("emb-f"), &tx);
  Web web(rig);
  const std::string cand = "provider=openai&model=text-embedding-3-small&dims=16";
  auto t0 = std::chrono::steady_clock::now();
  const ApiResp v = web.call("POST", "/api/mem/embedverify", cand);
  const double waited = secondsSince(t0);
  c.ok(waited < 6.5, "embedverify answers within the bound while the call is still running (waited " +
                         std::to_string((int)waited) + " s)");
  c.eqi(v.status, 202, "it answers 202: accepted, still checking");
  c.ok(has(v.body, "\"ok\":false") && has(v.body, "\"checking\":true") && has(v.body, "Still checking this model"),
       "the page shows \"still checking\" and saves nothing (never \"Verified\" without a width)");
  t0 = std::chrono::steady_clock::now();
  const ApiResp early = web.call("POST", "/api/mem/embedcfg", cand + "&reset=1");
  c.ok(secondsSince(t0) < 1.0, "a save during the check answers at once");
  c.eqi(early.status, 503, "a save during the check is not a 2xx (the page reads any 2xx as saved)");
  c.eqi(rig.embedWidth(), 8, "nothing changed");
  tx.released.store(true);   // the embeddings host answers now
  std::string cfgBody;
  for (int i = 0; i < 100 && !has(cfgBody, "\"state\":\"ok\""); i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    cfgBody = web.call("GET", "/api/mem/embedcfg").body;   // 503 while the engine runs the call
  }
  c.ok(has(cfgBody, "\"check\":{") && has(cfgBody, "\"state\":\"ok\"") && has(cfgBody, "\"width\":16"),
       "GET /api/mem/embedcfg shows the settled check");
  const ApiResp again = web.call("POST", "/api/mem/embedverify", cand);
  c.ok(again.status == 200 && has(again.body, "\"ok\":true") && has(again.body, "\"dims\":16"),
       "the next verify answers from the settled check");
  const ApiResp saved = web.call("POST", "/api/mem/embedcfg", cand + "&reset=1");
  c.eqi(saved.status, 200, "the save applies the checked width");
  c.eqi(rig.embedWidth(), 16, "the width is the one the model returned");
  c.eqi(tx.calls.load(), 1, "one live call served the check, the verify and the save");
  clearEnv();
}

static void testQueuedSaveIsNotSaved(ndtest::Ctx& c) {
  std::printf("  -- (f2) a save still queued past the bound is not answered as saved --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport tx;
  tx.script.push_back(embedOk(kCumuloHost, "/router/openai/v1/embeddings", 16));
  Config cfg;
  NimbusdRig rig(cfg, opts("emb-f2"), &tx);
  Web web(rig);
  const std::string cand = "provider=openai&model=text-embedding-3-small&dims=16";
  c.eqi(web.call("POST", "/api/mem/embedverify", cand).status, 200, "the candidate is checked");
  web.eng.postWork([] { std::this_thread::sleep_for(std::chrono::milliseconds(6500)); });   // work ahead of the save
  const ApiResp r = web.call("POST", "/api/mem/embedcfg", cand);
  c.eqi(r.status, 503, "a save still queued past the bound answers 503, never a 2xx (it may yet be refused)");
  c.ok(has(r.body, "Still saving"), "with a sentence that says to look again");
  c.ok(drained(web.eng), "the queued save ran");
  c.eqi(rig.embedWidth(), 16, "and applied, since nothing refused it meanwhile");
  clearEnv();
}

static void testUncheckedWidthNeverSaved(ndtest::Ctx& c) {
  std::printf("  -- (g) a width the model does not produce is never saved --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport tx;
  tx.script.push_back(embedOk(kCumuloHost, "/router/mistral/v1/embeddings", 1024));   // the save's check
  tx.script.push_back(embedOk(kCumuloHost, "/router/mistral/v1/embeddings", 1024));   // a verify after it
  tx.script.push_back(embedOk(kCumuloHost, "/router/mistral/v1/embeddings", 8));      // a memory
  Config cfg;
  NimbusdRig rig(cfg, opts("emb-g"), &tx);
  {
    Web web(rig);
    const std::string cand = "provider=mistral&model=mistral-embed&dims=512";
    const ApiResp r = web.call("POST", "/api/mem/embedcfg", cand);
    c.eqi(r.status, 400, "a width the model does not return is refused");
    c.ok(has(r.body, "returns 1024 dimensions"), "the refusal names the real width");
    c.eqi(rig.embedWidth(), 8, "the saved width is unchanged");
    const ApiResp v = web.call("POST", "/api/mem/embedverify", cand);
    c.ok(v.status == 400 && has(v.body, "\"dims\":1024") && has(v.body, "Set Dimensions to 1024 or 0"),
         "verify fails it too, naming the fix (the page used to save the typed width after a green verify)");
  }
  rig.callTool("memory.write", R"({"content":"still remembered"})");
  c.eqi(rig.vectors().size(), 1, "memory still works on the saved width");
  clearEnv();
}

static void testLoadReconcilesWidth(ndtest::Ctx& c) {
  std::printf("  -- (h) vectors of another width than the saved config are not compared --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  const NimbusdRig::Options o = opts("emb-h");
  const std::string mem = o.dataDir + "/mem";
  std::string oldBlob, emptyBlob;
  {
    FakeHttpTransport tx;
    tx.script.push_back(embedOk(kCumuloHost, "/router/mistral/v1/embeddings", 8));
    Config cfg;
    NimbusdRig rig(cfg, o, &tx);
    emptyBlob = rig.vectors().serialize();
    rig.callTool("memory.write", R"({"content":"made by the old model"})");
    c.eqi(rig.vectors().size(), 1, "one 8-wide memory");
    oldBlob = rig.vectors().serialize();
  }
  // A restart inside a switch: embed.txt already says 16 wide, vectors.bin is the old 8-wide store.
  fsutil::writeFileAtomic(mem + "/embed.txt", "provider=openai\nmodel=text-embedding-3-small\ndims=16\n");
  fsutil::writeFileAtomic(mem + "/vectors.bin", oldBlob);
  {
    Config cfg;
    NimbusdRig rig(cfg, o);
    c.eqi(rig.vectors().size(), 0, "the incompatible memories are not loaded");
    c.eqi(rig.vectors().dims(), 16, "the store is the saved width");
    c.eqi(rig.embedWidth(), 16, "and so is the config");
    c.ok(NimbusdRig::loggedContaining("cannot be compared"), "the log says why memory starts empty");
    const std::vector<std::string> kept = mismatchCopies(mem);
    std::string copy;
    c.ok(kept.size() == 1 && fsutil::readFile(mem + "/" + kept[0], copy) && copy == oldBlob,
         "the old store is kept aside byte for byte, not destroyed");
  }
  // An EMPTY store whose header carries the old width simply takes the saved width.
  fsutil::writeFileAtomic(mem + "/vectors.bin", emptyBlob);
  for (const auto& f : mismatchCopies(mem)) ::unlink((mem + "/" + f).c_str());
  {
    Config cfg;
    NimbusdRig rig(cfg, o);
    c.eqi(rig.vectors().dims(), 16, "an empty store takes the saved width");
    c.ok(mismatchCopies(mem).empty(), "nothing is set aside for an empty store");
  }
  // When no copy can be written, nothing is erased: the memories stay at their width.
  if (geteuid() != 0) {   // root ignores directory permissions (the Docker build stage)
    fsutil::writeFileAtomic(mem + "/vectors.bin", oldBlob);
    ::chmod(mem.c_str(), 0500);
    Config cfg;
    NimbusdRig rig(cfg, o);
    ::chmod(mem.c_str(), 0700);
    c.eqi(rig.vectors().size(), 1, "with a read-only volume the memories are kept, not erased");
    c.eqi(rig.embedWidth(), 8, "and embeddings use their width");
    c.ok(NimbusdRig::loggedContaining("could not be written"), "the log says so");
    std::string still;
    c.ok(fsutil::readFile(mem + "/vectors.bin", still) && still == oldBlob, "vectors.bin is untouched");
  }
  clearEnv();
}

// CUM-540 gate: a check queued behind other engine work (a long turn) was presumed
// lost 90 s after it was QUEUED, so a second press queued a second paid embeddings
// call and the first one's result was thrown away. The lost window now runs from
// when a check starts, a queued check is not presumed lost while it waits, and a
// check a newer one replaced makes no call when its turn comes. Driven on the
// slot's own clock, in the order the web layer drives it.
static void testQueuedCheckNeverPaysTwice(ndtest::Ctx& c) {
  std::printf("  -- (h) a check queued behind other work is never presumed lost, and never pays twice --\n");
  using Next = EmbedCheckSlot::Next;
  EmbedCandidate cand;
  cand.provider = "openai";
  cand.model = "text-embedding-3-small";
  cand.dims = 16;
  EmbedCheck r;
  EmbedCheckSlot slot;
  c.ok(slot.begin(cand, 1000, r) == Next::Run, "the first press queues one check");
  const uint32_t first = r.gen;
  c.ok(slot.begin(cand, 1095, r) == Next::Checking,
       "95 s later, still queued behind a long turn: still checking, no second call");
  c.ok(slot.start(first, 1120), "the queued check starts when its turn comes");
  c.ok(slot.begin(cand, 1200, r) == Next::Checking, "80 s into its run: still checking");
  slot.settle(first, 16, "", 1205);
  c.ok(slot.begin(cand, 1210, r) == Next::Settled && r.width == 16, "the next press answers from its result");

  // A RUNNING check is presumed lost 90 s after it started; only its replacement settles.
  EmbedCheckSlot run;
  run.begin(cand, 0, r);
  const uint32_t a = r.gen;
  c.ok(run.start(a, 10), "a check starts");
  c.ok(run.begin(cand, 99, r) == Next::Checking, "89 s into its run: still checking");
  c.ok(run.begin(cand, 100, r) == Next::Run, "90 s into its run: presumed lost, one new check is queued");
  const uint32_t b = r.gen;
  run.settle(a, 16, "", 101);
  c.ok(run.get().state == EmbedCheck::State::Checking && run.get().gen == b, "the lost check's late result is dropped");
  c.ok(run.start(b, 102), "the new check runs");

  // A queued check replaced by a newer one makes no call when it runs at last.
  EmbedCheckSlot q;
  q.begin(cand, 0, r);
  const uint32_t old = r.gen;
  c.ok(q.begin(cand, EmbedCheckSlot::kLostQueuedS, r) == Next::Run, "a check queued past the queued bound is replaced");
  const uint32_t fresh = r.gen;
  c.ok(!q.start(old, EmbedCheckSlot::kLostQueuedS + 1), "the replaced check makes no call when its turn comes");
  c.ok(q.start(fresh, EmbedCheckSlot::kLostQueuedS + 2), "the newer one does");
  c.ok(!q.start(fresh, EmbedCheckSlot::kLostQueuedS + 3), "and only once");
}

int main() {
  ndtest::Ctx c;
  std::printf("=== embeddings through the Cumulo router (CUM-540, offline) ===\n");
  testRouterCarriesMemory(c);
  testDirectKeyWins(c);
  testNoKeyNoRequest(c);
  testCumuloProviderRoute(c);
  testVerifyThenSave(c);
  testSlowCheckNeverParksTheHttpThread(c);
  testQueuedCheckNeverPaysTwice(c);
  testQueuedSaveIsNotSaved(c);
  testUncheckedWidthNeverSaved(c);
  testLoadReconcilesWidth(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
