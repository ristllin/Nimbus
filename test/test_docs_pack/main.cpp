// Docs pack (W13) - host tests over the GENERATED embedded documentation table
// (tools/gen_docs_pack.py -> nimbus/docs_pack_data.h) + the portable access
// layer (nimbus/docs_pack.h). Guards the properties the device relies on:
// enough sections, unique ids, bodies under the tool-result clamp, search that
// finds real content and rejects garbage, and valid UTF-8 (the generator must
// never split a body mid-codepoint - an invalid byte can 400 the next provider
// request when the section is fed back as a tool result).

#include <unity.h>

#include <cstring>
#include <set>
#include <string>

#include "nimbus/docs_pack.h"

using namespace nimbus::docs;

void setUp() {}
void tearDown() {}

// ---- shape ------------------------------------------------------------------

static void test_pack_has_enough_sections() {
  TEST_ASSERT_GREATER_THAN(100, (int)sectionCount());
  TEST_ASSERT_GREATER_THAN(10, (int)fileCount());
}

static void test_every_id_unique_and_well_formed() {
  std::set<std::string> ids;
  for (size_t i = 0; i < sectionCount(); i++) {
    const DocSection& s = section(i);
    TEST_ASSERT_NOT_NULL(s.id);
    TEST_ASSERT_NOT_NULL(s.title);
    TEST_ASSERT_NOT_NULL(s.body);
    TEST_ASSERT_TRUE_MESSAGE(std::strchr(s.id, '#') != nullptr, s.id);
    TEST_ASSERT_TRUE_MESSAGE(ids.insert(s.id).second, s.id);  // unique
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, (int)std::strlen(s.body), s.id);
  }
}

static void test_no_body_exceeds_result_clamp() {
  for (size_t i = 0; i < sectionCount(); i++) {
    const DocSection& s = section(i);
    TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(4096, (int)std::strlen(s.body), s.id);
  }
}

static void test_file_groups_cover_all_sections_contiguously() {
  size_t covered = 0;
  for (size_t i = 0; i < fileCount(); i++) {
    const DocFile& f = file(i);
    TEST_ASSERT_EQUAL_MESSAGE((int)covered, (int)f.first, f.slug);
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, (int)f.count, f.slug);
    // Every section in the run carries the file's slug as its id prefix.
    for (size_t k = f.first; k < (size_t)(f.first + f.count); k++) {
      const DocSection& s = section(k);
      TEST_ASSERT_EQUAL_MESSAGE(
          0, std::strncmp(s.id, f.slug, std::strlen(f.slug)), s.id);
      TEST_ASSERT_EQUAL_CHAR_MESSAGE('#', s.id[std::strlen(f.slug)], s.id);
    }
    covered += f.count;
  }
  TEST_ASSERT_EQUAL((int)sectionCount(), (int)covered);
}

// ---- lookup -----------------------------------------------------------------

static void test_find_known_and_unknown_id() {
  const DocSection& s0 = section(0);
  const DocSection* hit = find(s0.id);
  TEST_ASSERT_NOT_NULL(hit);
  TEST_ASSERT_EQUAL_PTR(&s0, hit);
  TEST_ASSERT_NULL(find("no-such-file#no-such-section"));
}

static void test_find_file_by_slug() {
  TEST_ASSERT_NOT_NULL(findFile("orchestrator-world"));
  TEST_ASSERT_NOT_NULL(findFile("hardware"));
  TEST_ASSERT_NULL(findFile("test-plan"));   // excluded from the curated pack
}

static void test_near_misses_for_wrong_heading() {
  // A wrong heading slug on a real file offers that file's real sections.
  std::string near = nearMisses("hardware#nonexistent-heading", 5);
  TEST_ASSERT_FALSE(near.empty());
  TEST_ASSERT_TRUE(near.find("hardware#") != std::string::npos);
}

// ---- search (the docs.search tool path) -------------------------------------

static void test_search_finds_known_sections() {
  const DocSection* hits[8];
  size_t n = search("scratchpad", hits, 8);
  TEST_ASSERT_GREATER_THAN(0, (int)n);
  bool anyBodyHasTerm = false;
  for (size_t i = 0; i < n; i++) {
    std::string hay = std::string(hits[i]->title) + "\n" + hits[i]->body;
    for (char& c : hay) c = (char)tolower((unsigned char)c);
    if (hay.find("scratchpad") != std::string::npos) anyBodyHasTerm = true;
  }
  TEST_ASSERT_TRUE(anyBodyHasTerm);
}

static void test_search_is_and_match_and_case_insensitive() {
  const DocSection* hits[8];
  // Multi-word AND: both terms must appear (spawn + brief - sub-sessions).
  size_t n = search("SPAWN brief", hits, 8);
  TEST_ASSERT_GREATER_THAN(0, (int)n);
  for (size_t i = 0; i < n; i++) {
    std::string hay = std::string(hits[i]->title) + "\n" + hits[i]->body;
    for (char& c : hay) c = (char)tolower((unsigned char)c);
    TEST_ASSERT_TRUE_MESSAGE(hay.find("spawn") != std::string::npos, hits[i]->id);
    TEST_ASSERT_TRUE_MESSAGE(hay.find("brief") != std::string::npos, hits[i]->id);
  }
}

static void test_search_misses_garbage() {
  const DocSection* hits[8];
  TEST_ASSERT_EQUAL(0, (int)search("zzxqv wibblefrob", hits, 8));
}

static void test_search_respects_max() {
  const DocSection* hits[3];
  TEST_ASSERT_LESS_OR_EQUAL(3, (int)search("the", hits, 3));
}

static void test_snippet_is_bounded_and_relevant() {
  const DocSection* hits[1];
  size_t n = search("scratchpad", hits, 1);
  TEST_ASSERT_EQUAL(1, (int)n);
  std::string snip = snippet(*hits[0], "scratchpad");
  TEST_ASSERT_FALSE(snip.empty());
  TEST_ASSERT_LESS_OR_EQUAL(200, (int)snip.size());
}

// ---- encoding ---------------------------------------------------------------

static bool validUtf8(const char* s) {
  const unsigned char* p = (const unsigned char*)s;
  while (*p) {
    if (*p < 0x80) { p++; continue; }
    int cont;
    if ((*p & 0xE0) == 0xC0) cont = 1;
    else if ((*p & 0xF0) == 0xE0) cont = 2;
    else if ((*p & 0xF8) == 0xF0) cont = 3;
    else return false;   // stray continuation or invalid lead byte
    p++;
    for (int i = 0; i < cont; i++, p++)
      if ((*p & 0xC0) != 0x80) return false;   // includes early NUL = split codepoint
  }
  return true;
}

static void test_every_body_and_title_valid_utf8() {
  for (size_t i = 0; i < sectionCount(); i++) {
    const DocSection& s = section(i);
    TEST_ASSERT_TRUE_MESSAGE(validUtf8(s.title), s.id);
    TEST_ASSERT_TRUE_MESSAGE(validUtf8(s.body), s.id);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_pack_has_enough_sections);
  RUN_TEST(test_every_id_unique_and_well_formed);
  RUN_TEST(test_no_body_exceeds_result_clamp);
  RUN_TEST(test_file_groups_cover_all_sections_contiguously);
  RUN_TEST(test_find_known_and_unknown_id);
  RUN_TEST(test_find_file_by_slug);
  RUN_TEST(test_near_misses_for_wrong_heading);
  RUN_TEST(test_search_finds_known_sections);
  RUN_TEST(test_search_is_and_match_and_case_insensitive);
  RUN_TEST(test_search_misses_garbage);
  RUN_TEST(test_search_respects_max);
  RUN_TEST(test_snippet_is_bounded_and_relevant);
  RUN_TEST(test_every_body_and_title_valid_utf8);
  return UNITY_END();
}
