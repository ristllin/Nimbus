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

// The RAM-active tags the store holds right now.
static std::map<std::string, bool> activeTagsHeld(const Journal& j, const FlakyStore& st) {
  std::map<std::string, bool> out;
  const auto held = st.held();
  for (int i = 0; i < j.count(); ++i) {
    JobRecord a{};
    j.get(i, a);
    if (held.count(a.tag)) out[a.tag] = true;
  }
  return out;
}

static bool isActive(const Journal& j, const std::string& tag) {
  JobRecord a{};
  for (int i = 0; i < j.count(); ++i)
    if (j.get(i, a) && tag == a.tag) return true;
  return false;
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
      std::map<std::string, bool> mustStay = activeTagsHeld(j, st);
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
        if (!isActive(j, kv.first)) continue;   // the op itself made it seen
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

// ---- wave 2: the journal file, the finished-job ring, delivered once ----------------

// The device's journal file I/O over RAM: the file, the legacy NVS records, and the
// failures a full file system or a torn write produce.
struct FakeFileIo : JournalFileIo {
  bool fileExists = false;
  std::string file;
  std::map<int, std::string> legacy;   // NVS j<slot>
  bool failWrite = false;
  int writes = 0, erases = 0;
  bool readFile(std::string& out) override {
    if (!fileExists) return false;
    out = file;
    return true;
  }
  bool writeFileAtomic(const std::string& v) override {
    ++writes;
    if (failWrite) return false;   // tmp + rename: the previous file stays
    file = v;
    fileExists = true;
    return true;
  }
  std::string readLegacySlot(int slot) override {
    auto it = legacy.find(slot);
    return it == legacy.end() ? std::string() : it->second;
  }
  bool eraseLegacy() override {
    ++erases;
    legacy.clear();
    return true;
  }
};

static std::string recJson(const JobRecord& r) {
  char buf[kJournalSerializeBuf];
  serializeRecord(r, buf, sizeof(buf));
  return buf;
}

static FinishedJob fin(const char* name, const char* prj, const char* art, bool failed = false) {
  FinishedJob f{};
  std::strncpy(f.name, name, sizeof(f.name) - 1);
  std::strncpy(f.prj, prj, sizeof(f.prj) - 1);
  std::strncpy(f.artifact, art, sizeof(f.artifact) - 1);
  std::strncpy(f.finishedAt, "2026-10-22 Thu 21:14 BST", sizeof(f.finishedAt) - 1);
  std::strncpy(f.chatId, "12345", sizeof(f.chatId) - 1);
  f.finishedEpoch = 1792700000u;
  f.failed = failed;
  return f;
}

// The moved-store legs for the journal: file only, legacy NVS only, both, neither.
static void test_journal_file_migration_legs() {
  const std::string a = recJson(rec("job0003", "openai:resp_a"));
  const std::string b = recJson(rec("job0004", "openai:resp_b"));
  {   // legacy only: the records move to the file at once; NVS is never written
    FakeFileIo io;
    io.legacy[0] = a;
    io.legacy[4] = b;
    FileJournalStore st(io);
    TEST_ASSERT_EQUAL_INT((int)JournalSource::Legacy, (int)st.load());
    TEST_ASSERT_TRUE(io.fileExists);
    TEST_ASSERT_TRUE(st.durable());
    TEST_ASSERT_EQUAL_size_t(2, io.legacy.size());   // kept until a valid-image boot
    Journal j;
    j.begin(&st);
    TEST_ASSERT_EQUAL_INT(2, j.count());
    TEST_ASSERT_TRUE(j.restored("job0003"));
    // The next boot reads the file; on a trial boot the legacy copy stays, on a boot
    // whose image was valid it goes.
    FileJournalStore st2(io);
    TEST_ASSERT_EQUAL_INT((int)JournalSource::File, (int)st2.load());
    TEST_ASSERT_FALSE(st2.retireLegacy(/*imageValidAtBoot=*/false));
    TEST_ASSERT_EQUAL_size_t(2, io.legacy.size());
    TEST_ASSERT_TRUE(st2.retireLegacy(true));
    TEST_ASSERT_TRUE(io.legacy.empty());
    Journal j2;
    j2.begin(&st2);
    JobRecord out{};
    TEST_ASSERT_TRUE(j2.get(1, out));
    TEST_ASSERT_EQUAL_STRING("openai:resp_b", out.jobId);   // byte-identical re-attach key
  }
  {   // file only
    FakeFileIo io;
    std::string slots[kJournalSlots];
    slots[2] = a;
    io.file = encodeJournalFile(slots);
    io.fileExists = true;
    FileJournalStore st(io);
    TEST_ASSERT_EQUAL_INT((int)JournalSource::File, (int)st.load());
    TEST_ASSERT_FALSE(st.retireLegacy(true));   // nothing to retire
    TEST_ASSERT_EQUAL_INT(0, io.erases);
    TEST_ASSERT_EQUAL_STRING(a.c_str(), st.get(2).c_str());
  }
  {   // both, the NVS copy already in the file (the migration's own copy): the file
      // wins, nothing is duplicated, the NVS copy retires on a valid boot. (Running NVS
      // records the file lacks are merged: test_journal_file_merges_records_...)
    FakeFileIo io;
    std::string slots[kJournalSlots];
    slots[0] = b;
    io.file = encodeJournalFile(slots);
    io.fileExists = true;
    io.legacy[0] = b;
    FileJournalStore st(io);
    TEST_ASSERT_EQUAL_INT((int)JournalSource::File, (int)st.load());
    Journal j;
    j.begin(&st);
    TEST_ASSERT_EQUAL_INT(1, j.count());
    JobRecord out{};
    TEST_ASSERT_TRUE(j.get(0, out));
    TEST_ASSERT_EQUAL_STRING("job0004", out.tag);
    TEST_ASSERT_TRUE(st.retireLegacy(true));
  }
  {   // neither (a fresh device): nothing written until the first job
    FakeFileIo io;
    FileJournalStore st(io);
    TEST_ASSERT_EQUAL_INT((int)JournalSource::None, (int)st.load());
    TEST_ASSERT_EQUAL_INT(0, io.writes);
    Journal j;
    j.begin(&st);
    TEST_ASSERT_EQUAL_INT(0, j.count());
    TEST_ASSERT_TRUE(j.write(rec("job0000", "anthropic:sesn_1")));
    TEST_ASSERT_EQUAL_INT(1, io.writes);
  }
  {   // legacy only and the first file write fails: served from RAM, NVS untouched,
      // and the next write that lands stores everything
    FakeFileIo io;
    io.legacy[1] = a;
    io.failWrite = true;
    FileJournalStore st(io);
    TEST_ASSERT_EQUAL_INT((int)JournalSource::Legacy, (int)st.load());
    TEST_ASSERT_FALSE(st.durable());
    TEST_ASSERT_FALSE(io.fileExists);
    TEST_ASSERT_EQUAL_size_t(1, io.legacy.size());
    Journal j;
    j.begin(&st);
    TEST_ASSERT_EQUAL_INT(1, j.count());
    io.failWrite = false;
    TEST_ASSERT_TRUE(j.write(rec("job0009", "openai:resp_9")));
    std::string back[kJournalSlots];
    TEST_ASSERT_TRUE(decodeJournalFile(io.file, back));
    TEST_ASSERT_EQUAL_STRING(a.c_str(), back[1].c_str());   // the legacy record rode along
  }
}

// A rollback, then this release again: the file is older than the NVS records the
// earlier release wrote meanwhile. A running NVS record the file lacks is added (never
// orphaned), a delivered one is not, one already held is not duplicated, and a tag the
// file uses for another job is renumbered; the NVS copy then retires on a valid boot.
static void test_journal_file_merges_records_an_earlier_release_left() {
  FakeFileIo io;
  std::string slots[kJournalSlots];
  slots[0] = recJson(rec("job0000", "openai:resp_old"));             // from before the rollback
  FinishedJob f = fin("t", "", "");
  std::strncpy(f.tag, "job0001", sizeof(f.tag) - 1);
  char buf[kJournalSerializeBuf];
  serializeFinished(f, buf, sizeof(buf));
  slots[kJournalFinishedSlot0] = buf;
  io.file = encodeJournalFile(slots);
  io.fileExists = true;
  io.legacy[0] = recJson(rec("job0000", "anthropic:sesn_new"));      // tag clash, another job
  io.legacy[1] = recJson(rec("job0002", "openai:resp_old"));         // already in the file
  io.legacy[2] = recJson(rec("job0003", "openai:resp_done", JobState::Done, /*seen=*/true));
  FileJournalStore st(io);
  TEST_ASSERT_EQUAL_INT((int)JournalSource::Merged, (int)st.load());
  TEST_ASSERT_TRUE(st.durable());
  Journal j;
  j.begin(&st);
  TEST_ASSERT_EQUAL_INT(2, j.count());   // the file's job and the merged one
  JobRecord a{}, b{};
  TEST_ASSERT_TRUE(j.get(0, a));
  TEST_ASSERT_TRUE(j.get(1, b));
  TEST_ASSERT_EQUAL_STRING("openai:resp_old", a.jobId);
  TEST_ASSERT_EQUAL_STRING("anthropic:sesn_new", b.jobId);
  TEST_ASSERT_EQUAL_STRING("job0002", b.tag);   // renumbered past job0000 and job0001
  TEST_ASSERT_TRUE(j.restored("job0002"));
  // The next boot reads the merged file and retires the NVS copy on a valid image.
  FileJournalStore st2(io);
  TEST_ASSERT_EQUAL_INT((int)JournalSource::File, (int)st2.load());
  TEST_ASSERT_TRUE(st2.retireLegacy(true));
  TEST_ASSERT_TRUE(io.legacy.empty());
  // Six running jobs in the file: a seventh from NVS cannot be added (the file's stay).
  FakeFileIo full;
  std::string six[kJournalSlots];
  char tag[16], jid[24];
  for (int i = 0; i < kAgentMaxJobs; ++i) {
    std::snprintf(tag, sizeof(tag), "job%04d", i);
    std::snprintf(jid, sizeof(jid), "openai:resp_%d", i);
    six[i] = recJson(rec(tag, jid));
  }
  full.file = encodeJournalFile(six);
  full.fileExists = true;
  full.legacy[0] = recJson(rec("job0009", "openai:resp_9"));
  FileJournalStore st3(full);
  TEST_ASSERT_EQUAL_INT((int)JournalSource::File, (int)st3.load());
}

// A write the file system refuses leaves the previous image whole and reports false;
// an unchanged slot already on disk needs no write.
static void test_journal_file_write_is_atomic_and_reports() {
  FakeFileIo io;
  FileJournalStore st(io);
  st.load();
  Journal j;
  j.begin(&st);
  TEST_ASSERT_TRUE(j.write(rec("job0001", "o:1")));
  const std::string before = io.file;
  io.failWrite = true;
  TEST_ASSERT_FALSE(j.write(rec("job0002", "o:2")));
  TEST_ASSERT_FALSE(st.durable());
  TEST_ASSERT_EQUAL_STRING(before.c_str(), io.file.c_str());
  TEST_ASSERT_EQUAL_INT(2, j.count());   // RAM still polls both this boot
  io.failWrite = false;
  const int w = io.writes;
  TEST_ASSERT_TRUE(st.put(0, st.get(0)));   // the file already holds it
  TEST_ASSERT_EQUAL_INT(w, io.writes);
  // A reboot finds exactly what landed.
  FileJournalStore st2(io);
  st2.load();
  Journal j2;
  j2.begin(&st2);
  TEST_ASSERT_EQUAL_INT(1, j2.count());
  // A corrupt file reads as no file (the legacy copy, if any, is the fallback).
  FakeFileIo bad;
  bad.fileExists = true;
  bad.file = "{not json";
  FileJournalStore st3(bad);
  TEST_ASSERT_EQUAL_INT((int)JournalSource::None, (int)st3.load());
}

// L19-35: the last 8 finished jobs, newest first, across a restart.
static void test_finished_ring_keeps_the_last_eight_across_a_restart() {
  FakeFileIo io;
  FileJournalStore st(io);
  st.load();
  Journal j;
  j.begin(&st);
  char tag[16], name[16];
  for (int i = 0; i < 11; ++i) {
    std::snprintf(tag, sizeof(tag), "job%04d", i);
    std::snprintf(name, sizeof(name), "task-%d", i);
    TEST_ASSERT_TRUE(j.write(rec(tag, "o:x")));
    TEST_ASSERT_TRUE(j.finish(tag, fin(name, "prj", "prj/out.md")));
    j.gc();
  }
  TEST_ASSERT_EQUAL_INT(kJournalFinishedKeep, j.finishedCount());
  FinishedJob f{};
  bool before = true;
  TEST_ASSERT_TRUE(j.finished(0, f, &before));
  TEST_ASSERT_EQUAL_STRING("job0010", f.tag);
  TEST_ASSERT_FALSE(before);   // this boot's
  TEST_ASSERT_TRUE(j.finished(7, f));
  TEST_ASSERT_EQUAL_STRING("job0003", f.tag);
  TEST_ASSERT_FALSE(j.finished(8, f));
  TEST_ASSERT_EQUAL_INT(0, j.count());
  // Restart: the ring comes back in the same order, every entry from before the boot.
  FileJournalStore st2(io);
  st2.load();
  Journal j2;
  j2.begin(&st2);
  TEST_ASSERT_EQUAL_INT(kJournalFinishedKeep, j2.finishedCount());
  for (int i = 0; i < kJournalFinishedKeep; ++i) {
    TEST_ASSERT_TRUE(j2.finished(i, f, &before));
    std::snprintf(tag, sizeof(tag), "job%04d", 10 - i);
    TEST_ASSERT_EQUAL_STRING(tag, f.tag);
    TEST_ASSERT_TRUE(before);
    TEST_ASSERT_EQUAL_STRING("prj/out.md", f.artifact);
    TEST_ASSERT_EQUAL_STRING("2026-10-22 Thu 21:14 BST", f.finishedAt);
    TEST_ASSERT_EQUAL_UINT32(1792700000u, f.finishedEpoch);
  }
  TEST_ASSERT_EQUAL(10, j2.highestTagNumber());
  // A new finish after the restart is the newest and pushes out the oldest.
  TEST_ASSERT_TRUE(j2.write(rec("job0011", "o:y")));
  TEST_ASSERT_TRUE(j2.finish("job0011", fin("late", "", "", /*failed=*/true)));
  TEST_ASSERT_TRUE(j2.finished(0, f, &before));
  TEST_ASSERT_EQUAL_STRING("job0011", f.tag);
  TEST_ASSERT_FALSE(before);
  TEST_ASSERT_TRUE(f.failed);
  TEST_ASSERT_TRUE(j2.finished(7, f));
  TEST_ASSERT_EQUAL_STRING("job0004", f.tag);
  // The record shape round-trips, and refuses an over-buffer serialize like a record.
  // Every field at its maximum still fits the record buffer.
  FinishedJob big = fin("n", "p", "a", true);
  std::memset(big.tag, 't', sizeof(big.tag) - 1);
  std::memset(big.name, 'n', sizeof(big.name) - 1);
  std::memset(big.prj, 'p', sizeof(big.prj) - 1);
  std::memset(big.artifact, 'x', sizeof(big.artifact) - 1);
  std::memset(big.finishedAt, 'a', sizeof(big.finishedAt) - 1);
  std::memset(big.chatId, 'c', sizeof(big.chatId) - 1);
  big.seq = 0xFFFFFFFFu;
  big.finishedEpoch = 0xFFFFFFFFu;
  char buf[kJournalSerializeBuf];
  TEST_ASSERT_TRUE(serializeFinished(big, buf, sizeof(buf)) + 1 <= sizeof(buf));
  FinishedJob back{};
  TEST_ASSERT_TRUE(deserializeFinished(buf, back));
  TEST_ASSERT_EQUAL_STRING(big.artifact, back.artifact);
  char tiny[16];
  TEST_ASSERT_TRUE(serializeFinished(big, tiny, sizeof(tiny)) + 1 > sizeof(tiny));
  TEST_ASSERT_EQUAL_CHAR(0, tiny[0]);
}

// Delivered once: finish() stores the ring entry before the seen mark, so over every
// failure pattern a restart never brings back as active a job whose finished record
// landed, and a job whose seen mark landed is never active again either.
static void test_a_finished_job_is_never_active_after_any_restart() {
  for (uint32_t seed = 1; seed <= 300; ++seed) {
    FlakyStore st;
    st.rng = seed;
    st.failPutPct = (int)(seed % 4) * 25;   // 0, 25, 50, 75 %
    Journal j;
    j.begin(&st);
    char tag[16];
    for (int i = 0; i < 12; ++i) {
      std::snprintf(tag, sizeof(tag), "job%04d", i);
      if (!j.write(rec(tag, "o:x"))) continue;   // RAM-only: a restart loses it (said elsewhere)
      j.finish(tag, fin("t", "", ""));
      if (i % 3 == 2) j.gc();
      // Restart now: no finished job comes back active.
      Journal r;
      r.begin(&st);
      for (int k = 0; k < r.count(); ++k) {
        JobRecord a{};
        TEST_ASSERT_TRUE(r.get(k, a));
        FinishedJob f{};
        for (int n = 0; r.finished(n, f); ++n) {
          char msg[64];
          std::snprintf(msg, sizeof(msg), "seed %u: %s active after a restart", (unsigned)seed, a.tag);
          TEST_ASSERT_TRUE_MESSAGE(std::strcmp(a.tag, f.tag) != 0, msg);
        }
      }
    }
  }
}

// The counter-test: without the finished entry, a job whose seen mark did not land
// IS active again after a restart (what the ring prevents).
static void test_without_the_finished_entry_a_restart_resurrects_the_job() {
  FlakyStore st;
  Journal j;
  j.begin(&st);
  TEST_ASSERT_TRUE(j.write(rec("job0005", "openai:resp_5")));
  st.failPutPct = 100;
  TEST_ASSERT_FALSE(j.finish("job0005", fin("t", "", "")));   // neither landed
  Journal r;
  r.begin(&st);
  TEST_ASSERT_EQUAL_INT(1, r.count());          // resurrected: polled and delivered again
  // With only the ring entry landed (the seen mark refused), it loads as delivered.
  FlakyStore st2;
  Journal k;
  k.begin(&st2);
  TEST_ASSERT_TRUE(k.write(rec("job0006", "openai:resp_6")));
  struct OnlyRing : JournalStore {
    FlakyStore& inner;
    explicit OnlyRing(FlakyStore& s) : inner(s) {}
    std::string get(int s) override { return inner.get(s); }
    bool put(int s, const std::string& v) override {
      return s >= kJournalFinishedSlot0 ? inner.put(s, v) : false;
    }
    bool remove(int s) override { return inner.remove(s); }
    void clearNs() override { inner.clearNs(); }
  } ring(st2);
  Journal k2;
  k2.begin(&ring);
  TEST_ASSERT_FALSE(k2.finish("job0006", fin("t", "", "")));   // the seen mark was refused
  Journal r2;
  r2.begin(&st2);
  TEST_ASSERT_EQUAL_INT(0, r2.count());   // finished before the restart: not active
  TEST_ASSERT_EQUAL_INT(1, r2.finishedCount());
}

// A finish with its terminal state is two writes (the ring entry, then the record with
// its state and seen mark together), and the record reloads terminal and seen.
static void test_finish_stores_the_terminal_state_with_the_seen_mark() {
  struct Counting : FakeStore {
    int puts = 0;
    bool put(int slot, const std::string& v) override { ++puts; return FakeStore::put(slot, v); }
  } st;
  Journal j;
  j.begin(&st);
  TEST_ASSERT_TRUE(j.write(rec("job0001", "o:1")));
  st.puts = 0;
  TEST_ASSERT_TRUE(j.finish("job0001", fin("t", "", "", true), JobState::Error));
  TEST_ASSERT_EQUAL_INT(2, st.puts);
  JobRecord back{};
  TEST_ASSERT_TRUE(deserializeRecord(st.slots[0].c_str(), back));
  TEST_ASSERT_EQUAL_INT((int)JobState::Error, (int)back.state);
  TEST_ASSERT_TRUE(back.resultSeen);
  // Without a state the record keeps its own.
  TEST_ASSERT_TRUE(j.write(rec("job0002", "o:2", JobState::Running)));
  TEST_ASSERT_TRUE(j.finish("job0002", fin("t", "", "")));
  TEST_ASSERT_TRUE(deserializeRecord(st.slots[1].c_str(), back));
  TEST_ASSERT_EQUAL_INT((int)JobState::Running, (int)back.state);
  TEST_ASSERT_TRUE(back.resultSeen);
}

// restored() is true only for records the boot loaded; highestTagNumber covers active
// and finished tags (a new boot's tags start past both).
static void test_restored_and_highest_tag() {
  FakeStore st;
  Journal j;
  j.begin(&st);
  TEST_ASSERT_EQUAL(-1, j.highestTagNumber());
  TEST_ASSERT_TRUE(j.write(rec("job0012", "mistral:local_1")));
  TEST_ASSERT_FALSE(j.restored("job0012"));
  TEST_ASSERT_TRUE(j.write(rec("job0002", "o:2")));
  TEST_ASSERT_TRUE(j.finish("job0002", fin("t", "", "")));
  Journal r;
  r.begin(&st);
  TEST_ASSERT_TRUE(r.restored("job0012"));
  TEST_ASSERT_FALSE(r.restored("nope"));
  TEST_ASSERT_EQUAL(12, r.highestTagNumber());
  TEST_ASSERT_TRUE(r.write(rec("job0013", "o:13")));
  TEST_ASSERT_FALSE(r.restored("job0013"));
  JobRecord odd = rec("custom-tag", "o:z");
  TEST_ASSERT_TRUE(r.write(odd));
  TEST_ASSERT_EQUAL(13, r.highestTagNumber());   // a tag of another shape is ignored
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
  RUN_TEST(test_journal_file_migration_legs);
  RUN_TEST(test_journal_file_write_is_atomic_and_reports);
  RUN_TEST(test_journal_file_merges_records_an_earlier_release_left);
  RUN_TEST(test_finished_ring_keeps_the_last_eight_across_a_restart);
  RUN_TEST(test_a_finished_job_is_never_active_after_any_restart);
  RUN_TEST(test_without_the_finished_entry_a_restart_resurrects_the_job);
  RUN_TEST(test_restored_and_highest_tag);
  RUN_TEST(test_finish_stores_the_terminal_state_with_the_seen_mark);
  return UNITY_END();
}
