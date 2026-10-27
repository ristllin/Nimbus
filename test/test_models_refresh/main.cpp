#include <unity.h>

#include <ArduinoJson.h>

#include <map>
#include <string>
#include <vector>

#include "nimbus/orch/model_catalog.h"
#include "nimbus/orch/model_refresh.h"
#include "nimbus/cloud/relay_heap.h"
#include "nimbus/orch/models_tool.h"
#include "nimbus/orch/provider_park.h"
#include "nimbus/orch/provider_slots.h"
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/tool_taint.h"

// R5 refresh policy (model_refresh.h): one RAM queue for every refresh request
// (the head's models.list refresh:true and orch_model misses, the owner's
// dropdown, the passive check), at most one refresh per provider per hour, started
// only between turns, one provider at a time; the tool itself never fetches; the
// passive check reads the firmware version from the model file's header line.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

static const uint32_t kHour = ModelRefreshQueue::kMinGapMs;

// ---- the queue: one refresh per provider per hour --------------------------

// The class rule over every provider slot: N requests inside the hour queue one
// refresh, which starts once; the next request inside the hour is Recent; the
// first one after the hour queues again.
static void test_n_requests_in_an_hour_queue_exactly_one_refresh_per_provider() {
  for (const ProviderSlot& slot : kProviderSlots) {
    ModelRefreshQueue q;
    uint32_t now = 5000;
    int queued = 0, starts = 0;
    for (int i = 0; i < 30; ++i) {
      now += 100000;   // 30 requests over 50 minutes
      if (q.request(slot.slug, true, now) == RefreshAsk::Queued) queued++;
      if (i == 4 || i == 20) {   // the main loop serves the queue twice in that span
        while (const char* due = q.due(false, true, false, true, now)) {
          q.started(due, now);
          starts++;
        }
      }
    }
    TEST_ASSERT_EQUAL_MESSAGE(1, queued, slot.slug);
    TEST_ASSERT_EQUAL_MESSAGE(1, starts, slot.slug);
    TEST_ASSERT_EQUAL(RefreshAsk::Recent, q.request(slot.slug, true, now));
    uint32_t at = 0;
    TEST_ASSERT_TRUE(q.lastStart(slot.slug, at));
    TEST_ASSERT_EQUAL(RefreshAsk::Queued, q.request(slot.slug, true, at + kHour));   // the window ends
  }
}

// Requests for several providers queue one each and start one at a time, taking
// turns in registry order; nothing starts twice.
static void test_providers_start_one_at_a_time_in_turn() {
  ModelRefreshQueue q;
  for (const ProviderSlot& slot : kProviderSlots)
    TEST_ASSERT_EQUAL(RefreshAsk::Queued, q.request(slot.slug, true, 1000));
  std::vector<std::string> order;
  uint32_t now = 2000;
  while (const char* due = q.due(false, true, false, true, now)) {
    order.push_back(due);
    q.started(due, now += 1000);
  }
  TEST_ASSERT_EQUAL((int)kProviderSlotCount, (int)order.size());
  for (size_t i = 0; i < kProviderSlotCount; ++i)
    TEST_ASSERT_EQUAL_STRING(kProviderSlots[i].slug, order[i].c_str());
}

// Never during a turn, offline, beside a running verify, or below the heap floor;
// a refresh that could not start stays queued and a provider with no key or an
// unknown slug never queues.
static void test_due_only_when_the_device_is_free() {
  ModelRefreshQueue q;
  TEST_ASSERT_EQUAL(RefreshAsk::Queued, q.request("anthropic", true, 1000));
  TEST_ASSERT_EQUAL(RefreshAsk::Pending, q.request("anthropic", true, 2000));
  TEST_ASSERT_NULL(q.due(true, true, false, true, 2000));    // a turn is in flight
  TEST_ASSERT_NULL(q.due(false, false, false, true, 2000));  // offline
  TEST_ASSERT_NULL(q.due(false, true, true, true, 2000));    // the verify slot is busy
  TEST_ASSERT_NULL(q.due(false, true, false, false, 2000));  // resting heap below the floor
  TEST_ASSERT_TRUE(q.queued("anthropic"));
  TEST_ASSERT_EQUAL_STRING("anthropic", q.due(false, true, false, true, 2000));
  TEST_ASSERT_EQUAL(RefreshAsk::NoKey, q.request("openai", false, 3000));
  TEST_ASSERT_EQUAL(RefreshAsk::NoKey, q.request("custom", true, 3000));
  TEST_ASSERT_EQUAL(RefreshAsk::NoKey, q.request(nullptr, true, 3000));
  TEST_ASSERT_FALSE(q.queued("openai"));
}

// The window survives the millis() wrap.
static void test_the_rate_window_survives_the_clock_wrap() {
  ModelRefreshQueue q;
  const uint32_t nearWrap = 0xFFFFFFFFu - 1000u;
  TEST_ASSERT_EQUAL(RefreshAsk::Queued, q.request("zai", true, nearWrap));
  q.started("zai", nearWrap);
  TEST_ASSERT_EQUAL(RefreshAsk::Recent, q.request("zai", true, 5000));   // wrapped, 6 s later
  TEST_ASSERT_EQUAL(RefreshAsk::Queued, q.request("zai", true, nearWrap + kHour));
}

// ---- the tool never fetches: refresh:true only sets the flag ------------------

// The tool runs inside a turn: refresh:true answers from the cache at once and only
// queues; the fetch (the verify task, the only code that opens a connection) runs
// when the main loop finds the device free after the turn.
static void test_never_a_fetch_inside_the_tool() {
  ModelRefreshQueue q;
  uint32_t now = 1000;
  bool inTurn = true;
  int fetches = 0;
  auto pump = [&] {
    if (const char* due = q.due(inTurn, true, false, true, now)) {
      q.started(due, now);
      fetches++;
    }
  };
  ModelsToolHandlers h;
  h.keyedProviders = [] { return std::vector<std::string>{"anthropic"}; };
  h.load = [](const std::string& p, ProviderModels& out) {
    out.provider = p;
    out.fetched = true;
    out.fetchedAt = 1792454400;   // 2026-10-20 00:00Z
    ModelRow r;
    r.id = "claude-sonnet-5-5";
    out.rows.push_back(r);
    return true;
  };
  h.requestRefresh = [&](const std::string& p) { return q.request(p.c_str(), true, now); };
  ToolRegistry reg;
  registerModelsTool(reg, h);
  JsonDocument a;
  a["provider"] = "anthropic";
  a["refresh"] = true;
  Principal who;
  who.ns = "1001";
  ToolResult r = reg.dispatch("models.list", a.as<ArduinoJson::JsonObjectConst>(), who);
  TEST_ASSERT_TRUE(r.success);
  TEST_ASSERT_TRUE(r.output.find("\"refresh\":\"queued\"") != std::string::npos);
  TEST_ASSERT_TRUE(r.output.find("\"fetchedAt\":\"2026-10-20T00:00Z\"") != std::string::npos);
  TEST_ASSERT_TRUE(r.output.find("claude-sonnet-5-5") != std::string::npos);
  pump();                      // still inside the turn
  TEST_ASSERT_EQUAL(0, fetches);
  r = reg.dispatch("models.list", a.as<ArduinoJson::JsonObjectConst>(), who);   // asked again
  TEST_ASSERT_TRUE(r.output.find("\"refresh\":\"pending\"") != std::string::npos);
  inTurn = false;              // the turn ended
  pump();
  pump();
  TEST_ASSERT_EQUAL(1, fetches);
  now += 60000;
  inTurn = true;
  r = reg.dispatch("models.list", a.as<ArduinoJson::JsonObjectConst>(), who);
  TEST_ASSERT_TRUE(r.output.find("\"refresh\":\"recent\"") != std::string::npos);
}

// The rows are the providers' own text (ids, names; the router's from its upstreams):
// a turn that read them has read outside content, so the leaked-markup repair and the
// other taint rails treat it as such. It changes nothing on the device.
static void test_models_list_output_is_outside_text() {
  TEST_ASSERT_EQUAL(int(Taint::External), int(toolTaint("models.list")));
  TEST_ASSERT_EQUAL(int(ToolEffect::Read), int(toolEffect("models.list")));
}

// ---- the stamp in the model file header and the passive refresh ---------------

// A model store as lane F writes it (model_catalog.h serializeModelStore): the header
// line, then rows. The passive refresh reads only the header.
static std::string store(uint32_t fetchedAt, const std::string& fw) {
  ModelStoreMeta meta;
  meta.fetchedAt = fetchedAt;
  meta.fw = fw;
  ModelInfo m = classifyCatalogEntry("anthropic", "claude-sonnet-5-5");
  m.name = "Claude Sonnet 5.5";
  return serializeModelStore(meta, std::vector<ModelInfo>{m});
}

// The stamp is lane F's store header: fetch time and the firmware version that wrote it.
static void test_stamp_reads_the_model_store_header() {
  ModelsStamp s = stampFromStore(store(1792454400, "v4.6.0"));
  TEST_ASSERT_TRUE(s.present);
  TEST_ASSERT_EQUAL_UINT32(1792454400u, s.fetchedAt);
  TEST_ASSERT_EQUAL_STRING("v4.6.0", s.fw.c_str());
  // Keys the header gains later, in any order, do not matter.
  s = stampFromStore("#models v1 total=3 fw=v5.0.0 more=0 fetchedAt=7 cut=0 src=api\n");
  TEST_ASSERT_TRUE(s.present);
  TEST_ASSERT_EQUAL_UINT32(7u, s.fetchedAt);
  TEST_ASSERT_EQUAL_STRING("v5.0.0", s.fw.c_str());
  // No store, or text with no header: no stamp.
  TEST_ASSERT_FALSE(stampFromStore("").present);
  TEST_ASSERT_FALSE(stampFromStore("claude-sonnet-5-5\tx\n").present);
}

// The passive refresh decides from the store header alone: the firmware version that
// wrote the list and when it was fetched. Every reason, plus the counter-cases.
static void test_passive_refresh_reads_the_firmware_from_the_file_header() {
  const uint32_t now = 1792540800;   // 2026-10-21
  const std::string fw = "v4.6.0";
  struct Case { std::string file; uint32_t now; PassiveWhy want; };
  const Case cases[] = {
      {"claude-x\t...\n", now, PassiveWhy::NoStamp},                          // no header
      {"", now, PassiveWhy::NoStamp},                                         // no store
      {store(now - 3600, "v4.5.16"), now, PassiveWhy::Firmware},              // after an update
      {store(now - 3600, "v4.5.16"), 0, PassiveWhy::Firmware},                // even with no clock
      {store(now - kPassiveMaxAgeS, fw), now, PassiveWhy::Stale},             // a week old
      {store(now - kPassiveMaxAgeS + 60, fw), now, PassiveWhy::None},         // not yet
      {store(0, fw), now, PassiveWhy::UnknownAge},                            // fetched clockless
      {store(0, fw), 0, PassiveWhy::None},                                    // still clockless
      {store(now - 60, fw), 0, PassiveWhy::None},
      {store(now + 3600, fw), now, PassiveWhy::None},                         // a clock step back
      {"#models v1 fetchedAt=0 fw=v4.6.0 src=legacy\n", now, PassiveWhy::Legacy},   // the boot merge
      {"#models v1 fetchedAt=0 fw=v4.6.0 src=legacy\n", 0, PassiveWhy::Legacy},     // ...no clock needed
      {"#models v1 fetchedAt=0 fw=v4.5.16 src=legacy\n", 0, PassiveWhy::Firmware},
  };
  for (const Case& c : cases)
    TEST_ASSERT_EQUAL_STRING_MESSAGE(passiveWhyToken(c.want),
                                     passiveWhyToken(passiveRefreshDue(stampFromStore(c.file), fw, c.now)),
                                     c.file.substr(0, 60).c_str());
}

// After an update the passive check queues each keyed provider once; the store a
// refresh writes then carries the running firmware and nothing more is due.
static void test_after_an_update_each_provider_refreshes_once() {
  const uint32_t now = 1792540800;
  const std::string fw = "v4.6.0";
  std::map<std::string, std::string> files;   // the model store per provider
  for (const ProviderSlot& slot : kProviderSlots) files[slot.slug] = store(now - 7200, "v4.5.16");
  ModelRefreshQueue q;
  uint32_t ms = 600000;
  int starts = 0;
  for (int pass = 0; pass < 3; ++pass) {   // the passive check runs again later
    for (auto& f : files)
      if (passiveRefreshDue(stampFromStore(f.second), fw, now) != PassiveWhy::None)
        q.request(f.first.c_str(), true, ms);
    while (const char* due = q.due(false, true, false, true, ms)) {
      q.started(due, ms += 5000);
      files[due] = store(now, fw);   // the refresh writes a new store
      starts++;
    }
    ms += 6u * kHour;
  }
  TEST_ASSERT_EQUAL((int)kProviderSlotCount, starts);
}

// ---- the 26 Oct production defect: an updated device never refreshed ----------

// What Lumi's model store held after the v4.5.16 -> v4.6.0 update: the boot merge of
// the older build's NVS short lists (src=legacy, fetchedAt 0, stamped with the running
// firmware) for four providers, and no store at all for Mistral (no older list).
static std::map<std::string, std::string> lumiAfterTheUpdate(const std::string& fw) {
  std::map<std::string, std::string> files;
  files["anthropic"] = mergeLegacyLists("anthropic", "", "", "claude-opus-5,claude-sonnet-5,claude-haiku-5-5", fw);
  files["openai"] = mergeLegacyLists("openai", "", "", "gpt-5.6-sol,gpt-5.6-luna", fw);
  files["zai"] = mergeLegacyLists("zai", "", "", "glm-5.3,glm-5.3-flash", fw);
  files["cumulo"] = mergeLegacyLists("cumulo", "", "", "openai/gpt-5.6-sol,mistral/mistral-large-latest", fw);
  files["mistral"] = "";
  return files;
}

// The gate that kept every refresh queued: the resting largest block against the
// verify's 8,000 B. Lumi rests at 7,156 B (7,668 B on v4.5.16) and opens every turn's
// connection there; a refresh takes the same one-connection floor. Counter-test: a
// heap below that floor (where a handshake cannot run) still holds the refresh back.
static void test_the_refresh_floor_is_the_one_connection_floor() {
  TEST_ASSERT_EQUAL_UINT32((uint32_t)nimbus::cloud::kRelayHeapFloorLargest, kRefreshMinLargest);
  TEST_ASSERT_TRUE(kRefreshMinLargest < 8000u);   // the verify's floor parked Lumi's refreshes
  TEST_ASSERT_TRUE(refreshHeapOk(7156));          // Lumi on v4.6.0, resting
  TEST_ASSERT_TRUE(refreshHeapOk(7668));          // Lumi on v4.5.16, resting
  TEST_ASSERT_TRUE(refreshHeapOk(kRefreshMinLargest));
  TEST_ASSERT_FALSE(refreshHeapOk(kRefreshMinLargest - 1));
  TEST_ASSERT_FALSE(refreshHeapOk(0));
}

// The replay: Lumi's stores after the update, its resting heap, its clock (set, and a
// boot whose clock is not set yet). Every keyed provider is wanted by the passive check
// and starts once, one at a time, between turns; once each wrote a fetched list nothing
// more is due. On the old gate (8,000 B) not one would have started.
static void test_an_updated_device_with_a_fragmented_heap_refreshes_every_list() {
  const std::string fw = "v4.6.0";
  for (uint32_t clock : {1792540800u, 0u}) {
    std::map<std::string, std::string> files = lumiAfterTheUpdate(fw);
    TEST_ASSERT_TRUE(stampFromStore(files["anthropic"]).present);
    TEST_ASSERT_EQUAL_STRING("legacy", stampFromStore(files["anthropic"]).src.c_str());
    ModelRefreshQueue q;
    uint32_t ms = 600000;   // the first passive pass, 10 min after boot
    for (auto& f : files)
      if (passiveWants(true, 1, stampFromStore(f.second), fw, clock)) q.request(f.first.c_str(), true, ms);
    TEST_ASSERT_NULL_MESSAGE(q.due(false, true, false, 7156 >= 8000u, ms), "the old gate started one");
    int starts = 0;
    while (const char* due = q.due(false, true, false, refreshHeapOk(7156), ms)) {
      q.started(due, ms += 1000);
      ModelStoreMeta meta;
      meta.fetchedAt = clock;
      meta.fw = fw;
      files[due] = serializeModelStore(meta, std::vector<ModelInfo>{classifyCatalogEntry(due, "m-1")});
      q.finished(due, passiveWants(true, 1, stampFromStore(files[due]), fw, clock), ms);
      starts++;
    }
    TEST_ASSERT_EQUAL_MESSAGE((int)files.size(), starts, clock ? "clock set" : "no clock");
    for (auto& f : files)
      TEST_ASSERT_FALSE_MESSAGE(passiveWants(true, 1, stampFromStore(f.second), fw, clock), f.first.c_str());
  }
}

// A refresh that ran but left the list due (no answer, a cut list, a failed write) is
// tried again on a growing delay - 2, 4, 8, 16, 32, 60 minutes - not after the hourly
// window plus the next 6-hour passive pass; never before its time; after six retries it is
// left to the passive pass; a landed refresh ends the streak. A request meanwhile answers
// Pending.
static void test_a_failed_refresh_retries_with_backoff() {
  ModelRefreshQueue q;
  uint32_t now = 1000;
  TEST_ASSERT_EQUAL(RefreshAsk::Queued, q.request("mistral", true, now));
  const uint32_t want[] = {2, 4, 8, 16, 32, 60};
  for (uint32_t minutes : want) {
    TEST_ASSERT_EQUAL_STRING("mistral", q.due(false, true, false, true, now));
    q.started("mistral", now);
    TEST_ASSERT_EQUAL_UINT32(minutes * 60000u, q.finished("mistral", true, now + 5000));
    TEST_ASSERT_TRUE(q.queued("mistral"));
    TEST_ASSERT_EQUAL(RefreshAsk::Pending, q.request("mistral", true, now + 6000));
    TEST_ASSERT_NULL(q.due(false, true, false, true, now + 5000 + minutes * 60000u - 1));
    now += 5000 + minutes * 60000u;
  }
  TEST_ASSERT_EQUAL_STRING("mistral", q.due(false, true, false, true, now));
  q.started("mistral", now);
  TEST_ASSERT_EQUAL_UINT32(0u, q.finished("mistral", true, now + 5000));   // a 7th failure: given up
  TEST_ASSERT_FALSE(q.queued("mistral"));
  now += 6u * kHour;
  TEST_ASSERT_EQUAL(RefreshAsk::Queued, q.request("mistral", true, now));   // the passive pass
  TEST_ASSERT_EQUAL_STRING("mistral", q.due(false, true, false, true, now));
  q.started("mistral", now);
  TEST_ASSERT_EQUAL_UINT32(0u, q.finished("mistral", false, now + 5000));   // landed
  TEST_ASSERT_FALSE(q.queued("mistral"));
  TEST_ASSERT_EQUAL(RefreshAsk::Recent, q.request("mistral", true, now + 6000));   // the hour holds again
  TEST_ASSERT_EQUAL(RefreshAsk::Queued, q.request("mistral", true, now + kHour));
  q.started("mistral", now + kHour);
  TEST_ASSERT_EQUAL_UINT32(2u * 60000u, q.finished("mistral", true, now + kHour));   // a new streak
  // A waiting retry does not hold up another provider's refresh.
  TEST_ASSERT_EQUAL(RefreshAsk::Queued, q.request("openai", true, now + kHour));
  TEST_ASSERT_EQUAL_STRING("openai", q.due(false, true, false, true, now + kHour + 1));
}

// The list GET is free: a provider paused by its monthly limit or parked by a billing
// or key refusal - closed on every paid seam - is still wanted and started (prism
// row 3). Only a rejected key (verdict 0) and a missing key stop it.
static void test_a_paused_or_parked_provider_still_refreshes_its_list() {
  const std::string fw = "v4.6.0";
  const ModelsStamp legacy = stampFromStore(lumiAfterTheUpdate(fw)["anthropic"]);
  ProviderPark park;
  park.pause("anthropic", kPauseProviderLimit);
  park.park("openai", PaidSeam::Chat, 0, ParkKind::Billing, 429, 1000);
  for (const char* p : {"anthropic", "openai"}) {
    for (int s = 0; s < kPaidSeamCount; ++s)
      TEST_ASSERT_FALSE_MESSAGE(park.available(p, (PaidSeam)s, 0, 2000).open, p);
    TEST_ASSERT_TRUE_MESSAGE(passiveWants(true, 1, legacy, fw, 1792540800u), p);
    TEST_ASSERT_TRUE_MESSAGE(passiveWants(true, -1, legacy, fw, 1792540800u), p);   // never verified
    ModelRefreshQueue q;
    TEST_ASSERT_EQUAL(RefreshAsk::Queued, q.request(p, true, 3000));
    TEST_ASSERT_EQUAL_STRING(p, q.due(false, true, false, refreshHeapOk(7156), 3000));
  }
  TEST_ASSERT_FALSE(passiveWants(true, 0, legacy, fw, 1792540800u));    // a rejected key
  TEST_ASSERT_FALSE(passiveWants(false, 1, legacy, fw, 1792540800u));   // no key
}

// Where a due refresh runs (the reviewer's catch: the 5,000 B gate alone would still
// leave Lumi with no refresh, since its own worker's 8 KB stack needs one internal block
// and Lumi's largest is 7,156 B). Its own task when that block and the spawn margin are
// there; else the Telegram poll task at a turn's floors; else it waits, never during a
// turn. Lumi's own figures: 32,308 B free, 7,156 B largest at rest.
static void test_the_refresh_runs_where_it_fits() {
  using R = RefreshRoute;
  struct Case { bool inTurn, poll; uint32_t freeB, live, resting; R want; const char* what; };
  const Case cases[] = {
      {false, true, 32308, 7156, 7156, R::Poll, "Lumi at rest: no 8 KB block, the poll task"},
      {false, false, 32308, 7156, 7156, R::Wait, "Lumi with no poll task: waits"},
      {false, true, 44000, 14324, 14324, R::Task, "a bench unit at rest: its own task"},
      {false, false, 44000, 14324, 14324, R::Task, "its own task needs no poll task"},
      {true, true, 44000, 14324, 14324, R::Wait, "never during a turn"},
      {false, true, 44000, 14324, 4999, R::Wait, "below the one-connection floor"},
      {false, true, 20000, 14324, 14324, R::Poll, "free under the spawn margin: the poll task"},
      {false, true, 11999, 7156, 7156, R::Wait, "under a turn's total-free floor"},
      {false, true, 44000, kRefreshTaskStackBytes + kRefreshTaskSlackBytes - 1, 9000, R::Poll,
       "a block just short of the stack"},
      {false, true, 44000, kRefreshTaskStackBytes + kRefreshTaskSlackBytes, 9000, R::Task, "a block that fits"},
  };
  for (const Case& c : cases)
    TEST_ASSERT_EQUAL_STRING_MESSAGE(refreshRouteToken(c.want),
                                     refreshRouteToken(refreshRoute(c.inTurn, c.poll, c.freeB, c.live, c.resting)),
                                     c.what);
}

// A key saved or verified again (a "verify" ask) may change the list: it skips the
// hourly window and a waiting retry's delay; any other ask keeps both.
static void test_a_key_verify_skips_the_hour_and_the_retry_delay() {
  ModelRefreshQueue q;
  q.request("mistral", true, 1000);
  q.started("mistral", 1000);
  q.finished("mistral", false, 2000);
  TEST_ASSERT_EQUAL(RefreshAsk::Recent, q.request("mistral", true, 3000));
  TEST_ASSERT_EQUAL(RefreshAsk::Queued, q.request("mistral", true, 3000, /*force=*/true));
  q.started("mistral", 3000);
  q.finished("mistral", true, 4000);   // failed: waits 2 minutes
  TEST_ASSERT_NULL(q.due(false, true, false, true, 5000));
  TEST_ASSERT_EQUAL(RefreshAsk::Pending, q.request("mistral", true, 5000));
  TEST_ASSERT_NULL(q.due(false, true, false, true, 5000));
  TEST_ASSERT_EQUAL(RefreshAsk::Pending, q.request("mistral", true, 5000, /*force=*/true));
  TEST_ASSERT_EQUAL_STRING("mistral", q.due(false, true, false, true, 5000));
}

// HFM review, LOW: a list that never lands (cut by the read deadline over a legacy merge, an
// empty answer, a router that refuses) is tried six times (2 .. 60 min) and then left to the
// next 6-hourly passive pass, never retried hourly for the whole uptime.
static void test_a_refresh_that_never_lands_gives_up_after_six_tries() {
  ModelRefreshQueue q;
  uint32_t now = 1000;
  q.request("cumulo", true, now);
  int starts = 0;
  for (int hour = 0; hour < 24 * 60; ++hour, now += 60000) {
    if (const char* due = q.due(false, true, false, true, now)) {
      q.started(due, now);
      ++starts;
      q.finished(due, true, now + 2000);
    }
  }
  TEST_ASSERT_EQUAL(1 + 6, starts);   // the first try and six retries, in a day
  TEST_ASSERT_FALSE(q.queued("cumulo"));
  TEST_ASSERT_EQUAL(RefreshAsk::Queued, q.request("cumulo", true, now));   // the passive pass asks again
}

// A refresh waiting out its retry delay is not under way: the owner's page does not
// poll on it (pending), and it is queued again once its time comes.
static void test_a_waiting_retry_is_not_pending() {
  ModelRefreshQueue q;
  q.request("anthropic", true, 1000);
  q.started("anthropic", 1000);
  q.finished("anthropic", true, 2000);
  TEST_ASSERT_TRUE(q.queued("anthropic"));
  TEST_ASSERT_TRUE(q.retryWaiting("anthropic", 2000 + ModelRefreshQueue::kRetryFirstMs - 1));
  TEST_ASSERT_FALSE(q.retryWaiting("anthropic", 2000 + ModelRefreshQueue::kRetryFirstMs));
  TEST_ASSERT_FALSE(q.retryWaiting("openai", 2000));
}

// ---- the texts ----------------------------------------------------------------

static void test_miss_texts() {
  TEST_ASSERT_EQUAL_STRING("not in anthropic list cached 2026-10-20; refresh queued",
                           modelMissText("anthropic", 1792454400, RefreshAsk::Queued).c_str());
  TEST_ASSERT_EQUAL_STRING("not in anthropic list cached 2026-10-20; refresh queued",
                           modelMissText("anthropic", 1792454400 + 86399, RefreshAsk::Pending).c_str());
  TEST_ASSERT_EQUAL_STRING("not in openai list cached (date unknown); refresh queued",
                           modelMissText("openai", 0, RefreshAsk::Queued).c_str());
  TEST_ASSERT_TRUE(modelMissText("zai", 1792454400, RefreshAsk::Recent).find("refreshed within the hour") !=
                   std::string::npos);
  TEST_ASSERT_EQUAL_STRING("2026-10-20T14:05Z", utcMinute(1792454400 + 14 * 3600 + 5 * 60).c_str());
  TEST_ASSERT_EQUAL_STRING("", utcDate(0).c_str());
  TEST_ASSERT_EQUAL_STRING("2024-02-29", utcDate(1709164800).c_str());   // a leap day
  TEST_ASSERT_EQUAL_STRING("queued", refreshAskToken(RefreshAsk::Queued));
  TEST_ASSERT_EQUAL_STRING("no-key", refreshAskToken(RefreshAsk::NoKey));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_n_requests_in_an_hour_queue_exactly_one_refresh_per_provider);
  RUN_TEST(test_providers_start_one_at_a_time_in_turn);
  RUN_TEST(test_due_only_when_the_device_is_free);
  RUN_TEST(test_the_rate_window_survives_the_clock_wrap);
  RUN_TEST(test_never_a_fetch_inside_the_tool);
  RUN_TEST(test_models_list_output_is_outside_text);
  RUN_TEST(test_stamp_reads_the_model_store_header);
  RUN_TEST(test_passive_refresh_reads_the_firmware_from_the_file_header);
  RUN_TEST(test_after_an_update_each_provider_refreshes_once);
  RUN_TEST(test_the_refresh_floor_is_the_one_connection_floor);
  RUN_TEST(test_an_updated_device_with_a_fragmented_heap_refreshes_every_list);
  RUN_TEST(test_a_failed_refresh_retries_with_backoff);
  RUN_TEST(test_a_paused_or_parked_provider_still_refreshes_its_list);
  RUN_TEST(test_the_refresh_runs_where_it_fits);
  RUN_TEST(test_a_waiting_retry_is_not_pending);
  RUN_TEST(test_a_key_verify_skips_the_hour_and_the_retry_delay);
  RUN_TEST(test_a_refresh_that_never_lands_gives_up_after_six_tries);
  RUN_TEST(test_miss_texts);
  return UNITY_END();
}
