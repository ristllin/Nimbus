#include <unity.h>

#include <string>

#include "nimbus/orch/compact.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// ---- model context table -----------------------------------------------------
static void test_ctx_table_families() {
  TEST_ASSERT_EQUAL_UINT32(200000, modelCtxTokens("anthropic", "claude-sonnet-4-6"));
  TEST_ASSERT_EQUAL_UINT32(200000, modelCtxTokens("anthropic", "claude-sonnet-4-7"));  // unseen snapshot, family match
  TEST_ASSERT_EQUAL_UINT32(272000, modelCtxTokens("openai", "gpt-5.5"));
  TEST_ASSERT_EQUAL_UINT32(272000, modelCtxTokens("openai", "gpt-5.4-mini"));
  TEST_ASSERT_EQUAL_UINT32(200000, modelCtxTokens("openai", "o4-mini-deep-research"));
  TEST_ASSERT_EQUAL_UINT32(128000, modelCtxTokens("mistral", "mistral-large-latest"));
  // CUM-288: the Cumulo router head resolves to gpt-4o by default. Its real 128K
  // window must drive budgeting, not the conservative 100K default a "" model or
  // an unknown family would produce (which compacted the router head early).
  TEST_ASSERT_EQUAL_UINT32(128000, modelCtxTokens("cumulo", "gpt-4o"));
  TEST_ASSERT_EQUAL_UINT32(128000, modelCtxTokens("cumulo", "gpt-4o-mini"));
}

static void test_ctx_table_unknown_is_conservative() {
  // Unknown family -> the conservative default (compacts EARLY, never late).
  TEST_ASSERT_EQUAL_UINT32(kCtxDefaultTokens, modelCtxTokens("custom", "llama-3-70b"));
  // "openai"/"omni"-style names must not match the o-series rule.
  TEST_ASSERT_EQUAL_UINT32(kCtxDefaultTokens, modelCtxTokens("custom", "omni-chat"));
}

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

// ---- breaker -----------------------------------------------------------------
static void test_breaker_pauses_on_third_fail_alerts_once() {
  CompactBreaker b;
  TEST_ASSERT_FALSE(b.noteResult(false));
  TEST_ASSERT_FALSE(b.noteResult(false));
  TEST_ASSERT_FALSE(b.paused);
  TEST_ASSERT_TRUE(b.noteResult(false));   // pause edge -> alert exactly here
  TEST_ASSERT_TRUE(b.paused);
  TEST_ASSERT_FALSE(b.noteResult(false));  // already paused -> no re-alert
  TEST_ASSERT_TRUE(b.noteResult(false) == false && b.paused);
}

static void test_breaker_success_resets() {
  CompactBreaker b;
  b.noteResult(false); b.noteResult(false);
  TEST_ASSERT_FALSE(b.noteResult(true));
  TEST_ASSERT_EQUAL_UINT8(0, b.fails);
  TEST_ASSERT_FALSE(b.paused);
  // A manual success while paused un-pauses (recovery path).
  b.noteResult(false); b.noteResult(false); b.noteResult(false);
  TEST_ASSERT_TRUE(b.paused);
  b.noteResult(true);
  TEST_ASSERT_FALSE(b.paused);
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
  TEST_ASSERT_EQUAL_INT((int)FoldDue::No, (int)fs.evaluateDue("c", 48000, 200));
  fs.noteMessage("c", 9000);
  TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)fs.evaluateDue("c", 48000, 200));
  // Disabled (bytesThreshold 0) never fires.
  TEST_ASSERT_EQUAL_INT((int)FoldDue::No, (int)fs.evaluateDue("c", 0, 0));
  // Message-count threshold fires independently.
  FoldStore fs2; FakeIO io2; fs2.begin(&io2);
  for (int i = 0; i < 200; i++) fs2.noteMessage("m", 10);
  TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)fs2.evaluateDue("m", 48000, 200));
}

static void test_foldstore_thrash_pauses_once() {
  FakeIO io;
  FoldStore fs;
  fs.begin(&io);
  fs.noteMessage("c", 60000);
  fs.noteTurn("c");
  TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)fs.evaluateDue("c", 48000, 0));
  fs.applyFold("c", "sum", 1000);
  // Immediately over threshold again within kThrashTurns turns -> ThrashPaused.
  fs.noteMessage("c", 60000);
  fs.noteTurn("c");
  TEST_ASSERT_EQUAL_INT((int)FoldDue::ThrashPaused, (int)fs.evaluateDue("c", 48000, 0));
  // Paused: stays No (the alert fired exactly once on the edge).
  TEST_ASSERT_EQUAL_INT((int)FoldDue::No, (int)fs.evaluateDue("c", 48000, 0));
  // resume() re-arms.
  fs.resume("c");
  fs.noteTurn("c"); fs.noteTurn("c"); fs.noteTurn("c");   // move past the thrash window
  TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)fs.evaluateDue("c", 48000, 0));
}

static void test_foldstore_breaker_and_lru() {
  FakeIO io;
  FoldStore fs;
  fs.begin(&io);
  TEST_ASSERT_FALSE(fs.noteFoldFailed("c"));
  TEST_ASSERT_FALSE(fs.noteFoldFailed("c"));
  TEST_ASSERT_TRUE(fs.noteFoldFailed("c"));    // 3rd fail = pause edge (alert once)
  TEST_ASSERT_FALSE(fs.noteFoldFailed("c"));
  fs.noteMessage("c", 999999);
  TEST_ASSERT_EQUAL_INT((int)FoldDue::No, (int)fs.evaluateDue("c", 48000, 0));
  // LRU: one chat past the ceiling evicts the OLDEST; a touched chat survives.
  for (size_t i = 0; i < kFoldMaxChats; i++) fs.noteMessage("chat" + std::to_string(i), 1);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kFoldMaxChats, (uint32_t)fs.chatCount());
  TEST_ASSERT_EQUAL_UINT32(0, fs.get("c").msgsSinceFold);   // "c" evicted (oldest)
  TEST_ASSERT_EQUAL_UINT32(
      1, fs.get("chat" + std::to_string(kFoldMaxChats - 1)).msgsSinceFold);
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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_ctx_table_families);
  RUN_TEST(test_ctx_table_unknown_is_conservative);
  RUN_TEST(test_threshold_window_math);
  RUN_TEST(test_threshold_floors);
  RUN_TEST(test_should_compact);
  RUN_TEST(test_overflow_error_classifier);
  RUN_TEST(test_breaker_pauses_on_third_fail_alerts_once);
  RUN_TEST(test_breaker_success_resets);
  RUN_TEST(test_build_inputs_fresh_vs_anchored);
  RUN_TEST(test_prompt_contract_lines);
  RUN_TEST(test_cap_summary_utf8_safe);
  RUN_TEST(test_foldstore_accumulate_and_persist_cadence);
  RUN_TEST(test_foldstore_roundtrip_and_tolerant_load);
  RUN_TEST(test_foldstore_due_and_thresholds);
  RUN_TEST(test_foldstore_thrash_pauses_once);
  RUN_TEST(test_foldstore_blob_is_byte_bounded_dropping_oldest);
  RUN_TEST(test_foldstore_breaker_and_lru);
  RUN_TEST(test_foldstore_summary_separators_stripped);
  return UNITY_END();
}
