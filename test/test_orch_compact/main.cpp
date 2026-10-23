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
  // gpt-6-astra (2026-09): 1,050,000 published context minus the 128,000 max
  // output = the 922K INPUT window LiteLLM reports. Later generations inherit it.
  TEST_ASSERT_EQUAL_UINT32(922000, modelCtxTokens("openai", "gpt-6-astra"));
  TEST_ASSERT_EQUAL_UINT32(922000, modelCtxTokens("openai", "gpt-7-x"));
  TEST_ASSERT_EQUAL_UINT32(200000, modelCtxTokens("openai", "o4-mini-deep-research"));
  TEST_ASSERT_EQUAL_UINT32(128000, modelCtxTokens("mistral", "mistral-large-latest"));
  // CUM-288 class rule: whatever model the Cumulo router head resolves to must
  // get its real family window, never the conservative 100K default a "" model
  // or an unknown family would produce (which compacted the router head early).
  TEST_ASSERT_EQUAL_UINT32(128000, modelCtxTokens("cumulo", "gpt-4o"));
  TEST_ASSERT_EQUAL_UINT32(128000, modelCtxTokens("cumulo", "gpt-4o-mini"));
  TEST_ASSERT_EQUAL_UINT32(272000, modelCtxTokens("cumulo", "gpt-5.6"));  // the 2026-09 default
}

// Class rule: no gpt-5-or-newer generation may fall through to the conservative
// default. gpt-6-astra did (it was keyed on the literal "gpt-5" prefix), which
// would have compacted a 922K-window model at 100K.
static void test_ctx_table_every_gpt_generation_is_known() {
  for (int gen = 5; gen <= 9; ++gen) {
    const std::string id = "gpt-" + std::to_string(gen) + "-x";
    TEST_ASSERT_TRUE_MESSAGE(modelCtxTokens("openai", id) >= 272000, id.c_str());
  }
}

// CUM-530: the Cumulo picker stores every pick as a router selector
// ("<upstream>/<id>"; re-picking the default stores "openai/gpt-5.6"). The window
// lookup resolves it through the ONE router rule (resolveRouterRoute) and reads
// the upstream's window for the bare id: the prefixed default fell to the 100K
// default, a 2.7x smaller recent window and compaction at a third of the window.
static void test_ctx_router_selectors_resolve_to_the_upstream_window() {
  TEST_ASSERT_EQUAL_UINT32(272000, modelCtxTokens("cumulo", "openai/gpt-5.6"));
  TEST_ASSERT_EQUAL_UINT32(200000, modelCtxTokens("cumulo", "anthropic/claude-opus-5"));
  TEST_ASSERT_EQUAL_UINT32(922000, modelCtxTokens("cumulo", "openai/gpt-6-astra"));
  TEST_ASSERT_EQUAL_UINT32(128000, modelCtxTokens("cumulo", "mistral/mistral-large-latest"));
  TEST_ASSERT_EQUAL_UINT32(272000, modelCtxTokens("cumulo", "OpenAI/GPT-5.6"));   // case-insensitive
  // The bare spelling keeps the router's default upstream (openai): unchanged.
  TEST_ASSERT_EQUAL_UINT32(272000, modelCtxTokens("cumulo", "gpt-5.6"));
  TEST_ASSERT_EQUAL_UINT32(128000, modelCtxTokens("cumulo", "gpt-4o"));
  // Another provider's id is split only when its prefix is a router upstream; a
  // LAN server's namespaced id keeps the conservative default (never a bigger
  // family window: "mistralai/Mistral-7B" is a 32K model, not mistral-* 128K).
  TEST_ASSERT_EQUAL_UINT32(200000, modelCtxTokens("custom", "anthropic/claude-sonnet-5"));
  TEST_ASSERT_EQUAL_UINT32(kCtxDefaultTokens, modelCtxTokens("custom", "meta-llama/llama-3-70b"));
  TEST_ASSERT_EQUAL_UINT32(kCtxDefaultTokens,
                           modelCtxTokens("custom", "mistralai/Mistral-7B-Instruct-v0.3"));
  TEST_ASSERT_EQUAL_UINT32(kCtxDefaultTokens, modelCtxTokens("custom", "TheBloke/Mistral-7B-GGUF"));
  // A leading '/' is not an upstream marker (the router rule's own semantics).
  TEST_ASSERT_EQUAL_UINT32(kCtxDefaultTokens, modelCtxTokens("custom", "/gpt-5.6"));
}

// The class: whatever (upstream, model) the router serves, the selector's window
// equals the direct upstream's window for the bare id, and a bare id on the router
// equals the default upstream's. A new family added to the table is covered here
// with no per-selector edit.
static void test_ctx_router_selector_class() {
  struct Pick { const char* up; const char* id; };
  static const Pick kPicks[] = {
      {"openai", "gpt-5.6"},          {"openai", "gpt-5.6-terra"},   {"openai", "gpt-6-astra"},
      {"openai", "gpt-4o-mini"},      {"openai", "o4-mini"},         {"anthropic", "claude-opus-5"},
      {"anthropic", "claude-haiku-4-5"}, {"mistral", "mistral-large-latest"},
      {"mistral", "mistral-small-2"}, {"zai", "glm-5.3"},            {"zai", "glm-4.5-flash"},
  };
  for (const Pick& k : kPicks) {
    const std::string sel = std::string(k.up) + "/" + k.id;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(modelCtxTokens(k.up, k.id), modelCtxTokens("cumulo", sel),
                                     sel.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(modelCtxTokens("openai", k.id), modelCtxTokens("cumulo", k.id),
                                     k.id);
  }
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
  // L19-27: the byte budget is stated, and the durable sections come FIRST in
  // the order, so a trim takes the volatile tail.
  TEST_ASSERT_TRUE(p.find("3,500 bytes") != std::string::npos);
  TEST_ASSERT_TRUE(p.find("Decisions & preferences") != std::string::npos);
  TEST_ASSERT_TRUE(p.find("Decisions & preferences") < p.find("Owner intent & active threads"));
  TEST_ASSERT_TRUE(p.find("Facts worth remembering long-term") < p.find("Device & task state"));
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

// The kept text always holds the durable sections' headers (the amendment's
// numbering: 2 decisions, 4 pending, 5 long-term facts), never splits UTF-8,
// and never exceeds the cap.
static void test_l1931_cap_keeps_durable_headers_and_never_splits_utf8() {
  uint32_t seed = 12345;
  for (int i = 0; i < 400; i++) {
    const size_t target = 1000 + (size_t)(i * 47 % 19000);
    const std::string s = genSummary(seed, target);
    const std::string capped = capSummary(s);
    TEST_ASSERT_TRUE_MESSAGE(capped.size() <= kChatSummaryMax, "over the cap");
    TEST_ASSERT_TRUE_MESSAGE(utf8Whole(capped), "a torn UTF-8 sequence");
    if (s.find("1. Decisions & preferences") != std::string::npos)
      TEST_ASSERT_TRUE_MESSAGE(capped.find("1. Decisions & preferences") != std::string::npos,
                               "the decisions header was dropped");
    if (s.find("2. Pending items & next steps") != std::string::npos)
      TEST_ASSERT_TRUE_MESSAGE(capped.find("2. Pending items & next steps") != std::string::npos,
                               "the pending header was dropped");
    if (s.find("3. Facts worth remembering long-term") != std::string::npos)
      TEST_ASSERT_TRUE_MESSAGE(capped.find("3. Facts worth remembering long-term") !=
                                   std::string::npos,
                               "the long-term facts header was dropped");
    // The cut is visible, never as a doubled marker.
    TEST_ASSERT_TRUE_MESSAGE(capped.find("\xE2\x80\xA6\xE2\x80\xA6") == std::string::npos,
                             "two markers");
    if (capped.size() < s.size())
      TEST_ASSERT_TRUE_MESSAGE(capped.find("\xE2\x80\xA6") != std::string::npos,
                               "an invisible cut");
    if (s.size() <= kChatSummaryMax)
      TEST_ASSERT_TRUE_MESSAGE(capped == s, "an under-cap summary was changed");
  }
}

// The counter-test: a 3 KB summary is kept whole, byte for byte.
static void test_l1931_a_3kb_summary_is_kept_whole() {
  uint32_t seed = 777;
  const std::string s = genSummary(seed, 3000);
  TEST_ASSERT_TRUE(s.size() <= kChatSummaryMax);
  TEST_ASSERT_EQUAL_STRING(s.c_str(), capSummary(s).c_str());
}

// The replay fixture: a fold's capped output feeds the next anchored fold,
// whose model output carries the cut line and one new line. The already cut
// line is never re-truncated: the new line is what the next trim drops, the
// previous cut stands byte for byte, and the result is idempotent.
static void test_l1931_two_successive_folds_do_not_recut_a_cut_line() {
  uint32_t seed = 4242;
  const std::string big = genSummary(seed, 12000);
  const std::string cut = capSummary(big);
  TEST_ASSERT_TRUE(cut.size() <= kChatSummaryMax);
  TEST_ASSERT_TRUE(cut.find("\xE2\x80\xA6") != std::string::npos);
  // The next fold's model output carries the cut summary with one more line
  // after it. The already cut line is not cut again: the trim drops the new
  // line whole, the previous cut stands byte for byte, no second marker, and
  // a third pass changes nothing.
  const std::string next = cut + "\n- one more device note\n";
  const std::string again = capSummary(next);
  TEST_ASSERT_TRUE(again.size() <= kChatSummaryMax);
  TEST_ASSERT_EQUAL_STRING(cut.c_str(), again.c_str());
  TEST_ASSERT_EQUAL_STRING(cut.c_str(), capSummary(again).c_str());
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
  TEST_ASSERT_EQUAL_INT((int)FoldDue::No, (int)fs.evaluateDue("c", 48000, kFoldMsgsThreshold));
  FoldStore fs2;
  FakeIO io2;
  fs2.begin(&io2);
  for (int i = 0; i < 200; i++) fs2.noteOwnerMessage("d", "distinct line " + std::to_string(i));
  TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)fs2.evaluateDue("d", 48000, kFoldMsgsThreshold));
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
  TEST_ASSERT_EQUAL_INT((int)FoldDue::No, (int)fs.evaluateDue("c", 48000, kFoldMsgsThreshold));
  fs.setSoftStart("c", "m00000040");
  TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)fs.evaluateDue("c", 48000, kFoldMsgsThreshold));
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
  TEST_ASSERT_EQUAL_INT((int)FoldDue::No, (int)fs.evaluateDue("c", 48000, kFoldMsgsThreshold));
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
  for (int i = 0; longSummary.size() < 6000; i++)
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
  RUN_TEST(test_ctx_table_families);
  RUN_TEST(test_ctx_table_every_gpt_generation_is_known);
  RUN_TEST(test_ctx_table_unknown_is_conservative);
  RUN_TEST(test_ctx_router_selectors_resolve_to_the_upstream_window);
  RUN_TEST(test_ctx_router_selector_class);
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
  RUN_TEST(test_foldstore_anchor_roundtrip_rollback_safe);
  RUN_TEST(test_foldstore_owner_repeats_count_once);
  RUN_TEST(test_foldstore_hidden_rows_make_a_fold_due);
  RUN_TEST(test_anchored_folds_never_recut_a_cut_summary);
  RUN_TEST(test_l1931_cap_keeps_durable_headers_and_never_splits_utf8);
  RUN_TEST(test_l1931_a_3kb_summary_is_kept_whole);
  RUN_TEST(test_l1931_two_successive_folds_do_not_recut_a_cut_line);
  return UNITY_END();
}
