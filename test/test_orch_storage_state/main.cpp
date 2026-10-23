#include <unity.h>

#include <string>

#include <ArduinoJson.h>

#include "nimbus/orch/storage_state.h"

// storage_state - the settings-storage and Telegram-offset block of device.status
// (L19-9). Under test: the golden JSON on a fresh device and on the NVSFILL-0
// shape (host fakes for lane A's getters), the presence property (every key is
// always present; values may be null) and that a failing key name stays a safe
// JSON string - a key name only, never a value.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// The golden on a fresh device: real free space, no failed save, nothing owed.
static void test_golden_fresh_device() {
  StorageState s;   // defaults are the fresh shape
  s.nvsStats = true;
  s.nvsFree = 16384;
  TEST_ASSERT_EQUAL_STRING(
      "{\"nvs\":{\"free\":16384,\"fails\":0,\"lastFail\":null},"
      "\"nvsReserve\":{\"starved\":false,\"refused\":0},\"tgOffsetSaved\":true,"
      "\"embedParked\":false,\"embedParkedWhy\":null,\"visionLastErr\":null}",
      storageStateJson(s).c_str());
}

// The golden on the NVSFILL-0 shape (lane A's getters faked host-side): the
// partition is nearly full, saves failed, the last failing key is the Telegram
// offset, and its stored copy is behind - the shape the head must be able to
// cite instead of blaming the owner's app.
static void test_golden_nvsfill0_shape() {
  StorageState s;
  s.nvsStats = true;
  s.nvsFree = 288;
  s.saveFails = 2;
  s.lastSaveFail = "tgOffset";
  s.tgOffsetSaved = false;
  TEST_ASSERT_EQUAL_STRING(
      "{\"nvs\":{\"free\":288,\"fails\":2,\"lastFail\":\"tgOffset\"},"
      "\"nvsReserve\":{\"starved\":false,\"refused\":0},\"tgOffsetSaved\":false,"
      "\"embedParked\":false,\"embedParkedWhy\":null,\"visionLastErr\":null}",
      storageStateJson(s).c_str());
}

// The presence property: over every shape the builder can be handed, all four
// keys are always present and the JSON parses; `free` is null exactly when the
// usage query did not answer, `lastFail` exactly when no key failed.
static void test_keys_always_present_values_may_be_null() {
  for (int shape = 0; shape < 8; ++shape) {
    StorageState s;
    s.nvsStats = shape & 1;
    s.nvsFree = (shape & 2) ? 288 : 16384;
    s.saveFails = (shape & 4) ? 3 : 0;
    s.lastSaveFail = (shape & 4) ? (shape & 2 ? "tgOffset" : "sfxLvlN") : "";
    s.tgOffsetSaved = !(shape & 2);
    s.nvsReserveStarved = shape & 1;
    s.nvsReserveRefused = (shape & 2) ? 24u : 0u;
    const std::string j = storageStateJson(s);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(j.c_str(), "\"nvs\""), j.c_str());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(j.c_str(), "\"free\""), j.c_str());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(j.c_str(), "\"fails\""), j.c_str());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(j.c_str(), "\"lastFail\""), j.c_str());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(j.c_str(), "\"nvsReserve\""), j.c_str());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(j.c_str(), "\"starved\""), j.c_str());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(j.c_str(), "\"refused\""), j.c_str());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(j.c_str(), "\"tgOffsetSaved\""), j.c_str());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(j.c_str(), "\"embedParked\""), j.c_str());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(j.c_str(), "\"embedParkedWhy\""), j.c_str());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(j.c_str(), "\"visionLastErr\":null"), j.c_str());
    JsonDocument d;
    const std::string err = deserializeJson(d, j) ? "parse failed" : "";
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", err.c_str(), j.c_str());
    TEST_ASSERT_EQUAL_MESSAGE(s.nvsStats, !d["nvs"]["free"].isNull(), j.c_str());
    TEST_ASSERT_EQUAL_MESSAGE(s.lastSaveFail.empty(), d["nvs"]["lastFail"].isNull(), j.c_str());
    TEST_ASSERT_EQUAL_UINT32(s.saveFails, d["nvs"]["fails"].as<uint32_t>());
    TEST_ASSERT_EQUAL(s.tgOffsetSaved, d["tgOffsetSaved"].as<bool>());
    TEST_ASSERT_EQUAL_MESSAGE(s.nvsReserveStarved, d["nvsReserve"]["starved"].as<bool>(), j.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(s.nvsReserveRefused, d["nvsReserve"]["refused"].as<uint32_t>(), j.c_str());
    TEST_ASSERT_EQUAL_MESSAGE(false, d["embedParked"].as<bool>(), j.c_str());
    TEST_ASSERT_TRUE_MESSAGE(d["embedParkedWhy"].isNull(), j.c_str());
  }
}

// L19-9 (lane P): the embedding-outage shape. embedParked is true and its why is
// the secret-safe cause slug, so the head can say memories are waiting on the
// device instead of claiming its memory path is working (19 Oct head incident).
static void test_golden_embedding_parked_shape() {
  StorageState s;
  s.nvsStats = true;
  s.nvsFree = 16384;
  s.embedParked = true;
  s.embedParkedWhy = "out of embedding credit at openai";
  TEST_ASSERT_EQUAL_STRING(
      "{\"nvs\":{\"free\":16384,\"fails\":0,\"lastFail\":null},"
      "\"nvsReserve\":{\"starved\":false,\"refused\":0},\"tgOffsetSaved\":true,"
      "\"embedParked\":true,\"embedParkedWhy\":\"out of embedding credit at openai\","
      "\"visionLastErr\":null}",
      storageStateJson(s).c_str());
}

// L19-9 (lane C): the starved-reserve golden - the device is so full that
// background saves are refused to keep owner settings landing. The head cites
// starved instead of guessing, and refused names how many writes were turned
// away since boot.
static void test_golden_starved_reserve_shape() {
  StorageState s;
  s.nvsStats = true;
  s.nvsFree = 288;
  s.nvsReserveStarved = true;
  s.nvsReserveRefused = 17;
  TEST_ASSERT_EQUAL_STRING(
      "{\"nvs\":{\"free\":288,\"fails\":0,\"lastFail\":null},"
      "\"nvsReserve\":{\"starved\":true,\"refused\":17},\"tgOffsetSaved\":true,"
      "\"embedParked\":false,\"embedParkedWhy\":null,\"visionLastErr\":null}",
      storageStateJson(s).c_str());
}

// L19-9 (lane E): the photo reader's last failure - when, which provider, the typed
// class and the HTTP status, so the head can say photos are not being read because
// the provider is out of credit instead of blaming the owner's phone. Never a body.
static void test_golden_vision_last_error_shape() {
  StorageState s;
  s.nvsStats = true;
  s.nvsFree = 16384;
  s.visionErrAtS = 4021;
  s.visionErrProvider = "openai";
  s.visionErrClass = "billing";
  s.visionErrStatus = 429;
  TEST_ASSERT_EQUAL_STRING(
      "{\"nvs\":{\"free\":16384,\"fails\":0,\"lastFail\":null},"
      "\"nvsReserve\":{\"starved\":false,\"refused\":0},\"tgOffsetSaved\":true,"
      "\"embedParked\":false,\"embedParkedWhy\":null,"
      "\"visionLastErr\":{\"atS\":4021,\"provider\":\"openai\",\"class\":\"billing\",\"status\":429}}",
      storageStateJson(s).c_str());
}

// A failing key name is a JSON string like any other: quotes and backslashes in
// it cannot break the block (NVS keys cannot hold them, but the builder must
// not be the thing that breaks if one ever does).
static void test_failing_key_name_is_json_safe() {
  StorageState s;
  s.nvsStats = true;
  s.lastSaveFail = "a\"b\\c";
  s.saveFails = 1;
  const std::string j = storageStateJson(s);
  JsonDocument d;
  const DeserializationError de = deserializeJson(d, j);
  TEST_ASSERT_TRUE_MESSAGE(de == DeserializationError::Ok, j.c_str());
  TEST_ASSERT_EQUAL_STRING("a\"b\\c", d["nvs"]["lastFail"].as<const char*>());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_golden_fresh_device);
  RUN_TEST(test_golden_starved_reserve_shape);
  RUN_TEST(test_golden_vision_last_error_shape);
  RUN_TEST(test_golden_embedding_parked_shape);
  RUN_TEST(test_golden_nvsfill0_shape);
  RUN_TEST(test_keys_always_present_values_may_be_null);
  RUN_TEST(test_failing_key_name_is_json_safe);
  return UNITY_END();
}
