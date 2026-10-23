#include <unity.h>

#include <cstdio>
#include <string>
#include <vector>

#include "nimbus/net/tg_media_memo.h"
#include "nimbus/orch/blob_store.h"
#include "nimbus/orch/media_label.h"

// The one inbound media label (nimbus/orch/media_label.h): "[image k/N ref=<hex16>]"
// plus a class suffix, used by the turn text, the episodic row and the history. A
// table over every class and every optional part, the cut on a character boundary,
// and the reader that recognizes every format (and the older heads).

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

static MediaLabel img(int k, int n, const char* ref, MediaClass c, const char* detail,
                      const char* sent = "") {
  MediaLabel l;
  l.kind = MediaKind::Image;
  l.k = k;
  l.n = n;
  l.ref = ref;
  l.cls = c;
  l.detail = detail;
  l.sentAt = sent;
  return l;
}

static void test_format_table() {
  struct Row {
    MediaLabel l;
    const char* want;
  } rows[] = {
      {img(2, 3, "0123456789abcdef", MediaClass::Described, "a plate of pasta"),
       "[image 2/3 ref=0123456789abcdef] a plate of pasta"},
      {img(3, 3, "fedcba9876543210", MediaClass::NotDescribed, "", "09:58"),
       "[image 3/3 ref=fedcba9876543210, sent 09:58] (couldn't be described)"},
      {img(1, 1, "", MediaClass::FetchFailed, "too large"), "[image 1/1] (could not be fetched: too large)"},
      {img(1, 1, "", MediaClass::FetchFailed, ""), "[image 1/1] (could not be fetched)"},
      {img(1, 1, "0123456789abcdef", MediaClass::Described, ""),
       "[image 1/1 ref=0123456789abcdef] (couldn't be described)"},
      {img(0, 0, "0123456789abcdef", MediaClass::Described, "x"), "[image ref=0123456789abcdef] x"},
  };
  for (const Row& r : rows) TEST_ASSERT_EQUAL_STRING(r.want, mediaSegment(r.l).c_str());
  MediaLabel f = img(1, 2, "0011223344556677", MediaClass::Stored, "menu.pdf");
  f.kind = MediaKind::File;
  TEST_ASSERT_EQUAL_STRING("[file 1/2 ref=0011223344556677] menu.pdf", mediaSegment(f).c_str());
  f.detail = "";
  TEST_ASSERT_EQUAL_STRING("[file 1/2 ref=0011223344556677] attachment", mediaSegment(f).c_str());
}

// Every class renders, and each one differently (a class with no wording fails). A
// kept file (Stored) is labelled as a file.
static void test_every_class_has_its_own_suffix() {
  std::string seen[(int)MediaClass::Count];
  for (int c = 0; c < (int)MediaClass::Count; ++c) {
    MediaLabel l = img(1, 1, "0123456789abcdef", (MediaClass)c, "d");
    if ((MediaClass)c == MediaClass::Stored) l.kind = MediaKind::File;
    const std::string s = mediaSegment(l);
    TEST_ASSERT_TRUE(s.size() > std::string("[image 1/1 ref=0123456789abcdef]").size());
    seen[c] = s;
    for (int p = 0; p < c; ++p) TEST_ASSERT_TRUE(seen[p] != s);
  }
}

// The ref is exactly 16 lowercase hex characters (the blob store's digest); anything
// else is left out of the label rather than passed on.
static void test_ref_is_the_blob_hash() {
  const std::string h = blobHash("some jpeg bytes");
  TEST_ASSERT_TRUE(validRef(h));
  TEST_ASSERT_EQUAL_STRING(("[image 1/1 ref=" + h + "]").c_str(), mediaTag(img(1, 1, h.c_str(), MediaClass::Described, "")).c_str());
  for (const char* bad : {"", "0123456789abcde", "0123456789abcdef0", "0123456789ABCDEF", "0123456789abcdeg",
                          "../../etc/passwd"}) {
    TEST_ASSERT_FALSE(validRef(bad));
    TEST_ASSERT_EQUAL_STRING("[image 1/1]", mediaTag(img(1, 1, bad, MediaClass::Described, "")).c_str());
  }
}

// A long description is cut at kMediaTextMax on a character boundary and marked; a
// description at the limit is kept whole.
static void test_description_cut_on_a_character_boundary() {
  std::string d(kMediaTextMax - 1, 'a');
  d += "\xC3\xA9";   // a 2-byte character straddling the limit
  d += "tail";
  const std::string s = mediaSegment(img(1, 1, "", MediaClass::Described, d.c_str()));
  TEST_ASSERT_TRUE(s.size() >= std::string(kMediaCutMark).size());
  TEST_ASSERT_EQUAL_STRING(kMediaCutMark, s.substr(s.size() - std::string(kMediaCutMark).size()).c_str());
  TEST_ASSERT_TRUE(s.find("\xC3") == std::string::npos);   // never half a character
  const std::string whole(kMediaTextMax, 'b');
  const std::string w = mediaSegment(img(1, 1, "", MediaClass::Described, whole.c_str()));
  TEST_ASSERT_TRUE(w.find(kMediaCutMark) == std::string::npos);
}

// The reader knows every label head, the history's "[image, sent 09:58]" and the
// older "[image]" / "[file]" rows; it never mistakes ordinary text for a label.
static void test_label_head_reader() {
  MediaKind k = MediaKind::File;
  TEST_ASSERT_TRUE(mediaLabelHead(mediaSegment(img(2, 3, "0123456789abcdef", MediaClass::Described, "x")), k));
  TEST_ASSERT_EQUAL_INT((int)MediaKind::Image, (int)k);
  for (const char* t : {"[image] old row", "[image, sent 09:58]", "[image 1/1] (could not be fetched)"}) {
    TEST_ASSERT_TRUE_MESSAGE(mediaLabelHead(t, k), t);
    TEST_ASSERT_EQUAL_INT((int)MediaKind::Image, (int)k);
  }
  for (const char* t : {"[file] notes.pdf", "[file 1/1 ref=0011223344556677] a.pdf"}) {
    TEST_ASSERT_TRUE(mediaLabelHead(t, k));
    TEST_ASSERT_EQUAL_INT((int)MediaKind::File, (int)k);
  }
  for (const char* t : {"", "[images are nice]", "[imagex]", "image 1/1", "[filed] it", "[image"})
    TEST_ASSERT_FALSE_MESSAGE(mediaLabelHead(t, k), t);
}

// Late items: only with a set clock, and only past kLateItemS.
static void test_late_item_rule() {
  const int64_t now = 1792400000;
  TEST_ASSERT_FALSE(lateItem(now, now));
  TEST_ASSERT_FALSE(lateItem(now, now - kLateItemS));
  TEST_ASSERT_TRUE(lateItem(now, now - kLateItemS - 1));
  TEST_ASSERT_FALSE(lateItem(now, 0));
  TEST_ASSERT_FALSE(lateItem(86400, 1));          // clock unset
  TEST_ASSERT_FALSE(lateItem(now, now + 600));    // a date ahead of the clock is not late
}

// ---- every label path uses the one formatter --------------------------------------

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

// Comments out, string literals kept.
static std::string code(const char* path) {
  const std::string s = readFile(path);
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i], n = i + 1 < s.size() ? s[i + 1] : 0;
    if (c == '"' || c == '\'') {
      const char q = c;
      out += c;
      for (++i; i < s.size(); ++i) {
        out += s[i];
        if (s[i] == '\\' && i + 1 < s.size()) { out += s[++i]; continue; }
        if (s[i] == q) break;
      }
    } else if (c == '/' && n == '/') {
      i = s.find('\n', i);
      if (i == std::string::npos) break;
      out += '\n';
    } else if (c == '/' && n == '*') {
      i = s.find("*/", i + 2);
      if (i == std::string::npos) break;
      ++i;
    } else {
      out += c;
    }
  }
  return out;
}

static size_t count(const std::string& hay, const std::string& needle) {
  size_t n = 0;
  for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) ++n;
  return n;
}

// No receive-path source writes a label by hand: no "[image" or "[file" literal, no
// "[photo" either. The device builds every label (the turn's segment, which is also the
// episodic row's text) with mediaSegment, and a failure is a class of the same label.
static void test_every_label_path_uses_the_formatter() {
  for (const char* f : {"src/agent/telegram.cpp", "src/agent/memory_subsystem.cpp", "src/agent/tg_archive.cpp",
                        "lib/core/src/orch_msg_batch.cpp", "lib/core/include/nimbus/net/tg_receive.h"}) {
    const std::string c = code(f);
    for (const char* lit : {"\"[image", "\"[file", "\"[photo", "[image]", "[file]"})
      TEST_ASSERT_EQUAL_size_t_MESSAGE(0, count(c, lit), (std::string(f) + ": " + lit).c_str());
  }
  const std::string tg = code("src/agent/telegram.cpp");
  // Two segment builds: a failure (failedSegment, every refusal and failed fetch) and
  // the result; both are mediaSegment(label), and the row text is the segment.
  TEST_ASSERT_EQUAL_size_t(2, count(tg, "nimbus::orch::mediaSegment(label)"));
  TEST_ASSERT_EQUAL_size_t(2, count(tg, "return failedSegment(label, why, slot, seg);"));
  TEST_ASSERT_EQUAL_size_t(0, count(tg, "mediaTag("));
  // The row text is the segment of the row's label (a photo the head saw is stored
  // pending its record, R1): still built by the formatter, never by hand.
  TEST_ASSERT_TRUE(tg.find("String(row.c_str()), localPath, photo ? \"jpg\" : \"bin\", m.date);") != std::string::npos);
  TEST_ASSERT_EQUAL_size_t(1, count(tg, "return nimbus::orch::mediaSegment(row);"));
}

// The blob-hash dedupe of the description step: the same chat, bytes (content hash)
// and caption context reuse the description; a new caption, another chat, another
// photo or a failed description never do; the ring forgets the oldest.
static void test_description_memo() {
  nimbus::net::MediaMemo m;
  std::string out;
  const std::string h = blobHash("jpeg bytes 1");
  TEST_ASSERT_FALSE(m.find("77", h, "dinner", out));
  m.put("77", h, "dinner", "a plate of pasta");
  TEST_ASSERT_TRUE(m.find("77", h, "dinner", out));
  TEST_ASSERT_EQUAL_STRING("a plate of pasta", out.c_str());
  TEST_ASSERT_FALSE(m.find("77", h, "how many calories?", out));   // a new question
  TEST_ASSERT_FALSE(m.find("78", h, "dinner", out));               // another chat
  TEST_ASSERT_FALSE(m.find("77", blobHash("jpeg bytes 2"), "dinner", out));
  m.put("77", blobHash("x"), "", "");                                // a failure is not kept
  TEST_ASSERT_FALSE(m.find("77", blobHash("x"), "", out));
  for (size_t i = 0; i < nimbus::net::kMediaMemoSlots; ++i)
    m.put("77", blobHash("fill " + std::to_string(i)), "", "d" + std::to_string(i));
  TEST_ASSERT_FALSE(m.find("77", h, "dinner", out));                // the oldest fell out
  TEST_ASSERT_TRUE(m.find("77", blobHash("fill 7"), "", out));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_format_table);
  RUN_TEST(test_every_class_has_its_own_suffix);
  RUN_TEST(test_ref_is_the_blob_hash);
  RUN_TEST(test_description_cut_on_a_character_boundary);
  RUN_TEST(test_label_head_reader);
  RUN_TEST(test_late_item_rule);
  RUN_TEST(test_every_label_path_uses_the_formatter);
  RUN_TEST(test_description_memo);
  return UNITY_END();
}
