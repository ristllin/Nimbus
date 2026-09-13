#include <unity.h>

#include <cstring>
#include <functional>
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
using nimbus::orch::DrainPage;
using nimbus::orch::DrainResult;
using nimbus::orch::DrainTurn;
using nimbus::orch::drainPages;

static ClassifiedUpdate txt(int32_t id, const char* chat, const char* from, const char* text) {
  ClassifiedUpdate u;
  u.updateId = id;
  u.action = ClassifiedUpdate::Action::BatchText;
  u.chatId = chat;
  u.from = from;
  u.text = text;
  return u;
}

// A canned getUpdates server: replays pre-loaded pages in order and records the
// offset each fetch asked for (so a test can prove pagination advanced correctly).
struct FakeServer {
  std::vector<DrainPage> pages;
  std::vector<int32_t> offsetsSeen;
  size_t next = 0;
  DrainPage operator()(int32_t offset, bool /*first*/) {
    offsetsSeen.push_back(offset);
    if (next < pages.size()) return pages[next++];
    DrainPage empty;
    empty.rawCount = 0;
    empty.limit = 10;
    return empty;
  }
};

static DrainPage page(std::vector<ClassifiedUpdate> us, int limit = 10) {
  DrainPage p;
  p.rawCount = static_cast<int>(us.size());
  p.limit = limit;
  p.updates = std::move(us);
  return p;
}

// A backlog that spans more than one full getUpdates page is drained fully before
// any turn runs, then emitted as one turn PER chat (bounded so a single chat never
// exceeds its cap: the page is split across two chats), in arrival order, with the
// committed offset past the last update.
static void test_drain_paginates_until_empty() {
  MessageBatcher b;
  FakeServer srv;
  // Page 1: a full page (rawCount == limit) so the driver must paginate; alternate
  // two chats so neither reaches the per-chat cap while draining across pages.
  std::vector<ClassifiedUpdate> p1;
  for (int i = 0; i < 10; ++i) {
    const char* chat = (i % 2 == 0) ? "A" : "B";
    p1.push_back(txt(100 + i, chat, "u", ("m" + std::to_string(i)).c_str()));
  }
  srv.pages.push_back(page(std::move(p1), 10));
  // Page 2: partial (rawCount < limit) -> server drained.
  srv.pages.push_back(page({txt(110, "A", "u", "m10"), txt(111, "B", "u", "m11")}, 10));

  std::vector<DrainTurn> turns;
  DrainResult r = drainPages(b, 100, std::ref(srv), turns);

  TEST_ASSERT_EQUAL_INT(2, r.pagesFetched);
  TEST_ASSERT_EQUAL_INT(2, r.turnsEmitted);       // one turn PER chat
  TEST_ASSERT_EQUAL_INT(12, r.messagesBatched);   // whole backlog drained
  TEST_ASSERT_FALSE(r.stopped);
  TEST_ASSERT_EQUAL_INT32(112, r.ackOffset);      // 111 + 1
  // Pagination advanced the offset for page 2 (100, then past page 1's last id 109).
  TEST_ASSERT_EQUAL_UINT(2, srv.offsetsSeen.size());
  TEST_ASSERT_EQUAL_INT32(100, srv.offsetsSeen[0]);
  TEST_ASSERT_EQUAL_INT32(110, srv.offsetsSeen[1]);
  // Chat A's turn carries its own messages across BOTH pages, in arrival order.
  TEST_ASSERT_EQUAL_STRING("A", turns[0].chatId.c_str());
  TEST_ASSERT_TRUE(has(turns[0].text, "m0"));
  TEST_ASSERT_TRUE(has(turns[0].text, "m10"));
  TEST_ASSERT_TRUE(turns[0].text.find("m0") < turns[0].text.find("m10"));
}

// A first partial page stops immediately (no needless extra fetch).
static void test_drain_stops_when_server_drained() {
  MessageBatcher b;
  FakeServer srv;
  srv.pages.push_back(page({txt(5, "9", "A", "hi")}, 10));  // 1 < limit -> drained
  std::vector<DrainTurn> turns;
  DrainResult r = drainPages(b, 5, std::ref(srv), turns);
  TEST_ASSERT_EQUAL_INT(1, r.pagesFetched);
  TEST_ASSERT_EQUAL_INT(1, r.turnsEmitted);
  TEST_ASSERT_EQUAL_INT32(6, r.ackOffset);
}

// A busy deferred slot (voice/attachment) stops the drain WITHOUT acking that update,
// so it and everything after it are re-served next cycle (nothing lost).
static void test_drain_busy_slot_reserves_from_that_update() {
  MessageBatcher b;
  FakeServer srv;
  ClassifiedUpdate busy;
  busy.updateId = 52;
  busy.action = ClassifiedUpdate::Action::StopNoAck;
  srv.pages.push_back(page({txt(50, "9", "A", "a"), txt(51, "9", "A", "b"), busy,
                            txt(53, "9", "A", "c")}, 10));
  std::vector<DrainTurn> turns;
  DrainResult r = drainPages(b, 50, std::ref(srv), turns);
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
  srv.pages.push_back(page(std::move(us), 10));
  std::vector<DrainTurn> turns;
  DrainResult r = drainPages(b, 200, std::ref(srv), turns);
  TEST_ASSERT_TRUE(r.stopped);
  TEST_ASSERT_EQUAL_INT32(deferredId, r.ackOffset);   // deferred update NOT acked
  TEST_ASSERT_EQUAL_INT(1, r.turnsEmitted);
  TEST_ASSERT_EQUAL_UINT(cap::kBatchMaxMsgsPerChat, (size_t)r.messagesBatched);
  TEST_ASSERT_TRUE(has(turns[0].text, "waiting"));    // honest "more waiting" note
}

// A page-0 fetch error accumulates nothing and reports fetchError with the offset
// unmoved, so the caller backs off and re-serves the whole batch.
static void test_drain_page0_error_backs_off() {
  MessageBatcher b;
  FakeServer srv;
  DrainPage err;
  err.fetchError = true;
  srv.pages.push_back(err);
  std::vector<DrainTurn> turns;
  DrainResult r = drainPages(b, 42, std::ref(srv), turns);
  TEST_ASSERT_TRUE(r.fetchError);
  TEST_ASSERT_EQUAL_INT(0, r.turnsEmitted);
  TEST_ASSERT_EQUAL_INT32(42, r.ackOffset);   // unmoved
}

// Different chats across the drain produce one turn EACH, never merged.
static void test_drain_multi_chat_one_turn_each() {
  MessageBatcher b;
  FakeServer srv;
  srv.pages.push_back(page({txt(1, "A", "ua", "a1"), txt(2, "B", "ub", "b1"),
                            txt(3, "A", "ua", "a2")}, 10));
  std::vector<DrainTurn> turns;
  DrainResult r = drainPages(b, 1, std::ref(srv), turns);
  TEST_ASSERT_EQUAL_INT(2, r.turnsEmitted);
  TEST_ASSERT_EQUAL_STRING("A", turns[0].chatId.c_str());
  TEST_ASSERT_EQUAL_STRING("B", turns[1].chatId.c_str());
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
  srv.pages.push_back(page({a, txt(71, "9", "A", "hi")}, 10));
  std::vector<DrainTurn> turns;
  DrainResult r = drainPages(b, 70, std::ref(srv), turns);
  TEST_ASSERT_EQUAL_INT(1, r.turnsEmitted);        // only the text update
  TEST_ASSERT_EQUAL_INT(1, r.messagesBatched);
  TEST_ASSERT_EQUAL_INT32(72, r.ackOffset);        // advanced past both
}

// The pagination is bounded: a server that always returns a full page stops at the
// page ceiling instead of looping forever.
static void test_drain_page_ceiling_bounds_pagination() {
  MessageBatcher b;
  FakeServer srv;
  for (int pg = 0; pg < cap::kBatchMaxPages + 5; ++pg) {
    // Each page is "full" (rawCount == limit) but only 1 update to keep it under the
    // per-chat cap across pages: spread across many chats so nothing defers.
    srv.pages.push_back(page({txt(1000 + pg, ("c" + std::to_string(pg % 4)).c_str(), "u", "x")}, 1));
  }
  std::vector<DrainTurn> turns;
  DrainResult r = drainPages(b, 1000, std::ref(srv), turns);
  TEST_ASSERT_EQUAL_INT(cap::kBatchMaxPages, r.pagesFetched);  // bounded, not infinite
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
  RUN_TEST(test_drain_paginates_until_empty);
  RUN_TEST(test_drain_stops_when_server_drained);
  RUN_TEST(test_drain_busy_slot_reserves_from_that_update);
  RUN_TEST(test_drain_cap_reserves_deferred_update);
  RUN_TEST(test_drain_page0_error_backs_off);
  RUN_TEST(test_drain_multi_chat_one_turn_each);
  RUN_TEST(test_drain_ackonly_advances_without_turn);
  RUN_TEST(test_drain_page_ceiling_bounds_pagination);
  return UNITY_END();
}
