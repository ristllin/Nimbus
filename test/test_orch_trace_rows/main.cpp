// test_orch_trace_rows - the glass-box tool row and its honesty marks (L19-24,
// CUM-676): a clipped tool result wants its full text parked as a sidecar; when
// the sidecar write fails (the SD open fails under internal-heap starvation),
// the row carries "full text not kept: low memory" instead of quietly showing
// only the clipped text, and the turn-end row counts the loss (blobFails).
// The FS fake is the injected writer: healthy, failing, or absent (no card,
// which is degraded, never broken). Counter-test: a healthy fake leaves the
// row unchanged.
#include <unity.h>

#include <string>

#include "nimbus/orch/blob_store.h"
#include "nimbus/orch/failure_rows.h"
#include "nimbus/orch/trace_rows.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// A healthy store: every blob lands, keyed by content.
static std::string healthyStore(const std::string& full, bool* failed) {
  *failed = false;
  return "/mem/blobs/" + blobHash(full) + ".txt";
}

// The SD open fails under internal-heap starvation: nothing lands.
static std::string failingStore(const std::string&, bool* failed) {
  *failed = true;
  return "";
}

// No card (degraded mode): nothing lands, and that is not a failure.
static std::string noCardStore(const std::string&, bool* failed) {
  *failed = false;
  return "";
}

static TraceToolRow rowIn(bool blobWanted = true) {
  TraceToolRow r;
  r.name = "memory.write";
  r.args = "{\"content\":\"" + std::string(400, 'a') + "\"}";
  r.output = std::string(2000, 'b');
  r.blobWanted = blobWanted;
  return r;
}

static void test_a_healthy_store_leaves_the_row_changed_only_by_the_clips() {
  const TraceToolRowOut out = buildTraceToolRow(rowIn(), healthyStore);
  TEST_ASSERT_TRUE(out.clipped);
  TEST_ASSERT_FALSE(out.blobFailed);
  TEST_ASSERT_TRUE(out.blobPath.find("/mem/blobs/") == 0);
  TEST_ASSERT_EQUAL(std::string::npos, out.text.find(kTraceBlobLostMark));
  TEST_ASSERT_TRUE(out.text.find("memory.write({\"content\":\"aaaa") == 0);
  TEST_ASSERT_TRUE(out.text.find("\xE2\x80\xA6[truncated]") != std::string::npos);
  TEST_ASSERT_TRUE(out.text.find("\xE2\x86\x92 bbbb") != std::string::npos);
}

static void test_a_failing_store_marks_the_row_and_reports_the_loss() {
  const TraceToolRowOut out = buildTraceToolRow(rowIn(), failingStore);
  TEST_ASSERT_TRUE(out.blobFailed);
  TEST_ASSERT_TRUE(out.blobPath.empty());
  TEST_ASSERT_TRUE(out.text.find(kTraceBlobLostMark) != std::string::npos);
  TEST_ASSERT_TRUE(out.text.find("full text not kept: low memory") != std::string::npos);
  // The clips are still there: the row is the clipped text plus the mark.
  TEST_ASSERT_TRUE(out.text.find("\xE2\x80\xA6[truncated]") != std::string::npos);
}

static void test_no_card_is_degraded_not_failed() {
  const TraceToolRowOut out = buildTraceToolRow(rowIn(), noCardStore);
  TEST_ASSERT_FALSE(out.blobFailed);
  TEST_ASSERT_TRUE(out.blobPath.empty());
  TEST_ASSERT_EQUAL(std::string::npos, out.text.find(kTraceBlobLostMark));
}

static void test_a_spent_budget_never_writes_nor_marks() {
  int writes = 0;
  const TraceToolRowOut out = buildTraceToolRow(rowIn(/*blobWanted=*/false),
                                               [&](const std::string&, bool*) {
                                                 writes++;
                                                 return "";
                                               });
  TEST_ASSERT_EQUAL(0, writes);
  TEST_ASSERT_FALSE(out.blobFailed);
  TEST_ASSERT_EQUAL(std::string::npos, out.text.find(kTraceBlobLostMark));
  TEST_ASSERT_TRUE(out.clipped);   // the row still says it clipped
}

static void test_an_unclipped_row_needs_no_sidecar() {
  TraceToolRow r;
  r.name = "device.status";
  r.args = "{}";
  r.output = "{\"heap\":123456}";
  int writes = 0;
  const TraceToolRowOut out =
      buildTraceToolRow(r, [&](const std::string&, bool*) { writes++; return "/mem/blobs/x.txt"; });
  TEST_ASSERT_EQUAL(0, writes);
  TEST_ASSERT_FALSE(out.clipped);
  TEST_ASSERT_TRUE(out.blobPath.empty());
  TEST_ASSERT_EQUAL(std::string::npos, out.text.find(kTraceBlobLostMark));
  TEST_ASSERT_TRUE(out.text.find("device.status({})\n\xE2\x86\x92 {\"heap\":123456}") == 0);
}

static void test_the_turn_end_row_counts_the_loss() {
  TurnEndRowIn r;
  r.host = "anthropic";
  r.in = 100;
  r.out = 50;
  r.blobFails = 2;
  const std::string js = turnEndRowJson(r);
  TEST_ASSERT_TRUE(js.find("\"blobFails\":2") != std::string::npos);
  // Zero losses leave the key out (the row stays small when nothing failed).
  r.blobFails = 0;
  TEST_ASSERT_EQUAL(std::string::npos, turnEndRowJson(r).find("blobFails"));
  // The key survives shrinking until the bare row.
  r.blobFails = 2;
  TEST_ASSERT_TRUE(turnEndRowJson(r, kTurnEndRowMin + 24).find("blobFails") != std::string::npos);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_healthy_store_leaves_the_row_changed_only_by_the_clips);
  RUN_TEST(test_a_failing_store_marks_the_row_and_reports_the_loss);
  RUN_TEST(test_no_card_is_degraded_not_failed);
  RUN_TEST(test_a_spent_budget_never_writes_nor_marks);
  RUN_TEST(test_an_unclipped_row_needs_no_sidecar);
  RUN_TEST(test_the_turn_end_row_counts_the_loss);
  return UNITY_END();
}
