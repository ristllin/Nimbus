#include <unity.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <set>
#include <map>
#include <regex>
#include <string>
#include <vector>

#include <ArduinoJson.h>

#include "nimbus/orch/model_ledger.h"
#include "nimbus/orch/provider_slots.h"
#include "nimbus/store/nvs_diag.h"
#include "nimbus/orch/scratchpad.h"
#include "nimbus/store/persist.h"
#include "nimbus/store/moved_store.h"
#include "nimbus/store/nvs_reserve.h"
#include "nimbus/store/verdict_memo.h"

// ============================================================================
// test_nvs_full (CUM-479) - owner-config writes must never report ok on a full NVS,
// and rebuildable catalogs must not be able to fill it.
//
// The class (CUM-242 -> CUM-389 -> CUM-408 -> CUM-479): a store setter dropped
// solide::memory's `false` on a full 20 KB partition and the route answered
// {"ok":true}; each earlier fix hardened ONE victim (catalog, cloud credential,
// profile seed). This suite pins the class, not the instance:
//
//   1. The answer: every covered save route, driven against a full-partition fake,
//      replies 507 storage_full and never claims ok (route table below).
//   2. The write verdicts: the portable primitives the store's setters use report
//      the truth on a fake that refuses writes exactly like a full partition -
//      including the driver quirk that says "true" for an empty write.
//   3. The remedy works on a 100% full partition: removing a key or connector is an
//      erase / shrink-safe rewrite that lands without a free entry.
//   4. Headroom: catalogs never live in NVS; the legacy eviction covers every
//      registry slot (a new slot is covered with no new list) and heals a device
//      that is full today; the NVS budget model keeps the owner headroom after six
//      providers verify, where the old placement exhausted it.
//   5. Static class guards over the device seam: every store write goes through the
//      verdict helpers, every store setter returns bool, every covered route opens a
//      SaveScope and answers through sendSaved (never a raw ok), every NVS-writing
//      POST route is covered or a tracked gap (CUM-530), raw NVS writers are a
//      reviewed file set, embedcfg stores its config before it wipes anything, and
//      the web UI shows the storage-full text.
// ============================================================================

using namespace nimbus::store;

void setUp() {}
void tearDown() {}

// ---- a full-partition NVS fake ----------------------------------------------
// Models what matters on the device: a string of n bytes costs one header entry
// plus ceil((n + 1) / 32) data entries (NVS stores the NUL); an int costs one; a
// rewrite is new-then-erase (the new value must fit while the old one still holds
// its entries); an identical rewrite is a no-op that succeeds; and setString answers
// like solide::memory::setString - TRUE for an empty value whatever happened. One
// simplification: the fake pools every free entry, while real NVS fits a string
// inside one 126-entry page, so the headroom figures here are an upper bound (which
// is why the `low` flag is documented as a guide and the 507 contract as exact).
struct FakeNvs {
  size_t capacity;   // usable entries (the GC page is already excluded)
  std::map<std::string, std::string> str;
  std::map<std::string, int32_t>     ints;
  int setCalls = 0;

  explicit FakeNvs(size_t cap) : capacity(cap) {}

  static size_t strEntries(size_t n) { return 1 + (n + 1 + 31) / 32; }
  size_t used() const {
    size_t u = 0;
    for (const auto& kv : str) u += strEntries(kv.second.size());
    return u + ints.size();
  }
  size_t freeEntries() const { return capacity > used() ? capacity - used() : 0; }

  bool setString(const char* k, const std::string& v) {
    ++setCalls;
    auto it = str.find(k);
    const bool same = it != str.end() && it->second == v;
    const bool landed = same || strEntries(v.size()) <= freeEntries();
    if (landed) str[k] = v;
    return landed || v.empty();   // the driver quirk: an empty write always "succeeds"
  }
  std::string getString(const char* k, const char* def) {
    auto it = str.find(k);
    return it == str.end() ? std::string(def) : it->second;
  }
  bool has(const char* k) { return str.count(k) || ints.count(k); }
  bool erase(const char* k) { return (str.erase(k) + ints.erase(k)) > 0; }
  bool setInt(const char* k, int32_t v) {
    auto it = ints.find(k);
    if (it != ints.end() && it->second == v) return true;
    if (freeEntries() < 1) return false;
    ints[k] = v;
    return true;
  }
  // Fill every remaining entry so the partition is exactly 100% full.
  void fillToFull() {
    int i = 0;
    while (freeEntries() > 0) {
      const size_t f = freeEntries();
      if (f < 2) { ints["fill_i" + std::to_string(i++)] = 1; continue; }
      str["fill_" + std::to_string(i++)] = std::string(std::min<size_t>((f - 1) * 32 - 1, 3000), 'x');
    }
  }
};

// One route-level write: which primitive the device setter uses, the NVS key, value.
enum class Prim { Str, OrErase, ShrinkSafe, Int };
struct Write {
  Prim        prim;
  const char* key;
  std::string value;
  int32_t     iv;
};

// Run a route's writes the way the device does: each verdict feeds the ledger on the
// route's task, and the reply comes from the ledger scope through saveReply().
static SaveReply runRoute(FakeNvs& nvs, PersistLedger& led, uintptr_t task,
                          const std::vector<Write>& writes, const std::string& okBody) {
  const uint32_t since = led.seq();
  for (const Write& w : writes) {
    bool ok = false;
    switch (w.prim) {
      case Prim::Str:        ok = writeString(nvs, w.key, w.value); break;
      case Prim::OrErase:    ok = writeOrErase(nvs, w.key, w.value); break;
      case Prim::ShrinkSafe: ok = writeShrinkSafe(nvs, w.key, w.value); break;
      case Prim::Int:        ok = nvs.setInt(w.key, w.iv); break;
    }
    if (!ok) led.noteFailure(task, w.key);
  }
  return saveReply(!led.failedSince(since, task), 200, okBody);
}

static bool claimsOk(const std::string& body) {
  return body.find("\"ok\":true") != std::string::npos;
}

// ---- 1. the honest answer -----------------------------------------------------
static void test_save_reply_is_507_storage_full_never_ok() {
  const SaveReply bad = saveReply(false, 200, "{\"ok\":true}");
  TEST_ASSERT_EQUAL_INT(507, bad.status);
  TEST_ASSERT_FALSE(claimsOk(bad.body));
  JsonDocument d;
  TEST_ASSERT_TRUE(deserializeJson(d, bad.body) == DeserializationError::Ok);
  TEST_ASSERT_EQUAL_STRING("storage_full", d["error"] | "");
  TEST_ASSERT_EQUAL_STRING(kStorageFullMessage, d["message"] | "");
  // A save that landed keeps the route's own body and status.
  const SaveReply good = saveReply(true, 200, "{\"ok\":true,\"restart\":false}");
  TEST_ASSERT_EQUAL_INT(200, good.status);
  TEST_ASSERT_EQUAL_STRING("{\"ok\":true,\"restart\":false}", good.body.c_str());
}

// The message follows the copy guide: what happened, then the one next step;
// sentence case; no em dash; names both remedies the store makes work when full.
static void test_storage_full_message_copy() {
  const std::string m = kStorageFullMessage;
  TEST_ASSERT_EQUAL_STRING(
      "Couldn't save: device storage is full. Remove an unused provider key or connector "
      "and try again.",
      m.c_str());
  TEST_ASSERT_TRUE(m.find("\xE2\x80\x94") == std::string::npos);
  TEST_ASSERT_TRUE(m.find("provider key") != std::string::npos);
  TEST_ASSERT_TRUE(m.find("connector") != std::string::npos);
}

// Every covered route, driven against a 100% full partition, answers 507 and does
// not claim ok. The writes are the NVS keys each device route persists (the device
// keys from src/agent/agent_config.h and src/sys/config_nvs.cpp).
static void test_every_covered_route_answers_507_on_a_full_nvs() {
  const std::string gcal =
      "[{\"name\":\"gcal\",\"type\":\"gcal\",\"prov\":\"mistral\",\"kind\":\"connector\","
      "\"cid\":\"connector_googlecalendar\",\"en\":1}]";
  struct RouteCase {
    const char*        route;
    std::vector<Write> writes;
    const char*        okBody;
  };
  const std::vector<RouteCase> routes = {
      {"POST /api/connectors blob", {{Prim::ShrinkSafe, "connectors", gcal, 0}}, "{\"ok\":true}"},
      {"POST /api/connectors patch",
       {{Prim::ShrinkSafe, "connectors", gcal + std::string(40, ' '), 0}}, "{\"ok\":true}"},
      {"POST /api/orch orchHost", {{Prim::OrErase, "orchHost", "zai", 0}}, "{\"ok\":true}"},
      {"POST /api/orch provPrio", {{Prim::Str, "provPrio", "zai,openai,anthropic,mistral", 0}},
       "{\"ok\":true}"},
      {"POST /api/orch oaiKey",
       {{Prim::OrErase, "oaiKey", "sk-proj-" + std::string(150, 'a'), 0}}, "{\"ok\":true}"},
      {"POST /api/orch zaiKey", {{Prim::OrErase, "zaiKey", std::string(49, 'z'), 0}},
       "{\"ok\":true}"},
      {"POST /api/orch sysPrompt", {{Prim::OrErase, "sysPrompt", "Be brief.", 0}},
       "{\"ok\":true}"},
      {"POST /api/orch budget", {{Prim::OrErase, "usgLedger", "openai|0|0|0|5000|0|1", 0}},
       "{\"ok\":true}"},
      {"POST /api/orch sfxVol", {{Prim::Int, "sfxVol", "", 0}}, "{\"ok\":true}"},
      {"POST /api/config devTz", {{Prim::OrErase, "devTz", "GMT0BST,M3.5.0/1,M10.5.0/2", 0}},
       "{\"ok\":true}"},
      {"POST /api/config saverMin",
       {{Prim::Int, "saverMin", "", 10}, {Prim::Int, "saverMinSet", "", 1}}, "{\"ok\":true}"},
      {"POST /api/config mode", {{Prim::Int, "nimbus_mode", "", 1}}, "{\"ok\":true}"},
      {"POST /api/config profile", {{Prim::Int, "nimbus_prof", "", 2}}, "{\"ok\":true}"},
      {"POST /api/config devName", {{Prim::OrErase, "nimbus_name", "Nimbus-Desk", 0}},
       "{\"ok\":true}"},
      {"POST /api/mem/embedcfg",
       {{Prim::Str, "embProv", "mistral", 0}, {Prim::Str, "embModel", "mistral-embed", 0},
        {Prim::Int, "embDims", "", 1024}},
       "{\"ok\":true}"},
      {"POST /api/onboard/complete", {{Prim::Int, "onbrded", "", 1}},
       "{\"ok\":true,\"restart\":false}"},
      {"POST /api/token/regen", {{Prim::Str, "webTok", "0123456789abcdef01234567", 0}},
       "{\"ok\":true,\"token\":\"0123456789abcdef01234567\"}"},
      {"POST /savewifi (wizard)",
       {{Prim::Str, "staNets", "HomeNet\tpw\t0", 0}, {Prim::Str, "staSsid", "HomeNet", 0},
        {Prim::Str, "staPass", "pw-pw-pw", 0}},
       "{\"ok\":true}"},
      {"POST /api/wifi add", {{Prim::Str, "staNets", "HomeNet\tpw\t0;Hotspot\tpw\t0", 0}},
       "{\"ok\":true,\"saved\":\"Hotspot\"}"},
  };
  for (const RouteCase& rc : routes) {
    FakeNvs nvs(504);
    nvs.str["oaiKey"] = "sk-old";
    nvs.str["orchHost"] = "openai";
    nvs.str["webTok"] = "ffffffffffffffffffffffff";
    nvs.ints["sfxVol"] = 50;
    nvs.ints["nimbus_mode"] = 0;
    nvs.fillToFull();
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, nvs.freeEntries(), rc.route);
    PersistLedger led;
    const SaveReply rep = runRoute(nvs, led, /*task=*/7, rc.writes, rc.okBody);
    TEST_ASSERT_EQUAL_INT_MESSAGE(507, rep.status, rc.route);
    TEST_ASSERT_FALSE_MESSAGE(claimsOk(rep.body), rc.route);
    TEST_ASSERT_TRUE_MESSAGE(rep.body.find("storage_full") != std::string::npos, rc.route);
    // ...and with room, the very same route answers its ok body (no false 507).
    FakeNvs roomy(504);
    PersistLedger led2;
    const SaveReply ok = runRoute(roomy, led2, 7, rc.writes, rc.okBody);
    TEST_ASSERT_EQUAL_INT_MESSAGE(200, ok.status, rc.route);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(rc.okBody, ok.body.c_str(), rc.route);
  }
}

// ---- 2. write verdicts on a full partition -------------------------------------
// The driver says "true" for an empty write whatever happened; the verdict reads the
// key back. The full truth table of stringWriteLanded.
static void test_string_write_verdict_truth_table() {
  for (int m = 0; m < 16; ++m) {
    const bool drv = m & 1, empty = m & 2, present = m & 4, rbEmpty = m & 8;
    const bool want = empty ? (drv && present && rbEmpty) : drv;
    TEST_ASSERT_EQUAL(want, stringWriteLanded(drv, empty, present, rbEmpty));
  }
}

static void test_write_primitives_tell_the_truth_on_a_full_nvs() {
  FakeNvs nvs(504);
  nvs.str["tchCal"] = "0,320,0,240,5";
  nvs.fillToFull();
  // A fresh non-empty value cannot land.
  TEST_ASSERT_FALSE(writeString(nvs, "connectors", std::string("[{\"name\":\"gh\"}]")));
  // Writing "" over a stored value: the DRIVER says true, the verdict says false.
  TEST_ASSERT_TRUE(nvs.setString("tchCal", ""));                  // the quirk, reproduced
  TEST_ASSERT_EQUAL_STRING("0,320,0,240,5", nvs.getString("tchCal", "").c_str());
  TEST_ASSERT_FALSE(writeString(nvs, "tchCal", std::string("")));  // ...caught
  // An identical rewrite is a no-op and lands even when full (a reflexive re-save).
  TEST_ASSERT_TRUE(writeString(nvs, "tchCal", std::string("0,320,0,240,5")));
  // An empty value for an absent key cannot be "stored" on a full partition either.
  TEST_ASSERT_FALSE(writeString(nvs, "devTz", std::string("")));
}

// ---- 3. the remedy lands on a 100% full partition --------------------------------
static void test_removing_a_key_or_connector_lands_when_full() {
  FakeNvs nvs(504);
  nvs.str["oaiKey"] = "sk-proj-" + std::string(150, 'a');
  nvs.str["vfy_openai"] = "1:1760000000";
  const std::string two =
      "[{\"name\":\"gcal\",\"kind\":\"connector\",\"cid\":\"connector_googlecalendar\"},"
      "{\"name\":\"github\",\"kind\":\"mcp\",\"url\":\"https://api.githubcopilot.com/mcp/\"}]";
  nvs.str["connectors"] = two;
  nvs.fillToFull();
  TEST_ASSERT_EQUAL_size_t(0, nvs.freeEntries());

  // Removing a key: "" is an erase (writeOrErase), and the verify reset (-1, 0) is an
  // erase too (store::setVerify) - both land with zero free entries.
  TEST_ASSERT_TRUE(writeOrErase(nvs, "oaiKey", std::string("")));
  TEST_ASSERT_FALSE(nvs.has("oaiKey"));
  TEST_ASSERT_TRUE(eraseLanded(nvs, "vfy_openai"));
  TEST_ASSERT_TRUE(eraseLanded(nvs, "vfy_openai"));   // idempotent: absent is "cleared"

  nvs.fillToFull();
  // Removing a connector: a shrinking rewrite. The plain write cannot land (NVS
  // writes new-before-erase), the shrink-safe path does.
  const std::string one =
      "[{\"name\":\"gcal\",\"kind\":\"connector\",\"cid\":\"connector_googlecalendar\"}]";
  TEST_ASSERT_FALSE(nvs.setString("connectors", one));
  TEST_ASSERT_TRUE(writeShrinkSafe(nvs, "connectors", one));
  TEST_ASSERT_EQUAL_STRING(one.c_str(), nvs.getString("connectors", "").c_str());
  // ...but a GROWING rewrite is never erase-then-write (that could lose the set).
  nvs.fillToFull();
  TEST_ASSERT_FALSE(writeShrinkSafe(nvs, "connectors", two));
  TEST_ASSERT_EQUAL_STRING(one.c_str(), nvs.getString("connectors", "").c_str());
  // Deleting the last connector is an erase.
  TEST_ASSERT_TRUE(writeShrinkSafe(nvs, "connectors", std::string("")));
  TEST_ASSERT_FALSE(nvs.has("connectors"));
}

// The erase-then-write fallback puts the old value back when the new write still
// fails (another task's write took the freed room first), so a failed delete never
// silently loses the whole set while the route answers 507.
struct FlakyNvs : FakeNvs {
  using FakeNvs::FakeNvs;
  int failSetAfterErase = 0;   // fail this many setString calls after an erase
  bool erase(const char* k) {
    failSetAfterErase = 1;
    return FakeNvs::erase(k);
  }
  bool setString(const char* k, const std::string& v) {
    if (failSetAfterErase > 0) { --failSetAfterErase; return false; }
    return FakeNvs::setString(k, v);
  }
};

static void test_shrink_safe_restores_the_old_value_when_the_write_fails() {
  FlakyNvs nvs(504);
  const std::string two = "[{\"name\":\"gcal\"},{\"name\":\"github\"}]";
  nvs.str["connectors"] = two;
  nvs.fillToFull();
  TEST_ASSERT_FALSE(writeShrinkSafe(nvs, "connectors", std::string("[{\"name\":\"gcal\"}]")));
  TEST_ASSERT_EQUAL_STRING(two.c_str(), nvs.getString("connectors", "").c_str());
}

// ---- the per-task ledger ----------------------------------------------------------
static void test_ledger_attributes_failures_to_their_task() {
  PersistLedger led;
  TEST_ASSERT_FALSE(led.failedSince(0, 1));
  TEST_ASSERT_EQUAL_STRING("", led.lastKey());
  led.noteFailure(/*task=*/2, "orchConv");          // before the scope: not ours
  const uint32_t since = led.seq();
  TEST_ASSERT_FALSE(led.failedSince(since, 1));
  led.noteFailure(2, "usgLedger");                  // another task (the turn task)
  TEST_ASSERT_FALSE(led.failedSince(since, 1));     // never a false 507
  TEST_ASSERT_TRUE(led.failedSince(since, 2));
  led.noteFailure(1, "connectors");                 // ours
  TEST_ASSERT_TRUE(led.failedSince(since, 1));
  TEST_ASSERT_EQUAL_STRING("connectors", led.lastKey());
  // Keys are truncated to the NVS 15-char limit, never overrun.
  led.noteFailure(3, "a_key_far_longer_than_fifteen");
  TEST_ASSERT_EQUAL_size_t(15, std::strlen(led.lastKey()));
}

// More failures since the scope than the ring holds: the dropped ones cannot be
// attributed, so the answer is fail-closed (a save is never reported ok on a guess).
static void test_ledger_overflow_is_fail_closed() {
  PersistLedger led;
  const uint32_t since = led.seq();
  for (size_t i = 0; i < PersistLedger::kRing; ++i) led.noteFailure(9, "orchConv");
  TEST_ASSERT_FALSE(led.failedSince(since, 1));   // exactly a ring: still attributable
  led.noteFailure(9, "orchConv");
  TEST_ASSERT_TRUE(led.failedSince(since, 1));    // overflowed: fail closed
}

// ---- 4. headroom: catalogs out of NVS -------------------------------------------
// The class: the eviction walks the canonical registry + custom, so a provider slot
// added to kProviderSlots is covered with no new list (the CUM-242 drift).
static void test_legacy_catalog_eviction_covers_every_provider() {
  FakeNvs nvs(504);
  int planted = 0;
  forEachCatalogProvider([&](const char* slug) {
    nvs.str[legacyCatalogKey(slug)] = std::string(300, 'c');
    ++planted;
  });
  TEST_ASSERT_EQUAL_INT((int)nimbus::orch::kProviderSlotCount + 1, planted);
  for (size_t i = 0; i < nimbus::orch::kProviderSlotCount; ++i) {
    const char* slug = nimbus::orch::kProviderSlots[i].slug;
    TEST_ASSERT_TRUE_MESSAGE(nvs.has(legacyCatalogKey(slug).c_str()), slug);
  }
  nvs.str["mcat_note"] = "not a provider";   // never touched: only provider caches go
  TEST_ASSERT_EQUAL_INT(planted, evictLegacyCatalogs(nvs));
  forEachCatalogProvider(
      [&](const char* slug) { TEST_ASSERT_FALSE(nvs.has(legacyCatalogKey(slug).c_str())); });
  TEST_ASSERT_TRUE(nvs.has("mcat_note"));
  TEST_ASSERT_EQUAL_INT(0, evictLegacyCatalogs(nvs));   // second boot: nothing to do
}

static void test_catalog_paths_are_safe() {
  forEachCatalogProvider([](const char* slug) {
    TEST_ASSERT_TRUE_MESSAGE(isCatalogSlug(slug), slug);
    TEST_ASSERT_EQUAL_STRING(("/data/mcat_" + std::string(slug) + ".json").c_str(),
                             catalogCachePath(slug).c_str());
    TEST_ASSERT_TRUE(legacyCatalogKey(slug).size() <= 15);   // NVS key ceiling
  });
  const char* bad[] = {"", "../etc", "Openai", "a/b", "openai.json", "abcdefghijk", nullptr};
  for (int i = 0; bad[i]; ++i) {
    TEST_ASSERT_FALSE_MESSAGE(isCatalogSlug(bad[i]), bad[i]);
    TEST_ASSERT_TRUE(catalogCachePath(bad[i]).empty());
    TEST_ASSERT_TRUE(legacyCatalogKey(bad[i]).empty());
  }
  TEST_ASSERT_TRUE(catalogCachePath(nullptr).empty());
}

// The NVS budget model of the bench scenario (5 keys verified + Wi-Fi + basics +
// gcal/GitHub connectors), on the real geometry: 0x5000 = 5 pages x 126 entries, one
// page held for GC -> 504 usable. Calibrated against bench unit A (v4.5.8, 5 keys,
// ROM dump 2026-10-10): 494 entries used, of which the four catalogs held 309 (mcat_
// mistral 100, zai 96, openai 71, anthropic 42) and RF cal_data ~66; everything else
// fit in ~119. This model seeds ~348 non-catalog entries (system 120 + owner ~228),
// HEAVIER than measured, so the headroom assertion holds with margin. Representative
// sizes, not secrets. With the OLD placement the catalogs (store, then shrink-until-
// it-fits) exhaust it and the connector save fails - the CUM-479 repro; with catalogs
// in flash files the owner headroom holds and even the largest owner value (a 3500 B
// connectors blob) still lands.
static void seedProvisionedOwnerConfig(FakeNvs& nvs) {
  for (int i = 0; i < 4; ++i) nvs.str["sys_blob" + std::to_string(i)] = std::string(900, 's');
  for (int i = 0; nvs.used() < 120; ++i) nvs.ints["sys_i" + std::to_string(i)] = i;
  nvs.str["oaiKey"]     = std::string(164, 'k');
  nvs.str["antKey"]     = std::string(108, 'k');
  nvs.str["mistralKey"] = std::string(32, 'k');
  nvs.str["zaiKey"]     = std::string(49, 'k');
  nvs.str["tavilyKey"]  = std::string(41, 'k');
  const char* provs[] = {"openai", "anthropic", "mistral", "zai", "tavily", "telegram", "cumulo"};
  for (const char* p : provs) nvs.str[std::string("vfy_") + p] = "1:1760000000";
  const char* heads[] = {"openai", "anthropic", "mistral", "zai", "cumulo"};
  for (const char* p : heads) nvs.str[std::string("mch_") + p] = std::string(300, 'm');
  nvs.str["provPrio"] = "openai,anthropic,mistral,zai";
  nvs.str["subPrio"]  = "openai,anthropic,mistral";
  nvs.str["webTok"]   = std::string(24, 't');
  nvs.str["apPass"]   = std::string(10, 'p');
  nvs.str["nimbus_name"] = "Nimbus-2";
  nvs.str["staNets"]  = std::string(120, 'w');
  nvs.str["staSsid"]  = std::string(16, 'w');
  nvs.str["staPass"]  = std::string(20, 'w');
  nvs.str["tgToken"]  = std::string(46, 'g');
  nvs.str["tgAllow"]  = "123456789";
  nvs.str["usgLedger"] = std::string(800, 'u');
  nvs.str["fbRules"]   = std::string(600, 'f');
  nvs.str["orchConv"]  = std::string(60, 'c');
  nvs.str["tchCal"]    = "0,320,0,240,5";
  nvs.str["otaType"]   = "nimbus-tft";
  for (int i = 0; i < 60; ++i) nvs.ints["own_i" + std::to_string(i)] = i;
}

static void test_nvs_budget_old_placement_exhausts_new_keeps_headroom() {
  const std::string connectors = std::string(250, 'j');   // gcal + GitHub
  // OLD: each verify packed its catalog into whatever NVS was left.
  FakeNvs old(504);
  seedProvisionedOwnerConfig(old);
  forEachCatalogProvider([&](const char* slug) {
    size_t n = kCatalogMaxBytes;
    while (n > 64 && !old.setString(legacyCatalogKey(slug).c_str(), std::string(n, 'c'))) n -= 200;
  });
  TEST_ASSERT_TRUE(old.freeEntries() * kNvsEntryBytes < kOwnerHeadroomBytes);
  TEST_ASSERT_FALSE(writeShrinkSafe(old, "connectors", connectors));   // CUM-479

  // HEAL: the first boot of this build evicts the legacy caches - room comes back.
  TEST_ASSERT_TRUE(evictLegacyCatalogs(old) > 0);
  TEST_ASSERT_TRUE(old.freeEntries() * kNvsEntryBytes >= kOwnerHeadroomBytes);
  TEST_ASSERT_TRUE(writeShrinkSafe(old, "connectors", connectors));

  // NEW: six providers verify; their catalogs go to flash files, NVS sees only the
  // bounded verdict + choice-list writes the model already holds. Then the owner
  // saves the connectors and switches the host - both land with headroom to spare.
  FakeNvs nvs(504);
  seedProvisionedOwnerConfig(nvs);
  const size_t before = nvs.used();
  forEachCatalogProvider([&](const char* slug) {
    TEST_ASSERT_FALSE(catalogCachePath(slug).empty());   // a file, never an NVS key
  });
  TEST_ASSERT_EQUAL_size_t(before, nvs.used());
  TEST_ASSERT_TRUE(writeShrinkSafe(nvs, "connectors", connectors));
  TEST_ASSERT_TRUE(writeOrErase(nvs, "orchHost", std::string("zai")));
  const NvsUsage u = nvsUsageFromEntries(nvs.used(), nvs.freeEntries(), 630);
  TEST_ASSERT_FALSE(u.low);
  TEST_ASSERT_TRUE(u.freeBytes >= kOwnerHeadroomBytes);
  // The largest single owner value still fits on top (the headroom rule's premise).
  TEST_ASSERT_TRUE(writeShrinkSafe(nvs, "connectors", std::string(3500, 'j')));
}

static void test_nvs_usage_bytes() {
  const NvsUsage u = nvsUsageFromEntries(300, 204, 630);
  TEST_ASSERT_EQUAL_UINT32(9600, u.usedBytes);
  TEST_ASSERT_EQUAL_UINT32(6528, u.freeBytes);
  TEST_ASSERT_EQUAL_UINT32(20160, u.totalBytes);
  TEST_ASSERT_FALSE(u.low);
  TEST_ASSERT_TRUE(nvsUsageFromEntries(400, 127, 630).low);    // 4064 B < 4096
  TEST_ASSERT_FALSE(nvsUsageFromEntries(400, 128, 630).low);   // 4096 B: the rule holds
  // The headroom rule's premise: a max connectors blob (3500 B) fits inside it.
  TEST_ASSERT_TRUE(FakeNvs::strEntries(3500) * kNvsEntryBytes <= kOwnerHeadroomBytes);
}

// ---- 5. static class guards over the device seam -----------------------------------
static std::string readFile(const char* path) {
  FILE* f = std::fopen(path, "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

static size_t countOf(const std::string& hay, const std::string& needle) {
  size_t n = 0;
  for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) ++n;
  return n;
}

// Every NVS write in store.cpp goes through the verdict helpers: no bare
// solide::memory set/erase after the helper block, so no setter can skip the ledger.
static void test_store_writes_all_go_through_the_ledger() {
  const std::string src = readFile("src/agent/store.cpp");
  const size_t helpersEnd = src.find("// ---- provider-config read cache (CUM-238)");
  TEST_ASSERT_TRUE(helpersEnd != std::string::npos);
  const std::string body = src.substr(helpersEnd);
  TEST_ASSERT_EQUAL_size_t_MESSAGE(0, countOf(body, "solide::memory::set"),
                                   "a bare solide::memory set call skips the persist ledger");
  TEST_ASSERT_EQUAL_size_t_MESSAGE(0, countOf(body, "solide::memory::eraseKey"),
                                   "a bare solide::memory eraseKey skips the persist ledger");
  // config_nvs.cpp: every solide::memory write line reports through notePersist.
  const std::string cfg = readFile("src/sys/config_nvs.cpp");
  size_t pos = 0;
  while ((pos = cfg.find("solide::memory::set", pos)) != std::string::npos) {
    const size_t ls = cfg.rfind('\n', pos) + 1;
    const size_t le = cfg.find('\n', pos);
    const std::string line = cfg.substr(ls, le - ls);
    TEST_ASSERT_TRUE_MESSAGE(line.find("notePersist(") != std::string::npos, line.c_str());
    pos = le;
  }
}

// Every store setter returns its verdict (the void-setter class). The only void
// setter allowed is RAM-only.
static void test_store_setters_return_bool() {
  const std::string h = readFile("src/agent/store.h");
  std::regex voidSetter(R"(\bvoid\s+(set\w+|clear\w+|replace\w+)\s*\()");
  for (auto it = std::sregex_iterator(h.begin(), h.end(), voidSetter);
       it != std::sregex_iterator(); ++it) {
    const std::string name = (*it)[1];
    TEST_ASSERT_TRUE_MESSAGE(name == "setBootScreenIsTft", name.c_str());
  }
  TEST_ASSERT_TRUE(h.find("bool   setConnectorsJson(") != std::string::npos);
  TEST_ASSERT_TRUE(h.find("bool   setOnboarded(") != std::string::npos);
  TEST_ASSERT_TRUE(h.find("bool   setEmbedConfig(") != std::string::npos);
}

// The text of one route handler: from its registration to the next registration
// (s_server.on( or s_server.onNotFound(, whichever comes first).
static std::string routeRegion(const std::string& src, const std::string& route) {
  const std::string tag = "s_server.on(\"" + route + "\", HTTP_POST";
  const size_t s = src.find(tag);
  TEST_ASSERT_TRUE_MESSAGE(s != std::string::npos, route.c_str());
  const size_t e = src.find("s_server.on", s + tag.size());
  return src.substr(s, e == std::string::npos ? std::string::npos : e - s);
}

// The covered save routes (the DoD list + the wizard's Wi-Fi saves + token rotation).
struct Covered { const char* route; size_t minSaved; };
static const Covered kCovered[] = {
    {"/api/config", 2},           // ok + battery-warn bodies
    {"/api/orch", 1},
    {"/api/connectors", 2},       // blob mode + patch/del mode
    {"/api/onboard/complete", 1},
    {"/api/token/regen", 1},
    {"/api/wifi", 1},             // add/forget/reorder/rename/connect, via its ok() lambda
    {"/savewifi", 1},             // the setup wizard's Wi-Fi step
    {"/api/fallbacks", 1},        // program 2026-10-18 lane C: the 2c rows
    {"/api/telegram/add", 1},
    {"/api/telegram/remove", 3},  // stopped halfway + revoked-with-note + ok
    {"/api/telegram/rename", 1},
    {"/api/telegram/role", 1},
    {"/api/telegram/approve", 1},
    {"/api/telegram/public", 1},
};
// NVS-writing POST routes NOT yet answering through sendSaved: outside this lane's
// handler set, tracked as Linear CUM-530. Their setters already
// return bool, so wiring each is a SaveScope + sendSaved. Shrinking this list is the
// goal; a NEW writing route must land covered, not here.
static const char* kKnownGaps[] = {"/api/cloud"};

// Every covered webui route opens a SaveScope and answers success ONLY through
// sendSaved: no raw 200 send of any shape (literal, ternary, or variable body) in
// the region, so an added early-return ok path fails here.
static void test_covered_routes_answer_through_sendSaved() {
  const std::string src = readFile("src/net/webui.cpp");
  std::regex raw200(R"(r->send\(\s*[^,;]*\b200\b)");
  for (const Covered& c : kCovered) {
    const std::string region = routeRegion(src, c.route);
    TEST_ASSERT_TRUE_MESSAGE(region.find("nimbus::sys::SaveScope save;") != std::string::npos,
                             c.route);
    TEST_ASSERT_TRUE_MESSAGE(countOf(region, "sendSaved(r, save") >= c.minSaved, c.route);
    TEST_ASSERT_FALSE_MESSAGE(std::regex_search(region, raw200), c.route);
  }
  // The helper itself decides through the portable saveReply on the scope.
  TEST_ASSERT_TRUE(src.find("nimbus::store::saveReply(!save.failed()") != std::string::npos);
}

// The class, not a hand list: EVERY webui POST route that writes NVS (directly or via
// the orch/config field appliers or the Wi-Fi store) is either covered or a tracked
// known gap. A new save route that is neither fails here.
static void test_every_nvs_writing_route_is_covered_or_tracked() {
  const std::string src = readFile("src/net/webui.cpp");
  // Direct store writes plus the module helpers known to write NVS on the request's
  // task (telegram approvals, the cloud opt-in). A new helper that writes NVS for a
  // route belongs here too.
  const char* writers[] = {"agent::store::set", "agent::store::clear", "agent::store::regen",
                           "agent::store::replace", "sys::save", "saveAndConnect(",
                           "addNetwork(", "wifistore::", "applyOrchField(", "applyParam(",
                           "approvePending(", "relay::requestOptIn("};
  std::set<std::string> known;
  for (const Covered& c : kCovered) known.insert(c.route);
  for (const char* g : kKnownGaps) known.insert(g);
  std::regex post(R"re(s_server\.on\("([^"]+)", HTTP_POST)re");
  int writing = 0;
  for (auto it = std::sregex_iterator(src.begin(), src.end(), post);
       it != std::sregex_iterator(); ++it) {
    const std::string route = (*it)[1];
    const std::string region = routeRegion(src, route);
    bool writes = false;
    for (const char* w : writers) writes = writes || region.find(w) != std::string::npos;
    if (!writes) continue;
    ++writing;
    TEST_ASSERT_TRUE_MESSAGE(known.count(route) == 1,
                             ("NVS-writing route neither covered nor tracked: " + route).c_str());
  }
  TEST_ASSERT_TRUE(writing >= (int)(sizeof(kCovered) / sizeof(kCovered[0])));
  // A known gap that got covered must leave the gap list (keep the list honest).
  for (const char* g : kKnownGaps)
    TEST_ASSERT_TRUE_MESSAGE(routeRegion(src, g).find("SaveScope") == std::string::npos, g);
}

// Raw NVS writers are a reviewed set: a NEW file that writes NVS without going through
// agent::store / nimbus::sys::notePersist fails here until it is routed or reviewed.
static void test_raw_nvs_writers_are_a_reviewed_set() {
  const std::set<std::string> reviewed = {
      "src/agent/store.cpp",            // the owner-config store (helper block, scanned above)
      "src/sys/config_nvs.cpp",         // the persist ledger + its reporting int saves
      "src/sys/config_nvs.h",           // the NvsKv seam
      "src/agent/provider_verify.cpp",  // verify-task diagnostics sidecars (vrs_/vrm_)
      "src/agent/memory_subsystem.cpp", // orchmem: scratchpad + memory config (own failure logs)
      "src/agent/orch_persist.cpp",     // agjournal turn journal
      "src/agent/orch_persist.h",       //   (its interface comment)
      "src/sys/ota_update.cpp",         // rollback guard over raw nvs handles
      "src/main.cpp",                   // factory-reset identity restore over raw handles
      "src/bringup/provision.cpp",      // bring-up sketch, not in the product image
      "src/net/webui.h",                // a comment naming solide::memory::setInt
      "src/test_console.cpp",           // NIMBUS_TEST-only: NVSFILL writes a SCRATCH namespace
                                        // ("nfill_scratch") with the raw IDF API ON PURPOSE, to
                                        // fill the partition past any reserve for the full-NVS legs
                                        // (critique E1). Never in a production image (whole TU is
                                        // #if NIMBUS_TEST; the test-console source guard enforces it).
  };
  std::regex raw(R"(solide::memory::(set[A-Z]\w*|eraseKey|clear)\(|nvs_set_|nvs_erase_|\bPreferences\b)");
  for (const char* root : {"src", "include"}) {
    for (const auto& e : std::filesystem::recursive_directory_iterator(root)) {
      if (!e.is_regular_file()) continue;
      const std::string path = e.path().generic_string();
      const std::string ext = e.path().extension().string();
      if (ext != ".cpp" && ext != ".h") continue;
      // Cheap substring prefilter first: std::regex over the multi-MB generated
      // headers (embedded sounds, the web page) is slow, and none of them can match.
      const std::string text = readFile(path.c_str());
      if (text.find("solide::memory::") == std::string::npos &&
          text.find("nvs_") == std::string::npos &&
          text.find("Preferences") == std::string::npos)
        continue;
      if (!std::regex_search(text, raw)) continue;
      TEST_ASSERT_TRUE_MESSAGE(reviewed.count(path) == 1,
                               ("unreviewed raw NVS writer: " + path).c_str());
    }
  }
}

// embedcfg stores the config BEFORE it may wipe the vector memory, and answers 507
// when the store did not land - a reset must never wipe memories for an unsaved config.
static void test_embedcfg_stores_before_it_wipes() {
  const std::string src = readFile("src/net/web_memory.cpp");
  const size_t fn = src.find("void handleEmbedCfgPost(");
  TEST_ASSERT_TRUE(fn != std::string::npos);
  const std::string body = src.substr(fn, src.find("\n}\n", fn) - fn);
  const size_t store = body.find("if (!agent::store::setEmbedConfig(provider, model, dims))");
  const size_t wipe = body.find("mem::vectors().flushAll()");
  TEST_ASSERT_TRUE(store != std::string::npos);
  TEST_ASSERT_TRUE(wipe != std::string::npos);
  TEST_ASSERT_TRUE_MESSAGE(store < wipe, "embedcfg must store its config before any wipe");
  TEST_ASSERT_TRUE(body.find("nimbus::store::storageFullJson()") != std::string::npos);
}

// The web UI shows the device's sentence on a 507 (its offline fallback is the same
// text, JS-escaped) and never toasts "Saved" for it. A 400 refusal's own sentence is
// shown the same way (CUM-514), so it shares the long toast.
static void test_web_ui_shows_the_storage_full_text() {
  const std::string js = readFile("include/web/ui_js.h");
  std::string esc;
  for (const char* p = kStorageFullMessage; *p; ++p) {
    if (*p == '\'') esc += "\\'";
    else esc += *p;
  }
  TEST_ASSERT_TRUE_MESSAGE(js.find("const STORAGE_FULL_MSG='" + esc + "';") != std::string::npos,
                           "ui_js.h STORAGE_FULL_MSG drifted from kStorageFullMessage");
  TEST_ASSERT_TRUE(js.find("if(r.status!==507)throw r.status;") != std::string::npos);
  TEST_ASSERT_TRUE(js.find("function hasServerText(e){return e===507||(e===400&&!!_refuseErr);}") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(js.find("if(hasServerText(e)){toast(saveFailText(e),6000);return;}") != std::string::npos);
}

// ---- per-model prices (2026-10): the owner's model price file ------------------------
// A model price lives in /data/usage_mrates.txt (LittleFS, never NVS, so usgLedger and
// the NVS budget model above are untouched). A failed write of it reports
// "lfs_mrates" to the SAME per-task persist ledger, so the route answers 507
// storage_full like any NVS key, and the RAM table is put back exactly (CUM-479).
static void test_model_price_write_failure_is_507_with_ram_reverted() {
  using nimbus::orch::ModelLedger;
  using nimbus::orch::RateEdit;
  ModelLedger L;
  L.setOwnerRate("openai", "gpt-5.6", 4000000, 20000000);
  const std::string before = L.serializeOwnerRates();
  PersistLedger led;
  const uint32_t since = led.seq();
  // What store::setModelRate does: the durable edit, then the ledger note on failure.
  const RateEdit r = L.setOwnerRateDurable("openai", "gpt-5.6-luna", 200000, 1200000,
                                           [](const std::string&) { return false; });
  TEST_ASSERT_EQUAL((int)RateEdit::WriteFailed, (int)r);
  led.noteFailure(/*task=*/7, "lfs_mrates");
  const SaveReply rep = saveReply(!led.failedSince(since, 7), 200, "{\"ok\":true}");
  TEST_ASSERT_EQUAL_INT(507, rep.status);
  TEST_ASSERT_FALSE(claimsOk(rep.body));
  TEST_ASSERT_TRUE(rep.body.find("storage_full") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING(before.c_str(), L.serializeOwnerRates().c_str());   // RAM reverted
  TEST_ASSERT_NULL(L.ownerRate("openai", "gpt-5.6-luna"));
  TEST_ASSERT_EQUAL_STRING("lfs_mrates", led.lastKey());
  // With room, the same edit stores and the route answers its ok body.
  PersistLedger led2;
  const uint32_t since2 = led2.seq();
  TEST_ASSERT_EQUAL((int)RateEdit::Ok,
                    (int)L.setOwnerRateDurable("openai", "gpt-5.6-luna", 200000, 1200000,
                                               [](const std::string&) { return true; }));
  TEST_ASSERT_EQUAL_INT(200, saveReply(!led2.failedSince(since2, 7), 200, "{\"ok\":true}").status);
}

// The device seam: setModelRate stores through the atomic LittleFS writer and the
// durable edit, reports "lfs_mrates" on failure, and makes no NVS write and no bare
// solide::memory call; the route field goes through POST /api/orch (a covered route).
static void test_model_price_store_reports_lfs_mrates() {
  const std::string src = readFile("src/agent/store.cpp");
  const size_t s = src.find("nimbus::orch::RateEdit setModelRate(");
  TEST_ASSERT_TRUE(s != std::string::npos);
  const std::string body = src.substr(s, src.find("\n}\n", s) - s);
  TEST_ASSERT_TRUE(body.find("setOwnerRateDurable(") != std::string::npos);
  TEST_ASSERT_TRUE(body.find("writeFileAtomic(kUsageMratesPath") != std::string::npos);
  TEST_ASSERT_TRUE(body.find("notePersist(false, \"lfs_mrates\")") != std::string::npos);
  for (const char* nvs : {"putStr", "putOrClear", "putInt", "clearKey", "solide::memory"})
    TEST_ASSERT_TRUE_MESSAGE(body.find(nvs) == std::string::npos, nvs);
  const std::string web = readFile("src/net/webui.cpp");
  TEST_ASSERT_TRUE(web.find("if (n == \"mrate\") { applyModelRateField(v, err); return true; }") !=
                   std::string::npos);
  // usgLedger stays the one ledger key: the model files never reach NVS.
  TEST_ASSERT_TRUE(src.find("\"/data/usage_mrates.txt\"") != std::string::npos);
  TEST_ASSERT_TRUE(src.find("\"/data/usage_models.txt\"") != std::string::npos);
  TEST_ASSERT_TRUE(src.find("\"/data/usage_rprices.txt\"") != std::string::npos);
}


// ---- 6. the Telegram offset and the full-NVS visibility (lane A, 14 Oct loop) -----
// The fifth recurrence of the class above: the Telegram poll loop threw away the
// offset write's result and then RE-READ the stored offset after every drain, so a
// full NVS replayed the same page forever. The RAM cursor (nimbus/telegram_offset.h,
// property-tested in test_telegram_offset) is the truth while the device runs: the
// stored offset is read exactly once, when the poll loop starts, and the only write
// hands its verdict to the cursor (which retries and counts it).
static void test_telegram_offset_read_once_and_checked() {
  const std::string tg = readFile("src/agent/telegram.cpp");
  TEST_ASSERT_EQUAL_size_t_MESSAGE(1, countOf(tg, "store::telegramOffset()"),
                                   "the stored Telegram offset is read once, at poll start");
  TEST_ASSERT_TRUE(tg.find("g_cursor.seed(store::telegramOffset());") != std::string::npos);
  TEST_ASSERT_EQUAL_size_t(1, countOf(tg, "store::setTelegramOffset("));
  TEST_ASSERT_TRUE(tg.find("static bool writeOffset(int32_t v) { return store::setTelegramOffset(v); }") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(tg.find("g_cursor.persist(millis(), writeOffset);") != std::string::npos);
  TEST_ASSERT_TRUE(tg.find("g_cursor.checkpoint(millis(), writeOffset);") != std::string::npos);
  // No other firmware file reads the stored offset to drive a poll.
  for (const auto& e : std::filesystem::recursive_directory_iterator("src")) {
    const std::string path = e.path().generic_string();
    if (!e.is_regular_file() || path == "src/agent/telegram.cpp" || path == "src/agent/store.cpp" ||
        path == "src/agent/store.h")
      continue;
    if (e.path().extension() != ".cpp" && e.path().extension() != ".h") continue;
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, countOf(readFile(path.c_str()), "telegramOffset()"),
                                     path.c_str());
  }
}

// nvs.recentFails: every failure is kept (the log line is rate limited, this is not),
// newest first, with a monotonic number so a reader can count failures between reads.
static void test_recent_fails_ring() {
  RecentFails rf;
  RecentFails::Entry out[RecentFails::kRing];
  TEST_ASSERT_EQUAL_size_t(0, rf.snapshot(out, RecentFails::kRing));
  rf.note("tgOffset", 1000);
  rf.note("usgLedger", 2000);
  TEST_ASSERT_EQUAL_size_t(2, rf.snapshot(out, RecentFails::kRing));
  TEST_ASSERT_EQUAL_STRING("usgLedger", out[0].key);
  TEST_ASSERT_EQUAL_UINT32(2, out[0].seq);
  TEST_ASSERT_EQUAL_STRING("tgOffset", out[1].key);
  TEST_ASSERT_EQUAL_UINT32(1000, out[1].ms);
  for (uint32_t i = 0; i < 20; ++i) rf.note(i % 2 ? "a_very_long_key_name_over_15" : "tgOffset", 3000 + i);
  TEST_ASSERT_EQUAL_size_t(RecentFails::kRing, rf.snapshot(out, RecentFails::kRing));
  TEST_ASSERT_EQUAL_UINT32(22, rf.total());
  for (size_t i = 0; i < RecentFails::kRing; ++i) {
    TEST_ASSERT_EQUAL_UINT32(22 - i, out[i].seq);   // newest first, contiguous
    TEST_ASSERT_TRUE(std::strlen(out[i].key) <= 15);
  }
  TEST_ASSERT_EQUAL_size_t(3, rf.snapshot(out, 3));
  rf.note(nullptr, 1);
  rf.snapshot(out, 1);
  TEST_ASSERT_EQUAL_STRING("?", out[0].key);
}

// The owner notice: nothing before a failure, exactly once after any number of them.
static void test_storage_notice_once_per_boot() {
  OnceNotice n;
  TEST_ASSERT_FALSE(n.take());   // a healthy boot sends nothing
  TEST_ASSERT_FALSE(n.pending());
  for (int i = 0; i < 50; ++i) n.raise();
  TEST_ASSERT_TRUE(n.pending());
  TEST_ASSERT_TRUE(n.take());
  for (int i = 0; i < 50; ++i) { n.raise(); TEST_ASSERT_FALSE(n.take()); }
  TEST_ASSERT_TRUE(n.sent());
  // Copy rules (AGENTS.md section 6): plain words, no em dash, no I/me, a next step.
  const std::string t = kStorageNoticeText;
  TEST_ASSERT_TRUE(t.find("\xE2\x80\x94") == std::string::npos);
  TEST_ASSERT_TRUE(t.find(" I ") == std::string::npos && t.find(" me ") == std::string::npos);
  TEST_ASSERT_TRUE(t.find("remove connectors or provider keys") != std::string::npos);
}

// The wiring the notice and the ring depend on: notePersist records every failure and
// raises the notice (atomic, any task); only the Telegram poll task takes it, on its
// own outbound path (critique D1: never a send from the failing writer's task).
static void test_failures_feed_the_ring_and_the_notice() {
  const std::string cfg = readFile("src/sys/config_nvs.cpp");
  const size_t fn = cfg.find("bool notePersist(bool ok, const char* key) {");
  TEST_ASSERT_TRUE(fn != std::string::npos);
  const std::string body = cfg.substr(fn, cfg.find("\n}\n", fn) - fn);
  TEST_ASSERT_TRUE(body.find("s_recentFails.note(k, now);") != std::string::npos);
  TEST_ASSERT_TRUE(body.find("s_storageNotice.raise();") != std::string::npos);
  TEST_ASSERT_TRUE(body.find("send(") == std::string::npos);
  size_t takers = 0;
  for (const auto& e : std::filesystem::recursive_directory_iterator("src")) {
    if (!e.is_regular_file()) continue;
    const std::string path = e.path().generic_string();
    const size_t n = countOf(readFile(path.c_str()), "takeStorageNotice()");
    if (path == "src/agent/telegram.cpp") TEST_ASSERT_EQUAL_size_t(1, n);
    takers += n;
  }
  TEST_ASSERT_EQUAL_size_t(3, takers);   // the declaration, the definition, the poll task
  const std::string tg = readFile("src/agent/telegram.cpp");
  TEST_ASSERT_TRUE(tg.find("if (haveToken) sendStorageNotice();") != std::string::npos);
}

// GET /api/nvs/stats counts entries the way the partition does: the listing's string
// arithmetic agrees with the full-partition fake above for every size up to the
// connectors cap, and an int is one entry.
static void test_listing_entry_math_matches_the_partition_model() {
  for (size_t n = 0; n <= 3600; ++n)
    TEST_ASSERT_EQUAL_size_t(FakeNvs::strEntries(n), nvsEntriesFor(NvsKind::Str, n + 1));
  TEST_ASSERT_EQUAL_size_t(1, nvsEntriesFor(NvsKind::Int, 0));
  TEST_ASSERT_EQUAL_size_t(2 + 4, nvsEntriesFor(NvsKind::Blob, 100));
  TEST_ASSERT_EQUAL_STRING("str", nvsKindName(NvsKind::Str));
}

// The listing names keys and sizes, never values: the route is owner-only, the
// inventory opens namespaces read-only, asks NVS for sizes with a NULL buffer only,
// and writes nothing.
static void test_nvs_stats_names_never_values() {
  const std::string web = readFile("src/net/webui.cpp");
  const size_t r = web.find("s_server.on(\"/api/nvs/stats\", HTTP_GET");
  TEST_ASSERT_TRUE(r != std::string::npos);
  const size_t body = web.find('{', r);
  const size_t gate = web.find("if (authBlocked(r)) return;", r);
  TEST_ASSERT_TRUE(gate != std::string::npos && gate - body < 16);   // the first statement
  const std::string inv = readFile("src/sys/nvs_inventory.cpp");
  TEST_ASSERT_EQUAL_size_t(1, countOf(inv, "nvs_get_str("));
  TEST_ASSERT_EQUAL_size_t(1, countOf(inv, "nvs_get_blob("));
  TEST_ASSERT_TRUE(inv.find("nvs_get_str(h, key, nullptr, &len)") != std::string::npos);
  TEST_ASSERT_TRUE(inv.find("nvs_get_blob(h, key, nullptr, &len)") != std::string::npos);
  TEST_ASSERT_EQUAL_size_t(0, countOf(inv, "nvs_get_i") + countOf(inv, "nvs_get_u"));
  TEST_ASSERT_TRUE(inv.find("NVS_READWRITE") == std::string::npos);
  TEST_ASSERT_TRUE(inv.find("NVS_READONLY") != std::string::npos);
  TEST_ASSERT_TRUE(inv.find("nvs_set_") == std::string::npos && inv.find("nvs_erase_") == std::string::npos);
}

// ---- 7. the reserve (program 2026-10-18 lane C) -----------------------------------
// Every key the firmware defines has a class, so the reserve knows what may spend the
// last free entries. The class: a NEW key with no row fails here (it would silently
// read as owner config). Scans every place a key name is defined.
static std::vector<std::string> definedKeys(const char* path, const std::regex& re) {
  const std::string src = readFile(path);
  std::vector<std::string> out;
  for (auto it = std::sregex_iterator(src.begin(), src.end(), re); it != std::sregex_iterator(); ++it)
    out.push_back((*it)[1]);
  return out;
}

static void assertClassified(const char* ns, const std::string& key, const char* where) {
  // A name ending in '_' is a family prefix: any member must be classified.
  const std::string probe = key.back() == '_' ? key + "openai" : key;
  const std::string msg = std::string(where) + ": " + ns + "/" + key + " has no class";
  TEST_ASSERT_NOT_NULL_MESSAGE(findNvsKeyClass(ns, probe.c_str()), msg.c_str());
}

static void test_every_defined_key_has_a_class() {
  size_t checked = 0;
  for (const std::string& k : definedKeys("src/agent/agent_config.h",
                                          std::regex(R"re(#define\s+AKEY_\w+\s+"([^"]+)")re"))) {
    assertClassified("solide", k, "agent_config.h");
    ++checked;
  }
  TEST_ASSERT_TRUE(checked >= 110);   // every AKEY_* (116 at the time of writing)
  for (const std::string& k : definedKeys("src/sys/config_nvs.cpp",
                                          std::regex(R"re(constexpr char k\w+Key\[\]\s*=\s*"([^"]+)")re")))
    assertClassified("solide", k, "config_nvs.cpp");
  for (const std::string& k : definedKeys("include/nimbus_config.h",
                                          std::regex(R"re(#define\s+NIMBUS_KEY_\w+\s+"([^"]+)")re")))
    assertClassified("solide", k, "nimbus_config.h");
  for (const std::string& k : definedKeys("src/agent/memory_subsystem.cpp",
                                          std::regex(R"re(const char\*\s+k\w+Key\s*=\s*"([^"]+)")re")))
    assertClassified("orchmem", k, "memory_subsystem.cpp");
  for (const std::string& k : definedKeys("src/sys/ota_update.cpp",
                                          std::regex(R"re(const char\*\s+k\w+Key\s*=\s*"([^"]+)")re")))
    assertClassified("solide", k, "ota_update.cpp");
  for (const std::string& k : definedKeys("src/agent/provider_verify.cpp",
                                          std::regex(R"re(const char\*\s+const\s+k\w+Pfx\s*=\s*"([^"]+)")re")))
    assertClassified("solide", k, "provider_verify.cpp");
  // Keys spelled inline in the store (a literal or a String prefix).
  for (const std::string& k : definedKeys("src/agent/store.cpp",
                                          std::regex(R"re((?:putInt|putStr|putStrShrinkSafe|putBool|putOrClear|putIntDef|putBoolDef|clearKey)\("(\w+)")re")))
    assertClassified("solide", k, "store.cpp");
  for (const std::string& k : definedKeys("src/agent/store.cpp", std::regex(R"re(String\("(\w+_)"\))re")))
    assertClassified("solide", k, "store.cpp");
  // The journal slots j0..j<max>.
  for (int i = 0; i < 10; ++i)
    assertClassified("agjournal", "j" + std::to_string(i), "orch_persist.cpp");
  // Counter-test: an undefined key really has no row (the lookup does not match all).
  TEST_ASSERT_NULL(findNvsKeyClass("solide", "brandNewKey"));
  TEST_ASSERT_NULL(findNvsKeyClass("orchmem", "tgOffset"));   // namespaces are distinct
  TEST_ASSERT_EQUAL((int)NvsClass::Owner, (int)classifyNvsKey("solide", "brandNewKey"));
}

// The critical set is exactly the device's safety; the eviction list is exactly the
// rebuildable caches the plan names, and never the managed-agent ids or model choices.
static void test_key_classes_and_eviction_rows() {
  const char* critical[] = {"tgOffset", "otaPend", "otaBoots", "otaPrev", "otaLast",
                            "webTok", "cloudDevId", "cloudCred", "cloudHost"};
  for (const char* k : critical)
    TEST_ASSERT_EQUAL_MESSAGE((int)NvsClass::Critical, (int)classifyNvsKey("solide", k), k);
  const char* neverEvicted[] = {"antEnv", "antAgents", "antOrchAg", "mch_openai", "vfy_openai",
                                "usgLedger", "connectors", "tgOffset", "cloudCred"};
  for (const char* k : neverEvicted) {
    const NvsKeyClass* row = findNvsKeyClass("solide", k);
    TEST_ASSERT_NOT_NULL_MESSAGE(row, k);
    TEST_ASSERT_EQUAL_MESSAGE(0, row->evictOrder, k);
  }
  size_t n = 0;
  const NvsKeyClass* t = nvsKeyTable(&n);
  for (size_t i = 0; i < n; ++i) {
    if (!t[i].evictOrder) continue;
    // Only rebuildable caches, plus fbRules (owner config, evicted only as a cloud copy).
    const bool ok = t[i].cls == NvsClass::Rebuildable || std::string(t[i].key) == "fbRules";
    TEST_ASSERT_TRUE_MESSAGE(ok, t[i].key);
  }
  TEST_ASSERT_EQUAL((int)NvsClass::Telemetry, (int)classifyNvsKey("agjournal", "j3"));
  TEST_ASSERT_EQUAL((int)NvsClass::Telemetry, (int)classifyNvsKey("orchmem", "scratchB"));
  TEST_ASSERT_EQUAL((int)NvsClass::Owner, (int)classifyNvsKey("orchmem", "memcfg"));
}

static void test_eviction_plan_order_and_exclusions() {
  const char* keys[] = {"antEnv", "dreamScrHash", "vfy_openai", "mch_openai", "fbRules",
                        "vrs_openai", "oaiKey", "mcat_zai", "vrm_anthropic", "antAgents",
                        "tgOffset", "usgLedger"};
  const size_t n = sizeof(keys) / sizeof(keys[0]);
  size_t out[16];
  const size_t m = nvsEvictionPlan(keys, n, /*fbRulesIsCloudCopy=*/false, out, 16);
  std::vector<std::string> got;
  for (size_t i = 0; i < m; ++i) got.push_back(keys[out[i]]);
  const std::vector<std::string> want = {"vrs_openai", "vrm_anthropic", "mcat_zai", "dreamScrHash"};
  TEST_ASSERT_EQUAL_size_t(want.size(), got.size());
  for (size_t i = 0; i < want.size(); ++i) TEST_ASSERT_EQUAL_STRING(want[i].c_str(), got[i].c_str());
  // A cloud copy of the fallback rules goes last; a local owner edit never goes.
  const size_t m2 = nvsEvictionPlan(keys, n, true, out, 16);
  TEST_ASSERT_EQUAL_size_t(want.size() + 1, m2);
  TEST_ASSERT_EQUAL_STRING("fbRules", keys[out[m2 - 1]]);
  // The output cap is respected.
  TEST_ASSERT_EQUAL_size_t(2, nvsEvictionPlan(keys, n, true, out, 2));
}

// The arithmetic matches the fake partition's (one header + data entries with the NUL).
static void test_value_entries_match_the_partition_model() {
  for (size_t b = 0; b < 4000; b += 7)
    TEST_ASSERT_EQUAL_size_t(FakeNvs::strEntries(b), nvsValueEntries(NvsValKind::Str, b));
  TEST_ASSERT_EQUAL_size_t(1, nvsValueEntries(NvsValKind::Int, 0));
  TEST_ASSERT_EQUAL_size_t(2, nvsValueEntries(NvsValKind::Blob, 0));
  TEST_ASSERT_EQUAL_size_t(3, nvsValueEntries(NvsValKind::Blob, 32));
  TEST_ASSERT_EQUAL_size_t(4, nvsValueEntries(NvsValKind::Blob, 33));
}

// The reserve property, over every class row and a spread of value sizes: a write that
// would leave fewer than the reserve is refused unless it is critical; one more free
// entry and it lands. A write that cannot fit at all - fewer available entries than the
// new value takes, because the IDF writes the new value before it erases the old one -
// is refused for every class (RefuseNoRoom, the L19-20 "nothing left" refusal: the
// driver would fail it anyway, and a refusal reports through notePersist like any
// failed write). Erases and non-growing rewrites always land, even at 0 free.
static void test_reserve_property_over_every_row() {
  size_t n = 0;
  const NvsKeyClass* t = nvsKeyTable(&n);
  const size_t sizes[] = {0, 1, 8, 31, 32, 100, 500, 3500};
  for (size_t i = 0; i < n; ++i) {
    for (size_t b : sizes) {
      for (int kind = 0; kind < 3; ++kind) {
        NvsAdmitIn in;
        in.cls = t[i].cls;
        in.newEntries = nvsValueEntries((NvsValKind)kind, b);
        // A new key, then a growing rewrite of a smaller stored value.
        for (size_t old : {(size_t)0, in.newEntries > 2 ? in.newEntries - 2 : (size_t)0}) {
          in.oldEntries = old;
          const size_t growth = in.newEntries - old;
          const bool critical = t[i].cls == NvsClass::Critical;
          // One entry short: refused - RefuseNoRoom when even the whole value cannot
          // fit, otherwise the reserve's RefuseReserve (critical still lands).
          in.available = kNvsReserveEntries + growth - 1;
          const int oneShort = in.available < in.newEntries
                                   ? (int)NvsAdmit::RefuseNoRoom
                                   : critical ? (int)NvsAdmit::Write : (int)NvsAdmit::RefuseReserve;
          TEST_ASSERT_EQUAL_MESSAGE(oneShort, (int)nvsAdmit(in), t[i].key);
          // One more entry: the reserve is whole, so it lands - unless the value
          // itself still cannot fit (a rewrite over a 24-entry stored value), which
          // is RefuseNoRoom for every class.
          in.available = kNvsReserveEntries + growth;
          const int enough = in.available < in.newEntries
                                  ? (int)NvsAdmit::RefuseNoRoom : (int)NvsAdmit::Write;
          TEST_ASSERT_EQUAL_MESSAGE(enough, (int)nvsAdmit(in), t[i].key);
          // Starved (the pass could not restore the reserve): owner config lands
          // again when it fits, rebuildable and telemetry stay refused. When the
          // partition truly has nothing left, nothing growing lands, whatever its
          // class.
          in.available = in.newEntries;
          in.starved = true;
          if (in.available < kNvsReserveEntries + growth) {
            const bool lands = critical || t[i].cls == NvsClass::Owner;
            TEST_ASSERT_EQUAL_MESSAGE(lands ? (int)NvsAdmit::Write : (int)NvsAdmit::RefuseReserve,
                                      (int)nvsAdmit(in), t[i].key);
          }
          in.available = 0;
          TEST_ASSERT_EQUAL_MESSAGE((int)NvsAdmit::RefuseNoRoom, (int)nvsAdmit(in), t[i].key);
          in.starved = false;
        }
        // Erase and a same-size or smaller rewrite: always, at 0 free, any class.
        in.available = 0;
        in.erase = true;
        TEST_ASSERT_EQUAL((int)NvsAdmit::Write, (int)nvsAdmit(in));
        in.erase = false;
        in.oldEntries = in.newEntries;
        TEST_ASSERT_EQUAL((int)NvsAdmit::Write, (int)nvsAdmit(in));
        in.oldEntries = in.newEntries + 3;
        TEST_ASSERT_EQUAL((int)NvsAdmit::Write, (int)nvsAdmit(in));
      }
    }
  }
}

// L19-20a: the available-entries source (nvs_get_stats on the device) is read before
// every growing write, critical ones included, and never for a write that does not
// grow (an erase never reaches the gate: the device seam short-circuits it); with
// nothing left, every growing write is refused whatever its class (a refusal reports
// through notePersist like any failed write - the helper pattern
// test_device_seam_asks_the_reserve pins). Exercised through nvsAdmitWrite, the exact
// path src/sys/nvs_guard.cpp takes.
static void test_growing_writes_read_stats_and_refuse_when_nothing_is_left() {
  int reads = 0;
  const auto available = [&reads]() {
    ++reads;
    return (size_t)0;
  };
  // Non-growing rewrites never read the stats: the remedy always works, and the
  // device seam short-circuits erases before the gate (an erase only frees entries).
  reads = 0;
  TEST_ASSERT_EQUAL((int)NvsAdmit::Write, (int)nvsAdmitWrite(NvsClass::Owner, NvsValKind::Str,
                                                             10, 12, false, available));
  TEST_ASSERT_EQUAL_INT(0, reads);
  TEST_ASSERT_TRUE(readFile("src/sys/nvs_guard.cpp").find("if (erase) return true;") !=
                   std::string::npos);
  // A growing write reads them exactly once, whatever its class - critical included.
  for (int c = 0; c < 4; ++c) {
    reads = 0;
    TEST_ASSERT_EQUAL((int)NvsAdmit::RefuseNoRoom,
                      (int)nvsAdmitWrite((NvsClass)c, NvsValKind::Str, 10, 0, false,
                                         available));
    TEST_ASSERT_EQUAL_INT(1, reads);
  }
  // A blob reads them even when it shrinks: the span check comes first.
  reads = 0;
  TEST_ASSERT_EQUAL((int)NvsAdmit::RefuseNoRoom,
                    (int)nvsAdmitWrite(NvsClass::Owner, NvsValKind::Blob, 64, 100, false,
                                       available));
  TEST_ASSERT_EQUAL_INT(1, reads);
  // With entries left, the critical growing write lands (it may spend the reserve).
  TEST_ASSERT_EQUAL((int)NvsAdmit::Write, (int)nvsAdmitWrite(NvsClass::Critical, NvsValKind::Str,
                                                             10, 0, false,
                                                             [] { return (size_t)5; }));
  // The device gate asks it the same way: every class, stats read, starved passed on.
  const std::string guard = readFile("src/sys/nvs_guard.cpp");
  TEST_ASSERT_TRUE(guard.find("nvsAdmitWrite(classifyNvsKey(ns, key), kind, bytes, "
                              "storedEntries(ns, key),") != std::string::npos);
  TEST_ASSERT_TRUE(guard.find("s_starved.load(), [] { return nvsAvailableEntries(); })") !=
                   std::string::npos);
}

// L19-20b: a blob write, new or rewrite, is admitted only when the available entries
// cover the whole span the value takes while the IDF writes it (nvsBlobWriteSpan: one
// data entry per 32 B, one chunk header per 125-entry page, the index entry, and up to
// 13 entries of a first page tail too small for the value, which still count as free -
// ESP-IDF 5.5.4 skips a tail under 400 B and keeps one entry back). At span minus 1
// the write is refused before nvs_set_blob is ever called, so the IDF never enters the
// multi-page cleanup that strands entries after a failed rewrite; at span it lands.
struct SpyBlobNvs {
  size_t free = 0;
  int    setBlobCalls = 0;
  bool setBlob(const char*, const std::string& v) {
    ++setBlobCalls;
    const size_t need = nvsBlobWriteSpan(v.size());
    if (need > free) return false;
    free -= need;
    return true;
  }
};

// The device pattern: the gate first, and only an admission calls the blob writer.
static bool deviceBlobWrite(SpyBlobNvs& nvs, NvsClass cls, size_t bytes, size_t oldEntries) {
  const NvsAdmit a = nvsAdmitWrite(cls, NvsValKind::Blob, bytes, oldEntries, false,
                                   [&nvs] { return nvs.free; });
  if (a != NvsAdmit::Write) return false;   // reported through notePersist, like any failed write
  return nvs.setBlob("blob", std::string(bytes, 'x'));
}

static void test_blob_span_check_refuses_before_nvs_set_blob() {
  // The span model, pinned: a 32 B blob on the worst page (data + chunk header + index
  // + a skipped 1-entry tail) and a 4,000 B one (125 data + a header + the index + the
  // 13-entry tail allowance); always at least the value's own entries.
  TEST_ASSERT_EQUAL_size_t(4, nvsBlobWriteSpan(32));
  TEST_ASSERT_EQUAL_size_t(140, nvsBlobWriteSpan(4000));
  for (size_t b = 0; b <= 4000; b += 61)
    TEST_ASSERT_TRUE(nvsBlobWriteSpan(b) >= nvsValueEntries(NvsValKind::Blob, b));
  // At span minus 1 the write is refused and nvs_set_blob is never called.
  SpyBlobNvs nvs{nvsBlobWriteSpan(1000) - 1};
  TEST_ASSERT_FALSE(deviceBlobWrite(nvs, NvsClass::Owner, 1000, 0));
  TEST_ASSERT_EQUAL_INT(0, nvs.setBlobCalls);
  // At span it lands: the span gate is the only thing between span-1 and span, so the
  // class is critical (an owner blob additionally waits for the reserve - the
  // property below covers that composition).
  nvs.free = nvsBlobWriteSpan(1000);
  TEST_ASSERT_TRUE(deviceBlobWrite(nvs, NvsClass::Critical, 1000, 0));
  TEST_ASSERT_EQUAL_INT(1, nvs.setBlobCalls);
  // A REWRITE that shrinks is span-checked all the same: the IDF writes the new
  // version completely before it erases the old one (the Lumi failure mode), and so
  // is a critical blob.
  const size_t span500 = nvsBlobWriteSpan(500);
  nvs.free = span500 - 1;
  TEST_ASSERT_FALSE(deviceBlobWrite(nvs, NvsClass::Owner, 500,
                                    nvsValueEntries(NvsValKind::Blob, 2000)));
  TEST_ASSERT_EQUAL_INT(1, nvs.setBlobCalls);   // only the 1000 B call above
  TEST_ASSERT_FALSE(deviceBlobWrite(nvs, NvsClass::Critical, 500, 0));
  TEST_ASSERT_EQUAL_INT(1, nvs.setBlobCalls);
  nvs.free = span500;
  TEST_ASSERT_TRUE(deviceBlobWrite(nvs, NvsClass::Owner, 500,
                                   nvsValueEntries(NvsValKind::Blob, 2000)));
  TEST_ASSERT_EQUAL_INT(2, nvs.setBlobCalls);
  // The property: blob sizes 1 to 4,000 B, free counts 0 to 60. A critical blob lands
  // exactly when its span fits; an owner blob additionally waits for the reserve.
  // Either refusal leaves nvs_set_blob uncalled.
  for (size_t b = 1; b <= 4000; ++b) {
    const size_t span = nvsBlobWriteSpan(b);
    const size_t entries = nvsValueEntries(NvsValKind::Blob, b);
    for (size_t free = 0; free <= 60; ++free) {
      SpyBlobNvs crit{free};
      const bool critLands = free >= span;
      TEST_ASSERT_TRUE(deviceBlobWrite(crit, NvsClass::Critical, b, 0) == critLands);
      TEST_ASSERT_EQUAL_INT(critLands ? 1 : 0, crit.setBlobCalls);
      SpyBlobNvs own{free};
      const bool ownerLands = free >= span && free >= kNvsReserveEntries + entries;
      TEST_ASSERT_TRUE(deviceBlobWrite(own, NvsClass::Owner, b, 0) == ownerLands);
      TEST_ASSERT_EQUAL_INT(ownerLands ? 1 : 0, own.setBlobCalls);
    }
  }
}

// The reserve covers the critical working set: a cloud credential re-mint and the OTA
// guard rewrite, plus the Telegram offset, all on a device with no other room.
static void test_reserve_covers_the_critical_working_set() {
  const size_t credential = nvsValueEntries(NvsValKind::Str, 256) +   // cloudCred
                            nvsValueEntries(NvsValKind::Str, 36) +    // cloudDevId
                            nvsValueEntries(NvsValKind::Str, 21);     // cloudHost
  const size_t otaGuard = 2 * nvsValueEntries(NvsValKind::Int, 0) +   // otaPend, otaBoots
                          nvsValueEntries(NvsValKind::Str, 4) +       // otaPrev "app1"
                          nvsValueEntries(NvsValKind::Str, 30);       // otaLast
  const size_t offset = nvsValueEntries(NvsValKind::Int, 0);
  TEST_ASSERT_TRUE(credential + otaGuard + offset <= kNvsReserveEntries);
  TEST_ASSERT_TRUE(kNvsReserveEntries >= 24);   // the owner decision (Q4): at least 24
}

// The reserve in action on a full-partition fake, the way the device gates: at
// reserve+1 free, non-critical saves are refused while tgOffset and otaPend land; a
// removal lands; and once the pass marks the device starved, an owner save lands while
// a telemetry write stays refused.
struct GatedNvs {
  FakeNvs nvs;
  bool    starved = false;
  explicit GatedNvs(size_t cap) : nvs(cap) {}
  size_t stored(const char* k) const {
    auto s = nvs.str.find(k);
    if (s != nvs.str.end()) return FakeNvs::strEntries(s->second.size());
    return nvs.ints.count(k) ? 1 : 0;
  }
  bool admitted(const char* ns, const char* k, NvsValKind kind, size_t bytes) const {
    NvsAdmitIn in;
    in.cls = classifyNvsKey(ns, k);
    in.newEntries = nvsValueEntries(kind, bytes);
    in.oldEntries = stored(k);
    in.available = nvs.freeEntries();
    in.starved = starved;
    return nvsAdmit(in) == NvsAdmit::Write;
  }
  bool putStr(const char* k, const std::string& v) {
    return admitted("solide", k, NvsValKind::Str, v.size()) && writeString(nvs, k, v);
  }
  bool putInt(const char* k, int32_t v) {
    return admitted("solide", k, NvsValKind::Int, 0) && nvs.setInt(k, v);
  }
  void fillTo(size_t freeLeft) {
    int i = 0;
    while (nvs.freeEntries() > freeLeft) nvs.ints["own_fill" + std::to_string(i++)] = 1;
  }
};

static void test_reserve_keeps_critical_writes_landing_on_a_filling_device() {
  GatedNvs g(504);
  g.nvs.ints["tgOffset"] = 100;
  g.nvs.ints["otaPend"] = 0;
  g.fillTo(kNvsReserveEntries + 1);
  TEST_ASSERT_FALSE(g.putStr("orchHost", "zai"));             // owner config: refused
  TEST_ASSERT_FALSE(g.putStr("vrs_openai", "nocredits"));     // rebuildable: refused
  TEST_ASSERT_FALSE(g.putStr("orchConv", "openai|c_123"));    // telemetry: refused
  // A new int takes one entry: at reserve+1 it may land, at the reserve it may not.
  TEST_ASSERT_TRUE(g.putInt("lbPingEp", 1760000000));
  TEST_ASSERT_FALSE(g.putInt("loopMaxCnt", 4));               // a NEW owner int: refused
  // The writes the device's safety depends on keep landing, many times over.
  for (int i = 0; i < 50; ++i) {
    TEST_ASSERT_TRUE(g.putInt("tgOffset", 101 + i));
    TEST_ASSERT_TRUE(g.putInt("otaPend", i & 1));
  }
  TEST_ASSERT_TRUE(g.putStr("otaPrev", "app1"));              // a new critical key lands too
  TEST_ASSERT_TRUE(g.nvs.freeEntries() >= kNvsReserveEntries - 3);
  // The remedy still works: removing a key is an erase.
  g.nvs.str["zaiKey"] = std::string(49, 'z');
  TEST_ASSERT_TRUE(eraseLanded(g.nvs, "zaiKey"));
  // Starved (the pass could not restore the reserve): owner config is accepted again.
  g.fillTo(4);
  g.starved = true;
  TEST_ASSERT_TRUE(g.putStr("orchHost", "zai"));
  TEST_ASSERT_FALSE(g.putStr("orchConv", "openai|c_123"));
  TEST_ASSERT_FALSE(g.putStr("vrs_openai", "nocredits"));
  // Fresh device: nothing is refused.
  GatedNvs fresh(504);
  TEST_ASSERT_TRUE(fresh.putStr("orchConv", "openai|c_1"));
  TEST_ASSERT_TRUE(fresh.putStr("vrs_openai", "nocredits"));
  TEST_ASSERT_TRUE(fresh.putInt("lbPingEp", 1));
}

// The eviction pass, end to end on the fake: evict in plan order until the reserve is
// back; a device whose fill is all owner config cannot get there and is starved.
static size_t runEvictionPass(FakeNvs& nvs, bool fbCloud, bool* starved) {
  std::vector<std::string> names;
  for (const auto& kv : nvs.str) names.push_back(kv.first);
  for (const auto& kv : nvs.ints) names.push_back(kv.first);
  std::vector<const char*> keys;
  for (const auto& k : names) keys.push_back(k.c_str());
  std::vector<size_t> order(keys.size());
  const size_t m = nvsEvictionPlan(keys.data(), keys.size(), fbCloud, order.data(), order.size());
  size_t evicted = 0;
  for (size_t i = 0; i < m && nvs.freeEntries() < kNvsReserveEntries; ++i)
    if (eraseLanded(nvs, keys[order[i]])) ++evicted;
  *starved = nvs.freeEntries() < kNvsReserveEntries;
  return evicted;
}

static void test_eviction_pass_restores_the_reserve_or_reports_starved() {
  FakeNvs nvs(504);
  nvs.str["antAgents"] = std::string(300, 'a');
  nvs.str["mch_openai"] = std::string(200, 'm');
  nvs.str["vrs_openai"] = "nocredits";
  nvs.str["vrs_zai"] = "connectfail";
  nvs.ints["vrm_openai"] = 7156;
  nvs.str["dreamScrHash"] = "0123456789abcdef";
  nvs.str["fbRules"] = std::string(600, 'f');
  int i = 0;
  while (nvs.freeEntries() > 4) nvs.ints["own_" + std::to_string(i++)] = 1;
  bool starved = false;
  // Local fallback rules (an owner edit): kept; the caches go, but are not enough.
  size_t ev = runEvictionPass(nvs, /*fbCloud=*/false, &starved);
  TEST_ASSERT_EQUAL_size_t(4, ev);
  TEST_ASSERT_TRUE(starved);
  TEST_ASSERT_TRUE(nvs.has("fbRules"));
  TEST_ASSERT_TRUE(nvs.has("antAgents"));
  TEST_ASSERT_TRUE(nvs.has("mch_openai"));
  // A cloud copy of the rules is rebuildable: evicting it restores the reserve.
  ev = runEvictionPass(nvs, /*fbCloud=*/true, &starved);
  TEST_ASSERT_EQUAL_size_t(1, ev);
  TEST_ASSERT_FALSE(starved);
  TEST_ASSERT_TRUE(nvs.has("antAgents"));   // never: orphans remote agents
  TEST_ASSERT_TRUE(nvs.has("mch_openai"));  // never: no file store for it yet
}

// The device seam: every store helper asks the reserve for the whole write before it
// writes; the gated NvsKv is what other callers get; the eviction pass runs on the main
// loop (otaupd::tick) and no web route runs it.
static void test_device_seam_asks_the_reserve() {
  const std::string st = readFile("src/agent/store.cpp");
  const size_t hb = st.find("// ---- write helpers (CUM-479)");
  const size_t he = st.find("// ---- provider-config read cache (CUM-238)");
  TEST_ASSERT_TRUE(hb != std::string::npos && he != std::string::npos && hb < he);
  const std::string helpers = st.substr(hb, he - hb);
  TEST_ASSERT_TRUE(helpers.find("nimbus::sys::NvsKvRaw g_nvs;") != std::string::npos);
  // Every helper that can grow NVS asks admit( on the same statement as its write.
  for (const char* w : {"nimbus::store::writeString(g_nvs", "nimbus::store::writeShrinkSafe(g_nvs",
                        "solide::memory::setInt(", "solide::memory::setBool("}) {
    size_t p = helpers.find(w);
    TEST_ASSERT_TRUE_MESSAGE(p != std::string::npos, w);
    for (; p != std::string::npos; p = helpers.find(w, p + 1)) {
      const size_t s = helpers.rfind("return notePersist(", p);
      TEST_ASSERT_TRUE_MESSAGE(s != std::string::npos, w);
      TEST_ASSERT_TRUE_MESSAGE(helpers.substr(s, p - s).find("admit(") != std::string::npos, w);
    }
  }
  const std::string h = readFile("src/sys/config_nvs.h");
  TEST_ASSERT_TRUE(h.find("return nvsAdmit(kNvsNsSolide, k, nimbus::store::NvsValKind::Str, v.length()) &&") !=
                   std::string::npos);
  const std::string cfg = readFile("src/sys/config_nvs.cpp");
  TEST_ASSERT_TRUE(cfg.find("nvsAdmit(kNvsNsSolide, key, nimbus::store::NvsValKind::Int, 0)") !=
                   std::string::npos);
  // The pass: called from the OTA main-loop tick, from nowhere in the web layer.
  TEST_ASSERT_TRUE(readFile("src/sys/ota_update.cpp").find("agent::store::nvsMaintain(now);") !=
                   std::string::npos);
  for (const char* web : {"src/net/webui.cpp", "src/net/web_memory.cpp"})
    TEST_ASSERT_TRUE_MESSAGE(readFile(web).find("nvsMaintain") == std::string::npos, web);
}


// ---- 7. the class guards: verdicts are never dropped (lane C) -----------------------
// Every store setter is [[nodiscard]], but there is no -Werror, so this is the
// enforcement: a call statement whose result is thrown away fails here unless its file
// is in the reviewed baseline below at exactly that count (the baseline only shrinks:
// a fixed call must lower it). An intentional, reviewed discard is written (void)...
// and does not count. The scan: a store::set*/clear*/replace* call is DISCARDED when
// it starts a statement (after ; { } ) else, or at a line start whose previous line
// ends one); a call inside an expression (return, =, &&, an argument) is used.
static std::string trimRight(std::string s) {
  while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
  return s;
}

static bool endsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// True when code ending in `ctx` (already right-trimmed) is followed by a new statement.
static bool startsStatement(const std::string& ctx) {
  return ctx.empty() || std::strchr(";{})", ctx.back()) || endsWith(ctx, "else");
}

static std::vector<int> discardedSetterLines(const std::string& text) {
  std::regex call(R"re((agent::)?store::(set|clear|replace)\w*\s*\()re");
  std::vector<int> out;
  std::string prev;
  int lineNo = 0;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(start, end - start);
    ++lineNo;
    line = line.substr(0, line.find("//"));
    for (auto it = std::sregex_iterator(line.begin(), line.end(), call); it != std::sregex_iterator(); ++it) {
      const std::string before = trimRight(line.substr(0, (size_t)it->position()));
      if (endsWith(before, "(void)")) continue;
      if (startsStatement(before.empty() ? trimRight(prev) : before)) out.push_back(lineNo);
    }
    if (!trimRight(line).empty()) prev = line;
    start = end + 1;
  }
  return out;
}

static void test_store_setter_verdicts_are_never_silently_dropped() {
  // The reviewed baseline (program 2026-10-18): files of other owners whose discards
  // a SaveScope or an explicit retry already covers, frozen at their count. Lane C's
  // files are at 0 (ota_update.cpp, store.cpp, connectors.cpp, the Telegram routes).
  const std::map<std::string, size_t> baseline = {
      {"src/agent/dream_subsystem.cpp", 1}, {"src/agent/memory_subsystem.cpp", 2},
      {"src/agent/orchestrator.cpp", 16},   {"src/agent/provider_verify.cpp", 8},
      {"src/agent/store_config.cpp", 1},    {"src/agent/telegram.cpp", 2},
      {"src/agent/transport_tls.cpp", 2},   {"src/main.cpp", 24},
      {"src/net/cloud_mint.cpp", 2},        {"src/net/relay_client.cpp", 6},
      {"src/net/web_memory.cpp", 1},        {"src/net/webui.cpp", 91},
      {"src/test_console.cpp", 5},
  };
  for (const char* root : {"src", "lib"}) {
    for (const auto& e : std::filesystem::recursive_directory_iterator(root)) {
      if (!e.is_regular_file()) continue;
      const std::string path = e.path().generic_string();
      const std::string ext = e.path().extension().string();
      if (ext != ".cpp" && ext != ".h") continue;
      const std::string text = readFile(path.c_str());
      if (text.find("store::") == std::string::npos) continue;
      const std::vector<int> lines = discardedSetterLines(text);
      auto it = baseline.find(path);
      const size_t allowed = it == baseline.end() ? 0 : it->second;
      std::string msg = path + ": " + std::to_string(lines.size()) + " discarded setter verdict(s), baseline " +
                        std::to_string(allowed) + " (check the result, or write (void) for a reviewed discard)";
      if (!lines.empty()) msg += "; first at line " + std::to_string(lines[0]);
      TEST_ASSERT_EQUAL_size_t_MESSAGE(allowed, lines.size(), msg.c_str());
    }
  }
  // Counter-tests: the scan sees a discard in each statement shape, and no false one.
  TEST_ASSERT_EQUAL_size_t(1, discardedSetterLines("  store::setOtaPending(0);\n").size());
  TEST_ASSERT_EQUAL_size_t(1, discardedSetterLines("  if (x) agent::store::setX(1);\n").size());
  TEST_ASSERT_EQUAL_size_t(1, discardedSetterLines("  if (x)\n    store::clearKey(k);\n").size());
  TEST_ASSERT_EQUAL_size_t(1, discardedSetterLines("  a(); store::replaceX(1);\n").size());
  TEST_ASSERT_EQUAL_size_t(0, discardedSetterLines("  return store::setX(1);\n").size());
  TEST_ASSERT_EQUAL_size_t(0, discardedSetterLines("  ok = ok && store::setX(1);\n").size());
  TEST_ASSERT_EQUAL_size_t(0, discardedSetterLines("  const bool ok =\n      store::setX(1);\n").size());
  TEST_ASSERT_EQUAL_size_t(0, discardedSetterLines("  if (!store::setX(1)) return;\n").size());
  TEST_ASSERT_EQUAL_size_t(0, discardedSetterLines("  (void)store::setX(1);   // reviewed\n").size());
  TEST_ASSERT_EQUAL_size_t(0, discardedSetterLines("  // store::setX(1);\n").size());
  // Every setter in the store header carries [[nodiscard]].
  const std::string h = readFile("src/agent/store.h");
  std::regex boolSetter(R"re(\bbool\s+((set|clear|replace)\w*)\s*\()re");
  for (auto it = std::sregex_iterator(h.begin(), h.end(), boolSetter); it != std::sregex_iterator(); ++it) {
    const size_t at = (size_t)it->position();
    const size_t ls = h.rfind('\n', at) + 1;
    const std::string head = h.substr(ls, at - ls);
    const bool marked = head.find("[[nodiscard]]") != std::string::npos ||
                        (at >= 14 && h.compare(at - 14, 14, "[[nodiscard]] ") == 0);
    TEST_ASSERT_TRUE_MESSAGE(marked, (*it)[1].str().c_str());
  }
}

// Every raw NVS writer reports its verdict: each raw write call sits in a statement
// that passes through notePersist (the "a raw writer skips the ledger" class: the
// journal, the scratchpad, the OTA guard and the verify side channels were all silent
// before). main.cpp writes only right after erasing the whole partition.
//
// L19-20c: the blob-capable primitives (putBytes, nvs_set_blob, nvs_set_str - the ones
// that can hit the multi-page blob path) are guarded over ALL of src/ and lib/: a file
// outside the reviewed set below that writes one fails here until it goes through the
// gated helpers (agent::store, NvsKv, the config_nvs saves). Reviewed raw writers:
// ota_update.cpp's guard wrappers (each raw line sits on a notePersist call) and
// main.cpp's factory-reset identity restore (best-effort, onto a partition it erased
// lines above, so there is nothing to refuse it).

// One file's raw blob/str lines against the reviewed set: file -> every raw line must
// sit on a notePersist call; a reviewed file without that demand writes onto a
// partition it just erased.
static void scanFileForRawBlobWriters(const std::string& path,
                                      const std::map<std::string, bool>& reviewed, int& seen) {
  const std::string text = readFile(path.c_str());
  if (text.find("putBytes") == std::string::npos &&
      text.find("nvs_set_blob") == std::string::npos &&
      text.find("nvs_set_str") == std::string::npos)
    return;
  std::regex blobPrim(R"re(\bputBytes\s*\(|\bnvs_set_blob\s*\(|\bnvs_set_str\s*\()re");
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(start, end - start);
    const size_t cm = line.find("//");
    if (cm != std::string::npos) line = line.substr(0, cm);
    if (std::regex_search(line, blobPrim)) {
      ++seen;
      const auto it = reviewed.find(path);
      TEST_ASSERT_TRUE_MESSAGE(it != reviewed.end(),
                               ("unreviewed raw blob/str NVS writer: " + path + ": " + line).c_str());
      if (it != reviewed.end() && it->second)
        TEST_ASSERT_TRUE_MESSAGE(line.find("notePersist(") != std::string::npos,
                                 (path + ": " + line).c_str());
    }
    start = end + 1;
  }
}

// L19-20c's all-of-src-and-lib scan over the blob-capable primitives, called by
// test_raw_nvs_writers_report_through_notePersist.
static void scanRawBlobWritersOutsideTheGatedHelpers() {
  const std::map<std::string, bool> reviewed = {
      {"src/sys/ota_update.cpp", true},
      {"src/main.cpp", false},   // the factory-reset identity restore, onto an erased partition
  };
  int seen = 0;
  for (const char* root : {"src", "lib"}) {
    for (const auto& e : std::filesystem::recursive_directory_iterator(root)) {
      if (!e.is_regular_file()) continue;
      const std::string path = e.path().generic_string();
      const std::string ext = e.path().extension().string();
      if (ext != ".cpp" && ext != ".h" && ext != ".hpp") continue;
      scanFileForRawBlobWriters(path, reviewed, seen);
    }
  }
  // The scan found the reviewed writers at all (the guard is not scanning nothing).
  TEST_ASSERT_TRUE(seen >= 2);
}

static void test_raw_nvs_writers_report_through_notePersist() {
  std::regex raw(R"re(solide::memory::(set[A-Z]\w*|eraseKey|clear)\(|nvs_set_\w+\(|nvs_erase_\w+\(|\b(p|s_prefs)\.(put[A-Z]\w*|remove|clear)\()re");
  const char* files[] = {"src/agent/provider_verify.cpp", "src/agent/memory_subsystem.cpp",
                         "src/agent/orch_persist.cpp", "src/sys/ota_update.cpp",
                         "src/sys/config_nvs.cpp"};
  for (const char* f : files) {
    const std::string src = readFile(f);
    size_t start = 0;
    int n = 0;
    while (start < src.size()) {
      size_t end = src.find('\n', start);
      if (end == std::string::npos) end = src.size();
      std::string line = src.substr(start, end - start);
      const size_t cm = line.find("//");
      if (cm != std::string::npos) line = line.substr(0, cm);
      if (std::regex_search(line, raw)) {
        ++n;
        TEST_ASSERT_TRUE_MESSAGE(line.find("notePersist(") != std::string::npos,
                                 (std::string(f) + ": " + line).c_str());
      }
      start = end + 1;
    }
    TEST_ASSERT_TRUE_MESSAGE(n > 0, f);   // the scan found the file's writers at all
  }
  scanRawBlobWritersOutsideTheGatedHelpers();
}

// ---- 8. the moved stores: the usage ledger and the scratchpad (lane C, D-2) -------
struct FakeMovedIo {
  bool fileExists = false, nvsExists = false;
  std::string file, nvs;
  bool failWrite = false, corruptWrite = false;
  int nvsErases = 0;
  int writes = 0;   // atomic writes attempted (the persist-on-change count)
  bool readFile(std::string& out) { if (!fileExists) return false; out = file; return true; }
  bool writeFileAtomic(const std::string& v) {
    ++writes;
    if (failWrite) return false;
    fileExists = true;
    file = corruptWrite ? v.substr(0, v.size() / 2) : v;
    return true;
  }
  bool readNvs(std::string& out) { if (!nvsExists) return false; out = nvs; return true; }
  bool eraseNvs() { ++nvsErases; nvsExists = false; nvs.clear(); return true; }
};

// The four migration legs (NVS only, file only, both, neither) and the retire rule.
static void test_moved_store_migration_legs() {
  using nimbus::store::MovedSource;
  // NVS only (the first boot of v4.5.14 on a device that ran v4.5.13).
  {
    FakeMovedIo io; io.nvsExists = true; io.nvs = "openai|1|2";
    const auto m = nimbus::store::loadMoved(io);
    TEST_ASSERT_EQUAL((int)MovedSource::Nvs, (int)m.src);
    TEST_ASSERT_EQUAL_STRING("openai|1|2", m.value.c_str());
    // Nothing to retire yet (no file): the legacy copy stays even on a valid boot.
    TEST_ASSERT_FALSE(nimbus::store::retireLegacy(io, true));
    TEST_ASSERT_TRUE(io.nvsExists);
    // The first persist writes the file, never NVS.
    TEST_ASSERT_TRUE(nimbus::store::persistMoved(io, "openai|1|3"));
    TEST_ASSERT_EQUAL_STRING("openai|1|2", io.nvs.c_str());
    // An OTA trial boot never retires (a rollback still reads v4.5.13's values)...
    TEST_ASSERT_FALSE(nimbus::store::retireLegacy(io, false));
    TEST_ASSERT_TRUE(io.nvsExists);
    // ...the boot after the image was marked valid does.
    TEST_ASSERT_TRUE(nimbus::store::retireLegacy(io, true));
    TEST_ASSERT_FALSE(io.nvsExists);
  }
  // File only (a device past its migration).
  {
    FakeMovedIo io; io.fileExists = true; io.file = "anthropic|5|6";
    const auto m = nimbus::store::loadMoved(io);
    TEST_ASSERT_EQUAL((int)MovedSource::File, (int)m.src);
    TEST_ASSERT_EQUAL_STRING("anthropic|5|6", m.value.c_str());
    TEST_ASSERT_FALSE(nimbus::store::retireLegacy(io, true));   // nothing in NVS to erase
    TEST_ASSERT_EQUAL_INT(0, io.nvsErases);
  }
  // Both: the file wins, the stale NVS copy goes on a valid boot only.
  {
    FakeMovedIo io; io.fileExists = true; io.file = "new"; io.nvsExists = true; io.nvs = "old";
    const auto m = nimbus::store::loadMoved(io);
    TEST_ASSERT_EQUAL((int)MovedSource::File, (int)m.src);
    TEST_ASSERT_EQUAL_STRING("new", m.value.c_str());
    TEST_ASSERT_FALSE(nimbus::store::retireLegacy(io, false));
    TEST_ASSERT_TRUE(nimbus::store::retireLegacy(io, true));
  }
  // Neither (a fresh device): empty, and the first persist creates the file.
  {
    FakeMovedIo io;
    const auto m = nimbus::store::loadMoved(io);
    TEST_ASSERT_EQUAL((int)MovedSource::None, (int)m.src);
    TEST_ASSERT_TRUE(m.value.empty());
    TEST_ASSERT_FALSE(nimbus::store::retireLegacy(io, true));
    TEST_ASSERT_TRUE(nimbus::store::persistMoved(io, ""));
    TEST_ASSERT_TRUE(io.fileExists);
    TEST_ASSERT_EQUAL((int)MovedSource::File, (int)nimbus::store::loadMoved(io).src);
  }
  // A write that did not land, or that reads back different, is not a save.
  {
    FakeMovedIo io; io.failWrite = true;
    TEST_ASSERT_FALSE(nimbus::store::persistMoved(io, "x|1"));
    FakeMovedIo torn; torn.corruptWrite = true;
    TEST_ASSERT_FALSE(nimbus::store::persistMoved(torn, "openai|123456"));
  }
  // The rule's truth table.
  for (int m = 0; m < 8; ++m)
    TEST_ASSERT_EQUAL((m & 7) == 7, nimbus::store::mayRetireLegacy(m & 1, m & 2, m & 4));
}

// The dirty flag: a failed persist leaves the store dirty, retried at most once a
// minute from the main loop; a landed one clears it. A legacy load is due at once.
static void test_moved_store_dirty_and_retry() {
  nimbus::store::DirtyRetry d(60000);
  TEST_ASSERT_TRUE(d.saved());
  TEST_ASSERT_FALSE(d.retryDue(5000));
  d.noteWrite(false, 10000);
  TEST_ASSERT_FALSE(d.saved());
  TEST_ASSERT_FALSE(d.retryDue(10000 + 59999));   // no retry storm
  TEST_ASSERT_TRUE(d.retryDue(10000 + 60000));
  d.noteWrite(false, 70000);
  TEST_ASSERT_FALSE(d.retryDue(100000));
  d.noteWrite(true, 130000);
  TEST_ASSERT_TRUE(d.saved());
  TEST_ASSERT_FALSE(d.retryDue(999999));
  d.markDirty(200000);
  TEST_ASSERT_FALSE(d.saved());
  TEST_ASSERT_TRUE(d.retryDue(200000));
  // millis() wrap: still exactly one retry per minute.
  nimbus::store::DirtyRetry w(60000);
  w.noteWrite(false, 0xFFFFF000u);
  TEST_ASSERT_FALSE(w.retryDue(0xFFFFF000u + 30000u));
  TEST_ASSERT_TRUE(w.retryDue(0xFFFFF000u + 60000u));
}

// L19-22: the scratchpad file is written only when its bytes differ from the ones that
// last landed. The scratchpad used to persist two or three times a turn whether or not
// anything changed; the fingerprint (a 64-bit hash plus the length, in RAM) skips
// identical persists, a write that did not land keeps the old fingerprint so the next
// persist tries the same bytes again, and the failure log line is throttled the way
// notePersist's is: one line per minute, carrying the count of failures it covers.
static void test_scratchpad_persists_only_on_change() {
  using nimbus::orch::Scratchpad;
  using nimbus::orch::Tier;
  using nimbus::store::PersistOutcome;
  // A real scratchpad: N identical persists produce one write; changing one tier
  // produces exactly one more.
  FakeMovedIo io;
  nimbus::store::LandedFingerprint landed;
  Scratchpad pad;
  TEST_ASSERT_TRUE(pad.replace(Tier::Short, {"read the pump schedule"}) > 0);
  TEST_ASSERT_TRUE(pad.add(Tier::Mid, "call the plumber about the leak"));
  const std::string v1 = pad.serialize();
  for (int i = 0; i < 6; ++i)   // a turn persists several times
    TEST_ASSERT_EQUAL((int)(i == 0 ? PersistOutcome::Landed : PersistOutcome::Unchanged),
                      (int)nimbus::store::persistIfChanged(io, landed, v1));
  TEST_ASSERT_EQUAL_INT(1, io.writes);
  TEST_ASSERT_TRUE(pad.replace(Tier::Long, {"fix the pump", "close the valve"}) > 0);
  const std::string v2 = pad.serialize();
  TEST_ASSERT_EQUAL((int)PersistOutcome::Landed,
                    (int)nimbus::store::persistIfChanged(io, landed, v2));
  for (int i = 0; i < 6; ++i)
    TEST_ASSERT_EQUAL((int)PersistOutcome::Unchanged,
                      (int)nimbus::store::persistIfChanged(io, landed, v2));
  TEST_ASSERT_EQUAL_INT(2, io.writes);
  // An empty scratchpad that booted from an empty file writes nothing either (the
  // device primes the fingerprint at boot when the file round-trips).
  FakeMovedIo emptyIo; emptyIo.fileExists = true;
  nimbus::store::LandedFingerprint emptyLanded;
  emptyLanded.noteLanded("");
  Scratchpad empty;
  TEST_ASSERT_EQUAL((int)PersistOutcome::Unchanged,
                    (int)nimbus::store::persistIfChanged(emptyIo, emptyLanded, empty.serialize()));
  TEST_ASSERT_EQUAL_INT(0, emptyIo.writes);
  // A write that did not land leaves the old fingerprint: the next persist of the same
  // bytes tries again, and once one lands the churn stops.
  FakeMovedIo failing; failing.failWrite = true;
  nimbus::store::LandedFingerprint retry;
  TEST_ASSERT_EQUAL((int)PersistOutcome::Failed,
                    (int)nimbus::store::persistIfChanged(failing, retry, "note|1"));
  TEST_ASSERT_EQUAL_INT(1, failing.writes);
  failing.failWrite = false;
  TEST_ASSERT_EQUAL((int)PersistOutcome::Landed,
                    (int)nimbus::store::persistIfChanged(failing, retry, "note|1"));
  TEST_ASSERT_EQUAL_INT(2, failing.writes);
  TEST_ASSERT_EQUAL((int)PersistOutcome::Unchanged,
                    (int)nimbus::store::persistIfChanged(failing, retry, "note|1"));
  TEST_ASSERT_EQUAL_INT(2, failing.writes);
  // A changed value that fails, then changes again before landing: the newer bytes
  // are what gets written (the RAM copy is the truth).
  FakeMovedIo torn; torn.failWrite = true;
  nimbus::store::LandedFingerprint tornFp;
  TEST_ASSERT_EQUAL((int)PersistOutcome::Failed,
                    (int)nimbus::store::persistIfChanged(torn, tornFp, "old"));
  torn.failWrite = false;
  TEST_ASSERT_EQUAL((int)PersistOutcome::Landed,
                    (int)nimbus::store::persistIfChanged(torn, tornFp, "new"));
  TEST_ASSERT_EQUAL_STRING("new", torn.file.c_str());
}

// The scratchpad failure line: at most one per minute, and it carries the count.
static void test_scratchpad_failure_log_is_throttled_with_a_count() {
  nimbus::store::FailureLog log(60000);
  TEST_ASSERT_EQUAL_UINT32(1, log.noteFailure(1000));   // the first failure logs at once
  for (uint32_t t = 2000; t <= 60000; t += 1000)
    TEST_ASSERT_EQUAL_UINT32(0, log.noteFailure(t));    // held back: no line inside the minute
  TEST_ASSERT_EQUAL_UINT32(60, log.noteFailure(61000)); // 59 held back + this one
  TEST_ASSERT_EQUAL_UINT32(0, log.noteFailure(61001));
  TEST_ASSERT_EQUAL_UINT32(0, log.noteFailure(62000));
  TEST_ASSERT_EQUAL_UINT32(3, log.noteFailure(121001)); // two held back + this one
  // millis() wrap: still exactly one line per minute.
  nimbus::store::FailureLog wrap(60000);
  TEST_ASSERT_EQUAL_UINT32(1, wrap.noteFailure(0xFFFFF000u));
  TEST_ASSERT_EQUAL_UINT32(0, wrap.noteFailure(0xFFFFF000u + 30000u));
  TEST_ASSERT_EQUAL_UINT32(2, wrap.noteFailure(0xFFFFF000u + 60000u));
}

// L19-29: the device seam of the tag-rows transition. The ledger's load path turns
// new (provider, tag) audit rows off only while it serves the legacy NVS copy
// (flash unmounted); the file branch leaves them on (the ledger lives on LittleFS).
static void test_new_tag_rows_follow_the_ledger_home() {
  const std::string st = readFile("src/agent/store.cpp");
  const size_t fb = st.find("if (LittleFS.totalBytes() == 0) {");   // the NVS fallback
  TEST_ASSERT_TRUE(fb != std::string::npos);
  const size_t fe = st.find("} else {", fb);
  TEST_ASSERT_TRUE(fe != std::string::npos);
  const std::string fallback = st.substr(fb, fe - fb);
  TEST_ASSERT_TRUE(fallback.find("g_ledger.setAllowNewTags(false);") != std::string::npos);
  // The file branch (from the else to the load's end) never disables new tags.
  const size_t lm = st.find("loadModelsLocked();", fe);
  TEST_ASSERT_TRUE(lm != std::string::npos);
  const std::string fileBranch = st.substr(fe, lm - fe);
  TEST_ASSERT_TRUE(fileBranch.find("setAllowNewTags") == std::string::npos);
  // And the fallback is the only place in the file that touches the switch.
  TEST_ASSERT_EQUAL_size_t(1, countOf(st, "setAllowNewTags"));
}

// The device seam of both moves: the ledger and the scratchpad persist through the
// moved-store rule into their files, never NVS, and retire the legacy copy only on a
// boot whose image was already valid.
static void test_moved_store_device_seams() {
  const std::string st = readFile("src/agent/store.cpp");
  TEST_ASSERT_TRUE(st.find("constexpr const char*  kUsageLedgerPath = \"/data/usage_ledger.txt\";") !=
                   std::string::npos);
  const size_t pl = st.find("bool persistLedgerLocked() {");
  TEST_ASSERT_TRUE(pl != std::string::npos);
  const std::string persist = st.substr(pl, st.find("\n}\n", pl) - pl);
  TEST_ASSERT_TRUE(persist.find("nimbus::store::persistMoved(io, s)") != std::string::npos);
  TEST_ASSERT_TRUE(persist.find("fitSerialized(nimbus::orch::UsageLedger::kSerializedMaxBytes") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(persist.find("notePersist(ok, \"lfs_ledger\")") != std::string::npos);
  for (const char* nvs : {"putStr", "putOrClear", "AKEY_USAGE_LEDGER"})
    TEST_ASSERT_TRUE_MESSAGE(persist.find(nvs) == std::string::npos, nvs);
  TEST_ASSERT_TRUE(st.find("nimbus::store::retireLegacy(io, otaupd::imageValidAtBoot())") != std::string::npos);
  const std::string ms = readFile("src/agent/memory_subsystem.cpp");
  TEST_ASSERT_TRUE(ms.find("const char* kScratchPath = \"/data/scratchpad.bin\";") != std::string::npos);
  const size_t ps = ms.find("bool persistScratchpad() {");
  TEST_ASSERT_TRUE(ps != std::string::npos);
  const std::string sp = ms.substr(ps, ms.find("\n}\n", ps) - ps);
  // L19-22: the persist goes through the change fingerprint and the failure line is
  // throttled (one line per minute with the count), like notePersist's.
  TEST_ASSERT_TRUE(sp.find("nimbus::store::persistIfChanged(io, g_scratchLanded, blob)") !=
                  std::string::npos);
  TEST_ASSERT_TRUE(sp.find("g_scratchFailLog.noteFailure(millis())") != std::string::npos);
  TEST_ASSERT_TRUE(sp.find("notePersist(ok, \"lfs_scratch\")") != std::string::npos);
  TEST_ASSERT_TRUE(sp.find("putBytes") == std::string::npos);
  // A boot whose file round-trips the loaded notes primes the fingerprint: the first
  // persist after it writes nothing until the notes change.
  TEST_ASSERT_TRUE(ms.find("g_scratchLanded.noteLanded(m.value)") != std::string::npos);
  TEST_ASSERT_TRUE(ms.find("nimbus::store::retireLegacy(io, otaupd::imageValidAtBoot())") != std::string::npos);
  // bootGuard sets the "valid at boot" latch only on the not-pending paths.
  const std::string ota = readFile("src/sys/ota_update.cpp");
  TEST_ASSERT_EQUAL_size_t(2, countOf(ota, "g_validAtBoot = true;"));
}

// The routes the 2c rows name answer 507 through the persist ledger: the scratchpad and
// memory-config routes, the Telegram owner routes (owners before the allowlist, the
// role row only after both), the OAuth refresh token, /mcp approve, and the verify
// cache written once per verdict.
static void test_2c_routes_report_storage_full() {
  const std::string wm = readFile("src/net/web_memory.cpp");
  for (const char* fn : {"void handleScratchPost(", "void handleConfigPut("}) {
    const size_t s = wm.find(fn);
    TEST_ASSERT_TRUE_MESSAGE(s != std::string::npos, fn);
    const std::string body = wm.substr(s, wm.find("\n}\n", s) - s);
    TEST_ASSERT_TRUE_MESSAGE(body.find("nimbus::sys::SaveScope save;") != std::string::npos, fn);
    TEST_ASSERT_TRUE_MESSAGE(body.find("nimbus::store::storageFullJson()") != std::string::npos, fn);
  }
  const std::string web = readFile("src/net/webui.cpp");
  const std::string rm = routeRegion(web, "/api/telegram/remove");
  const size_t owners = rm.find("agent::store::setTelegramOwners(ownOut)");
  const size_t allow = rm.find("agent::store::setTelegramAllowlist(out)");
  const size_t tenant = rm.find("agent::orchestrator::tenantRemove(");
  TEST_ASSERT_TRUE(owners != std::string::npos && allow != std::string::npos && tenant != std::string::npos);
  TEST_ASSERT_TRUE_MESSAGE(owners < allow && allow < tenant, "owners, then allowlist, then the role row");
  TEST_ASSERT_TRUE(rm.find("if (!landed) { sendSaved(r, save); return; }") < tenant);
  TEST_ASSERT_TRUE(routeRegion(web, "/api/telegram/rename").find("if (!onFreshAllowlist(id))") !=
                   std::string::npos);
  const std::string st = readFile("src/agent/store.cpp");
  TEST_ASSERT_TRUE(st.find("bool    setTelegramOwners(const String& v) { return putStrShrinkSafe(AKEY_TG_OWNERS, v); }") !=
                   std::string::npos);
  const std::string tg = readFile("src/agent/telegram.cpp");
  TEST_ASSERT_TRUE(tg.find("if (!store::setTelegramAllowlist(al)) return false;") != std::string::npos);
  const std::string cn = readFile("src/agent/connectors.cpp");
  TEST_ASSERT_TRUE(cn.find("if (!store::setConnectorsJson(out)) return RefreshWrite::StorageFull;") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(cn.find("if (w == RefreshWrite::StorageFull) { fail(nimbus::store::kStorageFullMessage); return; }") !=
                   std::string::npos);
  const std::string oc = readFile("src/agent/orchestrator.cpp");
  TEST_ASSERT_TRUE(oc.find("if (!store::setConnectorsJson(out)) {") != std::string::npos);
  TEST_ASSERT_TRUE(oc.find("s_verifyMemo.shouldWrite(ev.host.c_str(), 1)") != std::string::npos);
}

// The verify cache is written once per (host, verdict) per boot: a refused write is
// not retried at every turn end, and a changed verdict is written again.
static void test_verdict_memo_writes_only_on_change() {
  nimbus::store::VerdictMemo m;
  TEST_ASSERT_TRUE(m.shouldWrite("openai", 1));
  for (int turn = 0; turn < 100; ++turn) TEST_ASSERT_FALSE(m.shouldWrite("openai", 1));
  TEST_ASSERT_TRUE(m.shouldWrite("anthropic", 1));
  TEST_ASSERT_TRUE(m.shouldWrite("openai", 0));    // the verdict moved
  TEST_ASSERT_TRUE(m.shouldWrite("openai", 1));
  TEST_ASSERT_FALSE(m.shouldWrite(nullptr, 1));
  // More hosts than slots: an evicted host is re-attempted, never silently skipped.
  nimbus::store::VerdictMemo many;
  for (int i = 0; i < 20; ++i) TEST_ASSERT_TRUE(many.shouldWrite(("h" + std::to_string(i)).c_str(), 1));
  TEST_ASSERT_TRUE(many.shouldWrite("h0", 1));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_save_reply_is_507_storage_full_never_ok);
  RUN_TEST(test_storage_full_message_copy);
  RUN_TEST(test_every_covered_route_answers_507_on_a_full_nvs);
  RUN_TEST(test_string_write_verdict_truth_table);
  RUN_TEST(test_write_primitives_tell_the_truth_on_a_full_nvs);
  RUN_TEST(test_removing_a_key_or_connector_lands_when_full);
  RUN_TEST(test_shrink_safe_restores_the_old_value_when_the_write_fails);
  RUN_TEST(test_ledger_attributes_failures_to_their_task);
  RUN_TEST(test_ledger_overflow_is_fail_closed);
  RUN_TEST(test_legacy_catalog_eviction_covers_every_provider);
  RUN_TEST(test_catalog_paths_are_safe);
  RUN_TEST(test_nvs_budget_old_placement_exhausts_new_keeps_headroom);
  RUN_TEST(test_nvs_usage_bytes);
  RUN_TEST(test_store_writes_all_go_through_the_ledger);
  RUN_TEST(test_store_setters_return_bool);
  RUN_TEST(test_covered_routes_answer_through_sendSaved);
  RUN_TEST(test_every_nvs_writing_route_is_covered_or_tracked);
  RUN_TEST(test_raw_nvs_writers_are_a_reviewed_set);
  RUN_TEST(test_embedcfg_stores_before_it_wipes);
  RUN_TEST(test_web_ui_shows_the_storage_full_text);
  RUN_TEST(test_model_price_write_failure_is_507_with_ram_reverted);
  RUN_TEST(test_model_price_store_reports_lfs_mrates);
  RUN_TEST(test_telegram_offset_read_once_and_checked);
  RUN_TEST(test_recent_fails_ring);
  RUN_TEST(test_storage_notice_once_per_boot);
  RUN_TEST(test_failures_feed_the_ring_and_the_notice);
  RUN_TEST(test_listing_entry_math_matches_the_partition_model);
  RUN_TEST(test_nvs_stats_names_never_values);
  RUN_TEST(test_every_defined_key_has_a_class);
  RUN_TEST(test_key_classes_and_eviction_rows);
  RUN_TEST(test_eviction_plan_order_and_exclusions);
  RUN_TEST(test_value_entries_match_the_partition_model);
  RUN_TEST(test_reserve_property_over_every_row);
  RUN_TEST(test_growing_writes_read_stats_and_refuse_when_nothing_is_left);
  RUN_TEST(test_blob_span_check_refuses_before_nvs_set_blob);
  RUN_TEST(test_reserve_covers_the_critical_working_set);
  RUN_TEST(test_reserve_keeps_critical_writes_landing_on_a_filling_device);
  RUN_TEST(test_eviction_pass_restores_the_reserve_or_reports_starved);
  RUN_TEST(test_device_seam_asks_the_reserve);
  RUN_TEST(test_store_setter_verdicts_are_never_silently_dropped);
  RUN_TEST(test_raw_nvs_writers_report_through_notePersist);
  RUN_TEST(test_moved_store_migration_legs);
  RUN_TEST(test_moved_store_dirty_and_retry);
  RUN_TEST(test_scratchpad_persists_only_on_change);
  RUN_TEST(test_scratchpad_failure_log_is_throttled_with_a_count);
  RUN_TEST(test_moved_store_device_seams);
  RUN_TEST(test_new_tag_rows_follow_the_ledger_home);
  RUN_TEST(test_2c_routes_report_storage_full);
  RUN_TEST(test_verdict_memo_writes_only_on_change);
  return UNITY_END();
}
