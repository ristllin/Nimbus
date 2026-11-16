#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <memory>
#include <vector>

#include "nimbus/orch/compact.h"
#include "nimbus/orch/food_ledger.h"   // lane CTX: the fold's day
#include "nimbus/orch/wire_limits.h"   // kJsonStrGuard

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// ---- threshold math ----------------------------------------------------------
static void test_threshold_window_math() {
  // Claude Code's observed shape: 200K - 4K reserved - 13K buffer = 183K;
  // owner cap 60K wins when smaller.
  TEST_ASSERT_EQUAL_UINT32(183000, compactThreshold(200000, 4000, 0));
  TEST_ASSERT_EQUAL_UINT32(60000, compactThreshold(200000, 4000, 60000));
}

static void test_threshold_floors() {
  // A tiny window can't drive the threshold to zero (compact-every-turn).
  TEST_ASSERT_EQUAL_UINT32(kCompactMinThreshold, compactThreshold(10000, 4000, 0));
  // ctx==0 (misconfig) falls back to the default window, not UB.
  TEST_ASSERT_EQUAL_UINT32(100000 - 4000 - kCompactBufferTokens,
                           compactThreshold(0, 4000, 0));
  // Owner cap below the floor is floored too.
  TEST_ASSERT_EQUAL_UINT32(kCompactMinThreshold, compactThreshold(200000, 4000, 10));
}

static void test_should_compact() {
  TEST_ASSERT_FALSE(shouldCompact(59999, 60000));
  TEST_ASSERT_TRUE(shouldCompact(60000, 60000));
  TEST_ASSERT_FALSE(shouldCompact(1000000, 0));  // threshold 0 = disabled
}

// ---- reactive error classification -------------------------------------------
static void test_overflow_error_classifier() {
  TEST_ASSERT_TRUE(isContextOverflowError("resp HTTP 400: context_length_exceeded"));
  TEST_ASSERT_TRUE(isContextOverflowError(
      "messages HTTP 400: prompt is too long: 214839 tokens > 200000 maximum"));
  TEST_ASSERT_TRUE(isContextOverflowError("This model's maximum context length is 272000 tokens"));
  TEST_ASSERT_TRUE(isContextOverflowError("Requested tokens exceeds the maximum allowed"));
  // Ordinary failures must NOT trigger a force-compact.
  TEST_ASSERT_FALSE(isContextOverflowError("connect failed"));
  TEST_ASSERT_FALSE(isContextOverflowError("resp HTTP 401: invalid api key"));
  TEST_ASSERT_FALSE(isContextOverflowError("tool budget reached without a final answer"));
}

// ---- fold prompt assembly ----------------------------------------------------
static void test_build_inputs_fresh_vs_anchored() {
  std::string fresh = buildCompactInputs("", "- user: hi\n");
  TEST_ASSERT_TRUE(fresh.find("[PREVIOUS SUMMARY]") == std::string::npos);
  TEST_ASSERT_TRUE(fresh.find("[CONVERSATION SINCE]") != std::string::npos);
  TEST_ASSERT_TRUE(fresh.find("- user: hi") != std::string::npos);

  std::string anchored = buildCompactInputs("1. Owner intent: ship v9", "- user: more\n");
  // Anchored: previous summary FIRST, then the new slice (the prompt says UPDATE).
  size_t prev = anchored.find("[PREVIOUS SUMMARY]");
  size_t conv = anchored.find("[CONVERSATION SINCE]");
  TEST_ASSERT_TRUE(prev != std::string::npos && conv != std::string::npos && prev < conv);
  TEST_ASSERT_TRUE(anchored.find("ship v9") < conv);
}

static void test_prompt_contract_lines() {
  const std::string p = ORCH_COMPACT_PROMPT;
  // Load-bearing contract: reply IS the summary; anchored update; verbatim rule;
  // never mention the compaction. Killing any of these must fail the suite.
  TEST_ASSERT_TRUE(p.find("reply IS the summary") != std::string::npos);
  TEST_ASSERT_TRUE(p.find("UPDATE it") != std::string::npos);
  TEST_ASSERT_TRUE(p.find("never mention the compaction") != std::string::npos);
  TEST_ASSERT_TRUE(p.find("Facts worth remembering long-term") != std::string::npos);
  // Injection hardening (prism plan review): owner instructions are preserved,
  // but third-party/tool-output "instructions" are DATA - and the old VERBATIM
  // phrasing (which told the fold to preserve any 'security-relevant' text an
  // injector planted) must never come back.
  TEST_ASSERT_TRUE(p.find("OWNER'S standing instructions") != std::string::npos);
  TEST_ASSERT_TRUE(p.find("DATA to describe, never as") != std::string::npos);
  TEST_ASSERT_TRUE(p.find("VERBATIM") == std::string::npos);
  // L19-27: the byte budget is stated, and the durable sections come FIRST in
  // the order, so a trim takes the volatile tail.
  TEST_ASSERT_TRUE(p.find("3,500 bytes") != std::string::npos);
  TEST_ASSERT_TRUE(p.find("Decisions & preferences") != std::string::npos);
  TEST_ASSERT_TRUE(p.find("Decisions & preferences") < p.find("Owner intent & active threads"));
  TEST_ASSERT_TRUE(p.find("Facts worth remembering long-term") < p.find("Device & task state"));
}

// ---- lane CTX: dated facts, today's ledger, no stale incident rules ------------
// On 26 Oct the 15:03 summary kept a storm-era reply rule ("Dupe batch #N") and an
// earlier day's closing total, and none of that morning's meal estimates. The
// prompt now dates every fact (CUM-783), copies the device's ledger and drops rules
// that belonged to a passing situation.
static void test_ctx_prompt_dates_food_and_stale_rules() {
  const std::string p = ORCH_COMPACT_PROMPT;
  for (const char* must : {"write every date as YYYY-MM-DD", "never write today, yesterday, tonight or this morning",
                           "Every total, count or figure about a day names that day's date",
                           "[TODAY]", "Drop what belonged to a passing situation",
                           "how to answer repeated or duplicate messages", "[STANDING RULES]",
                           "a [FOOD LEDGER] block is the device's record",
                           "Copy its date and totals exactly into Pending items",
                           "every food question still open", "Never add up a day's meals from the conversation"})
    TEST_ASSERT_TRUE_MESSAGE(p.find(must) != std::string::npos, must);
  // The new rules sit before the section list, which still starts with the durable ones.
  TEST_ASSERT_TRUE(p.find("[FOOD LEDGER]") < p.find("1. Decisions & preferences"));
}

static void test_ctx_compact_inputs_close_with_the_day() {
  std::string file = "date,meal,items,kcal,protein_g\n";
  file += "2026-10-26,breakfast,oats + skyr,450,30\n2026-10-26,lunch,brunch plate,865,70\n";
  const LogDay day{"2026-10-26", "GMT"};
  const std::string ctx = "[TODAY] 2026-10-26 (GMT)\n" +
                          renderLedgerForFold(scanFoodLedger(file, true, day, kFoodLedgerProject, kFoodLedgerOwnerName));
  const std::string in = buildCompactInputs("1. Decisions & preferences\n- x\n", "- user: hi\n", ctx);
  TEST_ASSERT_TRUE(in.find("[CONVERSATION SINCE]") < in.find("[TODAY] 2026-10-26 (GMT)"));
  TEST_ASSERT_TRUE(in.find("Totals for 2026-10-26: 1315 kcal from 2 rows, 100 g protein.") != std::string::npos);
  TEST_ASSERT_TRUE(in.find("brunch plate") != std::string::npos);
  // No day: byte-identical to before.
  TEST_ASSERT_EQUAL_STRING(buildCompactInputs("s", "d").c_str(), buildCompactInputs("s", "d", "").c_str());
  // Near the one-string guard: whole, then the essentials ([TODAY] and the totals),
  // then nothing; never over the guard.
  const size_t head = std::string("[CONVERSATION SINCE]\n").size() + 2;
  size_t essentials = 0;
  for (size_t p = 0; p < ctx.size();) {
    const size_t nl = ctx.find('\n', p);
    const std::string line = ctx.substr(p, nl - p + 1);
    if (line.rfind("[TODAY]", 0) == 0 || line.rfind("Totals for ", 0) == 0) essentials += line.size();
    p = nl + 1;
  }
  const size_t whole = kJsonStrGuard - head - ctx.size();
  const std::string a = buildCompactInputs("", std::string(whole, 'd'), ctx);
  TEST_ASSERT_TRUE(a.size() <= kJsonStrGuard && a.find("brunch plate") != std::string::npos);
  const std::string b = buildCompactInputs("", std::string(whole + 1, 'd'), ctx);
  TEST_ASSERT_TRUE(b.size() <= kJsonStrGuard);
  TEST_ASSERT_TRUE(b.find("brunch plate") == std::string::npos);
  TEST_ASSERT_TRUE(b.find("[TODAY] 2026-10-26") != std::string::npos);
  TEST_ASSERT_TRUE(b.find("Totals for 2026-10-26") != std::string::npos);
  const std::string c = buildCompactInputs("", std::string(kJsonStrGuard - head - essentials + 1, 'd'), ctx);
  TEST_ASSERT_TRUE(c.size() <= kJsonStrGuard && c.find("[TODAY]") == std::string::npos);
}

// ---- L19-27: section-aware cap ------------------------------------------------
// A valid UTF-8 check: no continuation byte (0b10xxxxxx) starts a sequence.
static bool utf8Whole(const std::string& s) {
  for (size_t i = 0; i < s.size(); i++) {
    const unsigned char c = (unsigned char)s[i];
    if ((c & 0xC0) == 0x80) {
      if (i == 0) return false;
      const unsigned char prev = (unsigned char)s[i - 1];
      if ((prev & 0xC0) != 0xC0 && (prev & 0xC0) != 0x80) return false;
    }
  }
  return true;
}

// One generated summary: the five sections in the prompt's order, random line
// counts, sizes 1-20 KB, multibyte text sprinkled through the bodies.
static std::string genSummary(uint32_t& seed, size_t targetBytes) {
  auto rnd = [&seed](uint32_t m) { seed = seed * 1664525u + 1013904223u; return (seed >> 8) % m; };
  static const char* const kHeads[] = {
      "1. Decisions & preferences", "2. Pending items & next steps",
      "3. Facts worth remembering long-term", "4. Owner intent & active threads",
      "5. Device & task state"};
  std::string out;
  for (const char* h : kHeads) {
    if (out.size() >= targetBytes) break;
    out += std::string(h) + "\n";
    for (int l = 0, n = (int)rnd(9) + 1; l < n; l++) {
      out += std::string(rnd(60) + 10, 'a' + (char)rnd(26));
      if (rnd(3) == 0) out += " caf\xC3\xA9 \xE2\x82\xAC";   // multibyte
      out += "\n";
    }
  }
  while (out.size() < targetBytes) out += "filler line\n";
  return out;
}

// Lane F: the kept text holds EVERY section header, trims each body in proportion to
// its size (no section is emptied while another keeps most of its body), never splits
// UTF-8, never exceeds the cap (both head caps), and marks the cut once.
static void test_cap_keeps_every_header_and_trims_proportionally() {
  uint32_t seed = 12345;
  static const char* const kHeads[] = {"1. Decisions & preferences", "2. Pending items & next steps",
                                       "3. Facts worth remembering long-term", "4. Owner intent & active threads",
                                       "5. Device & task state"};
  for (size_t cap : {kChatSummarySmall, kChatSummaryMax}) {
    for (int i = 0; i < 400; i++) {
      const size_t target = 1000 + (size_t)(i * 47 % 19000);
      const std::string s = genSummary(seed, target);
      const std::string capped = capSummary(s, cap);
      TEST_ASSERT_TRUE_MESSAGE(capped.size() <= cap, "over the cap");
      TEST_ASSERT_TRUE_MESSAGE(utf8Whole(capped), "a torn UTF-8 sequence");
      for (const char* h : kHeads)
        if (s.find(h) != std::string::npos)
          TEST_ASSERT_TRUE_MESSAGE(capped.find(h) != std::string::npos, h);
      TEST_ASSERT_TRUE_MESSAGE(capped.find("\xE2\x80\xA6\xE2\x80\xA6") == std::string::npos, "two markers");
      if (capped.size() < s.size())
        TEST_ASSERT_TRUE_MESSAGE(capped.find("\xE2\x80\xA6") != std::string::npos, "an invisible cut");
      if (s.size() <= cap) TEST_ASSERT_TRUE_MESSAGE(capped == s, "an under-cap summary was changed");
      TEST_ASSERT_TRUE_MESSAGE(capSummary(capped, cap) == capped, "a capped summary was cut again");
    }
  }
  // Proportional: five sections of 2,000 B each over a 6,144 B cap all keep a body,
  // each within one line of an equal share.
  std::string even;
  for (const char* h : kHeads) {
    even += std::string(h) + "\n";
    for (int l = 0; l < 40; l++) even += "- line " + std::to_string(l) + " of fixed width text here\n";
  }
  const std::string cut = capSummary(even, kChatSummaryMax);
  size_t prev = std::string::npos;
  std::vector<size_t> bodies;
  for (const char* h : kHeads) {
    const size_t at = cut.find(h);
    TEST_ASSERT_TRUE(at != std::string::npos);
    if (prev != std::string::npos) bodies.push_back(at - prev);
    prev = at;
  }
  bodies.push_back(cut.size() - prev);
  for (size_t b : bodies) {
    TEST_ASSERT_TRUE_MESSAGE(b > 900, "a section lost most of its body");
    TEST_ASSERT_TRUE_MESSAGE(b < 1400, "a section kept more than its share");
  }
}

// The counter-test: a 3 KB summary is kept whole, byte for byte.
static void test_l1931_a_3kb_summary_is_kept_whole() {
  uint32_t seed = 777;
  const std::string s = genSummary(seed, 3000);
  TEST_ASSERT_TRUE(s.size() <= kChatSummaryMax);
  TEST_ASSERT_EQUAL_STRING(s.c_str(), capSummary(s).c_str());
}

// The replay fixture: a fold's capped output feeds the next anchored fold, whose
// model output carries the cut summary and one new line. The cap keeps every header,
// cuts whole lines (the earlier cut line is never torn) and is idempotent.
static void test_l1931_two_successive_folds_do_not_recut_a_cut_line() {
  std::string big = "1. Decisions & preferences\n";
  for (int i = 0; i < 200; i++) big += "- decision " + std::to_string(i) + ": keep the label figures\n";
  big += "4. Owner intent & active threads\n" + std::string(200, 'z') + "\n";
  const std::string cut = capSummary(big);
  TEST_ASSERT_TRUE(cut.size() <= kChatSummaryMax);
  TEST_ASSERT_TRUE(cut.size() > kChatSummaryMax - 200);
  TEST_ASSERT_TRUE(cut.rfind("\xE2\x80\xA6") == cut.size() - 3);   // the visible cut
  TEST_ASSERT_TRUE(cut.find("4. Owner intent & active threads") != std::string::npos);
  const std::string next = cut + "\n- one more device note: the hallway light is on a timer, the spare key is with the neighbor\n";
  TEST_ASSERT_TRUE(next.size() > kChatSummaryMax);
  const std::string again = capSummary(next);
  TEST_ASSERT_TRUE(again.size() <= kChatSummaryMax);
  TEST_ASSERT_EQUAL_STRING(again.c_str(), capSummary(again).c_str());
  // Every kept decision line is whole: a line is dropped or kept, never torn.
  for (size_t p = 0; (p = again.find("- decision ", p)) != std::string::npos; p++) {
    const size_t eol = again.find('\n', p);
    const std::string line = again.substr(p, eol == std::string::npos ? std::string::npos : eol - p);
    TEST_ASSERT_TRUE_MESSAGE(line.find(": keep the label figures") != std::string::npos, line.c_str());
  }
  // The head's cap: under 60k tokens 4,096 B, else 6,144 B.
  TEST_ASSERT_EQUAL_UINT32(kChatSummarySmall, summaryCapFor(32000));
  TEST_ASSERT_EQUAL_UINT32(kChatSummarySmall, summaryCapFor(kSummaryLargeHeadTokens - 1));
  TEST_ASSERT_EQUAL_UINT32(kChatSummaryMax, summaryCapFor(kSummaryLargeHeadTokens));
  TEST_ASSERT_EQUAL_UINT32(kChatSummaryMax, summaryCapFor(1048576));
  TEST_ASSERT_TRUE(capSummary(big, kChatSummarySmall).size() <= kChatSummarySmall);
}

// A head with the larger cap is told its budget; a 4,096 B head's input is unchanged.
static void test_compact_inputs_name_the_larger_budget() {
  TEST_ASSERT_EQUAL_STRING(buildCompactInputs("s", "d").c_str(), buildCompactInputs("s", "d", "", 4096).c_str());
  TEST_ASSERT_EQUAL_STRING(buildCompactInputs("s", "d").c_str(), buildCompactInputs("s", "d", "", 0).c_str());
  const std::string in = buildCompactInputs("s", "d", "", kChatSummaryMax);
  TEST_ASSERT_TRUE(in.rfind("[SUMMARY BUDGET] ", 0) == 0);
  TEST_ASSERT_TRUE(in.find("5,200 bytes") != std::string::npos);
  TEST_ASSERT_TRUE(in.find("[PREVIOUS SUMMARY]") != std::string::npos);
  const std::string p = ORCH_COMPACT_PROMPT;
  TEST_ASSERT_TRUE(p.find("[SUMMARY BUDGET]") != std::string::npos);
  // Never over the one-string guard: at the guard the budget line yields.
  const std::string full = buildCompactInputs("", std::string(kJsonStrGuard - 40, 'd'), "", kChatSummaryMax);
  TEST_ASSERT_TRUE(full.size() <= kJsonStrGuard);
}

// ---- summary cap -------------------------------------------------------------
static void test_cap_summary_utf8_safe() {
  std::string small = "ok";
  TEST_ASSERT_EQUAL_STRING("ok", capSummary(small).c_str());

  // Build a string whose kChatSummaryMax-3 boundary lands INSIDE a 3-byte
  // codepoint ("€" = E2 82 AC): pad so the cut would tear it, and assert the cap
  // backs off to the previous boundary instead.
  std::string s;
  while (s.size() < kChatSummaryMax - 4) s += 'a';
  s += "\xE2\x82\xAC";               // straddles the cap boundary
  while (s.size() <= kChatSummaryMax) s += 'b';
  std::string capped = capSummary(s);
  TEST_ASSERT_TRUE(capped.size() <= kChatSummaryMax);
  // No torn UTF-8: the last byte before the appended ellipsis must not be a
  // lone lead byte (0xE2) - i.e. the euro sign was dropped whole.
  const std::string ell = "\xE2\x80\xA6";
  TEST_ASSERT_TRUE(capped.size() >= ell.size() &&
                   capped.compare(capped.size() - ell.size(), ell.size(), ell) == 0);
  unsigned char beforeEll = (unsigned char)capped[capped.size() - ell.size() - 1];
  TEST_ASSERT_TRUE(beforeEll == 'a' || beforeEll == 'b');
}

// ---- FoldStore ---------------------------------------------------------------
struct FakeIO : FoldStoreIO {
  std::string blob;
  int saves = 0;
  std::string load() override { return blob; }
  void save(const std::string& b) override { blob = b; saves++; }
};

static void test_foldstore_accumulate_and_persist_cadence() {
  FakeIO io;
  FoldStore fs;
  fs.begin(&io);
  fs.noteMessage("tg1", 500);
  fs.noteMessage("tg1", 300);
  TEST_ASSERT_EQUAL_INT(0, io.saves);          // per-message = deferred, no flash write
  fs.noteTurn("tg1");
  TEST_ASSERT_EQUAL_INT(1, io.saves);          // one write per turn
  ChatFold f = fs.get("tg1");
  TEST_ASSERT_EQUAL_UINT32(800, f.bytesSinceFold);
  TEST_ASSERT_EQUAL_UINT32(2, f.msgsSinceFold);
  TEST_ASSERT_EQUAL_UINT32(1, f.turnsSinceFold);
}

static void test_foldstore_roundtrip_and_tolerant_load() {
  FakeIO io;
  {
    FoldStore fs;
    fs.begin(&io);
    fs.noteMessage("tg1", 1000);
    fs.applyFold("tg1", "1. Owner intent: test\ncat is Waffles", 1234567);
  }
  {
    FoldStore fs2;
    fs2.begin(&io);
    ChatFold f = fs2.get("tg1");
    TEST_ASSERT_EQUAL_STRING("1. Owner intent: test\ncat is Waffles", f.summary.c_str());
    TEST_ASSERT_EQUAL_UINT32(1234567, f.lastFoldEpoch);
    TEST_ASSERT_EQUAL_UINT32(0, f.bytesSinceFold);   // fold reset the counters
  }
  // Malformed record (garbage, no field separators) is dropped, never fatal;
  // valid records around it survive.
  io.blob = "garbage-no-separators\x1E" + io.blob;
  FoldStore fs3;
  fs3.begin(&io);
  TEST_ASSERT_EQUAL_UINT32(1234567, fs3.get("tg1").lastFoldEpoch);
}

static void test_foldstore_due_and_thresholds() {
  FakeIO io;
  FoldStore fs;
  fs.begin(&io);
  fs.noteMessage("c", 40000);
  fs.noteTurn("c");
  TEST_ASSERT_EQUAL_INT((int)FoldDue::No, (int)fs.evaluateDue("c", 48000, 200, 1000));
  fs.noteMessage("c", 9000);
  TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)fs.evaluateDue("c", 48000, 200, 1000));
  // Disabled (bytesThreshold 0) never fires.
  TEST_ASSERT_EQUAL_INT((int)FoldDue::No, (int)fs.evaluateDue("c", 0, 0, 1000));
  // Message-count threshold fires independently.
  FoldStore fs2; FakeIO io2; fs2.begin(&io2);
  for (int i = 0; i < 200; i++) fs2.noteMessage("m", 10);
  TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)fs2.evaluateDue("m", 48000, 200, 1000));
}

// Lane F (ruling 2): a refill right after a completed fold only schedules. The first
// evaluateDue is Yes and sets nextTry 15 min out; before then it is No; at nextTry it
// is Yes again. Never a pause, never a resume.
static void test_foldstore_thrash_only_schedules() {
  FakeIO io;
  FoldStore fs;
  fs.begin(&io);
  const uint32_t t0 = 1763200000;
  fs.noteMessage("c", 60000);
  fs.noteTurn("c");
  TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)fs.evaluateDue("c", 48000, 0, t0));
  fs.applyFold("c", "sum", t0);
  fs.noteMessage("c", 60000);
  fs.noteTurn("c");
  bool thrash = false;
  TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)fs.evaluateDue("c", 48000, 0, t0 + 60, &thrash));
  TEST_ASSERT_TRUE(thrash);
  TEST_ASSERT_EQUAL_UINT32(t0 + 60 + kFoldThrashRetryS, fs.get("c").nextTry);
  TEST_ASSERT_EQUAL_INT((int)FoldDue::No, (int)fs.evaluateDue("c", 48000, 0, t0 + 120));
  TEST_ASSERT_EQUAL_INT((int)FoldDue::No, (int)fs.evaluateDue("c", 48000, 0, t0 + 60 + kFoldThrashRetryS - 1));
  // The fold the Yes started succeeded: a thrash wait stays (the refill is not new).
  fs.applyFold("c", "sum 2", t0 + 61);
  fs.noteMessage("c", 60000);
  fs.noteTurn("c");
  TEST_ASSERT_EQUAL_INT((int)FoldDue::No, (int)fs.evaluateDue("c", 48000, 0, t0 + 300));
  TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)fs.evaluateDue("c", 48000, 0, t0 + 60 + kFoldThrashRetryS));
}

// The backoff schedule: 15 min, 1 h, 4 h, then every 12 h; a failed cycle sets nextTry
// from it and counts the failure; a success clears both; the digest halves per failed
// cycle down to 8 KB.
static void test_foldstore_backoff_schedule() {
  TEST_ASSERT_EQUAL_UINT32(0, foldBackoffS(0));
  TEST_ASSERT_EQUAL_UINT32(15u * 60u, foldBackoffS(1));
  TEST_ASSERT_EQUAL_UINT32(3600u, foldBackoffS(2));
  TEST_ASSERT_EQUAL_UINT32(4u * 3600u, foldBackoffS(3));
  for (uint32_t n = 4; n < 400; n++) TEST_ASSERT_EQUAL_UINT32(12u * 3600u, foldBackoffS(n));
  TEST_ASSERT_EQUAL_UINT32(53900, foldDigestAfterFailures(53900, 0));
  TEST_ASSERT_EQUAL_UINT32(26950, foldDigestAfterFailures(53900, 1));
  TEST_ASSERT_EQUAL_UINT32(13475, foldDigestAfterFailures(53900, 2));
  TEST_ASSERT_EQUAL_UINT32(kFoldDigestFloor, foldDigestAfterFailures(53900, 3));
  TEST_ASSERT_EQUAL_UINT32(kFoldDigestFloor, foldDigestAfterFailures(53900, 200));
  TEST_ASSERT_EQUAL_UINT32(4000, foldDigestAfterFailures(4000, 5));   // never above the step
  FakeIO io;
  FoldStore fs;
  fs.begin(&io);
  const uint32_t t0 = 1763200000;
  fs.setSoftStart("c", "m00000040");   // hidden rows: due
  TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)fs.evaluateDue("c", 48000, 200, t0));
  const uint32_t waits[] = {15u * 60u, 3600u, 4u * 3600u, 12u * 3600u, 12u * 3600u};
  uint32_t now = t0;
  for (uint32_t i = 0; i < 5; i++) {
    const uint32_t next = fs.noteCycleFailed("c", now);
    TEST_ASSERT_EQUAL_UINT32(now + waits[i], next);
    TEST_ASSERT_EQUAL_UINT32(i + 1, fs.get("c").failedCycles);
    TEST_ASSERT_EQUAL_UINT32(t0, fs.get("c").firstFail);   // the first failure's time
    TEST_ASSERT_EQUAL_INT((int)FoldDue::No, (int)fs.evaluateDue("c", 48000, 200, next - 1));
    TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)fs.evaluateDue("c", 48000, 200, next));
    now = next;
  }
  // No clock: the failure is counted, the wait is the caller's (nextTry stays 0).
  FoldStore nc;
  FakeIO io2;
  nc.begin(&io2);
  TEST_ASSERT_EQUAL_UINT32(0, nc.noteCycleFailed("d", 0));
  TEST_ASSERT_EQUAL_UINT32(1, nc.get("d").failedCycles);
  TEST_ASSERT_EQUAL_UINT32(0, nc.get("d").firstFail);
  // A success clears the count, the first failure and the backoff wait.
  fs.applyFold("c", "ok", now + 5, "m00000040");
  TEST_ASSERT_EQUAL_UINT32(0, fs.get("c").failedCycles);
  TEST_ASSERT_EQUAL_UINT32(0, fs.get("c").firstFail);
  TEST_ASSERT_EQUAL_UINT32(0, fs.get("c").nextTry);
  // A firmware change restarts every wait: due on the next pass.
  fs.setSoftStart("c", "m00000080");
  fs.noteCycleFailed("c", now + 10);
  TEST_ASSERT_EQUAL_INT((int)FoldDue::No, (int)fs.evaluateDue("c", 48000, 200, now + 11));
  fs.resetWaits();
  TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)fs.evaluateDue("c", 48000, 200, now + 11));
  TEST_ASSERT_EQUAL_UINT32(0, fs.get("c").failedCycles);
}

// A v5.1.1 record, the way FoldStore::encodeSlot wrote it (paused and fails set).
struct Legacy {
  std::string chat;
  uint32_t bytes, epoch;
  std::string thru;
  uint32_t turns;
  std::string soft;
  unsigned fails, paused;
  std::string summary;
};
static std::string legacyRecord(const Legacy& l) {
  std::string r = l.chat;
  r += '\x1F' + std::to_string(l.bytes) + '\x1F' + "3";
  r += '\x1F' + std::to_string(l.epoch) + (l.thru.empty() ? "" : ";" + l.thru);
  r += '\x1F' + std::to_string(l.turns) + (l.soft.empty() ? "" : ";" + l.soft);
  r += '\x1F' + std::to_string(l.fails) + '\x1F' + std::to_string(l.paused) + '\x1F' + l.summary + '\x1E';
  return r;
}

// Every legacy pause loads unpaused and due: paused=1 after three failures (Lumi, 2 Nov),
// paused=1 with no failure (a v4.6.0 thrash pause), paused=2 (a v4.6.1+ thrash pause), with
// hidden rows or a refill. No resume, no /compact, the first evaluateDue.
static void test_foldstore_legacy_pauses_lift_on_load() {
  const uint32_t now = 1763200000;
  struct Case { unsigned fails, paused; uint32_t bytes; const char* soft; };
  const Case cases[] = {{3, 1, 10, "m00001c96"}, {0, 1, 10, "m00001c96"}, {0, 2, 10, "m00001c96"},
                        {3, 1, 60000, ""},        {0, 2, 60000, ""},        {2, 0, 60000, ""}};
  for (const Case& c : cases) {
    FakeIO io;
    io.blob = legacyRecord({"owner", c.bytes, now - 13 * 86400, "m000017d9", 158, c.soft, c.fails, c.paused,
                            "1. Decisions & preferences\n- x\n"});
    FoldStore fs;
    fs.begin(&io);
    TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)fs.evaluateDue("owner", 48000, 200, now));
    TEST_ASSERT_EQUAL_UINT32(0, fs.get("owner").failedCycles);
    TEST_ASSERT_EQUAL_UINT32(0, fs.get("owner").nextTry);
  }
}

// The Lumi blob (lane E's fixture, the shape Lumi holds since 2 Nov): due on the first
// evaluateDue, with no resume and no clock advance.
static void test_foldstore_lumi_fixture_is_due_at_once() {
  std::ifstream f("test/support/fixtures/fold_blob_v5_1_1_lumi.txt", std::ios::binary);
  TEST_ASSERT_TRUE_MESSAGE(f.good(), "fold_blob_v5_1_1_lumi.txt");
  std::stringstream ss;
  ss << f.rdbuf();
  FakeIO io;
  io.blob = ss.str();
  FoldStore fs;
  fs.begin(&io);
  bool found = false;
  for (const std::string& chat : fs.chatIds()) {
    const ChatFold cf = fs.get(chat);
    if (cf.thru != "m000017d9") continue;
    found = true;
    TEST_ASSERT_EQUAL_STRING("m00001c96", cf.softStart.c_str());
    TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)fs.evaluateDue(chat, 48000, 200, 1763200000));
  }
  TEST_ASSERT_TRUE_MESSAGE(found, "the fixture holds Lumi's chat");
}

// The v5.1.1 parser (FoldStore::begin at 591245f4), for the rollback round trip.
struct LegacyChat { std::string chat, summary, thru, softStart; uint32_t epoch = 0, fails = 0; bool paused = false; };
static uint32_t legacyU32(const std::string& s) {
  uint32_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return v;
    v = v * 10u + (uint32_t)(c - '0');
  }
  return v;
}
static std::vector<LegacyChat> legacyParse(const std::string& raw) {
  std::vector<LegacyChat> out;
  size_t start = 0;
  while (start < raw.size()) {
    const size_t end = raw.find('\x1E', start);
    if (end == std::string::npos) break;
    const std::string rec = raw.substr(start, end - start);
    start = end + 1;
    std::vector<std::string> f;
    size_t p = 0;
    while (f.size() < 7) {
      const size_t q = rec.find('\x1F', p);
      if (q == std::string::npos) break;
      f.push_back(rec.substr(p, q - p));
      p = q + 1;
    }
    if (f.size() < 7 || f[0].empty()) continue;
    LegacyChat c;
    c.chat = f[0];
    c.epoch = legacyU32(f[3]);
    const size_t s3 = f[3].find(';');
    c.thru = s3 == std::string::npos ? "" : f[3].substr(s3 + 1);
    const size_t s4 = f[4].find(';');
    c.softStart = s4 == std::string::npos ? "" : f[4].substr(s4 + 1);
    c.fails = legacyU32(f[5]);
    c.paused = legacyU32(f[6]) != 0;
    c.summary = rec.substr(p);
    out.push_back(c);
  }
  return out;
}

// Rollback: a candidate blob with a failing chat (count, first failure, next try)
// reads on v5.1.1 with its summary, thru and softStart intact, not paused and with no
// failure counted; the candidate reads its own schedule back.
static void test_foldstore_rollback_round_trip() {
  FakeIO io;
  const uint32_t t0 = 1763200000;
  {
    FoldStore fs;
    fs.begin(&io);
    fs.applyFold("owner", "1. Decisions & preferences\n- kcal 1,900\n", t0, "m000017d9");
    fs.setSoftStart("owner", "m00001c96");
    fs.noteTurn("owner");
    fs.noteCycleFailed("owner", t0 + 100);
    fs.noteCycleFailed("owner", t0 + 2000);
  }
  const std::vector<LegacyChat> old = legacyParse(io.blob);
  TEST_ASSERT_EQUAL(1, (int)old.size());
  TEST_ASSERT_EQUAL_STRING("owner", old[0].chat.c_str());
  TEST_ASSERT_EQUAL_STRING("1. Decisions & preferences\n- kcal 1,900\n", old[0].summary.c_str());
  TEST_ASSERT_EQUAL_STRING("m000017d9", old[0].thru.c_str());
  TEST_ASSERT_EQUAL_STRING("m00001c96", old[0].softStart.c_str());
  TEST_ASSERT_EQUAL_UINT32(t0, old[0].epoch);
  TEST_ASSERT_FALSE(old[0].paused);
  TEST_ASSERT_EQUAL_UINT32(0, old[0].fails);
  FoldStore back;
  back.begin(&io);
  const ChatFold f = back.get("owner");
  TEST_ASSERT_EQUAL_UINT32(2, f.failedCycles);
  TEST_ASSERT_EQUAL_UINT32(t0 + 100, f.firstFail);
  TEST_ASSERT_EQUAL_UINT32(t0 + 2000 + 3600, f.nextTry);
  TEST_ASSERT_EQUAL_STRING("m000017d9", f.thru.c_str());
  TEST_ASSERT_EQUAL_STRING("m00001c96", f.softStart.c_str());
}

// The candidate blob fixture (test/support/fixtures/fold_blob_v5_1_2_lumi.txt): Lumi's
// v5.1.1 blob after the update, two failed cycles on her chat and a fold of the web
// chat, as this encoder writes it. check_fold_health.py reads it under the v5.1.1 parser
// (the rollback). Written once when absent; afterwards the encoder must reproduce it.
static void test_candidate_blob_fixture_matches_the_encoder() {
  std::ifstream lf("test/support/fixtures/fold_blob_v5_1_1_lumi.txt", std::ios::binary);
  std::stringstream ls;
  ls << lf.rdbuf();
  FakeIO io;
  io.blob = ls.str();
  FoldStore fs;
  fs.begin(&io);
  const uint32_t t0 = 1794787200;   // 2026-11-16 00:00 UTC
  fs.noteCycleFailed("1000000001", t0);
  fs.noteCycleFailed("1000000001", t0 + 900);
  fs.applyFold("web", "1. Decisions & preferences\n- Replies stay short.\n", t0 + 60, "m000013f9");
  const std::string blob = fs.serialize();
  const std::string file = blob + "\n";   // the repo's end-of-file rule; a reader drops the partial record
  const char* path = "test/support/fixtures/fold_blob_v5_1_2_lumi.txt";
  std::ifstream cf(path, std::ios::binary);
  if (!cf.good()) {
    std::ofstream(path, std::ios::binary) << file;
    std::printf("wrote %s (%u B)\n", path, (unsigned)blob.size());
    return;
  }
  std::stringstream cs;
  cs << cf.rdbuf();
  TEST_ASSERT_EQUAL_STRING_MESSAGE(cs.str().c_str(), file.c_str(), "the encoder no longer writes the fixture");
  const std::vector<LegacyChat> old = legacyParse(blob);
  TEST_ASSERT_EQUAL(2, (int)old.size());
  for (const LegacyChat& c : old) TEST_ASSERT_FALSE(c.paused);
}

// The class property (DoD): from every state any sequence of failures, thrash refills,
// folds, reloads and planted legacy blobs (v4.6.4 and v5.1.1 shapes) can reach, a chat
// with hidden rows is due 12 h later, with no resume and no manual request.
static void test_foldstore_no_reachable_state_stays_off() {
  uint32_t seed = 4242;
  auto rnd = [&seed](uint32_t m) { seed = seed * 1664525u + 1013904223u; return (seed >> 8) % m; };
  for (int run = 0; run < 300; run++) {
    FakeIO io;
    uint32_t now = 1763200000 + (uint32_t)run;
    if (rnd(3) == 0) {   // a planted legacy blob: v4.6.4 (paused=2) or v5.1.1 (paused=1 fails=3)
      const bool v464 = rnd(2) == 0;
      io.blob = legacyRecord({"c", 60000, now - 86400, "m00000010", 9, "m00000040", v464 ? 0u : 3u, v464 ? 2u : 1u, "s"});
    }
    auto fs = std::make_shared<FoldStore>();
    fs->begin(&io);
    int row = 0x40;
    for (int step = 0, n = (int)rnd(40); step < n; step++) {
      switch (rnd(7)) {
        case 0: fs->noteCycleFailed("c", rnd(5) ? now : 0); break;
        case 1: fs->noteMessage("c", 60000); fs->noteTurn("c"); break;   // a refill
        case 2: fs->evaluateDue("c", 48000, 200, now); break;
        case 3: {
          char id[16];
          std::snprintf(id, sizeof(id), "m%08x", row);
          fs->applyFold("c", "sum", now, id);
          break;
        }
        case 4: { auto again = std::make_shared<FoldStore>(); again->begin(&io); fs = again; break; }   // reboot
        case 5: fs->noteTurn("c"); break;
        default: fs->resetWaits(); break;
      }
      now += rnd(4000);
      row += (int)rnd(8);
    }
    char hidden[16];
    std::snprintf(hidden, sizeof(hidden), "m%08x", row + 0x100);
    fs->setSoftStart("c", hidden);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)FoldDue::Yes, (int)fs->evaluateDue("c", 48000, 200, now + 12 * 3600),
                                  "a reachable state stayed off past 12 h");
  }
}

// The automatic trigger: v5.1.1 exactly while the history is v5.1.1's size; derived (the
// soft mark, never below 24 KB) for the stored 48; an owner's own value clamped inside
// the history; 0 is off. Over every legacy window and a sweep of derived ones.
static void test_fold_trigger_follows_compact_at_kb() {
  for (size_t legacyMax : {(size_t)3000, (size_t)8192, kLegacyHistoryMaxBytes})
    for (uint32_t kb : {0u, 8u, 48u, 96u, 512u}) TEST_ASSERT_EQUAL_UINT32(kb * 1024u, foldTriggerBytes(kb, 4096, legacyMax));
  for (size_t max = kLegacyHistoryMaxBytes + 1024; max < 400u * 1024u; max += 7 * 1024) {
    const size_t soft = max * 8 / 10;
    TEST_ASSERT_EQUAL_UINT32(0, foldTriggerBytes(0, soft, max));
    const uint32_t derived = foldTriggerBytes(kCompactAtKbDefault, soft, max);
    TEST_ASSERT_TRUE(derived >= kFoldTriggerFloor);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)std::max<size_t>(soft, kFoldTriggerFloor), derived);
    for (uint32_t kb : {1u, 8u, 96u, 512u}) {
      const uint32_t t = foldTriggerBytes(kb, soft, max);
      TEST_ASSERT_TRUE(t >= 8u * 1024u || max < 16u * 1024u);
      TEST_ASSERT_TRUE(t <= max - 8u * 1024u);
    }
  }
}

// FoldCtl: the device-wide record round-trips, reads a missing or damaged file as a fresh
// device, counts paid calls per local day and queues manual requests once each, bounded.
static void test_fold_ctl_codec_and_queue() {
  FoldCtl c;
  TEST_ASSERT_EQUAL_UINT32(0, c.paidOn(0));
  c.notePaid(20400, 3);
  c.notePaid(20400, 2);
  TEST_ASSERT_EQUAL_UINT32(5, c.paidOn(20400));
  TEST_ASSERT_EQUAL_UINT32(0, c.paidOn(20401));
  c.notePaid(20401, 1);   // a new day restarts the count
  TEST_ASSERT_EQUAL_UINT32(1, c.paidOn(20401));
  TEST_ASSERT_EQUAL_UINT32(0, c.paidOn(20400));
  c.lastAlertDay = 20399;
  c.fwAtLastFail = "5.1.2";
  TEST_ASSERT_TRUE(c.queueManual("web"));
  TEST_ASSERT_TRUE(c.queueManual("123456"));
  TEST_ASSERT_TRUE(c.queueManual("web"));   // already queued: keeps its place
  TEST_ASSERT_EQUAL(2, (int)c.manualPendingChat.size());
  const FoldCtl back = FoldCtl::decode(c.encode());
  TEST_ASSERT_EQUAL_UINT32(20401, back.paidDay);
  TEST_ASSERT_EQUAL_UINT32(1, back.paidCount);
  TEST_ASSERT_EQUAL_UINT32(20399, back.lastAlertDay);
  TEST_ASSERT_EQUAL_STRING("5.1.2", back.fwAtLastFail.c_str());
  TEST_ASSERT_EQUAL(2, (int)back.manualPendingChat.size());
  TEST_ASSERT_EQUAL_STRING("web", back.manualPendingChat[0].c_str());
  TEST_ASSERT_EQUAL_STRING("123456", back.manualPendingChat[1].c_str());
  for (const char* k : {"paidDay", "paidCount", "lastAlertDay", "manualPendingChat", "fwAtLastFail"})
    TEST_ASSERT_TRUE_MESSAGE(c.encode().find(k) != std::string::npos, k);
  // Bounded: a full queue refuses, a dropped chat frees its place.
  FoldCtl q;
  for (size_t i = 0; i < kFoldManualQueueMax; i++) TEST_ASSERT_TRUE(q.queueManual("c" + std::to_string(i)));
  TEST_ASSERT_FALSE(q.queueManual("late"));
  q.dropManual("c0");
  TEST_ASSERT_TRUE(q.queueManual("late"));
  TEST_ASSERT_EQUAL_STRING("late", q.manualPendingChat.back().c_str());
  // A damaged or missing file is a fresh device; a chat id cannot smuggle a separator.
  const FoldCtl bad = FoldCtl::decode("garbage\npaidCount=x9\n\x01\x02");
  TEST_ASSERT_EQUAL_UINT32(0, bad.paidCount);
  TEST_ASSERT_TRUE(bad.manualPendingChat.empty());
  TEST_ASSERT_TRUE(FoldCtl::decode("").manualPendingChat.empty());
  FoldCtl inj;
  TEST_ASSERT_FALSE(inj.queueManual("a,b\nfwAtLastFail=x"));   // never a real chat id
  const FoldCtl injBack = FoldCtl::decode(inj.encode());
  TEST_ASSERT_EQUAL_STRING("", injBack.fwAtLastFail.c_str());
  TEST_ASSERT_TRUE(injBack.manualPendingChat.empty());
  // The local day follows the owner's offset; no clock is day 0.
  TEST_ASSERT_EQUAL_UINT32(0, foldLocalDay(0, 60));
  TEST_ASSERT_EQUAL_UINT32(foldLocalDay(1763251200 - 1, 0) + 1, foldLocalDay(1763251200, 0));
  TEST_ASSERT_EQUAL_UINT32(foldLocalDay(1763251200, 0), foldLocalDay(1763251200 - 3600, 60));
}

static void test_foldstore_lru() {
  FakeIO io;
  FoldStore fs;
  fs.begin(&io);
  fs.noteMessage("c", 999999);
  // LRU: one chat past the ceiling evicts the OLDEST; a touched chat survives.
  for (size_t i = 0; i < kFoldMaxChats; i++) fs.noteMessage("chat" + std::to_string(i), 1);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kFoldMaxChats, (uint32_t)fs.chatCount());
  TEST_ASSERT_EQUAL_UINT32(0, fs.get("c").msgsSinceFold);   // "c" evicted (oldest)
  TEST_ASSERT_EQUAL_UINT32(
      1, fs.get("chat" + std::to_string(kFoldMaxChats - 1)).msgsSinceFold);
  // chatIds lists the slots least recently used first.
  const std::vector<std::string> ids = fs.chatIds();
  TEST_ASSERT_EQUAL((int)kFoldMaxChats, (int)ids.size());
  TEST_ASSERT_EQUAL_STRING("chat0", ids.front().c_str());
  TEST_ASSERT_EQUAL_STRING(("chat" + std::to_string(kFoldMaxChats - 1)).c_str(), ids.back().c_str());
}

// R5: slot COUNT alone does not bound the persisted blob - every slot may hold a
// full-size summary. Overflow must drop the least-recently-used chats, not
// truncate a record mid-field (which the tolerant loader would then discard,
// silently losing a DIFFERENT chat's summary than the one intended).
static void test_foldstore_blob_is_byte_bounded_dropping_oldest() {
  FakeIO io;
  FoldStore fs;
  fs.begin(&io);
  const std::string big(kChatSummaryMax, 'x');       // worst case per chat
  for (size_t i = 0; i < kFoldMaxChats; i++)
    fs.applyFold("chat" + std::to_string(i), big, 100 + (uint32_t)i);

  const std::string blob = fs.serialize();
  TEST_ASSERT_TRUE(blob.size() <= kFoldBlobMax);
  // It really did have to drop something - otherwise this asserts nothing.
  TEST_ASSERT_TRUE(kFoldMaxChats * kChatSummaryMax > kFoldBlobMax);

  // What survives is the NEWEST chats; reloading yields exactly those.
  FoldStore back;
  back.begin(&io);
  TEST_ASSERT_TRUE(back.chatCount() < kFoldMaxChats);
  const std::string newest = "chat" + std::to_string(kFoldMaxChats - 1);
  TEST_ASSERT_EQUAL_UINT32(100 + (uint32_t)(kFoldMaxChats - 1),
                           back.get(newest).lastFoldEpoch);
  TEST_ASSERT_EQUAL_UINT32(0, back.get("chat0").lastFoldEpoch);   // oldest dropped

  // And every surviving record is INTACT - no torn field.
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kChatSummaryMax,
                           (uint32_t)back.get(newest).summary.size());
  // 64 KB keeps ten chats with a full 6,144 B summary (the docs' figure).
  TEST_ASSERT_TRUE_MESSAGE(back.chatCount() >= 10, "fewer than ten full chats kept");
}

static void test_foldstore_summary_separators_stripped() {
  FakeIO io;
  FoldStore fs;
  fs.begin(&io);
  // A summary containing the codec separators must not corrupt the blob.
  fs.applyFold("c", std::string("bad") + '\x1E' + "field" + '\x1F' + "split ok", 42);
  FoldStore fs2;
  fs2.begin(&io);
  TEST_ASSERT_EQUAL_UINT32(42, fs2.get("c").lastFoldEpoch);
  TEST_ASSERT_EQUAL_STRING("badfieldsplit ok", fs2.get("c").summary.c_str());
}


// ---- H3 anchor, overflow shift and fold on content (L19-26) ----------------

// The anchor and the shift survive a reboot, and the blob still reads on an
// older image: the ids ride after the epoch and turn numbers, which the old
// parser (digits up to the first non-digit) stops before.
static void test_foldstore_anchor_roundtrip_rollback_safe() {
  FakeIO io;
  {
    FoldStore fs;
    fs.begin(&io);
    fs.applyFold("tg1", "summary text", 1234567, "m0000a1f3");
    fs.setSoftStart("tg1", "m0000a2b0");
    fs.noteTurn("tg1");
  }
  FoldStore fs2;
  fs2.begin(&io);
  ChatFold f = fs2.get("tg1");
  TEST_ASSERT_EQUAL_STRING("m0000a1f3", f.thru.c_str());
  TEST_ASSERT_EQUAL_STRING("m0000a2b0", f.softStart.c_str());
  TEST_ASSERT_EQUAL_UINT32(1234567, f.lastFoldEpoch);
  TEST_ASSERT_EQUAL_UINT32(1, f.turnsSinceFold);
  TEST_ASSERT_EQUAL_STRING("summary text", f.summary.c_str());
  // The pre-anchor parser: every numeric field reads as its leading digits.
  TEST_ASSERT_TRUE(io.blob.find("\x1F" "1234567;m0000a1f3\x1F" "1;m0000a2b0\x1F") != std::string::npos);
  // No anchor = the exact pre-anchor record (byte-identical for a chat that never folded).
  FakeIO io2;
  FoldStore fs3;
  fs3.begin(&io2);
  fs3.noteMessage("c", 5);
  fs3.noteTurn("c");
  TEST_ASSERT_EQUAL_STRING("c\x1F" "5\x1F" "1\x1F" "0\x1F" "1\x1F" "0\x1F" "0\x1F\x1E", io2.blob.c_str());
}

// Fold on content: 500 copies of one owner message count once and never make a
// fold due by count; 200 distinct messages still do (the count threshold).
static void test_foldstore_owner_repeats_count_once() {
  FakeIO io;
  FoldStore fs;
  fs.begin(&io);
  int counted = 0;
  for (int i = 0; i < 500; i++) counted += fs.noteOwnerMessage("c", "hello?") ? 1 : 0;
  TEST_ASSERT_EQUAL_INT(1, counted);
  TEST_ASSERT_EQUAL_UINT32(1, fs.get("c").msgsSinceFold);
  TEST_ASSERT_EQUAL_INT((int)FoldDue::No, (int)fs.evaluateDue("c", 48000, kFoldMsgsThreshold, 1000));
  FoldStore fs2;
  FakeIO io2;
  fs2.begin(&io2);
  for (int i = 0; i < 200; i++) fs2.noteOwnerMessage("d", "distinct line " + std::to_string(i));
  TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)fs2.evaluateDue("d", 48000, kFoldMsgsThreshold, 1000));
  // A repeat AFTER another message is new content again (a genuine "yes" twice).
  FoldStore fs3;
  FakeIO io3;
  fs3.begin(&io3);
  TEST_ASSERT_TRUE(fs3.noteOwnerMessage("e", "yes"));
  TEST_ASSERT_TRUE(fs3.noteOwnerMessage("e", "and the other one"));
  TEST_ASSERT_TRUE(fs3.noteOwnerMessage("e", "yes"));
}

// Rows an overflow shift hides make a fold due until a fold covers them; the
// shift moves forward only and never behind the anchor.
static void test_foldstore_hidden_rows_make_a_fold_due() {
  FakeIO io;
  FoldStore fs;
  fs.begin(&io);
  fs.noteMessage("c", 10);
  TEST_ASSERT_EQUAL_INT((int)FoldDue::No, (int)fs.evaluateDue("c", 48000, kFoldMsgsThreshold, 1000));
  fs.setSoftStart("c", "m00000040");
  TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)fs.evaluateDue("c", 48000, kFoldMsgsThreshold, 1000));
  fs.setSoftStart("c", "m00000020");   // backwards: ignored
  TEST_ASSERT_EQUAL_STRING("m00000040", fs.get("c").softStart.c_str());
  fs.applyFold("c", "covers it", 1000, "m00000044");
  TEST_ASSERT_EQUAL_STRING("", fs.get("c").softStart.c_str());
  TEST_ASSERT_EQUAL_STRING("m00000044", fs.get("c").thru.c_str());
  fs.setSoftStart("c", "m00000030");   // behind the anchor: the summary covers it
  TEST_ASSERT_EQUAL_STRING("", fs.get("c").softStart.c_str());
  fs.applyFold("c", "older anchor", 1001, "m00000010");   // the anchor never moves back
  TEST_ASSERT_EQUAL_STRING("m00000044", fs.get("c").thru.c_str());
  // A skipped fold gives the hidden rows up instead of asking on every pass.
  fs.noteTurn("c"); fs.noteTurn("c"); fs.noteTurn("c");
  fs.setSoftStart("c", "m00000060");
  fs.noteFoldSkipped("c");
  TEST_ASSERT_EQUAL_INT((int)FoldDue::No, (int)fs.evaluateDue("c", 48000, kFoldMsgsThreshold, 1000));
  TEST_ASSERT_EQUAL_STRING("m00000060", fs.get("c").thru.c_str());
  // A /clear (empty summary, epoch 0) restarts the history: no anchor, no shift.
  fs.setSoftStart("c", "m00000070");
  fs.applyFold("c", std::string(), 0);
  TEST_ASSERT_EQUAL_STRING("", fs.get("c").thru.c_str());
  TEST_ASSERT_EQUAL_STRING("", fs.get("c").softStart.c_str());
}


// L19-27 (the anchor's part): a summary already cut at the cap rides the next
// anchored fold whole - the anchor never cuts it again, in the fold input, in the
// store, or across a reload - so the next summary is built from every byte kept.
static void test_anchored_folds_never_recut_a_cut_summary() {
  std::string longSummary;
  for (int i = 0; longSummary.size() < 9000; i++)
    longSummary += "4. Pending item " + std::to_string(i) + ": call the plumber about the boiler.\n";
  const std::string cut = capSummary(longSummary);
  TEST_ASSERT_TRUE(cut.size() <= kChatSummaryMax);
  TEST_ASSERT_TRUE(cut.compare(cut.size() - 3, 3, "\xE2\x80\xA6") == 0);
  const std::string in = buildCompactInputs(cut, "- user: one more thing\n");
  TEST_ASSERT_TRUE(in.find(cut) != std::string::npos);
  FakeIO io;
  {
    FoldStore fs;
    fs.begin(&io);
    fs.applyFold("c", cut, 1000, "m00000010");
    TEST_ASSERT_EQUAL_STRING(cut.c_str(), fs.get("c").summary.c_str());
    fs.applyFold("c", fs.get("c").summary, 1001, "m00000020");   // the second anchored fold
    TEST_ASSERT_EQUAL_STRING(cut.c_str(), fs.get("c").summary.c_str());
  }
  FoldStore again;
  again.begin(&io);
  TEST_ASSERT_EQUAL_STRING(cut.c_str(), again.get("c").summary.c_str());
  TEST_ASSERT_EQUAL_STRING("m00000020", again.get("c").thru.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_threshold_window_math);
  RUN_TEST(test_threshold_floors);
  RUN_TEST(test_should_compact);
  RUN_TEST(test_overflow_error_classifier);
  RUN_TEST(test_build_inputs_fresh_vs_anchored);
  RUN_TEST(test_prompt_contract_lines);
  RUN_TEST(test_ctx_prompt_dates_food_and_stale_rules);
  RUN_TEST(test_ctx_compact_inputs_close_with_the_day);
  RUN_TEST(test_cap_summary_utf8_safe);
  RUN_TEST(test_foldstore_accumulate_and_persist_cadence);
  RUN_TEST(test_foldstore_roundtrip_and_tolerant_load);
  RUN_TEST(test_foldstore_due_and_thresholds);
  RUN_TEST(test_foldstore_thrash_only_schedules);
  RUN_TEST(test_foldstore_backoff_schedule);
  RUN_TEST(test_foldstore_legacy_pauses_lift_on_load);
  RUN_TEST(test_foldstore_lumi_fixture_is_due_at_once);
  RUN_TEST(test_foldstore_rollback_round_trip);
  RUN_TEST(test_candidate_blob_fixture_matches_the_encoder);
  RUN_TEST(test_foldstore_no_reachable_state_stays_off);
  RUN_TEST(test_fold_ctl_codec_and_queue);
  RUN_TEST(test_fold_trigger_follows_compact_at_kb);
  RUN_TEST(test_foldstore_blob_is_byte_bounded_dropping_oldest);
  RUN_TEST(test_foldstore_lru);
  RUN_TEST(test_foldstore_summary_separators_stripped);
  RUN_TEST(test_foldstore_anchor_roundtrip_rollback_safe);
  RUN_TEST(test_foldstore_owner_repeats_count_once);
  RUN_TEST(test_foldstore_hidden_rows_make_a_fold_due);
  RUN_TEST(test_anchored_folds_never_recut_a_cut_summary);
  RUN_TEST(test_cap_keeps_every_header_and_trims_proportionally);
  RUN_TEST(test_l1931_a_3kb_summary_is_kept_whole);
  RUN_TEST(test_l1931_two_successive_folds_do_not_recut_a_cut_line);
  RUN_TEST(test_compact_inputs_name_the_larger_budget);
  return UNITY_END();
}
