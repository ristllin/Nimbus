#include <unity.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "nimbus/orch/caps.h"
#include "nimbus/orch/msg_batch.h"

// CUM-398 rapid-message batcher (nimbus::orch::MessageBatcher). These assert the
// CLASS rule ("no burst of messages in one chat becomes more than one turn; a cap
// never drops a message; different chats never merge"), not one instance - the
// caps are read from caps.h so the properties hold if the numbers move.

using nimbus::orch::BatchAdd;
using nimbus::orch::MessageBatcher;
namespace cap = nimbus::orch;

void setUp() {}
void tearDown() {}

static bool has(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}

// A single normal message must be byte-for-byte identical to the old one-turn-per-
// message behavior (raw text, no scaffolding) so nothing regresses for the common case.
static void test_single_message_is_raw_text() {
  MessageBatcher b;
  TEST_ASSERT_TRUE(BatchAdd::Accepted == b.add("100", "Alice", "hello world"));
  TEST_ASSERT_EQUAL_UINT(1, b.chatCount());
  TEST_ASSERT_EQUAL_STRING("100", b.chatIdAt(0));
  TEST_ASSERT_EQUAL_STRING("Alice", b.turnFromAt(0));
  TEST_ASSERT_EQUAL_UINT(1, b.messageCountAt(0));
  TEST_ASSERT_FALSE(b.cappedAt(0));
  TEST_ASSERT_EQUAL_STRING("hello world", b.render(0).c_str());
}

// A burst in one chat is ONE bucket, all messages present, in arrival order,
// rendered as distinct blocks (not a run-on) with the sender preserved.
static void test_burst_same_chat_one_ordered_turn() {
  MessageBatcher b;
  for (int i = 1; i <= 5; ++i) {
    std::string t = "seg" + std::to_string(i);
    TEST_ASSERT_TRUE(BatchAdd::Deferred != b.add("77", "Bob", t));
  }
  TEST_ASSERT_EQUAL_UINT(1, b.chatCount());
  TEST_ASSERT_EQUAL_UINT(5, b.messageCountAt(0));
  std::string r = b.render(0).c_str();
  size_t p1 = r.find("seg1"), p2 = r.find("seg2"), p3 = r.find("seg3"),
         p4 = r.find("seg4"), p5 = r.find("seg5");
  TEST_ASSERT_TRUE(p1 != std::string::npos);
  TEST_ASSERT_TRUE(p1 < p2 && p2 < p3 && p3 < p4 && p4 < p5);  // arrival order
  TEST_ASSERT_TRUE(has(r, "Message 1 of 5"));
  TEST_ASSERT_TRUE(has(r, "Message 5 of 5"));
  TEST_ASSERT_TRUE(has(r, "from Bob"));
  TEST_ASSERT_FALSE(b.cappedAt(0));
}

// Different chats in the same drain stay partitioned in first-seen order and never
// cross-contaminate (the allowlist/owner model depends on this).
static void test_mixed_chats_partitioned_no_contamination() {
  MessageBatcher b;
  b.add("A", "ua", "alpha1");
  b.add("B", "ub", "bravo1");
  b.add("A", "ua", "alpha2");
  b.add("B", "ub", "bravo2");
  TEST_ASSERT_EQUAL_UINT(2, b.chatCount());
  TEST_ASSERT_EQUAL_STRING("A", b.chatIdAt(0));
  TEST_ASSERT_EQUAL_STRING("B", b.chatIdAt(1));
  std::string ra = b.render(0).c_str(), rb = b.render(1).c_str();
  TEST_ASSERT_TRUE(has(ra, "alpha1") && has(ra, "alpha2"));
  TEST_ASSERT_FALSE(has(ra, "bravo1") || has(ra, "bravo2"));
  TEST_ASSERT_TRUE(has(rb, "bravo1") && has(rb, "bravo2"));
  TEST_ASSERT_FALSE(has(rb, "alpha1") || has(rb, "alpha2"));
}

// Per-chat COUNT cap: exactly kBatchMaxMsgsPerChat buffer; the next is Deferred
// (not dropped), the bucket is flagged capped, and its render says more are waiting.
static void test_per_chat_count_cap_defers_never_drops() {
  MessageBatcher b;
  for (size_t i = 0; i < cap::kBatchMaxMsgsPerChat; ++i)
    TEST_ASSERT_TRUE(BatchAdd::Deferred != b.add("C", "u", "x"));
  TEST_ASSERT_TRUE(BatchAdd::Deferred == b.add("C", "u", "OVERFLOW"));
  TEST_ASSERT_EQUAL_UINT(cap::kBatchMaxMsgsPerChat, b.messageCountAt(0));
  TEST_ASSERT_TRUE(b.cappedAt(0));
  std::string r = b.render(0).c_str();
  TEST_ASSERT_TRUE(has(r, "waiting"));
  TEST_ASSERT_FALSE(has(r, "OVERFLOW"));  // the shed message is NOT in this turn
}

// Per-chat BYTE cap: two half-cap-plus messages exceed the per-chat byte ceiling;
// the second is Deferred and flagged, never merged past the bound.
static void test_per_chat_byte_cap_defers() {
  MessageBatcher b;
  const std::string big(cap::kBatchMaxBytesPerChat / 2 + 100, 'z');
  TEST_ASSERT_TRUE(BatchAdd::Deferred != b.add("D", "u", big));
  TEST_ASSERT_TRUE(BatchAdd::Deferred == b.add("D", "u", big));
  TEST_ASSERT_EQUAL_UINT(1, b.messageCountAt(0));
  TEST_ASSERT_TRUE(b.cappedAt(0));
}

// A single message larger than the per-chat byte cap is still accepted whole (never
// truncated or dropped): the cap bounds ACCUMULATION, not a lone legitimate message.
static void test_single_oversized_message_never_dropped() {
  MessageBatcher b;
  const std::string huge(cap::kBatchMaxBytesPerChat + 2000, 'q');
  TEST_ASSERT_TRUE(BatchAdd::Deferred != b.add("E", "u", huge));
  TEST_ASSERT_EQUAL_UINT(1, b.messageCountAt(0));
  TEST_ASSERT_FALSE(b.cappedAt(0));
  TEST_ASSERT_EQUAL_UINT(huge.size(), std::strlen(b.render(0).c_str()));  // whole, raw
}

// Total-bytes cap across ALL chats: once the accumulator is near full a new chat's
// message is Deferred (re-served next drain), so the accumulator can never overflow.
static void test_total_bytes_cap_across_chats() {
  MessageBatcher b;
  const std::string big(5000, 'a');
  size_t created = 0;
  bool sawDefer = false;
  for (int i = 0; i < 32; ++i) {
    std::string chat = "chat" + std::to_string(i);
    if (BatchAdd::Deferred == b.add(chat, "u", big)) { sawDefer = true; break; }
    ++created;
  }
  TEST_ASSERT_TRUE(sawDefer);
  TEST_ASSERT_EQUAL_UINT(created, b.chatCount());
  // Every buffered chat's payload is bounded; the total never exceeded the ceiling.
  TEST_ASSERT_TRUE(created * big.size() <= cap::kBatchMaxTotalBytes);
}

// Distinct-chat count cap: past kBatchMaxChats a brand-new chat is Deferred (small
// messages, so bytes are not the limiter here).
static void test_chat_count_cap() {
  MessageBatcher b;
  for (size_t i = 0; i < cap::kBatchMaxChats; ++i) {
    std::string chat = "c" + std::to_string(i);
    TEST_ASSERT_TRUE(BatchAdd::Deferred != b.add(chat, "u", "x"));
  }
  TEST_ASSERT_EQUAL_UINT(cap::kBatchMaxChats, b.chatCount());
  TEST_ASSERT_TRUE(BatchAdd::Deferred == b.add("one-too-many", "u", "x"));
  TEST_ASSERT_EQUAL_UINT(cap::kBatchMaxChats, b.chatCount());  // unchanged
}

// A Deferred add must not partially mutate state (nothing half-added).
static void test_deferred_leaves_state_unchanged() {
  MessageBatcher b;
  for (size_t i = 0; i < cap::kBatchMaxMsgsPerChat; ++i) b.add("F", "u", "y");
  const size_t before = b.messageCountAt(0);
  TEST_ASSERT_TRUE(BatchAdd::Deferred == b.add("F", "u", "z"));
  TEST_ASSERT_EQUAL_UINT(before, b.messageCountAt(0));
}

// turnFrom is the FIRST sender; each message keeps its own from in the render (a
// group chat can have several senders in one batch).
static void test_from_preserved_per_message() {
  MessageBatcher b;
  b.add("G", "Alice", "hi");
  b.add("G", "Bob", "yo");
  TEST_ASSERT_EQUAL_STRING("Alice", b.turnFromAt(0));
  std::string r = b.render(0).c_str();
  TEST_ASSERT_TRUE(has(r, "from Alice"));
  TEST_ASSERT_TRUE(has(r, "from Bob"));
}

// An empty sender renders no " from" clause (no dangling "from :").
static void test_empty_from_no_clause() {
  MessageBatcher b;
  b.add("H", "", "one");
  b.add("H", "", "two");
  std::string r = b.render(0).c_str();
  TEST_ASSERT_TRUE(has(r, "Message 1 of 2:"));
  TEST_ASSERT_FALSE(has(r, " from "));
}

// clear() resets both the buckets and the shared byte budget so the instance can be
// reused across drains without leaking the previous drain's accounting.
static void test_clear_resets_budget() {
  MessageBatcher b;
  const std::string big(20000, 'a');
  b.add("X", "u", big);
  b.clear();
  TEST_ASSERT_TRUE(b.empty());
  TEST_ASSERT_EQUAL_UINT(0, b.chatCount());
  // The freed budget lets a fresh large message land again (no stale total).
  TEST_ASSERT_TRUE(BatchAdd::Deferred != b.add("Y", "u", big));
  TEST_ASSERT_EQUAL_UINT(1, b.chatCount());
}

// Property: a fresh accumulator ALWAYS accepts the first message for any chat, at
// any size up to a full Telegram message, so a message can never be permanently
// stuck (the caller's re-serve loop always makes progress).
static void test_fresh_accepts_first_message_always() {
  const size_t sizes[] = {1, 100, 4096, cap::kBatchMaxBytesPerChat,
                          cap::kBatchMaxBytesPerChat * 2};
  for (size_t s : sizes) {
    MessageBatcher b;
    std::string msg(s, 'm');
    TEST_ASSERT_TRUE(BatchAdd::Deferred != b.add("solo", "u", msg));
    TEST_ASSERT_EQUAL_UINT(1, b.messageCountAt(0));
  }
}

// ---- drain driver ----------------------------------------------------------

using nimbus::orch::ClassifiedUpdate;
using nimbus::orch::DrainResult;
using nimbus::orch::drainOnePage;
using nimbus::orch::RawPage;

// drainPages leaves the emitted turns in the batcher (one bucket per chat); the
// caller reads render(i) one at a time. This copies a chat's rendered turn text into
// a std::string for the assertions.
static std::string rendered(const MessageBatcher& b, size_t i) {
  return std::string(b.render(i).c_str());
}

static ClassifiedUpdate txt(int32_t id, const char* chat, const char* from, const char* text) {
  ClassifiedUpdate u;
  u.updateId = id;
  u.action = ClassifiedUpdate::Action::BatchText;
  u.chatId = chat;
  u.from = from;
  u.text = text;
  return u;
}

// A canned getUpdates server: replays pre-loaded pages in order, records the offset
// each fetch asked for (so a test can prove pagination advanced), and exposes the
// current page's updates through handle(i) - the same lazy fetch/handle split the
// device uses, so the test drives drainOnePage exactly as production does. Each
// drainOnePage call fetches ONE page (the next queued one).
struct FakeServer {
  std::vector<std::vector<ClassifiedUpdate>> pages;
  std::vector<int>                           limits;
  std::vector<bool>                          errors;
  std::vector<int32_t>                       offsetsSeen;
  std::vector<int>                           handledIdx;   // which updates handle() was called for
  std::vector<ClassifiedUpdate>              current;   // the page handle() indexes
  size_t next = 0;

  nimbus::orch::RawPage fetch(int32_t offset) {
    offsetsSeen.push_back(offset);
    nimbus::orch::RawPage rp;
    if (next >= pages.size()) { current.clear(); rp.count = 0; rp.limit = 10; return rp; }
    if (errors[next]) { rp.fetchError = true; ++next; return rp; }
    current = pages[next];
    rp.count = static_cast<int>(current.size());
    rp.limit = limits[next];
    ++next;
    return rp;
  }
  ClassifiedUpdate handle(int i) { handledIdx.push_back(i); return current[static_cast<size_t>(i)]; }
};

static void addPage(FakeServer& srv, std::vector<ClassifiedUpdate> us, int limit = 10) {
  srv.pages.push_back(std::move(us));
  srv.limits.push_back(limit);
  srv.errors.push_back(false);
}

static void addErrorPage(FakeServer& srv) {
  srv.pages.emplace_back();
  srv.limits.push_back(10);
  srv.errors.push_back(true);
}

static DrainResult runDrain(MessageBatcher& b, int32_t off, FakeServer& srv) {
  return drainOnePage(b, off,
                      [&](int32_t o) { return srv.fetch(o); },
                      [&](int i) { return srv.handle(i); });
}

// An offset-aware server that models real getUpdates window semantics: fetch(offset)
// returns up to `limit` updates with updateId >= offset. Fetching a higher offset is
// what confirms lower updates server-side. Used to prove the per-page no-loss
// guarantee across a simulated mid-drain reset (re-fetch at an un-committed offset
// returns the SAME page; a committed offset returns the NEXT page).
struct OffsetServer {
  std::vector<ClassifiedUpdate> all;   // master list, ascending updateId
  int limit = 10;
  std::vector<ClassifiedUpdate> current;

  nimbus::orch::RawPage fetch(int32_t offset) {
    current.clear();
    nimbus::orch::RawPage rp;
    rp.limit = limit;
    for (const auto& u : all) {
      if (u.updateId >= offset) {
        current.push_back(u);
        if (static_cast<int>(current.size()) >= limit) break;
      }
    }
    rp.count = static_cast<int>(current.size());
    return rp;
  }
  ClassifiedUpdate handle(int i) { return current[static_cast<size_t>(i)]; }
};

static DrainResult runDrainOffset(MessageBatcher& b, int32_t off, OffsetServer& srv) {
  return drainOnePage(b, off,
                      [&](int32_t o) { return srv.fetch(o); },
                      [&](int i) { return srv.handle(i); });
}

// ONE drain fetches exactly ONE getUpdates page - even a full page (count == limit,
// so a backlog remains). It does NOT paginate on within a single drain: draining
// several pages before committing the offset once would make earlier pages losable on
// a reset. A rapid burst that fits one page is still one turn; the backlog's next page
// is drained on the NEXT call.
static void test_drain_one_page_per_call() {
  MessageBatcher b;
  OffsetServer srv;
  srv.limit = 10;
  // 12 updates waiting, split across two chats (so neither hits the per-chat cap).
  for (int i = 0; i < 12; ++i) {
    const char* chat = (i % 2 == 0) ? "A" : "B";
    srv.all.push_back(txt(100 + i, chat, "u", ("m" + std::to_string(i)).c_str()));
  }
  DrainResult r = runDrainOffset(b, 100, srv);
  // Only the first 10 (one page) are drained this call, not all 12.
  TEST_ASSERT_EQUAL_INT(10, r.messagesBatched);
  TEST_ASSERT_EQUAL_INT(2, r.turnsEmitted);       // one turn per chat
  TEST_ASSERT_FALSE(r.stopped);                    // stopped because the page ended, not a cap
  TEST_ASSERT_EQUAL_INT32(110, r.ackOffset);      // 109 + 1: this page only
}

// A partial page (fewer than a full window) drains what is there; the offset advances
// past the last update so the next drain fetches only new messages.
static void test_drain_partial_page() {
  MessageBatcher b;
  FakeServer srv;
  addPage(srv, {txt(5, "9", "A", "hi")}, 10);  // 1 update
  DrainResult r = runDrain(b, 5, srv);
  TEST_ASSERT_EQUAL_INT(1, r.turnsEmitted);
  TEST_ASSERT_EQUAL_INT32(6, r.ackOffset);
}

// The per-page no-loss guarantee: if a reset happens after this page's turns ran but
// BEFORE the offset is committed, the persisted offset is unmoved, so re-draining at
// that offset re-serves the WHOLE page (nothing lost). Once the offset is committed,
// re-draining at the committed offset returns only the NEXT page (the page is not
// re-run). This is why the drain is one page at a time with a commit in between.
static void test_drain_uncommitted_page_is_reserved() {
  OffsetServer srv;
  srv.limit = 10;
  for (int i = 0; i < 12; ++i) {   // two pages' worth, two chats so no per-chat cap
    const char* chat = (i % 2 == 0) ? "A" : "B";
    srv.all.push_back(txt(500 + i, chat, "u", ("m" + std::to_string(i)).c_str()));
  }

  // Drain 1 at the persisted offset. Turns run; ackOffset is what WOULD be committed.
  MessageBatcher b1;
  DrainResult r1 = runDrainOffset(b1, 500, srv);
  TEST_ASSERT_EQUAL_INT(10, r1.messagesBatched);
  TEST_ASSERT_EQUAL_INT32(510, r1.ackOffset);

  // Simulate a reset BEFORE the commit: the persisted offset is still 500. Re-draining
  // there must re-serve the exact same page - no message from it is lost.
  MessageBatcher b2;
  DrainResult r2 = runDrainOffset(b2, 500, srv);
  TEST_ASSERT_EQUAL_INT(10, r2.messagesBatched);
  TEST_ASSERT_EQUAL_INT32(510, r2.ackOffset);
  TEST_ASSERT_TRUE(has(rendered(b2, 0), "m0"));   // page 1's first message is back

  // Now the commit succeeds (offset = 510). The next drain returns ONLY the next page;
  // the committed page is never re-run.
  MessageBatcher b3;
  DrainResult r3 = runDrainOffset(b3, r1.ackOffset, srv);
  TEST_ASSERT_EQUAL_INT(2, r3.messagesBatched);   // the remaining 2 (ids 510, 511)
  TEST_ASSERT_EQUAL_INT32(512, r3.ackOffset);
  TEST_ASSERT_FALSE(has(rendered(b3, 0), "m0"));  // page 1 not re-served
}

// A busy deferred slot (voice/attachment) stops the drain WITHOUT acking that update,
// so it and everything after it are re-served next cycle (nothing lost).
static void test_drain_busy_slot_reserves_from_that_update() {
  MessageBatcher b;
  FakeServer srv;
  ClassifiedUpdate busy;
  busy.updateId = 52;
  busy.action = ClassifiedUpdate::Action::StopNoAck;
  addPage(srv, {txt(50, "9", "A", "a"), txt(51, "9", "A", "b"), busy,
               txt(53, "9", "A", "c")}, 10);
  DrainResult r = runDrain(b, 50, srv);
  TEST_ASSERT_TRUE(r.stopped);
  TEST_ASSERT_EQUAL_INT32(52, r.ackOffset);   // acked 50,51; NOT 52 -> re-served
  TEST_ASSERT_EQUAL_INT(1, r.turnsEmitted);
  TEST_ASSERT_EQUAL_INT(2, r.messagesBatched);
}

// Hitting a cap mid-drain stops WITHOUT acking the deferred update; the accumulated
// batch runs as one turn and the rest is re-served (the offset does not pass it).
static void test_drain_cap_reserves_deferred_update() {
  MessageBatcher b;
  FakeServer srv;
  std::vector<ClassifiedUpdate> us;
  // One more than a chat can hold, all in a single (full) page.
  for (size_t i = 0; i <= cap::kBatchMaxMsgsPerChat; ++i)
    us.push_back(txt(200 + static_cast<int32_t>(i), "77", "B", "x"));
  const int32_t deferredId = 200 + static_cast<int32_t>(cap::kBatchMaxMsgsPerChat);
  addPage(srv, std::move(us), 10);
  DrainResult r = runDrain(b, 200, srv);
  TEST_ASSERT_TRUE(r.stopped);
  TEST_ASSERT_EQUAL_INT32(deferredId, r.ackOffset);   // deferred update NOT acked
  TEST_ASSERT_EQUAL_INT(1, r.turnsEmitted);
  TEST_ASSERT_EQUAL_UINT(cap::kBatchMaxMsgsPerChat, (size_t)r.messagesBatched);
  TEST_ASSERT_TRUE(has(rendered(b, 0), "waiting"));   // honest "more waiting" note
}

// A page-0 fetch error accumulates nothing and reports fetchError with the offset
// unmoved, so the caller backs off and re-serves the whole batch.
static void test_drain_page0_error_backs_off() {
  MessageBatcher b;
  FakeServer srv;
  addErrorPage(srv);
  DrainResult r = runDrain(b, 42, srv);
  TEST_ASSERT_TRUE(r.fetchError);
  TEST_ASSERT_EQUAL_INT(0, r.turnsEmitted);
  TEST_ASSERT_EQUAL_INT32(42, r.ackOffset);   // unmoved
}

// Different chats across the drain produce one turn EACH, never merged.
static void test_drain_multi_chat_one_turn_each() {
  MessageBatcher b;
  FakeServer srv;
  addPage(srv, {txt(1, "A", "ua", "a1"), txt(2, "B", "ub", "b1"),
               txt(3, "A", "ua", "a2")}, 10);
  DrainResult r = runDrain(b, 1, srv);
  TEST_ASSERT_EQUAL_INT(2, r.turnsEmitted);
  TEST_ASSERT_EQUAL_STRING("A", b.chatIdAt(0));
  TEST_ASSERT_EQUAL_STRING("B", b.chatIdAt(1));
  TEST_ASSERT_EQUAL_INT32(4, r.ackOffset);
}

// AckOnly updates (pending-approval, disallowed, a queued file) advance the offset
// but are not batched into a turn.
static void test_drain_ackonly_advances_without_turn() {
  MessageBatcher b;
  FakeServer srv;
  ClassifiedUpdate a;
  a.updateId = 70;
  a.action = ClassifiedUpdate::Action::AckOnly;
  addPage(srv, {a, txt(71, "9", "A", "hi")}, 10);
  DrainResult r = runDrain(b, 70, srv);
  TEST_ASSERT_EQUAL_INT(1, r.turnsEmitted);        // only the text update
  TEST_ASSERT_EQUAL_INT(1, r.messagesBatched);
  TEST_ASSERT_EQUAL_INT32(72, r.ackOffset);        // advanced past both
}

// Regression guard (a reviewer-found HIGH): when a text update trips a cap the drain
// stops and NEVER classifies (handle) the updates after it - so a side-effecting
// update (voice / file / pending) sitting after the cap-trip does not run its side
// effect for an update that is then re-served, which would run it twice. This is why
// handle() is called lazily by the driver rather than the whole page classified up
// front. The side effects live in handle(); asserting handle is never invoked past
// the stop point is asserting the side effect never fires there.
static void test_drain_no_side_effect_past_cap_stop() {
  MessageBatcher b;
  FakeServer srv;
  std::vector<ClassifiedUpdate> us;
  for (size_t i = 0; i <= cap::kBatchMaxMsgsPerChat; ++i)   // cap+1 texts, one chat
    us.push_back(txt(300 + static_cast<int32_t>(i), "Z", "u", "x"));
  ClassifiedUpdate after;   // a side-effecting update positioned AFTER the cap-trip
  after.updateId = 400;
  after.action = ClassifiedUpdate::Action::StopNoAck;
  us.push_back(after);
  const int afterIdx = static_cast<int>(us.size()) - 1;
  addPage(srv, std::move(us), 10);

  DrainResult r = runDrain(b, 300, srv);
  TEST_ASSERT_TRUE(r.stopped);
  // Stopped at the deferred (cap+1)th text; its update is NOT acked, and the update
  // after it was never even classified (no handle call -> no side effect).
  TEST_ASSERT_EQUAL_INT32(300 + static_cast<int32_t>(cap::kBatchMaxMsgsPerChat), r.ackOffset);
  for (int idx : srv.handledIdx)
    TEST_ASSERT_TRUE(idx < afterIdx);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_single_message_is_raw_text);
  RUN_TEST(test_burst_same_chat_one_ordered_turn);
  RUN_TEST(test_mixed_chats_partitioned_no_contamination);
  RUN_TEST(test_per_chat_count_cap_defers_never_drops);
  RUN_TEST(test_per_chat_byte_cap_defers);
  RUN_TEST(test_single_oversized_message_never_dropped);
  RUN_TEST(test_total_bytes_cap_across_chats);
  RUN_TEST(test_chat_count_cap);
  RUN_TEST(test_deferred_leaves_state_unchanged);
  RUN_TEST(test_from_preserved_per_message);
  RUN_TEST(test_empty_from_no_clause);
  RUN_TEST(test_clear_resets_budget);
  RUN_TEST(test_fresh_accepts_first_message_always);
  RUN_TEST(test_drain_one_page_per_call);
  RUN_TEST(test_drain_partial_page);
  RUN_TEST(test_drain_uncommitted_page_is_reserved);
  RUN_TEST(test_drain_busy_slot_reserves_from_that_update);
  RUN_TEST(test_drain_cap_reserves_deferred_update);
  RUN_TEST(test_drain_page0_error_backs_off);
  RUN_TEST(test_drain_multi_chat_one_turn_each);
  RUN_TEST(test_drain_ackonly_advances_without_turn);
  RUN_TEST(test_drain_no_side_effect_past_cap_stop);
  return UNITY_END();
}
