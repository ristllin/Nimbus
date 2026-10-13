// Host tests for the factory-reset tail and the internal file area erase plan
// (lib/core nimbus/orch/reset_plan.h, lane L31).
//
// Bench 2026-10-13: after a FULL factory reset a raw read of the internal file area
// (the LittleFS partition) still found the previous owner's routine text, because a
// delete only unlinks a file and LittleFS's format rewrites only its superblock. The
// FULL reset now erases every block of that partition. These pin the CLASS
// (AGENTS.md section 3):
//   - every reset step has a place in the FULL plan, exactly once (a new step with
//     no place fails here);
//   - the whole-partition erase runs BEFORE the settings are erased (an erase that
//     cannot finish never leaves a factory-fresh device over readable data), and
//     only a step before the settings erase may stop the tail;
//   - the card's final sweep is the last thing before the restart;
//   - the settings-only reset is exactly what it was (no sweep, no erase);
//   - the erase chunks cover the partition exactly, with no gap and no overlap, for
//     every partition size a table can hold, and stay small enough to feed the
//     watchdog between them.

#include <unity.h>

#include <cstdint>
#include <cstring>
#include <set>
#include <string>

#include "nimbus/orch/reset_plan.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

static int indexOf(const ResetPlan& p, ResetStep s) {
  for (size_t i = 0; i < p.count; ++i)
    if (p.steps[i] == s) return (int)i;
  return -1;
}

static void test_full_plan_has_every_step_once() {
  const ResetPlan full = factoryResetTail(true);
  for (int s = 0; s < (int)ResetStep::Count; ++s) {
    int seen = 0;
    for (size_t i = 0; i < full.count; ++i) seen += (int)full.steps[i] == s;
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, seen, resetStepStr((ResetStep)s));
  }
  TEST_ASSERT_EQUAL_size_t((size_t)ResetStep::Count, full.count);
}

static void test_full_plan_order_contract() {
  const ResetPlan full = factoryResetTail(true);
  const int scrub = indexOf(full, ResetStep::ScrubInternalFs);
  const int nvs = indexOf(full, ResetStep::ResetNvs);
  const int sweep = indexOf(full, ResetStep::FinalSweep);
  const int restart = indexOf(full, ResetStep::Restart);
  TEST_ASSERT_TRUE(scrub >= 0);
  // The internal file area is erased while the device still has its settings.
  TEST_ASSERT_TRUE(scrub < nvs);
  // Everything else runs before the card's final sweep, which runs right before
  // the restart, and the restart ends the plan.
  for (ResetStep before : {ResetStep::ScrubInternalFs, ResetStep::ResetNvs, ResetStep::EraseCoreDump})
    TEST_ASSERT_TRUE_MESSAGE(indexOf(full, before) < sweep, resetStepStr(before));
  TEST_ASSERT_EQUAL_INT(sweep + 1, restart);
  TEST_ASSERT_EQUAL_INT((int)full.count - 1, restart);
}

// Class rule over every step and both plans: a step whose failure stops the tail
// runs before the settings erase, so a stop always leaves a device that still
// reads as configured (never factory-fresh over data that survived).
static void test_only_steps_before_the_settings_erase_may_stop() {
  int stoppers = 0;
  for (bool eraseData : {true, false}) {
    const ResetPlan p = factoryResetTail(eraseData);
    const int nvs = indexOf(p, ResetStep::ResetNvs);
    TEST_ASSERT_TRUE(nvs >= 0);
    for (size_t i = 0; i < p.count; ++i) {
      if (!resetStepMayStop(p.steps[i])) continue;
      ++stoppers;
      TEST_ASSERT_TRUE_MESSAGE((int)i < nvs, resetStepStr(p.steps[i]));
    }
  }
  TEST_ASSERT_EQUAL_INT(1, stoppers);   // the internal file area erase, in the FULL plan
  TEST_ASSERT_TRUE(resetStepMayStop(ResetStep::ScrubInternalFs));
  for (ResetStep s : {ResetStep::ResetNvs, ResetStep::EraseCoreDump, ResetStep::FinalSweep,
                      ResetStep::Restart})
    TEST_ASSERT_FALSE_MESSAGE(resetStepMayStop(s), resetStepStr(s));
}

static void test_settings_reset_is_unchanged() {
  const ResetPlan settings = factoryResetTail(false);
  const ResetStep want[] = {ResetStep::ResetNvs, ResetStep::EraseCoreDump, ResetStep::Restart};
  TEST_ASSERT_EQUAL_size_t(3, settings.count);
  for (size_t i = 0; i < settings.count; ++i)
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)want[i], (int)settings.steps[i],
                                  resetStepStr(settings.steps[i]));
  TEST_ASSERT_EQUAL_INT(-1, indexOf(settings, ResetStep::FinalSweep));
  TEST_ASSERT_EQUAL_INT(-1, indexOf(settings, ResetStep::ScrubInternalFs));
}

static void test_step_names_distinct() {
  std::set<std::string> names;
  for (int s = 0; s < (int)ResetStep::Count; ++s) {
    const char* n = resetStepStr((ResetStep)s);
    TEST_ASSERT_NOT_EQUAL(0, std::strcmp(n, "?"));
    names.insert(n);
  }
  TEST_ASSERT_EQUAL_size_t((size_t)ResetStep::Count, names.size());
  TEST_ASSERT_EQUAL_STRING("?", resetStepStr(ResetStep::Count));
}

// One partition size: the chunks run 0..size with no gap or overlap, each within
// the cap and sector-aligned, and their count matches scrubChunkCount.
static void checkCoverage(uint32_t partSize) {
  uint32_t next = 0, off = 0, len = 0, n = 0;
  while (scrubChunk(partSize, n, off, len)) {
    TEST_ASSERT_EQUAL_UINT32(next, off);
    TEST_ASSERT_TRUE(len > 0 && len <= kScrubChunkBytes);
    TEST_ASSERT_EQUAL_UINT32(0, off % kFlashSectorBytes);
    TEST_ASSERT_EQUAL_UINT32(0, len % kFlashSectorBytes);
    next = off + len;
    ++n;
  }
  TEST_ASSERT_EQUAL_UINT32(partSize, next);
  TEST_ASSERT_EQUAL_UINT32(scrubChunkCount(partSize), n);
}

static void test_erase_covers_every_partition_size() {
  // Every whole number of sectors up to 2 MB, plus the shipped partition.
  for (uint32_t size = 0; size <= 2u * 1024u * 1024u; size += kFlashSectorBytes)
    checkCoverage(size);
  checkCoverage(0x360000);
}

static void test_shipped_partition() {
  // default_16MB.csv: spiffs at 0xc90000, 0x360000 bytes (the bench raw-read range).
  TEST_ASSERT_EQUAL_UINT32(54, scrubChunkCount(0x360000));
  uint32_t off = 0, len = 0;
  TEST_ASSERT_TRUE(scrubChunk(0x360000, 53, off, len));
  TEST_ASSERT_EQUAL_UINT32(0x350000, off);
  TEST_ASSERT_EQUAL_UINT32(0x10000, len);
  TEST_ASSERT_FALSE(scrubChunk(0x360000, 54, off, len));
  TEST_ASSERT_EQUAL_UINT32(0, scrubChunkCount(0));
  TEST_ASSERT_FALSE(scrubChunk(0, 0, off, len));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_full_plan_has_every_step_once);
  RUN_TEST(test_full_plan_order_contract);
  RUN_TEST(test_only_steps_before_the_settings_erase_may_stop);
  RUN_TEST(test_settings_reset_is_unchanged);
  RUN_TEST(test_step_names_distinct);
  RUN_TEST(test_erase_covers_every_partition_size);
  RUN_TEST(test_shipped_partition);
  return UNITY_END();
}
