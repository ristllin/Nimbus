#include <unity.h>

#include <cstring>
#include <map>
#include <string>

#include "nimbus/orch/caps.h"
#include "nimbus/orch/journal.h"

using namespace nimbus::orch;

// In-memory JournalStore standing in for the device NVS impl. A std::map keyed by
// slot mirrors NVS keys j0..j5; `slots` is public so a test can seed slots to
// simulate a reboot and assert byte-identical re-attach.
struct FakeStore : JournalStore {
  std::map<int, std::string> slots;
  std::string get(int slot) override {
    auto it = slots.find(slot);
    return it == slots.end() ? std::string() : it->second;
  }
  void put(int slot, const std::string& v) override { slots[slot] = v; }
  void remove(int slot) override { slots.erase(slot); }
  void clearNs() override { slots.clear(); }
};

static FakeStore g_store;

void setUp() { g_store = FakeStore(); }
void tearDown() {}

// Build a record with the fields that matter for the logic under test.
static JobRecord rec(const char* tag, const char* jobId, JobState st = JobState::Running,
                     bool seen = false) {
  JobRecord r{};
  std::strncpy(r.tag, tag, sizeof(r.tag) - 1);
  std::strncpy(r.jobId, jobId, sizeof(r.jobId) - 1);
  std::strncpy(r.backend, "openai", sizeof(r.backend) - 1);
  std::strncpy(r.category, "research", sizeof(r.category) - 1);
  std::strncpy(r.model, "gpt-x", sizeof(r.model) - 1);
  std::strncpy(r.chatId, "12345", sizeof(r.chatId) - 1);
  r.state = st;
  r.resultSeen = seen;
  return r;
}

// ---- JobState helpers -------------------------------------------------------

static void test_is_terminal() {
  TEST_ASSERT_TRUE(isTerminal(JobState::Done));
  TEST_ASSERT_TRUE(isTerminal(JobState::Error));
  TEST_ASSERT_TRUE(isTerminal(JobState::Cancelled));
  TEST_ASSERT_FALSE(isTerminal(JobState::Queued));
  TEST_ASSERT_FALSE(isTerminal(JobState::Running));
  TEST_ASSERT_FALSE(isTerminal(JobState::NeedsInput));  // HITL is non-terminal
  TEST_ASSERT_FALSE(isTerminal(JobState::Unknown));
}

// ---- write + get roundtrip --------------------------------------------------

static void test_write_and_get_roundtrip() {
  Journal j;
  j.begin(&g_store);
  TEST_ASSERT_TRUE(j.write(rec("job0001", "openai:abc")));
  TEST_ASSERT_EQUAL_INT(1, j.count());
  JobRecord out{};
  TEST_ASSERT_TRUE(j.get(0, out));
  TEST_ASSERT_EQUAL_STRING("job0001", out.tag);
  TEST_ASSERT_EQUAL_STRING("openai:abc", out.jobId);
  TEST_ASSERT_EQUAL_STRING("openai", out.backend);
  TEST_ASSERT_EQUAL_INT((int)JobState::Running, (int)out.state);
  TEST_ASSERT_FALSE(j.get(1, out));  // out of range
}

// ---- dedupe by TAG (not jobId) ----------------------------------------------

static void test_dedupe_by_tag_updates_in_place() {
  Journal j;
  j.begin(&g_store);
  j.write(rec("job0001", "openai:abc"));
  // Same tag, DIFFERENT jobId → updates the existing slot, count unchanged.
  j.write(rec("job0001", "openai:def", JobState::Done));
  TEST_ASSERT_EQUAL_INT(1, j.slotsUsed());
  JobRecord out{};
  TEST_ASSERT_TRUE(j.get(0, out));
  TEST_ASSERT_EQUAL_STRING("openai:def", out.jobId);
  TEST_ASSERT_EQUAL_INT((int)JobState::Done, (int)out.state);
}

// ---- count() counts only active (!resultSeen) -------------------------------

static void test_count_ignores_seen() {
  Journal j;
  j.begin(&g_store);
  j.write(rec("job0001", "o:1"));
  j.write(rec("job0002", "o:2", JobState::Done, /*seen=*/true));
  TEST_ASSERT_EQUAL_INT(2, j.slotsUsed());  // both occupy slots
  TEST_ASSERT_EQUAL_INT(1, j.count());      // only one is active
  // get() iterates only active records → the seen one is skipped.
  JobRecord out{};
  TEST_ASSERT_TRUE(j.get(0, out));
  TEST_ASSERT_EQUAL_STRING("job0001", out.tag);
  TEST_ASSERT_FALSE(j.get(1, out));
}

// ---- markSeen + update ------------------------------------------------------

static void test_mark_seen_and_update_state() {
  Journal j;
  j.begin(&g_store);
  j.write(rec("job0001", "o:1", JobState::Queued));
  TEST_ASSERT_TRUE(j.update("job0001", JobState::Running));
  JobRecord out{};
  j.get(0, out);
  TEST_ASSERT_EQUAL_INT((int)JobState::Running, (int)out.state);

  TEST_ASSERT_TRUE(j.markSeen("job0001"));
  TEST_ASSERT_EQUAL_INT(0, j.count());  // no longer active
  // Unknown tag operations return false.
  TEST_ASSERT_FALSE(j.markSeen("nope"));
  TEST_ASSERT_FALSE(j.update("nope", JobState::Done));
}

// ---- fill to 6, then eviction / refusal -------------------------------------

static void test_full_evicts_oldest_seen() {
  Journal j;
  j.begin(&g_store);
  // Slot 0 is seen; slots 1..5 are active. Journal now full (6).
  j.write(rec("job0000", "o:0", JobState::Done, /*seen=*/true));
  for (int i = 1; i < kAgentMaxJobs; i++) {
    char tag[24], jid[16];
    std::snprintf(tag, sizeof(tag), "job000%d", i);
    std::snprintf(jid, sizeof(jid), "o:%d", i);
    j.write(rec(tag, jid));
  }
  TEST_ASSERT_EQUAL_INT(kAgentMaxJobs, j.slotsUsed());
  // 7th write evicts the oldest SEEN record (job0000), reusing its slot.
  TEST_ASSERT_TRUE(j.write(rec("job0006", "o:6")));
  TEST_ASSERT_EQUAL_INT(kAgentMaxJobs, j.slotsUsed());  // still 6 slots
  // job0000 is gone; job0006 is present.
  JobRecord out{};
  TEST_ASSERT_FALSE(j.markSeen("job0000"));  // no longer present
  TEST_ASSERT_TRUE(j.update("job0006", JobState::Done));
}

// Eviction must target the OLDEST seen record by INSERTION order, not merely the
// first resultSeen slot in array order. Regression for the case where a prior
// eviction reused a mid-array slot: that reused slot then holds the NEWEST seen
// record but sits at a low index, so a naive "first seen slot" scan would evict it.
static void test_full_evicts_oldest_seen_not_first_slot() {
  Journal j;
  j.begin(&g_store);
  // Fill all 6 slots with SEEN records A..F (slots 0..5), inserted in that order.
  const char* tags[] = {"jobA", "jobB", "jobC", "jobD", "jobE", "jobF"};
  for (int i = 0; i < kAgentMaxJobs; i++)
    j.write(rec(tags[i], "o:x", JobState::Done, /*seen=*/true));
  TEST_ASSERT_EQUAL_INT(kAgentMaxJobs, j.slotsUsed());

  // write(G) evicts the oldest seen (jobA, slot 0) and lands G there as ACTIVE.
  TEST_ASSERT_TRUE(j.write(rec("jobG", "o:g")));
  TEST_ASSERT_FALSE(j.markSeen("jobA"));  // A evicted
  // markSeen(G) makes slot 0 the NEWEST seen record (lowest index, highest seq).
  TEST_ASSERT_TRUE(j.markSeen("jobG"));

  // write(H) must evict the OLDEST remaining seen = jobB (slot 1), NOT jobG (slot 0,
  // newest seen). The pre-fix "first seen slot" scan would wrongly evict jobG.
  TEST_ASSERT_TRUE(j.write(rec("jobH", "o:h")));
  TEST_ASSERT_TRUE(j.markSeen("jobG"));   // jobG still present (was NOT evicted)
  TEST_ASSERT_FALSE(j.markSeen("jobB"));  // jobB (oldest seen) was evicted
}

static void test_full_of_live_jobs_refuses_write() {
  Journal j;
  j.begin(&g_store);
  for (int i = 0; i < kAgentMaxJobs; i++) {
    char tag[24], jid[16];
    std::snprintf(tag, sizeof(tag), "job000%d", i);
    std::snprintf(jid, sizeof(jid), "o:%d", i);
    j.write(rec(tag, jid));  // all active, none seen
  }
  TEST_ASSERT_EQUAL_INT(kAgentMaxJobs, j.count());
  // No seen record to evict → the write is REFUSED (must not silently drop a live job).
  TEST_ASSERT_FALSE(j.write(rec("job0099", "o:99")));
  TEST_ASSERT_EQUAL_INT(kAgentMaxJobs, j.slotsUsed());
}

// ---- gc compacts out seen records -------------------------------------------

static void test_gc_compacts_seen() {
  Journal j;
  j.begin(&g_store);
  j.write(rec("job0001", "o:1"));                              // active
  j.write(rec("job0002", "o:2", JobState::Done, true));         // seen
  j.write(rec("job0003", "o:3"));                              // active
  TEST_ASSERT_EQUAL_INT(3, j.slotsUsed());
  j.gc();
  TEST_ASSERT_EQUAL_INT(2, j.slotsUsed());  // seen record removed
  TEST_ASSERT_EQUAL_INT(2, j.count());
  // Both survivors still reachable, in order.
  JobRecord a{}, b{};
  TEST_ASSERT_TRUE(j.get(0, a));
  TEST_ASSERT_TRUE(j.get(1, b));
  TEST_ASSERT_EQUAL_STRING("job0001", a.tag);
  TEST_ASSERT_EQUAL_STRING("job0003", b.tag);
  // The vacated tail slot was removed from the store.
  TEST_ASSERT_EQUAL_INT(2, (int)g_store.slots.size());
}

// ---- clearAll ---------------------------------------------------------------

static void test_clear_all() {
  Journal j;
  j.begin(&g_store);
  j.write(rec("job0001", "o:1"));
  j.write(rec("job0002", "o:2"));
  j.clearAll();
  TEST_ASSERT_EQUAL_INT(0, j.slotsUsed());
  TEST_ASSERT_EQUAL_INT(0, j.count());
  TEST_ASSERT_TRUE(g_store.slots.empty());
}

// ---- serialize refuses an over-buffer record --------------------------------

static void test_serialize_refuses_over_buffer() {
  // A record that serializes fine into the real buffer...
  JobRecord r = rec("job0001", "openai:remote-id-1234567890");
  char buf[kJournalSerializeBuf];
  const size_t need = serializeRecord(r, buf, sizeof(buf));
  TEST_ASSERT_TRUE(need > 0);
  TEST_ASSERT_TRUE(need + 1 <= sizeof(buf));
  TEST_ASSERT_TRUE(std::strlen(buf) > 0);

  // ...but into a too-small buffer, it writes NOTHING usable and reports the
  // needed length (so the caller refuses to persist a truncated record).
  char tiny[8];
  const size_t need2 = serializeRecord(r, tiny, sizeof(tiny));
  TEST_ASSERT_TRUE(need2 + 1 > sizeof(tiny));
  TEST_ASSERT_EQUAL_INT(0, (int)std::strlen(tiny));  // buf[0] = 0
}

// ---- reboot re-attach: loadAll restores jobId byte-identical ----------------

static void test_reboot_reattach_preserves_jobid() {
  const char* longJid = "anthropic:msg_01AbCdEfGhIjKlMnOpQrStUv-9876543210";
  {
    Journal j;
    j.begin(&g_store);
    j.write(rec("job0007", longJid, JobState::Running));
  }
  // Fresh instance over the SAME store = a reboot.
  Journal j2;
  j2.begin(&g_store);
  TEST_ASSERT_EQUAL_INT(1, j2.count());
  JobRecord out{};
  TEST_ASSERT_TRUE(j2.get(0, out));
  TEST_ASSERT_EQUAL_STRING("job0007", out.tag);
  TEST_ASSERT_EQUAL_STRING(longJid, out.jobId);  // durable re-attach key intact
  TEST_ASSERT_EQUAL_INT((int)JobState::Running, (int)out.state);
  TEST_ASSERT_EQUAL_STRING("openai", out.backend);
}

// ---- empty / malformed slots are skipped on load ----------------------------

static void test_load_skips_empty_and_malformed_slots() {
  g_store.slots[0] = "";                         // empty → skipped
  g_store.slots[1] = "{not valid json";          // malformed → skipped
  g_store.slots[2] = "{\"tag\":\"\",\"jid\":\"x\"}";  // empty tag → skipped
  g_store.slots[3] = "{\"tag\":\"job0003\",\"jid\":\"o:3\",\"st\":1}";  // good
  Journal j;
  j.begin(&g_store);
  TEST_ASSERT_EQUAL_INT(1, j.count());
  JobRecord out{};
  TEST_ASSERT_TRUE(j.get(0, out));
  TEST_ASSERT_EQUAL_STRING("job0003", out.tag);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_is_terminal);
  RUN_TEST(test_write_and_get_roundtrip);
  RUN_TEST(test_dedupe_by_tag_updates_in_place);
  RUN_TEST(test_count_ignores_seen);
  RUN_TEST(test_mark_seen_and_update_state);
  RUN_TEST(test_full_evicts_oldest_seen);
  RUN_TEST(test_full_evicts_oldest_seen_not_first_slot);
  RUN_TEST(test_full_of_live_jobs_refuses_write);
  RUN_TEST(test_gc_compacts_seen);
  RUN_TEST(test_clear_all);
  RUN_TEST(test_serialize_refuses_over_buffer);
  RUN_TEST(test_reboot_reattach_preserves_jobid);
  RUN_TEST(test_load_skips_empty_and_malformed_slots);
  return UNITY_END();
}
