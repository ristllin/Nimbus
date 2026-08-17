#include <unity.h>

#include <set>
#include <string>
#include <vector>

#include "nimbus/orch/blob_store.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// Identical bytes -> identical hash (dedup); different bytes -> different hash.
static void test_hash_dedups_identical_bytes() {
  std::string a = "the quick brown fox";
  std::string b = "the quick brown fox";
  std::string c = "the quick brown fix";
  TEST_ASSERT_EQUAL_STRING(blobHash(a).c_str(), blobHash(b).c_str());
  TEST_ASSERT_TRUE(blobHash(a) != blobHash(c));
  TEST_ASSERT_EQUAL(16, (int)blobHash(a).size());   // 16 hex chars
  // empty is stable + valid
  TEST_ASSERT_EQUAL(16, (int)blobHash("").size());
}

// Path builder + round-trip through blobHashOf.
static void test_path_and_hash_extraction() {
  std::string h = blobHash("audio-bytes");
  std::string p = blobPath("/mem/blobs", h, "ogg");
  TEST_ASSERT_EQUAL_STRING(("/mem/blobs/" + h + ".ogg").c_str(), p.c_str());
  TEST_ASSERT_EQUAL_STRING(h.c_str(), blobHashOf(p).c_str());
  // no ext
  TEST_ASSERT_EQUAL_STRING(("/mem/blobs/" + h).c_str(), blobPath("/mem/blobs", h, "").c_str());
  // trailing slash in dir is tolerated
  TEST_ASSERT_EQUAL_STRING(("/mem/blobs/" + h + ".mp3").c_str(), blobPath("/mem/blobs/", h, "mp3").c_str());
  // bare filename
  TEST_ASSERT_EQUAL_STRING("abc123", blobHashOf("abc123.wav").c_str());
  TEST_ASSERT_EQUAL_STRING("", blobHashOf("").c_str());
}

// Reference-count prune: unreferenced blobs are returned, referenced ones survive.
static void test_unreferenced_prune() {
  std::vector<std::string> present = {"aaaa.ogg", "bbbb.mp3", "cccc.wav", "dddd.png"};
  std::set<std::string> referenced = {"aaaa", "cccc"};  // bbbb + dddd are orphaned
  auto dead = unreferencedBlobs(present, referenced);
  TEST_ASSERT_EQUAL(2, (int)dead.size());
  TEST_ASSERT_EQUAL_STRING("bbbb.mp3", dead[0].c_str());
  TEST_ASSERT_EQUAL_STRING("dddd.png", dead[1].c_str());
  // all referenced -> nothing to prune
  std::set<std::string> allRef = {"aaaa", "bbbb", "cccc", "dddd"};
  TEST_ASSERT_EQUAL(0, (int)unreferencedBlobs(present, allRef).size());
  // none referenced -> all pruned
  TEST_ASSERT_EQUAL(4, (int)unreferencedBlobs(present, {}).size());
}

// Streaming BlobHasher fed in arbitrary chunks == blobHash of the whole buffer, so the
// device can content-address a large file without holding all its bytes in RAM.
static void test_streaming_hasher_matches_whole() {
  std::string data;
  for (int i = 0; i < 5000; i++) data.push_back((char)((i * 37 + 11) & 0xff));  // ~5 KB
  std::string whole = blobHash(data);
  // feed in ragged chunks of varying sizes
  BlobHasher h;
  size_t off = 0;
  int sizes[] = {1, 7, 512, 3, 100, 999};
  int si = 0;
  while (off < data.size()) {
    size_t n = sizes[si++ % 6];
    if (off + n > data.size()) n = data.size() - off;
    h.update(data.data() + off, n);
    off += n;
  }
  TEST_ASSERT_EQUAL_STRING(whole.c_str(), h.hex().c_str());
  // empty stream == blobHash("")
  BlobHasher e;
  TEST_ASSERT_EQUAL_STRING(blobHash("").c_str(), e.hex().c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_streaming_hasher_matches_whole);
  RUN_TEST(test_hash_dedups_identical_bytes);
  RUN_TEST(test_path_and_hash_extraction);
  RUN_TEST(test_unreferenced_prune);
  return UNITY_END();
}
