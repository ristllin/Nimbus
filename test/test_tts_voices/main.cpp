// Host regression tests for the Mistral TTS voice-catalog transform + pagination.
//
// The bug these lock down: Mistral's GET /v1/audio/voices is paginated and IGNORES
// the `page` field - only `offset` advances the window, while the body still echoes
// a misleading page/total_pages. The device read ONE page (the first 10 of 30
// voices) and stopped, so the picker only ever saw a third of the catalog (Jane's
// lone "sarcasm" of nine emotions; Marie/French absent entirely).
//
// `core::mergeMistralVoicesPage` is the ONE transform the device uses too
// (src/agent/adapters/tts_voices.cpp), so these asserts guard real firmware
// behavior, not a parallel copy: per-page dedup by slug (so a server that re-serves
// the same page can't inflate the list), persona/emotion split for the web cascade,
// and language shortening incl. French.

#include <unity.h>

#include <set>
#include <string>

#include <ArduinoJson.h>

#include "nimbus/tts_catalog.h"

using core::mergeMistralVoicesPage;

// Find the row object whose "value" == slug inside the accumulated rows string.
static bool findRow(const std::string& rows, const char* slug, JsonDocument& out) {
  std::string arr = "[" + rows + "]";
  JsonDocument doc;
  if (deserializeJson(doc, arr)) return false;
  for (JsonObjectConst v : doc.as<JsonArrayConst>()) {
    if (std::string(v["value"] | "") == slug) {
      out.set(v);
      return true;
    }
  }
  return false;
}

static int countRows(const std::string& rows) {
  std::string arr = "[" + rows + "]";
  JsonDocument doc;
  if (deserializeJson(doc, arr)) return -1;
  return (int)doc.as<JsonArrayConst>().size();
}

// One US English voice: full transform (persona/emotion split, lang shortening).
void test_transform_us(void) {
  const char* page = R"({"items":[
    {"name":"Paul - Sad","slug":"en_paul_sad","languages":["en_us"],"gender":"male"}
  ],"total":30,"page":1,"page_size":10,"total_pages":3})";
  std::set<std::string> seen;
  std::string rows;
  int added = -1, total = -1;
  int processed = mergeMistralVoicesPage(page, seen, rows, &added, &total);
  TEST_ASSERT_EQUAL_INT(1, processed);
  TEST_ASSERT_EQUAL_INT(1, added);
  TEST_ASSERT_EQUAL_INT(30, total);  // advertised size parsed (loop hint only)

  JsonDocument row;
  TEST_ASSERT_TRUE(findRow(rows, "en_paul_sad", row));
  TEST_ASSERT_EQUAL_STRING("Paul", row["name"] | "");
  TEST_ASSERT_EQUAL_STRING("sad", row["emotion"] | "");
  TEST_ASSERT_EQUAL_STRING("US", row["lang"] | "");
  TEST_ASSERT_EQUAL_STRING("male", row["gender"] | "");
  TEST_ASSERT_EQUAL_STRING("Paul (male, US) - sad", row["label"] | "");
}

// French voice: language shortens to FR, persona = Marie.
void test_transform_french(void) {
  const char* page = R"({"items":[
    {"name":"Marie - Happy","slug":"fr_marie_happy","languages":["fr_fr"],"gender":"female"}
  ],"total":30})";
  std::set<std::string> seen;
  std::string rows;
  mergeMistralVoicesPage(page, seen, rows, nullptr, nullptr);
  JsonDocument row;
  TEST_ASSERT_TRUE(findRow(rows, "fr_marie_happy", row));
  TEST_ASSERT_EQUAL_STRING("Marie", row["name"] | "");
  TEST_ASSERT_EQUAL_STRING("FR", row["lang"] | "");
  TEST_ASSERT_EQUAL_STRING("happy", row["emotion"] | "");
  TEST_ASSERT_EQUAL_STRING("Marie (female, FR) - happy", row["label"] | "");
}

// UK special-case: en_gb -> "UK" (not "GB").
void test_transform_uk(void) {
  const char* page = R"({"items":[
    {"name":"Jane - Sarcasm","slug":"gb_jane_sarcasm","languages":["en_gb"],"gender":"female"}
  ]})";
  std::set<std::string> seen;
  std::string rows;
  mergeMistralVoicesPage(page, seen, rows, nullptr, nullptr);
  JsonDocument row;
  TEST_ASSERT_TRUE(findRow(rows, "gb_jane_sarcasm", row));
  TEST_ASSERT_EQUAL_STRING("UK", row["lang"] | "");
  TEST_ASSERT_EQUAL_STRING("sarcasm", row["emotion"] | "");
}

// Accumulate across two DISTINCT offset pages -> all four unique voices collected.
void test_paginate_accumulates(void) {
  const char* pageA = R"({"items":[
    {"name":"Paul - Sad","slug":"en_paul_sad","languages":["en_us"],"gender":"male"},
    {"name":"Paul - Happy","slug":"en_paul_happy","languages":["en_us"],"gender":"male"}
  ],"total":4})";
  const char* pageB = R"({"items":[
    {"name":"Jane - Confused","slug":"gb_jane_confused","languages":["en_gb"],"gender":"female"},
    {"name":"Marie - Angry","slug":"fr_marie_angry","languages":["fr_fr"],"gender":"female"}
  ],"total":4})";
  std::set<std::string> seen;
  std::string rows;
  int addedA = 0, addedB = 0;
  int procA = mergeMistralVoicesPage(pageA, seen, rows, &addedA, nullptr);
  int procB = mergeMistralVoicesPage(pageB, seen, rows, &addedB, nullptr);
  TEST_ASSERT_EQUAL_INT(2, procA);
  TEST_ASSERT_EQUAL_INT(2, addedA);
  TEST_ASSERT_EQUAL_INT(2, procB);
  TEST_ASSERT_EQUAL_INT(2, addedB);
  TEST_ASSERT_EQUAL_INT(4, (int)seen.size());
  TEST_ASSERT_EQUAL_INT(4, countRows(rows));
}

// Regression for the ignored-`page`/`offset` bug: re-serving the SAME page must add
// NOTHING (added==0) and leave the rows unchanged - that added==0 is the device's
// signal to stop paginating instead of looping on duplicates.
void test_duplicate_page_adds_nothing(void) {
  const char* page = R"({"items":[
    {"name":"Paul - Sad","slug":"en_paul_sad","languages":["en_us"],"gender":"male"},
    {"name":"Oliver - Neutral","slug":"gb_oliver_neutral","languages":["en_gb"],"gender":"male"}
  ],"total":30})";
  std::set<std::string> seen;
  std::string rows;
  int added1 = 0, added2 = 0;
  mergeMistralVoicesPage(page, seen, rows, &added1, nullptr);
  std::string afterFirst = rows;
  int proc2 = mergeMistralVoicesPage(page, seen, rows, &added2, nullptr);
  TEST_ASSERT_EQUAL_INT(2, added1);
  TEST_ASSERT_EQUAL_INT(2, proc2);   // items were still processed...
  TEST_ASSERT_EQUAL_INT(0, added2);  // ...but none were new
  TEST_ASSERT_TRUE(rows == afterFirst);
  TEST_ASSERT_EQUAL_INT(2, countRows(rows));
}

// Garbage / empty bodies degrade cleanly (no crash, nothing added).
void test_bad_bodies(void) {
  std::set<std::string> seen;
  std::string rows;
  int added = -1;
  TEST_ASSERT_EQUAL_INT(0, mergeMistralVoicesPage("not json", seen, rows, &added, nullptr));
  TEST_ASSERT_EQUAL_INT(0, added);
  TEST_ASSERT_EQUAL_INT(0, mergeMistralVoicesPage(R"({"items":[]})", seen, rows, &added, nullptr));
  TEST_ASSERT_EQUAL_INT(0, added);
  TEST_ASSERT_EQUAL_INT(0, mergeMistralVoicesPage(R"({"total":30})", seen, rows, &added, nullptr));
  TEST_ASSERT_TRUE(rows.empty());
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_transform_us);
  RUN_TEST(test_transform_french);
  RUN_TEST(test_transform_uk);
  RUN_TEST(test_paginate_accumulates);
  RUN_TEST(test_duplicate_page_adds_nothing);
  RUN_TEST(test_bad_bodies);
  return UNITY_END();
}
