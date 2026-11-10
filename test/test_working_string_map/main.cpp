#include <unity.h>

#include <functional>
#include <string>
#include <string_view>
#include <type_traits>

#include "nimbus/orch/psram_alloc.h"

// Lane LINUXFIX: the hosted nimbusd image builds with g++ 12 (libstdc++), which
// has NO std::hash specialization for a basic_string with a custom allocator -
// only libc++ (macOS clang) has one. An unordered container keyed by
// WorkingString that relies on std::hash<WorkingString> compiles on every Mac
// and fails in the image (release run 42832344608, job nimbusd-image). These
// tests pin the class rule: the working-set map names an explicit content
// hasher, and that hasher hashes the string content identically on every
// standard library. tools/check_custom_alloc_hash.py holds the same rule over
// every source file in the battery.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// The class rule, at compile time: the working-set map's hasher must NOT be
// std::hash<WorkingString> (not invocable under libstdc++).
static_assert(!std::is_same_v<WorkingStringMap<size_t>::hasher, std::hash<WorkingString>>,
              "WorkingStringMap must name an explicit hasher; libstdc++ has no "
              "std::hash for a custom-allocator basic_string");
static_assert(std::is_invocable_v<WorkingStringMap<size_t>::hasher, const WorkingString&>,
              "the explicit hasher must be invocable for a WorkingString key");

static void test_hasher_is_content_based_and_portable() {
  WorkingString a = workingCopy("Project\tName.PDF");
  WorkingString b = workingCopy("Project\tName.PDF");
  WorkingString c = workingCopy("project\tname.pdf");
  WorkingStringMap<size_t>::hasher h;
  // Same content, different string objects: same hash (the map dedups on it).
  TEST_ASSERT_EQUAL_size_t(h(a), h(b));
  // The hash is the std::string_view hash of the content - the one hash every
  // standard library provides - so keys hash identically under g++ 12.
  TEST_ASSERT_EQUAL_size_t(
      h(a), std::hash<std::string_view>{}(std::string_view("Project\tName.PDF")));
  // Different content: (overwhelmingly) different hash.
  TEST_ASSERT_NOT_EQUAL(h(a), h(c));
}

static void test_map_keys_and_nodes_ride_the_working_allocator() {
  WorkingStringMap<size_t> m;
  m.reserve(64);
  m.emplace(workingCopy("alpha"), 1);
  m.emplace(workingCopy("beta"), 2);
  TEST_ASSERT_EQUAL_size_t(2, m.size());
  TEST_ASSERT_EQUAL_size_t(1, m.at(workingCopy("alpha")));
  TEST_ASSERT_EQUAL_size_t(2, m.at(workingCopy("beta")));
  TEST_ASSERT_TRUE(m.find(workingCopy("gamma")) == m.end());
  // The allocator is the working-set one, so keys and nodes land in PSRAM on
  // the device, not the internal heap.
  static_assert(
      std::is_same_v<WorkingStringMap<size_t>::allocator_type,
                     WorkingAllocator<std::pair<const WorkingString, size_t>>>,
      "map nodes must ride the working-set allocator");
}

static void test_equal_to_compares_content() {
  WorkingStringMap<size_t>::key_equal eq;
  TEST_ASSERT_TRUE(eq(workingCopy("same"), workingCopy("same")));
  TEST_ASSERT_FALSE(eq(workingCopy("same"), workingCopy("other")));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_hasher_is_content_based_and_portable);
  RUN_TEST(test_map_keys_and_nodes_ride_the_working_allocator);
  RUN_TEST(test_equal_to_compares_content);
  return UNITY_END();
}
