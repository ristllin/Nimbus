#include <unity.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../support/fake_fabric.h"
#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/jobs.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/billing_refusal.h"
#include "nimbus/orch/journal.h"
#include "nimbus/orch/provider_park.h"

// L19-8: ONE park per provider, consulted by every paid seam (chat, spawns,
// embeddings, vision, speech to text, text to speech, the download scan, image
// generation). A refusal learned on any seam closes the provider for all of them;
// the state and the owner-notice latch survive a restart; the pause switch holds a
// provider off every seam and is never probed.

namespace orch = nimbus::orch;
namespace providers = agent::providers;
using harness_test::FakeProviderDeps;
using harness_test::LogCapture;
using orch::PaidSeam;
using orch::ParkKind;
using orch::providerPark;

namespace {

const char* kCreditExhausted =
    "{\"error\":{\"message\":\"You have run out of credit.\",\"type\":\"insufficient_quota\","
    "\"code\":\"credit_balance_exhausted\"}}";

constexpr uint32_t kFp = 0x1234;   // a key fingerprint; a seam with no key passes 0

PaidSeam seamAt(int i) { return (PaidSeam)i; }

bool open(const char* prov, PaidSeam seam, uint32_t now, uint32_t fp = kFp) {
  return providerPark().available(prov, seam, fp, now).open;
}

// A fake flash file for the park: what save() wrote, load() reads back.
struct FakeFlash {
  std::string blob;
  bool has = false;
  bool ready = true;
  int saves = 0;
  uint32_t epoch = 0;   // wall clock (0 = not set yet)
  uint32_t now = 0;     // monotonic clock

  orch::ParkStorage storage() {
    orch::ParkStorage s;
    s.load = [this](std::string& out) {
      if (!ready) return -1;
      if (!has) return 0;
      out = blob;
      return 1;
    };
    s.save = [this](const std::string& b) {
      blob = b;
      has = true;
      saves++;
      return true;
    };
    s.epochS = [this] { return epoch; };
    s.nowMs = [this] { return now; };
    return s;
  }
};

// A restart: RAM is gone (millis starts again), the flash file stays.
void reboot(FakeFlash& f, uint32_t nowAfter) {
  providerPark().clear();
  providerPark().forgetStorage();
  f.now = nowAfter;
  providerPark().setStorage(f.storage());
}

}  // namespace

void setUp() {
  LogCapture::install();
  providerPark().forgetStorage();
  providerPark().clear();
}
void tearDown() {
  agent::hlog::setSink(nullptr);
  providerPark().forgetStorage();
  providerPark().clear();
}

// ---- 1. what closes a provider, for which seams: the property ---------------------

struct Refusal {
  int status;
  const char* body;
  bool parks;       // the provider is closed
  bool everySeam;   // ...for every seam (else only the one that met it)
};

const Refusal kRefusals[] = {
    {429, kCreditExhausted, true, true},                                                      // OpenAI embeddings
    {429, "{\"error\":{\"code\":\"insufficient_quota\"}}", true, true},                        // OpenAI quota
    {400, "{\"error\":{\"message\":\"Your credit balance is too low to access the Anthropic API.\"}}", true, true},
    {402, "{}", true, true},                                                                  // every 402
    {429, "{\"error\":\"ceiling_reached\"}", true, true},                                     // the router's cap
    {401, "{\"error\":{\"message\":\"invalid x-api-key\"}}", true, true},                     // a refused key
    {403, "{\"error\":{\"message\":\"model not allowed for this project\"}}", true, false},   // one model / endpoint
    {429, "{\"error\":{\"message\":\"Rate limit reached for requests per minute\"}}", false, false},   // counter-test
    {500, "{\"error\":{\"message\":\"server error\"}}", false, false},
    {529, "{\"error\":{\"type\":\"overloaded_error\"}}", false, false},
    {404, "{\"error\":{\"message\":\"model not found\"}}", false, false},
};

// For every seam a refusal can be learned on and every refusal kind: which seams of
// that provider close, which stay open, and another provider is never touched.
static void test_property_seam_x_refusal_kind() {
  for (int s = 0; s < orch::kPaidSeamCount; s++) {
    for (const Refusal& r : kRefusals) {
      providerPark().clear();
      const uint32_t now = 5000;
      const ParkKind k = providerPark().noteAnswer("openai", seamAt(s), kFp, r.status, r.body, now);
      char what[160];
      snprintf(what, sizeof(what), "learned on %s: HTTP %d %s", orch::paidSeamSlug(seamAt(s)), r.status, r.body);
      TEST_ASSERT_EQUAL_MESSAGE(r.parks, k != ParkKind::None, what);
      for (int t = 0; t < orch::kPaidSeamCount; t++) {
        const bool closed = !open("openai", seamAt(t), now + 1);
        const bool want = r.parks && (r.everySeam || t == s);
        TEST_ASSERT_EQUAL_MESSAGE(want, closed, what);
        TEST_ASSERT_TRUE_MESSAGE(open("mistral", seamAt(t), now + 1), what);
      }
    }
  }
}

// Every seam has a slug, every kind has a slug, and the words name the cause.
static void test_slugs_and_words() {
  for (int s = 0; s < orch::kPaidSeamCount; s++)
    TEST_ASSERT_TRUE(strlen(orch::paidSeamSlug(seamAt(s))) > 1);
  TEST_ASSERT_EQUAL_STRING("billing", orch::parkKindSlug(ParkKind::Billing));
  TEST_ASSERT_EQUAL_STRING("auth", orch::parkKindSlug(ParkKind::Auth));
  providerPark().noteAnswer("openai", PaidSeam::Embeddings, kFp, 429, kCreditExhausted, 1000);
  const orch::ParkVerdict v = providerPark().available("openai", PaidSeam::Vision, kFp, 1000 + 18 * 60000);
  TEST_ASSERT_EQUAL_STRING("openai says the account is out of credit (HTTP 429); it is checked again in 12 minutes",
                           orch::parkWords("openai", v).c_str());
  TEST_ASSERT_EQUAL_STRING("openai is out of credit. Add credit.", orch::parkShortWords("openai", v).c_str());
  TEST_ASSERT_TRUE(orch::parkShortWords("openai", v).size() <= 48);
}

// ---- 2. the re-check, the notice latch and a changed key -----------------------------

static void test_recheck_probe_requeue_and_success() {
  providerPark().park("anthropic", PaidSeam::Chat, kFp, ParkKind::Billing, 400, 1000);
  TEST_ASSERT_TRUE(orch::billingNoticeDue("anthropic"));
  TEST_ASSERT_FALSE(orch::billingNoticeDue("anthropic"));   // once per episode, every seam
  TEST_ASSERT_FALSE(open("anthropic", PaidSeam::Spawn, 1000 + orch::ProviderPark::kRecheckMs - 1));
  // Due: ONE probe goes, from whichever seam asks first.
  const orch::ParkVerdict probe =
      providerPark().available("anthropic", PaidSeam::Stt, kFp, 1000 + orch::ProviderPark::kRecheckMs);
  TEST_ASSERT_TRUE(probe.open);
  TEST_ASSERT_TRUE(probe.probe);
  // Refused again: re-parked quietly (the latch still holds: no second notice).
  providerPark().noteAnswer("anthropic", PaidSeam::Stt, kFp, 400, "credit balance is too low",
                            1000 + orch::ProviderPark::kRecheckMs + 10);
  TEST_ASSERT_FALSE(open("anthropic", PaidSeam::Chat, 1000 + orch::ProviderPark::kRecheckMs + 20));
  TEST_ASSERT_FALSE(orch::billingNoticeDue("anthropic"));
  // A non-refusal failure of the next probe re-arms the wait.
  uint32_t t = 1000 + 2 * orch::ProviderPark::kRecheckMs + 20;
  TEST_ASSERT_TRUE(providerPark().available("anthropic", PaidSeam::Chat, kFp, t).probe);
  providerPark().noteFailure("anthropic", t + 5);
  TEST_ASSERT_FALSE(open("anthropic", PaidSeam::Chat, t + 10));
  // A success on the probe ends the episode: open everywhere, and the next refusal
  // is a new episode the owner hears about once.
  t += orch::ProviderPark::kRecheckMs + 10;
  TEST_ASSERT_TRUE(providerPark().available("anthropic", PaidSeam::Embeddings, kFp, t).probe);
  TEST_ASSERT_TRUE(providerPark().noteSuccess("anthropic", PaidSeam::Embeddings));
  for (int s = 0; s < orch::kPaidSeamCount; s++) TEST_ASSERT_TRUE(open("anthropic", seamAt(s), t + 1));
  TEST_ASSERT_TRUE(orch::billingNoticeDue("anthropic"));
}

// A 403 closed only its own seam: a success elsewhere proves nothing about it.
static void test_seam_only_park_ends_only_on_its_seam() {
  providerPark().noteAnswer("openai", PaidSeam::Vision, kFp, 403, "not allowed", 1000);
  TEST_ASSERT_FALSE(providerPark().noteSuccess("openai", PaidSeam::Chat));
  TEST_ASSERT_FALSE(open("openai", PaidSeam::Vision, 2000));
  TEST_ASSERT_TRUE(open("openai", PaidSeam::Chat, 2000));
}

static void test_changed_key_is_a_new_start() {
  providerPark().park("openai", PaidSeam::Embeddings, kFp, ParkKind::Billing, 429, 1000);
  TEST_ASSERT_TRUE(orch::billingNoticeDue("openai"));
  TEST_ASSERT_FALSE(open("openai", PaidSeam::Chat, 2000, kFp));
  TEST_ASSERT_FALSE(open("openai", PaidSeam::Spawn, 2000, 0));   // no key at hand: not compared, still parked
  const orch::ParkVerdict v = providerPark().available("openai", PaidSeam::Chat, kFp + 1, 2002);
  TEST_ASSERT_TRUE(v.open);
  TEST_ASSERT_TRUE(v.keyChanged);
  TEST_ASSERT_TRUE(open("openai", PaidSeam::Embeddings, 2003, kFp + 1));
  TEST_ASSERT_TRUE(orch::billingNoticeDue("openai"));   // a new key: a new episode
}

// ---- 3. durable: the park, its re-check time and the latch survive a restart ----------

static void test_park_and_latch_survive_a_restart() {
  FakeFlash f;
  f.epoch = 1760000000;
  f.now = 50000;
  providerPark().setStorage(f.storage());
  providerPark().noteAnswer("openai", PaidSeam::Embeddings, kFp, 429, kCreditExhausted, 50000);
  TEST_ASSERT_TRUE(orch::billingNoticeDue("openai"));
  TEST_ASSERT_TRUE(f.saves >= 2);
  // Ten minutes later the device restarts: millis begins again at 3 s.
  f.epoch += 600;
  reboot(f, 3000);
  TEST_ASSERT_FALSE_MESSAGE(open("openai", PaidSeam::Chat, 3000), f.blob.c_str());   // still parked: no doomed request
  TEST_ASSERT_FALSE(orch::billingNoticeDue("openai"));                               // and no second notice
  // The wall clock says 20 minutes of the 30 are left: closed until then, then the probe.
  TEST_ASSERT_FALSE(open("openai", PaidSeam::Chat, 3000 + 19 * 60000));
  TEST_ASSERT_TRUE(providerPark().available("openai", PaidSeam::Chat, kFp, 3000 + 20 * 60000 + 1000).probe);
}

// Without a wall clock at the restart the saved wait is used whole (never shorter).
static void test_restart_without_clock_keeps_the_saved_wait() {
  FakeFlash f;
  f.epoch = 0;
  providerPark().setStorage(f.storage());
  providerPark().park("mistral", PaidSeam::Chat, kFp, ParkKind::Auth, 401, 100000);
  reboot(f, 2000);
  TEST_ASSERT_FALSE(open("mistral", PaidSeam::Tts, 2000));   // the first question loads the file
  TEST_ASSERT_FALSE(open("mistral", PaidSeam::Tts, 2000 + orch::ProviderPark::kRecheckMs - 1000));
  TEST_ASSERT_TRUE(providerPark().available("mistral", PaidSeam::Tts, kFp, 2000 + orch::ProviderPark::kRecheckMs + 1000).probe);
}

// Storage not mounted yet at the first question: the park asks again on the next one.
static void test_storage_not_ready_is_retried() {
  FakeFlash f;
  providerPark().setStorage(f.storage());
  providerPark().park("openai", PaidSeam::Chat, kFp, ParkKind::Billing, 429, 1000);
  f.ready = false;
  reboot(f, 1000);
  TEST_ASSERT_TRUE(open("openai", PaidSeam::Chat, 1000));   // nothing readable yet
  f.ready = true;
  TEST_ASSERT_FALSE(open("openai", PaidSeam::Chat, 1100));  // loaded on the next call
}

// A garbage or foreign file loads nothing and never crashes.
static void test_garbage_file_is_tolerated() {
  TEST_ASSERT_FALSE(providerPark().deserialize("hello", 0));
  TEST_ASSERT_FALSE(providerPark().deserialize("PK1\nnot a record\nUPPER 1 400 255 10 0 0 0 0 0\n", 0));
  TEST_ASSERT_TRUE(open("UPPER", PaidSeam::Chat, 10));
  TEST_ASSERT_TRUE(providerPark().deserialize("PK1\n", 0));
}

// The daily spend-warning latch: once per provider per day, kept across a restart.
static void test_daily_warning_latch() {
  FakeFlash f;
  providerPark().setStorage(f.storage());
  TEST_ASSERT_TRUE(providerPark().dailyWarningDue("anthropic", 20383));
  TEST_ASSERT_FALSE(providerPark().dailyWarningDue("anthropic", 20383));
  TEST_ASSERT_TRUE(providerPark().dailyWarningDue("openai", 20383));
  reboot(f, 10);
  TEST_ASSERT_FALSE(providerPark().dailyWarningDue("anthropic", 20383));
  TEST_ASSERT_TRUE(providerPark().dailyWarningDue("anthropic", 20384));
}

// ---- 4. the pause switch (lane DEF's spend freeze rides it) ---------------------------

static void test_pause_switch_closes_every_seam_and_is_never_probed() {
  providerPark().pause(orch::kPauseAllProviders, orch::kPauseSpendCeiling);
  for (const char* p : {"anthropic", "openai", "mistral", "zai", "cumulo", "custom"})
    for (int s = 0; s < orch::kPaidSeamCount; s++) {
      const orch::ParkVerdict v = providerPark().available(p, seamAt(s), kFp, 1000 + 3 * orch::ProviderPark::kRecheckMs);
      TEST_ASSERT_FALSE(v.open);
      TEST_ASSERT_FALSE(v.probe);
      TEST_ASSERT_EQUAL(orch::kPauseSpendCeiling, v.paused);
    }
  TEST_ASSERT_EQUAL_STRING("paid requests to openai are paused: the monthly spend limit is reached",
                           orch::parkWords("openai", providerPark().available("openai", PaidSeam::Chat, 0, 1)).c_str());
  // Causes are independent: the owner's own pause on one provider outlives the freeze.
  providerPark().pause("mistral", orch::kPauseOwner);
  providerPark().resume(orch::kPauseAllProviders, orch::kPauseSpendCeiling);
  TEST_ASSERT_TRUE(open("openai", PaidSeam::Chat, 1));
  TEST_ASSERT_FALSE(open("mistral", PaidSeam::Chat, 1));
  TEST_ASSERT_EQUAL(orch::kPauseOwner, providerPark().pausedBy("mistral"));
  providerPark().resume("mistral", orch::kPauseOwner);
  TEST_ASSERT_TRUE(open("mistral", PaidSeam::Chat, 1));
}

// RAM only: a load keeps a pause set before it, and the file never carries one.
static void test_pause_is_ram_only_and_survives_a_load() {
  FakeFlash f;
  providerPark().park("openai", PaidSeam::Chat, kFp, ParkKind::Billing, 429, 1000);
  providerPark().setStorage(f.storage());
  providerPark().pause("openai", orch::kPauseOwner);
  providerPark().noteFailure("openai", 1001);   // any call that saves
  providerPark().park("openai", PaidSeam::Chat, kFp, ParkKind::Billing, 429, 1002);
  TEST_ASSERT_TRUE(f.blob.find("openai") != std::string::npos);
  reboot(f, 5);
  providerPark().pause("anthropic", orch::kPauseSpendCeiling);   // set before the first load
  TEST_ASSERT_FALSE(open("openai", PaidSeam::Chat, 10));         // the park came back from flash
  TEST_ASSERT_EQUAL(0, providerPark().pausedBy("openai"));       // its pause did not
  TEST_ASSERT_EQUAL(orch::kPauseSpendCeiling, providerPark().pausedBy("anthropic"));
}

// The bench's clean slate (PARK clear): every park and latch ends and the file is
// saved without them, so a restart does not bring them back; a pause stays.
static void test_forget_parks_is_saved_and_keeps_pauses() {
  FakeFlash f;
  providerPark().setStorage(f.storage());
  providerPark().park("openai", PaidSeam::Embeddings, kFp, ParkKind::Billing, 429, 1000);
  TEST_ASSERT_TRUE(orch::billingNoticeDue("openai"));
  providerPark().pause("mistral", orch::kPauseOwner);
  TEST_ASSERT_FALSE(open("openai", PaidSeam::Chat, 1001));
  providerPark().forgetParks(1002);
  TEST_ASSERT_TRUE(open("openai", PaidSeam::Chat, 1003));
  TEST_ASSERT_EQUAL(orch::kPauseOwner, providerPark().pausedBy("mistral"));
  TEST_ASSERT_TRUE(f.blob.find("openai") == std::string::npos);
  reboot(f, 5);
  TEST_ASSERT_TRUE(open("openai", PaidSeam::Chat, 10));   // not back from flash
  TEST_ASSERT_TRUE(orch::billingNoticeDue("openai"));    // the next episode is told again
}

// The records live where setRecordMemory says (the device: PSRAM), allocated once on
// the first question, so the park object itself keeps only its locks and hooks in
// internal RAM (the static DRAM release gate).
alignas(8) static unsigned char g_recordArena[1024];
static int g_recordAllocs = 0;
static size_t g_recordBytes = 0;
static void* arenaAlloc(size_t n) {
  g_recordAllocs++;
  g_recordBytes = n;
  return n <= sizeof(g_recordArena) ? g_recordArena : nullptr;
}

static void test_records_live_in_the_memory_the_device_gives() {
  orch::ProviderPark park;
  park.setRecordMemory(arenaAlloc);
  TEST_ASSERT_EQUAL(0, g_recordAllocs);   // nothing until the first question
  TEST_ASSERT_TRUE(park.available("openai", PaidSeam::Chat, kFp, 1000).open);
  TEST_ASSERT_EQUAL(1, g_recordAllocs);
  TEST_ASSERT_TRUE(g_recordBytes >= orch::ProviderPark::kMaxProviders * 16);
  park.park("openai", PaidSeam::Chat, kFp, ParkKind::Billing, 402, 1001);
  TEST_ASSERT_FALSE(park.available("openai", PaidSeam::Vision, kFp, 1002).open);
  TEST_ASSERT_EQUAL_STRING("openai", reinterpret_cast<const char*>(g_recordArena));   // the record is there
  park.pause("mistral", orch::kPauseOwner);
  TEST_ASSERT_EQUAL(orch::kPauseOwner, park.pausedBy("mistral"));
  TEST_ASSERT_EQUAL(1, g_recordAllocs);   // once, never again
}

// ---- 5. the 19 Oct scenario across seams ---------------------------------------------

// A refusal replayed on the EMBEDDING seam (OpenAI 429 credit_balance_exhausted),
// then a photo turn and a spawn: zero requests to that provider, the caption step
// has the out-of-credit words, the spawn runs on the next sub provider.
struct SpawnRig {
  harness_test::FakePlatform plat;
  harness_test::FakeAdapter oai, ant;
  agent::HeavyFabric fabric;
  nimbus::orch::Journal journal;
  std::vector<std::string> delivered;
  std::unique_ptr<agent::JobEngine> eng;
  struct Store : orch::JournalStore {
    std::map<int, std::string> slots;
    std::string get(int s) override { auto it = slots.find(s); return it == slots.end() ? "" : it->second; }
    bool put(int s, const std::string& v) override { slots[s] = v; return true; }
    bool remove(int s) override { slots.erase(s); return true; }
    void clearNs() override { slots.clear(); }
  } store;

  SpawnRig() {
    oai.backend = "openai";
    ant.backend = "anthropic";
    fabric.registerAdapter(&oai);
    fabric.registerAdapter(&ant);
    journal.begin(&store);
    agent::JobEngine::Deps d;
    d.platform = plat.contract();
    d.fabric = &fabric;
    d.journal = &journal;
    d.deliver = [this](const std::string&, const std::string& t) { delivered.push_back(t); return true; };
    d.subPriority = [] { return std::string("openai,anthropic"); };
    d.providerHasKey = [](const std::string& p) { return p == "openai" || p == "anthropic"; };
    d.subModel = [](const std::string& p) { return "good-sub-" + p; };
    d.modelIsValid = [](const std::string&, const std::string& m) { return m.rfind("good-", 0) == 0; };
    eng.reset(new agent::JobEngine(std::move(d)));
  }
  void spawn(const char* provider) {
    orch::Spawn s;
    s.task = "research the thing";
    s.provider = provider;
    s.category = "research";
    s.note = "On it.";
    eng->enqueueSpawn(s, "chat1");
    eng->pump();
  }
};

static void test_embedding_refusal_closes_vision_spawn_and_chat() {
  // The embedding seam meets the refusal (device: embeddings.cpp -> gate::noteAnswer).
  TEST_ASSERT_EQUAL((int)ParkKind::Billing,
                    (int)providerPark().noteAnswer("openai", PaidSeam::Embeddings, 0, 429, kCreditExhausted, 1000));
  // The photo turn: the caption walk (device: image_vision.cpp resolve) sends nothing
  // to openai; with only openai keyed for vision the caption step has the words.
  auto keysOnlyOpenai = [](const std::string& p) { return p == "openai" ? std::string("sk-o") : std::string(); };
  orch::ProviderPick pick = orch::pickOpenProvider({"openai", "mistral", "anthropic", "cumulo"}, keysOnlyOpenai,
                                                   PaidSeam::Vision, 2000);
  TEST_ASSERT_TRUE(pick.provider.empty());
  TEST_ASSERT_TRUE_MESSAGE(pick.closedWhy.find("openai says the account is out of credit") == 0,
                           pick.closedWhy.c_str());
  // With another keyed provider the photo is described there instead.
  auto keysBoth = [](const std::string& p) { return p == "openai" || p == "mistral" ? std::string("k") : std::string(); };
  pick = orch::pickOpenProvider({"openai", "mistral"}, keysBoth, PaidSeam::Vision, 2000);
  TEST_ASSERT_EQUAL_STRING("mistral", pick.provider.c_str());
  // The spawn (no provider named): the next sub provider; and one that names openai
  // moves too. Nothing is dispatched to openai.
  SpawnRig r;
  r.spawn("");
  TEST_ASSERT_EQUAL(0, (int)r.oai.dispatched.size());
  TEST_ASSERT_EQUAL(1, (int)r.ant.dispatched.size());
  r.plat.ms += 5000;
  r.spawn("openai");
  TEST_ASSERT_EQUAL(0, (int)r.oai.dispatched.size());
  TEST_ASSERT_EQUAL(2, (int)r.ant.dispatched.size());
  TEST_ASSERT_TRUE(LogCapture::contains("orchestrator: spawn moved openai -> anthropic"));
  // A chat turn: zero requests to api.openai.com, the turn starts on anthropic.
  FakeProviderDeps d;
  d.http.script.push_back({"api.anthropic.com", "", 200,
      "{\"content\":[{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"orch_turn\",\"input\":{\"reply\":\"hi\"}}],"
      "\"stop_reason\":\"tool_use\",\"usage\":{\"input_tokens\":5,\"output_tokens\":5}}"});
  providers::ProviderDeps pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string out, err;
  TEST_ASSERT_TRUE_MESSAGE(providers::runFabricLoop(pd, {"openai", "anthropic"}, "S", "U", out, err, rig.ht,
                                                    nullptr, nullptr), err.c_str());
  for (const auto& req : d.http.seen) TEST_ASSERT_TRUE(req.host != "api.openai.com");
  TEST_ASSERT_EQUAL(1, (int)d.http.seen.size());
}

// Counter-test: a rate-limit 429 on the embedding seam parks nothing - the photo,
// the spawn and the chat turn still go to openai.
static void test_rate_limit_on_one_seam_parks_nothing() {
  TEST_ASSERT_EQUAL((int)ParkKind::None,
                    (int)providerPark().noteAnswer("openai", PaidSeam::Embeddings, 0, 429,
                                                   "{\"error\":{\"message\":\"Rate limit reached\"}}", 1000));
  auto keys = [](const std::string& p) { return p == "openai" ? std::string("k") : std::string(); };
  TEST_ASSERT_EQUAL_STRING("openai", orch::pickOpenProvider({"openai"}, keys, PaidSeam::Vision, 2000).provider.c_str());
  SpawnRig r;
  r.spawn("");
  TEST_ASSERT_EQUAL(1, (int)r.oai.dispatched.size());
}

// Every closed provider in the sub list: the spawn is refused with the park's words
// and nothing is dispatched.
static void test_spawn_refused_when_every_provider_is_closed() {
  providerPark().park("openai", PaidSeam::Chat, 0, ParkKind::Billing, 429, 1000);
  providerPark().park("anthropic", PaidSeam::Chat, 0, ParkKind::Billing, 400, 1000);
  SpawnRig r;
  r.spawn("");
  TEST_ASSERT_EQUAL(0, (int)r.oai.dispatched.size() + (int)r.ant.dispatched.size());
  bool told = false;
  for (const auto& t : r.delivered) told = told || t.find("out of credit") != std::string::npos;
  TEST_ASSERT_TRUE(told);
}

// The pause switch on every provider: no host is asked, the turn fails Paused.
static void test_paused_fabric_sends_nothing() {
  providerPark().pause(orch::kPauseAllProviders, orch::kPauseSpendCeiling);
  FakeProviderDeps d;
  providers::ProviderDeps pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string out, err;
  TEST_ASSERT_FALSE(providers::runFabricLoop(pd, {"anthropic", "openai"}, "S", "U", out, err, rig.ht, nullptr, nullptr));
  TEST_ASSERT_EQUAL(0, (int)d.http.seen.size());
  TEST_ASSERT_EQUAL((int)orch::StepErr::Paused, (int)orch::classifyStepError(err));
  // A paused head with only an alternate out of credit: neither is asked "anyway"
  // (lane PARK: zero requests to a parked provider). The turn ends with the park's
  // billing cause, naming the provider out of credit, and nothing is re-parked.
  providerPark().resume(orch::kPauseAllProviders, orch::kPauseSpendCeiling);
  providerPark().pause("anthropic", orch::kPauseOwner);
  providerPark().park("openai", PaidSeam::Chat, orch::parkKeyFingerprint("sk-fake-oai"), ParkKind::Billing, 429, 1);
  const uint32_t recheckBefore = providerPark().recheckInMs("openai");
  TEST_ASSERT_FALSE(providers::runFabricLoop(pd, {"anthropic", "openai"}, "S", "U", out, err, rig.ht, nullptr, nullptr));
  TEST_ASSERT_EQUAL(0, (int)d.http.seen.size());
  TEST_ASSERT_TRUE_MESSAGE(orch::isParkedRefusal(err), err.c_str());
  TEST_ASSERT_EQUAL((int)orch::StepErr::Billing, (int)orch::classifyStepError(err));
  TEST_ASSERT_EQUAL_STRING("parked: openai is out of credit (HTTP 429); no request sent", err.c_str());
  TEST_ASSERT_TRUE(providerPark().recheckInMs("openai") <= recheckBefore);   // the wait was not re-armed
  // Counter-test: a host closed by a refused KEY is still asked anyway (a fixed key or
  // model shows at once), never the paused head.
  providerPark().forgetParks(2);
  providerPark().park("openai", PaidSeam::Chat, orch::parkKeyFingerprint("sk-fake-oai"), ParkKind::Auth, 401, 3);
  d.http.script.push_back({"api.openai.com", "", 401, "{\"error\":{\"message\":\"invalid key\"}}"});
  TEST_ASSERT_FALSE(providers::runFabricLoop(pd, {"anthropic", "openai"}, "S", "U", out, err, rig.ht, nullptr, nullptr));
  TEST_ASSERT_EQUAL(1, (int)d.http.seen.size());
  TEST_ASSERT_EQUAL_STRING("api.openai.com", d.http.seen[0].host.c_str());
  providerPark().resume("anthropic", orch::kPauseOwner);
}

// Lane PARK: every billing refusal (each recorded message, in each provider's own error
// envelope), on every one of the fabric's hosts as the only host of the turn: once
// parked, later turns send nothing to it until its re-check, which is exactly one
// request; a new key is a new start.
struct BillingAnswer {
  int status;
  const char* message;
};
const BillingAnswer kBillingAnswers[] = {
    {400, "Your credit balance is too low to access the Anthropic API. Please go to Plans & Billing."},
    {429, "You exceeded your current quota, please check your plan and billing details."},
    {429, "You have no credits remaining. Add credits to continue using the API."},
    {402, "Payment Required"},
};
static std::string billingBody(const std::string& host, const char* message) {
  if (host == "mistral") return std::string("{\"message\":\"") + message + "\"}";
  return std::string("{\"error\":{\"message\":\"") + message + "\"}}";
}

static void test_fabric_never_asks_a_host_out_of_credit_outside_its_recheck() {
  struct Host { const char* name; const char* api; };
  const Host hosts[] = {{"anthropic", "api.anthropic.com"}, {"openai", "api.openai.com"}, {"mistral", "api.mistral.ai"}};
  int cases = 0;
  for (const Host& h : hosts) {
    for (const BillingAnswer& ba : kBillingAnswers) {
      const std::string body = billingBody(h.name, ba.message);
      const Refusal r{ba.status, body.c_str(), true, true};
      providerPark().clear();
      FakeProviderDeps d;
      providers::ProviderDeps pd = d.contract();
      FakeProviderDeps::ToolRig rig;
      d.fillTools(rig);
      std::string out, err;
      char what[192];
      snprintf(what, sizeof what, "%s: HTTP %d %s", h.name, r.status, r.body);
      d.http.script.push_back({h.api, "", r.status, r.body});
      TEST_ASSERT_FALSE_MESSAGE(providers::runFabricLoop(pd, {h.name}, "S", "U", out, err, rig.ht, nullptr, nullptr),
                                what);
      TEST_ASSERT_EQUAL_MESSAGE(1, (int)d.http.seen.size(), what);
      for (int t = 0; t < 3; t++) {
        TEST_ASSERT_FALSE_MESSAGE(providers::runFabricLoop(pd, {h.name}, "S", "U", out, err, rig.ht, nullptr, nullptr),
                                  what);
        TEST_ASSERT_TRUE_MESSAGE(orch::isParkedRefusal(err), what);
      }
      TEST_ASSERT_EQUAL_MESSAGE(1, (int)d.http.seen.size(), what);   // zero requests to the parked host
      d.now += providers::kParkProbeMs;   // the re-check: one request
      d.http.script.push_back({h.api, "", r.status, r.body});
      TEST_ASSERT_FALSE_MESSAGE(providers::runFabricLoop(pd, {h.name}, "S", "U", out, err, rig.ht, nullptr, nullptr),
                                what);
      TEST_ASSERT_FALSE_MESSAGE(providers::runFabricLoop(pd, {h.name}, "S", "U", out, err, rig.ht, nullptr, nullptr),
                                what);
      TEST_ASSERT_EQUAL_MESSAGE(2, (int)d.http.seen.size(), what);
      d.keys[h.name] = std::string("sk-new-") + h.name;   // a new key: asked at once
      d.http.script.push_back({h.api, "", r.status, r.body});
      providers::runFabricLoop(pd, {h.name}, "S", "U", out, err, rig.ht, nullptr, nullptr);
      TEST_ASSERT_EQUAL_MESSAGE(3, (int)d.http.seen.size(), what);
      cases++;
    }
  }
  printf("property: %d (host, billing refusal) cases, zero requests while parked, one per re-check\n", cases);
}

// ---- 6. told once: the notice, then one short answer, kept across a restart ------------

// The owner's view of one billing episode: the notice, then one short answer to a
// refused turn, then nothing, whatever seam or path asks; an answer or a new key
// ends the episode and both latches with it.
static void test_billing_turn_answer_is_notice_then_short_then_nothing() {
  providerPark().park("anthropic", PaidSeam::Chat, kFp, ParkKind::Billing, 402, 1000);
  TEST_ASSERT_EQUAL((int)orch::BillingTurnAnswer::Notice, (int)orch::billingTurnAnswer("anthropic", true));
  TEST_ASSERT_EQUAL((int)orch::BillingTurnAnswer::Short, (int)orch::billingTurnAnswer("anthropic", true));
  for (int i = 0; i < 5; i++)
    TEST_ASSERT_EQUAL((int)orch::BillingTurnAnswer::Nothing, (int)orch::billingTurnAnswer("anthropic", true));
  TEST_ASSERT_FALSE(orch::billingNoticeDue("anthropic"));   // the embedding seam stays quiet too
  orch::ParkEntry e[orch::ProviderPark::kMaxProviders];
  TEST_ASSERT_EQUAL(1, (int)providerPark().snapshot(e, orch::ProviderPark::kMaxProviders, 1001));
  TEST_ASSERT_TRUE(e[0].noticed);
  TEST_ASSERT_TRUE(e[0].answered);
  // A turn the owner did not write (a routine) never takes the short answer.
  providerPark().park("mistral", PaidSeam::Chat, kFp, ParkKind::Billing, 402, 1000);
  TEST_ASSERT_EQUAL((int)orch::BillingTurnAnswer::Notice, (int)orch::billingTurnAnswer("mistral", false));
  TEST_ASSERT_EQUAL((int)orch::BillingTurnAnswer::Nothing, (int)orch::billingTurnAnswer("mistral", false));
  TEST_ASSERT_EQUAL((int)orch::BillingTurnAnswer::Short, (int)orch::billingTurnAnswer("mistral", true));
  // Another provider's episode is its own.
  TEST_ASSERT_EQUAL((int)orch::BillingTurnAnswer::Notice, (int)orch::billingTurnAnswer("openai", true));
  // An answer ends the episode: notice, short answer, nothing again.
  const uint32_t due = 1000 + orch::ProviderPark::kRecheckMs;
  TEST_ASSERT_TRUE(providerPark().available("anthropic", PaidSeam::Chat, kFp, due).probe);
  TEST_ASSERT_TRUE(providerPark().noteSuccess("anthropic", PaidSeam::Chat));
  providerPark().park("anthropic", PaidSeam::Chat, kFp, ParkKind::Billing, 402, due + 10);
  TEST_ASSERT_EQUAL((int)orch::BillingTurnAnswer::Notice, (int)orch::billingTurnAnswer("anthropic", true));
  TEST_ASSERT_EQUAL((int)orch::BillingTurnAnswer::Short, (int)orch::billingTurnAnswer("anthropic", true));
  // A new key ends it too.
  TEST_ASSERT_TRUE(providerPark().available("anthropic", PaidSeam::Chat, kFp + 1, due + 20).keyChanged);
  TEST_ASSERT_EQUAL((int)orch::BillingTurnAnswer::Notice, (int)orch::billingTurnAnswer("anthropic", true));
  // The bench's clean slate ends it.
  TEST_ASSERT_EQUAL((int)orch::BillingTurnAnswer::Short, (int)orch::billingTurnAnswer("anthropic", true));
  providerPark().forgetParks(due + 30);
  TEST_ASSERT_EQUAL((int)orch::BillingTurnAnswer::Notice, (int)orch::billingTurnAnswer("anthropic", true));
}

// A restart in the middle of the episode neither repeats the notice nor the short
// answer (the file keeps both), and an image that predates the short answer reads
// the saved value as "told" (its file format is unchanged: 10 fields per record).
static void test_short_answer_survives_a_restart_and_old_images_read_it() {
  FakeFlash f;
  f.epoch = 1760000000;
  f.now = 5000;
  providerPark().setStorage(f.storage());
  providerPark().park("anthropic", PaidSeam::Chat, kFp, ParkKind::Billing, 400, 5000);
  TEST_ASSERT_EQUAL((int)orch::BillingTurnAnswer::Notice, (int)orch::billingTurnAnswer("anthropic", true));
  TEST_ASSERT_EQUAL((int)orch::BillingTurnAnswer::Short, (int)orch::billingTurnAnswer("anthropic", true));
  TEST_ASSERT_TRUE_MESSAGE(f.blob.find("anthropic 1 400 255 ") == 4, f.blob.c_str());
  unsigned kind = 0, mask = 0, noticed = 9;
  int status = 0;
  unsigned long inS = 0, ep = 0, since = 0, warned = 0, fp = 0;
  char prov[16] = {};
  TEST_ASSERT_EQUAL(10, std::sscanf(f.blob.c_str() + 4, "%15s %u %d %u %lu %lu %lu %u %lu %lx", prov, &kind, &status,
                                    &mask, &inS, &ep, &since, &noticed, &warned, &fp));
  TEST_ASSERT_EQUAL(2, (int)noticed);   // told, and answered once: non-zero, so an older image reads "told"
  f.epoch += 60;
  reboot(f, 2000);
  TEST_ASSERT_FALSE(open("anthropic", PaidSeam::Chat, 2000));
  TEST_ASSERT_EQUAL((int)orch::BillingTurnAnswer::Nothing, (int)orch::billingTurnAnswer("anthropic", true));
  // A file an older image wrote (told, value 1): the short answer is still due once.
  providerPark().clear();
  TEST_ASSERT_TRUE(providerPark().deserialize("PK1\nopenai 1 429 255 600 0 0 1 0 00001234\n", 2000));
  TEST_ASSERT_EQUAL((int)orch::BillingTurnAnswer::Short, (int)orch::billingTurnAnswer("openai", true));
  TEST_ASSERT_EQUAL((int)orch::BillingTurnAnswer::Nothing, (int)orch::billingTurnAnswer("openai", true));
}

// The wait the short answer names is read without handing out the re-check request.
static void test_recheck_in_reads_without_taking_the_probe() {
  providerPark().park("mistral", PaidSeam::Chat, kFp, ParkKind::Billing, 402, 1000);
  TEST_ASSERT_EQUAL(0u, providerPark().recheckInMs("openai"));   // not parked
  TEST_ASSERT_TRUE(providerPark().available("mistral", PaidSeam::Chat, kFp, 1000 + 4 * 60000).open == false);
  const uint32_t left = providerPark().recheckInMs("mistral");
  TEST_ASSERT_EQUAL(orch::ProviderPark::kRecheckMs - 4 * 60000, left);
  TEST_ASSERT_FALSE(providerPark().available("mistral", PaidSeam::Chat, kFp, 1000 + orch::ProviderPark::kRecheckMs - 1).open);
  TEST_ASSERT_EQUAL(1u, providerPark().recheckInMs("mistral"));
  // Due: the read says 0 and the next question still gets the probe.
  TEST_ASSERT_FALSE(providerPark().available("mistral", PaidSeam::Chat, kFp, 1000 + orch::ProviderPark::kRecheckMs - 1).probe);
}

// The short answer, for every provider and every wait: it says the message went
// unanswered and when the provider is asked again, and carries none of the words the
// notice did (credit, billing, quota, payment, paused, plan): one notice per episode.
static void test_short_answer_words_carry_no_second_notice() {
  const char* const words[] = {"credit", "billing", "quota", "payment", "paused", "plan"};
  int cases = 0;
  for (const char* label : {"Anthropic", "OpenAI", "Mistral", "Z.ai", "Cumulo Nimbus", "your custom endpoint"}) {
    for (uint32_t min = 0; min <= 31; min++) {
      const std::string t = orch::billingShortAnswer(label, min * 60000u);
      std::string low;
      for (char c : t) low += (char)std::tolower((unsigned char)c);
      for (const char* w : words) TEST_ASSERT_TRUE_MESSAGE(low.find(w) == std::string::npos, t.c_str());
      TEST_ASSERT_TRUE_MESSAGE(t.rfind("Your message was not answered.", 0) == 0, t.c_str());
      TEST_ASSERT_TRUE_MESSAGE(t.find(label) != std::string::npos, t.c_str());
      for (unsigned char c : t) TEST_ASSERT_TRUE_MESSAGE(c >= 0x20 && c < 0x7f, t.c_str());   // plain ASCII
      cases++;
    }
  }
  TEST_ASSERT_EQUAL_STRING("Your message was not answered. Anthropic is checked again in 27 minutes, and replies "
                           "resume when it accepts requests.",
                           orch::billingShortAnswer("Anthropic", 26 * 60000 + 1).c_str());
  TEST_ASSERT_EQUAL_STRING("Your message was not answered. Mistral is checked again in a minute, and replies resume "
                           "when it accepts requests.",
                           orch::billingShortAnswer("Mistral", 30000).c_str());
  printf("property: %d (provider, wait) short answers, none a second notice\n", cases);
}

// Every harness request feeds the park: a sub-agent dispatch refused for billing
// (here Mistral's 402) closes mistral for every seam, though no seam asked first.
static void test_sub_dispatch_refusal_parks_for_every_seam() {
  FakeProviderDeps d;
  d.http.script.push_back({"api.mistral.ai", "", 402, "{\"message\":\"Payment Required\"}"});
  providers::ProviderDeps pd = d.contract();
  agent::Directive dir;
  dir.instruction = "do it";
  char jobId[72] = {};
  providers::mistralDispatch(pd, "mistral-small-latest", dir, jobId);
  for (int s = 0; s < orch::kPaidSeamCount; s++) TEST_ASSERT_FALSE(open("mistral", seamAt(s), 100, 0));
  TEST_ASSERT_TRUE(LogCapture::contains("provider: mistral parked for every seam (billing, HTTP 402)"));
}


// The probe's lease: the seam that got the re-check may ask again while it builds the
// request, but a seam that never reports its answer cannot leave the provider open -
// past the lease the park closes and waits a whole re-check again.
static void test_probe_lease_and_a_seam_that_never_reports() {
  providerPark().park("openai", PaidSeam::Embeddings, kFp, ParkKind::Billing, 429, 1000);
  const uint32_t t = 1000 + orch::ProviderPark::kRecheckMs;
  TEST_ASSERT_TRUE(providerPark().available("openai", PaidSeam::Spawn, kFp, t).probe);
  const orch::ParkVerdict again = providerPark().available("openai", PaidSeam::Spawn, kFp, t + 10);
  TEST_ASSERT_TRUE(again.open);   // check, then call: still the probe's lease
  TEST_ASSERT_FALSE(again.probe);
  const uint32_t lapsed = t + orch::ProviderPark::kProbeLeaseMs + 1;
  TEST_ASSERT_FALSE(open("openai", PaidSeam::Chat, lapsed));   // nobody reported: closed again
  TEST_ASSERT_FALSE(open("openai", PaidSeam::Vision, lapsed + orch::ProviderPark::kRecheckMs - 1000));
  TEST_ASSERT_TRUE(providerPark().available("openai", PaidSeam::Chat, kFp, lapsed + orch::ProviderPark::kRecheckMs).probe);
}

// The first refusal of a kind names it: a later 403 never relabels a 401 park, and a
// billing park is never downgraded by a key refusal.
static void test_first_refusal_names_the_park() {
  providerPark().park("mistral", PaidSeam::Chat, kFp, ParkKind::Auth, 401, 1000);
  providerPark().noteAnswer("mistral", PaidSeam::Vision, kFp, 403, "not allowed", 1100);
  TEST_ASSERT_EQUAL(401, providerPark().available("mistral", PaidSeam::Chat, kFp, 1200).httpStatus);
  providerPark().noteAnswer("mistral", PaidSeam::Chat, kFp, 402, "{}", 1300);
  providerPark().noteAnswer("mistral", PaidSeam::Chat, kFp, 401, "bad key", 1400);
  const orch::ParkVerdict v = providerPark().available("mistral", PaidSeam::Chat, kFp, 1500);
  TEST_ASSERT_EQUAL((int)ParkKind::Billing, (int)v.kind);
  TEST_ASSERT_EQUAL(402, v.httpStatus);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_property_seam_x_refusal_kind);
  RUN_TEST(test_slugs_and_words);
  RUN_TEST(test_recheck_probe_requeue_and_success);
  RUN_TEST(test_seam_only_park_ends_only_on_its_seam);
  RUN_TEST(test_probe_lease_and_a_seam_that_never_reports);
  RUN_TEST(test_first_refusal_names_the_park);
  RUN_TEST(test_changed_key_is_a_new_start);
  RUN_TEST(test_park_and_latch_survive_a_restart);
  RUN_TEST(test_restart_without_clock_keeps_the_saved_wait);
  RUN_TEST(test_storage_not_ready_is_retried);
  RUN_TEST(test_garbage_file_is_tolerated);
  RUN_TEST(test_daily_warning_latch);
  RUN_TEST(test_pause_switch_closes_every_seam_and_is_never_probed);
  RUN_TEST(test_pause_is_ram_only_and_survives_a_load);
  RUN_TEST(test_forget_parks_is_saved_and_keeps_pauses);
  RUN_TEST(test_records_live_in_the_memory_the_device_gives);
  RUN_TEST(test_embedding_refusal_closes_vision_spawn_and_chat);
  RUN_TEST(test_rate_limit_on_one_seam_parks_nothing);
  RUN_TEST(test_spawn_refused_when_every_provider_is_closed);
  RUN_TEST(test_paused_fabric_sends_nothing);
  RUN_TEST(test_fabric_never_asks_a_host_out_of_credit_outside_its_recheck);
  RUN_TEST(test_billing_turn_answer_is_notice_then_short_then_nothing);
  RUN_TEST(test_short_answer_survives_a_restart_and_old_images_read_it);
  RUN_TEST(test_recheck_in_reads_without_taking_the_probe);
  RUN_TEST(test_short_answer_words_carry_no_second_notice);
  RUN_TEST(test_sub_dispatch_refusal_parks_for_every_seam);
  return UNITY_END();
}
