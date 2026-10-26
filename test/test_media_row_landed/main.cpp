#include <unity.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "nimbus/orch/episodic_log.h"
#include "nimbus/orch/store_full.h"

// Prism gate row 2 (a photo "stays in history" claimed before the row landed): an
// inbound photo the full file store refused gets storeFullFact's fact for the head,
// and its "The photo itself stays in this chat's history" arm fired whenever the
// photo's BLOB landed on the card, without checking its episodic ROW. A refused row
// write is only held in RAM (a restart before the card answers loses it), so the head
// promised something the device could not re-show. The claim now follows the row:
// mediaRowLanded() over the SD append-log's real state, in every state the log can
// be in, and captureMediaFile returns its answer.

using namespace nimbus::orch;

// An in-memory card that can refuse appends (a mid-op SD fault).
struct CardFs : EpiFs {
  std::map<std::string, std::string> files;
  bool refuse = false;
  long append(const std::string& path, const std::string& bytes) override {
    if (refuse) return -1;
    std::string& f = files[path];
    const long off = (long)f.size();
    f += bytes;
    return off;
  }
  std::string readAll(const std::string& path) const override {
    auto it = files.find(path);
    return it == files.end() ? std::string() : it->second;
  }
  std::string readRange(const std::string& path, long offset, long len) const override {
    auto it = files.find(path);
    if (it == files.end() || offset < 0 || len < 0 || (size_t)(offset + len) > it->second.size()) return {};
    return it->second.substr((size_t)offset, (size_t)len);
  }
  long size(const std::string& path) const override {
    auto it = files.find(path);
    return it == files.end() ? 0 : (long)it->second.size();
  }
  std::vector<std::string> list(const std::string& dir) const override {
    std::vector<std::string> out;
    const std::string prefix = dir + "/";
    for (const auto& kv : files)
      if (kv.first.compare(0, prefix.size(), prefix) == 0 && kv.first.find('/', prefix.size()) == std::string::npos)
        out.push_back(kv.first.substr(prefix.size()));
    return out;
  }
  bool remove(const std::string& path) override { return files.erase(path) > 0; }
  bool exists(const std::string& path) const override { return files.count(path) > 0; }
};

static int g_seq = 0;
static EpisodicMessage photoRow() {
  EpisodicMessage m;
  char id[16];
  std::snprintf(id, sizeof id, "m%08x", ++g_seq);
  m.id = id;
  m.sessionId = "1001";
  m.tsHours = 500000;
  m.role = "user";
  m.kind = MsgKind::Image;
  m.text = "[image] a photo";
  m.blobPath = "/mem/blobs/ab/abcdef0123456789.jpg";
  return m;
}

// What captureMediaFile answers right after the row's write on this log.
static bool landed(const AppendLogEpisodicStore& st) {
  return mediaRowLanded(true, true, st.lastAppendOk(), st.heldCount());
}

// The truth table: nothing captured never lands; a store with no SD log keeps the
// row as its only copy; on the SD log the write must be taken AND nothing held.
static void test_media_row_landed_truth_table() {
  for (int captured = 0; captured < 2; ++captured)
    for (int sdLog = 0; sdLog < 2; ++sdLog)
      for (int ok = 0; ok < 2; ++ok)
        for (int held = 0; held < 3; ++held) {
          const bool want = captured && (!sdLog || (ok && held == 0));
          TEST_ASSERT_EQUAL(want, mediaRowLanded(captured, sdLog, ok, held));
        }
}

// The SD append-log in every state: a refused write is held (not landed); with the
// card back, a row that queues behind held rows lands only once every held row is on
// the card - the write being "taken" alone is not enough when it only moved older rows.
static void test_the_claim_follows_the_row_on_the_append_log() {
  CardFs fs;
  AppendLogEpisodicStore st(fs, "/mem/episodic");
  st.addMessage(photoRow());
  TEST_ASSERT_TRUE(landed(st));               // a healthy card: the row is on it
  fs.refuse = true;
  st.addMessage(photoRow());
  TEST_ASSERT_FALSE(landed(st));              // refused: held in RAM only
  st.addMessage(photoRow());
  st.addMessage(photoRow());
  TEST_ASSERT_EQUAL(3, st.heldCount());
  fs.refuse = false;                          // the card answers again
  st.addMessage(photoRow());                  // moves 2 of the 4 queued rows
  TEST_ASSERT_TRUE(st.lastAppendOk());        // the write was taken...
  TEST_ASSERT_TRUE(st.heldCount() > 0);
  TEST_ASSERT_FALSE(landed(st));              // ...but this photo's row is still held
  while (st.heldCount() > 0) st.flushHeld(8);
  st.addMessage(photoRow());
  TEST_ASSERT_TRUE(landed(st));               // nothing held: the newest row is on the card
}

// The photo's fact carries the history arm only from that answer.
static void test_store_full_fact_history_arm_follows_the_answer() {
  TEST_ASSERT_TRUE(storeFullFact(true, true).find("photo itself stays in this chat's history") != std::string::npos);
  TEST_ASSERT_TRUE(storeFullFact(true, false).find("stays in this chat's history") == std::string::npos);
  TEST_ASSERT_TRUE(storeFullFact(false, false).find("stays in this chat's history") == std::string::npos);
}

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

// The device seam: captureMediaFile returns the row's answer (taken under the Lock
// with the capture), never true once the blob alone landed.
static void test_capture_media_file_returns_the_rows_answer() {
  const std::string mem = readFile("src/agent/memory_subsystem.cpp");
  const size_t a = mem.find("bool captureMediaFile(");
  TEST_ASSERT_TRUE(a != std::string::npos);
  const size_t b = mem.find("\n}\n", a);
  const std::string body = mem.substr(a, b - a);
  TEST_ASSERT_TRUE(body.find("capturedRowLanded(") != std::string::npos);
  TEST_ASSERT_TRUE(body.find("return landed;") != std::string::npos);
  // After the capture, no path answers a plain true.
  const size_t cap = body.find("captureMessageAt(");
  TEST_ASSERT_TRUE(cap != std::string::npos);
  TEST_ASSERT_TRUE(body.find("return true;", cap) == std::string::npos);
  TEST_ASSERT_TRUE(mem.find("nimbus::orch::mediaRowLanded(") != std::string::npos);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_media_row_landed_truth_table);
  RUN_TEST(test_the_claim_follows_the_row_on_the_append_log);
  RUN_TEST(test_store_full_fact_history_arm_follows_the_answer);
  RUN_TEST(test_capture_media_file_returns_the_rows_answer);
  return UNITY_END();
}
