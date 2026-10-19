#include <unity.h>

#include <cstdio>
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
  bool put(int slot, const std::string& v) override { slots[slot] = v; return true; }
  bool remove(int slot) override { slots.erase(slot); return true; }
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


// ---- no active record lost under any store failure (program 2026-10-18 lane C) ----
// A store that refuses puts and removes on a pattern, the way a full NVS refuses
// writes (an erase on the device always lands, but a removal is modeled as fallible
// too so the property covers every seam).
struct FlakyStore : JournalStore {
  std::map<int, std::string> slots;
  uint32_t rng = 1;
  int failPutPct = 0, failRemovePct = 0;
  bool roll(int pct) {
    rng = rng * 1103515245u + 12345u;
    return (int)((rng >> 16) % 100) < pct;
  }
  std::string get(int slot) override {
    auto it = slots.find(slot);
    return it == slots.end() ? std::string() : it->second;
  }
  bool put(int slot, const std::string& v) override {
    if (roll(failPutPct)) return false;
    slots[slot] = v;
    return true;
  }
  bool remove(int slot) override {
    if (roll(failRemovePct)) return false;
    slots.erase(slot);
    return true;
  }
  void clearNs() override { slots.clear(); }
  // Tags the store holds, with the record each slot holds.
  std::map<std::string, JobRecord> held() const {
    std::map<std::string, JobRecord> out;
    for (const auto& kv : slots) {
      JobRecord r{};
      if (deserializeRecord(kv.second.c_str(), r)) out[r.tag] = r;
    }
    return out;
  }
};

// The regression: a seen record below an active one, and a store that refuses every
// put. The old gc copied the active record down (refused) and then erased its source.
static void test_gc_never_erases_an_active_record_when_puts_fail() {
  FlakyStore st;
  {
    Journal j;
    j.begin(&st);
    TEST_ASSERT_TRUE(j.write(rec("jobA", "o:a", JobState::Done, /*seen=*/true)));
    TEST_ASSERT_TRUE(j.write(rec("jobB", "o:b")));
    st.failPutPct = 100;   // the partition fills
    j.gc();
    TEST_ASSERT_EQUAL_INT(1, j.count());
  }
  TEST_ASSERT_EQUAL_size_t(1, st.held().count("jobB"));   // still stored
  TEST_ASSERT_EQUAL_size_t(0, st.held().count("jobA"));   // the delivered one went
  Journal j2;
  j2.begin(&st);   // reboot: the active job re-attaches
  JobRecord out{};
  TEST_ASSERT_TRUE(j2.get(0, out));
  TEST_ASSERT_EQUAL_STRING("jobB", out.tag);
  TEST_ASSERT_EQUAL_STRING("o:b", out.jobId);
}

// A write that did not land answers false but keeps the record in RAM (the job is
// still polled); a markSeen that did not land does too.
static void test_write_verdicts_report_a_refused_store() {
  FlakyStore st;
  Journal j;
  j.begin(&st);
  st.failPutPct = 100;
  TEST_ASSERT_FALSE(j.write(rec("job0001", "o:1")));
  TEST_ASSERT_EQUAL_INT(1, j.count());          // held for this run
  TEST_ASSERT_FALSE(j.update("job0001", JobState::Done));
  TEST_ASSERT_FALSE(j.markSeen("job0001"));
  TEST_ASSERT_EQUAL_INT(0, j.count());          // seen in RAM
  st.failPutPct = 0;
  TEST_ASSERT_TRUE(j.write(rec("job0002", "o:2")));
  TEST_ASSERT_TRUE(j.markSeen("job0002"));
  TEST_ASSERT_FALSE(j.markSeen("nope"));
}

// The property, over random operation sequences and failure rates: an operation never
// takes a RAM-active record out of the store (only its own landed update changes it),
// a reboot re-attaches every stored record exactly once, and RAM never holds a slot
// twice.
static void test_no_active_record_lost_under_any_failure_pattern() {
  const char* tags[] = {"job01", "job02", "job03", "job04", "job05",
                        "job06", "job07", "job08", "job09", "job10"};
  for (uint32_t seed = 1; seed <= 400; ++seed) {
    FlakyStore st;
    st.rng = seed;
    st.failPutPct = (int)(seed % 5) * 25;        // 0, 25, 50, 75, 100 %
    st.failRemovePct = (int)((seed / 5) % 3) * 30;   // 0, 30, 60 %
    Journal j;
    j.begin(&st);
    uint32_t r = seed * 2654435761u;
    for (int step = 0; step < 60; ++step) {
      r = r * 1664525u + 1013904223u;
      const char* tag = tags[(r >> 8) % 10];
      // Which RAM-active tags the store holds BEFORE this operation.
      std::map<std::string, bool> mustStay;
      const auto before = st.held();
      for (int i = 0; i < j.count(); ++i) {
        JobRecord a{};
        j.get(i, a);
        if (before.count(a.tag)) mustStay[a.tag] = true;
      }
      const int op = (int)((r >> 20) % 6);
      bool reboot = false;
      switch (op) {
        case 0: case 1: j.write(rec(tag, "o:x", JobState::Running)); break;
        case 2: j.update(tag, JobState::NeedsInput); break;
        case 3: j.markSeen(tag); mustStay.erase(tag); break;   // its own change
        case 4: j.gc(); break;
        case 5: reboot = true; break;
      }
      const auto after = st.held();
      for (const auto& kv : mustStay) {
        JobRecord a{};
        bool stillActive = false;
        for (int i = 0; i < j.count(); ++i)
          if (j.get(i, a) && kv.first == a.tag) stillActive = true;
        if (!stillActive) continue;   // the op itself made it seen
        char msg[96];
        std::snprintf(msg, sizeof(msg), "seed %u step %d op %d lost %s", (unsigned)seed, step, op,
                      kv.first.c_str());
        TEST_ASSERT_EQUAL_size_t_MESSAGE(1, after.count(kv.first), msg);
      }
      if (reboot) {
        Journal j2;
        j2.begin(&st);
        // Every stored tag re-attaches exactly once (as active or seen).
        int loaded = j2.slotsUsed();
        TEST_ASSERT_EQUAL_INT((int)st.held().size(), loaded);
        j = j2;
      }
      TEST_ASSERT_TRUE(j.slotsUsed() <= kAgentMaxJobs);
    }
  }
}

// A duplicate left in the store (an old gc's copy whose source erase did not land)
// re-attaches once, and the extra slot is dropped.
static void test_begin_drops_a_duplicate_tag() {
  FlakyStore st;
  char buf[kJournalSerializeBuf];
  serializeRecord(rec("job0007", "o:7"), buf, sizeof(buf));
  st.slots[0] = buf;
  st.slots[3] = buf;
  Journal j;
  j.begin(&st);
  TEST_ASSERT_EQUAL_INT(1, j.count());
  TEST_ASSERT_EQUAL_size_t(1, st.slots.size());
  TEST_ASSERT_EQUAL_size_t(1, st.slots.count(0));
  // A record loaded from slot 3 keeps writing to slot 3 (no stray copy in slot 0).
  FlakyStore st2;
  st2.slots[3] = buf;
  Journal k;
  k.begin(&st2);
  TEST_ASSERT_TRUE(k.update("job0007", JobState::Done));
  TEST_ASSERT_EQUAL_size_t(1, st2.slots.size());
  TEST_ASSERT_EQUAL_size_t(1, st2.slots.count(3));
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
  RUN_TEST(test_gc_never_erases_an_active_record_when_puts_fail);
  RUN_TEST(test_write_verdicts_report_a_refused_store);
  RUN_TEST(test_no_active_record_lost_under_any_failure_pattern);
  RUN_TEST(test_begin_drops_a_duplicate_tag);
  return UNITY_END();
}
