#include <unity.h>

#include <string>
#include <vector>

#include <ArduinoJson.h>

// The portable restore parse/apply core the device seam (memory_subsystem /
// web_memory, POST /api/mem/import) runs. Testing it here proves the round-trip
// (a backup fixture -> import -> the store shows the vectors) without a board.
#include "../../src/agent/memory_restore.h"

#include "nimbus/orch/episodic.h"
#include "nimbus/orch/scratchpad.h"
#include "nimbus/orch/vector_memory.h"

using nimbus::orch::EpisodicMessage;
using nimbus::orch::InMemoryEpisodicStore;
using nimbus::orch::MsgQuery;
using nimbus::orch::Scratchpad;
using nimbus::orch::Tier;
using nimbus::orch::VecEntry;
using nimbus::orch::VectorMemory;
namespace restore = agent::memory::restore;

void setUp() {}
void tearDown() {}

// ---- helpers ---------------------------------------------------------------

// Build a "vectors.json"-shaped JSON string (the exact shape backup_device.py
// writes, including the base64 `vec` the CUM-406 browse export adds). Building it
// through ArduinoJson + restore::b64EncodeVec exercises the full
// encode -> serialize -> parse -> decode round-trip, not a hand-typed blob.
static std::string makeVectorsJson() {
  JsonDocument d;
  JsonArray arr = d["entries"].to<JsonArray>();
  {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = "v1";
    o["content"] = "the bilge pump serial is BX-4491";
    o["importance"] = 0.9f;
    o["permanent"] = true;
    o["creator"] = true;
    o["source"] = "user";
    o["ttlHours"] = -1;
    o["tsHours"] = 1000u;
    o["lastRecallHours"] = 1200u;
    o["ns"] = "owner";
    o["vec"] = restore::b64EncodeVec({10, -20, 30, -40});
  }
  {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = "v2";
    o["content"] = "owner prefers tea in the morning";
    o["importance"] = 0.4f;
    o["source"] = "system";
    o["ttlHours"] = 720;
    o["tsHours"] = 900u;
    o["ns"] = "owner";
    o["vec"] = restore::b64EncodeVec({-5, 60, -70, 80});
  }
  std::string out;
  serializeJson(d, out);
  return out;
}

static restore::VecReport applyVectorsStr(VectorMemory& vm, const std::string& json,
                                          int dims, bool dryRun) {
  JsonDocument d;
  TEST_ASSERT_FALSE(deserializeJson(d, json));
  return restore::applyVectors(vm, d["entries"].as<JsonArrayConst>(), dims, dryRun);
}

// ---- base64 round-trip -----------------------------------------------------

static void test_b64_int8_roundtrip() {
  std::vector<int8_t> v = {0, 1, -1, 127, -128, 42, -42, 99};
  std::string enc = restore::b64EncodeVec(v);
  std::vector<int8_t> dec = restore::b64DecodeInt8(enc.data(), enc.size());
  TEST_ASSERT_EQUAL_UINT(v.size(), dec.size());
  for (size_t i = 0; i < v.size(); i++) TEST_ASSERT_EQUAL_INT8(v[i], dec[i]);
  TEST_ASSERT_EQUAL_UINT(0, restore::b64EncodeVec({}).size());
}

// ---- the round-trip the DoD names -----------------------------------------

static void test_vectors_roundtrip_shows_in_store() {
  VectorMemory vm;
  vm.configure(4);
  const std::string js = makeVectorsJson();

  restore::VecReport rep = applyVectorsStr(vm, js, 4, /*dryRun=*/false);
  TEST_ASSERT_EQUAL_INT(2, rep.added);
  TEST_ASSERT_EQUAL_INT(0, rep.replaced);
  TEST_ASSERT_EQUAL_INT(0, rep.skippedNoVec);
  TEST_ASSERT_EQUAL_INT(0, rep.widthErrors);
  TEST_ASSERT_EQUAL_INT(2, vm.size());

  // The store shows the vectors, with every field + the embedding preserved.
  bool sawV1 = false, sawV2 = false;
  for (const VecEntry& e : vm.getAll()) {
    if (e.id == "v1") {
      sawV1 = true;
      TEST_ASSERT_EQUAL_STRING("the bilge pump serial is BX-4491", e.content.c_str());
      TEST_ASSERT_TRUE(e.permanentFlag);
      TEST_ASSERT_TRUE(e.creatorFlag);
      TEST_ASSERT_EQUAL_INT32(-1, e.ttlHours);
      TEST_ASSERT_EQUAL_UINT32(1000u, e.createdAtHours);
      TEST_ASSERT_EQUAL_UINT32(1200u, e.lastRecallHours);
      TEST_ASSERT_EQUAL_STRING("owner", e.ns.c_str());
      TEST_ASSERT_EQUAL_UINT(4, e.vec.size());
      const int8_t want[4] = {10, -20, 30, -40};
      for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL_INT8(want[i], e.vec[(size_t)i]);
    } else if (e.id == "v2") {
      sawV2 = true;
      TEST_ASSERT_FALSE(e.permanentFlag);
      TEST_ASSERT_EQUAL_INT32(720, e.ttlHours);
    }
  }
  TEST_ASSERT_TRUE(sawV1);
  TEST_ASSERT_TRUE(sawV2);

  // A restored embedding is searchable: querying with v1's own vector returns v1
  // at (near-)zero cosine distance - proof the embedding, not just the text, landed.
  auto hits = vm.search({10, -20, 30, -40}, 1);
  TEST_ASSERT_EQUAL_UINT(1, hits.size());
  TEST_ASSERT_EQUAL_STRING("v1", hits[0].id.c_str());
  TEST_ASSERT_TRUE(hits[0].distance < 0.001f);
}

static void test_vectors_idempotent_replace_by_id() {
  VectorMemory vm;
  vm.configure(4);
  const std::string js = makeVectorsJson();
  applyVectorsStr(vm, js, 4, false);

  // Re-running the SAME restore converges: no growth, both ids replaced.
  restore::VecReport rep2 = applyVectorsStr(vm, js, 4, false);
  TEST_ASSERT_EQUAL_INT(0, rep2.added);
  TEST_ASSERT_EQUAL_INT(2, rep2.replaced);
  TEST_ASSERT_EQUAL_INT(2, vm.size());
}

static void test_vectors_dryrun_writes_nothing() {
  VectorMemory vm;
  vm.configure(4);
  restore::VecReport rep = applyVectorsStr(vm, makeVectorsJson(), 4, /*dryRun=*/true);
  TEST_ASSERT_EQUAL_INT(2, rep.added);   // would-add count
  TEST_ASSERT_EQUAL_INT(0, rep.total);   // total is 0 in dry-run (nothing written)
  TEST_ASSERT_EQUAL_INT(0, vm.size());   // store untouched
}

static void test_vectors_missing_vec_and_width_mismatch_reported() {
  // A pre-CUM-406 backup row (no "vec") is skipped honestly; a wrong-width vec is
  // an error, never a silent drop. Neither aborts the good rows.
  JsonDocument d;
  JsonArray arr = d["entries"].to<JsonArray>();
  { JsonObject o = arr.add<JsonObject>(); o["id"] = "good"; o["content"] = "x";
    o["vec"] = restore::b64EncodeVec({1, 2, 3, 4}); }
  { JsonObject o = arr.add<JsonObject>(); o["id"] = "novec"; o["content"] = "y"; }
  { JsonObject o = arr.add<JsonObject>(); o["id"] = "wide"; o["content"] = "z";
    o["vec"] = restore::b64EncodeVec({1, 2, 3, 4, 5, 6}); }
  { JsonObject o = arr.add<JsonObject>(); o["content"] = "no id"; o["vec"] = "AAA="; }

  VectorMemory vm;
  vm.configure(4);
  restore::VecReport rep =
      restore::applyVectors(vm, d["entries"].as<JsonArrayConst>(), 4, false);
  TEST_ASSERT_EQUAL_INT(1, rep.added);
  TEST_ASSERT_EQUAL_INT(1, rep.skippedNoVec);
  TEST_ASSERT_EQUAL_INT(1, rep.widthErrors);
  TEST_ASSERT_EQUAL_INT(1, rep.badRows);
  TEST_ASSERT_EQUAL_INT(1, vm.size());
}

// ---- episodic --------------------------------------------------------------

static std::string makeEpisodicJson() {
  JsonDocument d;
  JsonArray arr = d["messages"].to<JsonArray>();
  { JsonObject o = arr.add<JsonObject>(); o["id"] = "m00000001"; o["session"] = "web";
    o["ts"] = 100u; o["role"] = "user"; o["kind"] = "message"; o["text"] = "hello";
    o["blob"] = ""; o["tags"] = "from:owner"; }
  { JsonObject o = arr.add<JsonObject>(); o["id"] = "m00000002"; o["session"] = "web";
    o["ts"] = 101u; o["role"] = "assistant"; o["kind"] = "message"; o["text"] = "hi there"; }
  // A duplicate id (retried page / dirty fixture) must be de-duped, not double-added.
  { JsonObject o = arr.add<JsonObject>(); o["id"] = "m00000001"; o["session"] = "web";
    o["ts"] = 100u; o["role"] = "user"; o["kind"] = "message"; o["text"] = "hello"; }
  std::string out; serializeJson(d, out); return out;
}

static void test_episodic_roundtrip_and_dedup() {
  InMemoryEpisodicStore es;
  JsonDocument d;
  TEST_ASSERT_FALSE(deserializeJson(d, makeEpisodicJson()));
  restore::EpiReport rep =
      restore::applyEpisodic(es, d["messages"].as<JsonArrayConst>(), /*dryRun=*/false);
  TEST_ASSERT_EQUAL_INT(2, rep.added);
  TEST_ASSERT_EQUAL_INT(1, rep.dupSkipped);
  TEST_ASSERT_EQUAL_INT(2, es.messageCount());

  MsgQuery q; q.limit = 10;
  auto msgs = es.query(q);
  TEST_ASSERT_EQUAL_UINT(2, msgs.size());
  bool sawHello = false;
  for (const auto& m : msgs)
    if (m.id == "m00000001") { sawHello = true; TEST_ASSERT_EQUAL_STRING("hello", m.text.c_str());
                               TEST_ASSERT_EQUAL_STRING("from:owner", m.tags.c_str()); }
  TEST_ASSERT_TRUE(sawHello);
}

static void test_episodic_dryrun_writes_nothing() {
  InMemoryEpisodicStore es;
  JsonDocument d;
  deserializeJson(d, makeEpisodicJson());
  restore::EpiReport rep =
      restore::applyEpisodic(es, d["messages"].as<JsonArrayConst>(), /*dryRun=*/true);
  TEST_ASSERT_EQUAL_INT(2, rep.added);
  TEST_ASSERT_EQUAL_INT(0, es.messageCount());
}

// ---- scratchpad ------------------------------------------------------------

static void test_scratchpad_roundtrip_replaces() {
  Scratchpad sp;
  sp.add(Tier::Short, "stale item that must be replaced");

  JsonDocument d;
  d["active"] = "restoring the desk device";
  JsonArray s = d["short"].to<JsonArray>(); s.add("check the bilge pump"); s.add("call the yard");
  JsonArray m = d["mid"].to<JsonArray>();   m.add("plan the haul-out");
  d["long"].to<JsonArray>();  // empty long tier

  restore::ScratchReport rep =
      restore::applyScratchpad(sp, d.as<JsonObjectConst>(), /*dryRun=*/false);
  TEST_ASSERT_EQUAL_INT(1, rep.active);
  TEST_ASSERT_EQUAL_INT(2, rep.shortN);
  TEST_ASSERT_EQUAL_INT(1, rep.midN);
  TEST_ASSERT_EQUAL_INT(0, rep.longN);

  TEST_ASSERT_EQUAL_STRING("restoring the desk device", sp.activeTask().c_str());
  TEST_ASSERT_EQUAL_UINT(2, sp.items(Tier::Short).size());   // replaced, not appended
  TEST_ASSERT_EQUAL_STRING("check the bilge pump", sp.items(Tier::Short)[0].c_str());
  TEST_ASSERT_EQUAL_UINT(1, sp.items(Tier::Mid).size());
  TEST_ASSERT_EQUAL_UINT(0, sp.items(Tier::Long).size());
}

static void test_scratchpad_dryrun_writes_nothing() {
  Scratchpad sp;
  sp.setActiveTask("original");
  JsonDocument d;
  d["active"] = "changed";
  d["short"].to<JsonArray>().add("x");
  restore::ScratchReport rep =
      restore::applyScratchpad(sp, d.as<JsonObjectConst>(), /*dryRun=*/true);
  TEST_ASSERT_EQUAL_INT(1, rep.shortN);
  TEST_ASSERT_EQUAL_STRING("original", sp.activeTask().c_str());   // untouched
  TEST_ASSERT_EQUAL_UINT(0, sp.items(Tier::Short).size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_b64_int8_roundtrip);
  RUN_TEST(test_vectors_roundtrip_shows_in_store);
  RUN_TEST(test_vectors_idempotent_replace_by_id);
  RUN_TEST(test_vectors_dryrun_writes_nothing);
  RUN_TEST(test_vectors_missing_vec_and_width_mismatch_reported);
  RUN_TEST(test_episodic_roundtrip_and_dedup);
  RUN_TEST(test_episodic_dryrun_writes_nothing);
  RUN_TEST(test_scratchpad_roundtrip_replaces);
  RUN_TEST(test_scratchpad_dryrun_writes_nothing);
  return UNITY_END();
}
