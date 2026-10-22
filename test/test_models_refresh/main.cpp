#include <unity.h>

#include <ArduinoJson.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "nimbus/orch/model_refresh.h"
#include "nimbus/orch/models_tool.h"
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
        while (const char* due = q.due(false, true, false, true)) {
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
  while (const char* due = q.due(false, true, false, true)) {
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
  TEST_ASSERT_NULL(q.due(true, true, false, true));    // a turn is in flight
  TEST_ASSERT_NULL(q.due(false, false, false, true));  // offline
  TEST_ASSERT_NULL(q.due(false, true, true, true));    // the verify slot is busy
  TEST_ASSERT_NULL(q.due(false, true, false, false));  // resting heap below the floor
  TEST_ASSERT_TRUE(q.queued("anthropic"));
  TEST_ASSERT_EQUAL_STRING("anthropic", q.due(false, true, false, true));
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
    if (const char* due = q.due(inTurn, true, false, true)) {
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

static void test_stamp_round_trips_and_ignores_unknown_keys() {
  const std::string line = formatModelsStamp(1792454400, "v4.5.16-23-g98137e0d");
  TEST_ASSERT_EQUAL_STRING("#models v1 fetchedAt=1792454400 fw=v4.5.16-23-g98137e0d\n", line.c_str());
  ModelsStamp s;
  TEST_ASSERT_TRUE(parseModelsStamp(line + "claude-sonnet-5-5|Claude Sonnet 5.5|...\n", s));
  TEST_ASSERT_TRUE(s.present);
  TEST_ASSERT_EQUAL_UINT32(1792454400u, s.fetchedAt);
  TEST_ASSERT_EQUAL_STRING("v4.5.16-23-g98137e0d", s.fw.c_str());
  // A later header with more keys, in any order, still reads.
  TEST_ASSERT_TRUE(parseModelsStamp("#models v1 rows=13 fw=v5.0.0 more=0 fetchedAt=7\r\n", s));
  TEST_ASSERT_EQUAL_UINT32(7u, s.fetchedAt);
  TEST_ASSERT_EQUAL_STRING("v5.0.0", s.fw.c_str());
  // No header: not present.
  TEST_ASSERT_FALSE(parseModelsStamp("claude-sonnet-5-5|x\n", s));
  TEST_ASSERT_FALSE(s.present);
  // A hostile version cannot break the space-separated header.
  const std::string bad = formatModelsStamp(1, "v1 fetchedAt=99\nfw=x");
  TEST_ASSERT_EQUAL(1, (int)std::count(bad.begin(), bad.end(), '\n'));
  TEST_ASSERT_TRUE(parseModelsStamp(bad, s));
  TEST_ASSERT_EQUAL_UINT32(1u, s.fetchedAt);
}

// The passive refresh decides from the model file's header alone: the firmware
// that fetched the list and when. Every reason, plus the counter-cases.
static void test_passive_refresh_reads_the_firmware_from_the_file_header() {
  const uint32_t now = 1792540800;   // 2026-10-21
  const std::string fw = "v4.6.0";
  struct Case { std::string file; uint32_t now; PassiveWhy want; };
  const Case cases[] = {
      {"claude-x|...\n", now, PassiveWhy::NoStamp},                                   // pre-stamp file
      {"", now, PassiveWhy::NoStamp},                                                 // no file
      {formatModelsStamp(now - 3600, "v4.5.16") + "rows\n", now, PassiveWhy::Firmware},   // after an update
      {formatModelsStamp(now - 3600, "v4.5.16"), 0, PassiveWhy::Firmware},              // even with no clock
      {formatModelsStamp(now - kPassiveMaxAgeS, fw), now, PassiveWhy::Stale},           // a week old
      {formatModelsStamp(now - kPassiveMaxAgeS + 60, fw), now, PassiveWhy::None},       // not yet
      {formatModelsStamp(0, fw), now, PassiveWhy::UnknownAge},                          // fetched clockless
      {formatModelsStamp(0, fw), 0, PassiveWhy::None},                                  // still clockless
      {formatModelsStamp(now - 60, fw), 0, PassiveWhy::None},
      {formatModelsStamp(now + 3600, fw), now, PassiveWhy::None},                       // a clock step back
  };
  for (const Case& c : cases) {
    ModelsStamp s;
    parseModelsStamp(c.file, s);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(passiveWhyToken(c.want), passiveWhyToken(passiveRefreshDue(s, fw, c.now)),
                                     c.file.c_str());
  }
}

// After an update the passive check queues each keyed provider once; the stamp a
// refresh writes then holds the running firmware and nothing more is due.
static void test_after_an_update_each_provider_refreshes_once() {
  const uint32_t now = 1792540800;
  const std::string fw = "v4.6.0";
  std::map<std::string, std::string> files;   // the model file per provider
  for (const ProviderSlot& slot : kProviderSlots) files[slot.slug] = formatModelsStamp(now - 7200, "v4.5.16");
  ModelRefreshQueue q;
  uint32_t ms = 600000;
  int starts = 0;
  for (int pass = 0; pass < 3; ++pass) {   // the passive check runs again later
    for (auto& f : files) {
      ModelsStamp s;
      parseModelsStamp(f.second, s);
      if (passiveRefreshDue(s, fw, now) != PassiveWhy::None) q.request(f.first.c_str(), true, ms);
    }
    while (const char* due = q.due(false, true, false, true)) {
      q.started(due, ms += 5000);
      files[due] = formatModelsStamp(now, fw);   // the refresh writes the new stamp
      starts++;
    }
    ms += 6u * kHour;
  }
  TEST_ASSERT_EQUAL((int)kProviderSlotCount, starts);
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
  RUN_TEST(test_stamp_round_trips_and_ignores_unknown_keys);
  RUN_TEST(test_passive_refresh_reads_the_firmware_from_the_file_header);
  RUN_TEST(test_after_an_update_each_provider_refreshes_once);
  RUN_TEST(test_miss_texts);
  return UNITY_END();
}
