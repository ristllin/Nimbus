#include <unity.h>

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

#include "nimbus/orch/provider_slots.h"
#include "nimbus/store/persist.h"

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
};
// NVS-writing POST routes NOT yet answering through sendSaved: outside this lane's
// handler set, tracked as Linear CUM-530. Their setters already
// return bool, so wiring each is a SaveScope + sendSaved. Shrinking this list is the
// goal; a NEW writing route must land covered, not here.
static const char* kKnownGaps[] = {"/api/fallbacks",        "/api/telegram/remove",
                                   "/api/telegram/rename",  "/api/telegram/role",
                                   "/api/telegram/public",  "/api/telegram/add",
                                   "/api/telegram/approve", "/api/cloud"};

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
// text, JS-escaped) and never toasts "Saved" for it.
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
  TEST_ASSERT_TRUE(js.find("if(e===507){toast(saveFailText(e),6000);return;}") != std::string::npos);
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
  return UNITY_END();
}
