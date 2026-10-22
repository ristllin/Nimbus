#include <unity.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "nimbus/orch/caps.h"
#include "nimbus/orch/command.h"
#include "nimbus/orch/media_label.h"
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
  TEST_ASSERT_EQUAL_UINT(1, b.turnCount());
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
  TEST_ASSERT_EQUAL_UINT(1, b.turnCount());
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
  TEST_ASSERT_EQUAL_UINT(2, b.turnCount());
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
  TEST_ASSERT_EQUAL_UINT(created, b.turnCount());
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
  TEST_ASSERT_EQUAL_UINT(cap::kBatchMaxChats, b.turnCount());
  TEST_ASSERT_TRUE(BatchAdd::Deferred == b.add("one-too-many", "u", "x"));
  TEST_ASSERT_EQUAL_UINT(cap::kBatchMaxChats, b.turnCount());  // unchanged
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
  TEST_ASSERT_EQUAL_UINT(0, b.turnCount());
  // The freed budget lets a fresh large message land again (no stale total).
  TEST_ASSERT_TRUE(BatchAdd::Deferred != b.add("Y", "u", big));
  TEST_ASSERT_EQUAL_UINT(1, b.turnCount());
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
    // As the device does: a page holding a slash command drains contiguously.
    for (const auto& u : current) rp.hasCommand = rp.hasCommand || nimbus::orch::isCommandText(u.text);
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


// ---- command boundary ------------------------------------------------------------
// A slash command never shares a turn: inside a "Message 1 of N" block it no longer
// starts with '/', so it reached the model as conversation instead of running (the
// 14 Oct "/skill approve meal-photo-science" that never ran). These pin the class
// over EVERY verb in the lifted table (nimbus/orch/command.h), plus unknown verbs.

using nimbus::orch::kCommandVerbs;

// The owner's burst "one", "two", "/<verb> ...", "four" for EVERY verb in the table:
// three turns in order - the batch, the command byte for byte, then "four".
static void test_burst_splits_at_every_verb() {
  for (const auto& cv : kCommandVerbs) {
    MessageBatcher b;
    const std::string cmd = std::string("/") + cv.verb + " arg1";
    TEST_ASSERT_TRUE(BatchAdd::Accepted == b.add("9", "Ann", "one"));
    TEST_ASSERT_TRUE(BatchAdd::Accepted == b.add("9", "Ann", "two"));
    TEST_ASSERT_TRUE_MESSAGE(BatchAdd::CommandBoundary == b.add("9", "Ann", cmd), cv.verb);
    TEST_ASSERT_TRUE(BatchAdd::Accepted == b.add("9", "Ann", "four"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3, b.turnCount(), cv.verb);
    const std::string t0 = b.render(0).c_str();
    TEST_ASSERT_TRUE(has(t0, "Message 1 of 2") && has(t0, "one") && has(t0, "two"));
    TEST_ASSERT_FALSE(has(t0, cmd.c_str()));
    TEST_ASSERT_TRUE(b.commandAt(1));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(cmd.c_str(), b.render(1).c_str(), cv.verb);
    const nimbus::orch::Command parsed = nimbus::orch::parseCommand(b.render(1).c_str());
    TEST_ASSERT_TRUE(parsed.isCommand);
    TEST_ASSERT_EQUAL_STRING(cv.verb, parsed.verb.c_str());
    TEST_ASSERT_EQUAL_STRING("four", b.render(2).c_str());
    for (size_t i = 0; i < 3; ++i) TEST_ASSERT_EQUAL_STRING("9", b.chatIdAt(i));
  }
}

// Every command form parseCommand accepts is a boundary: unknown verbs (they get the
// deterministic "try /help"), a leading space, Telegram's "@botname" form, upper case.
static void test_every_command_form_is_a_boundary() {
  for (const char* cmd : {"/zzz", " /help", "/loops@Example_bot", "/HELP", "/skill approve x"}) {
    MessageBatcher b;
    b.add("1", "u", "hello");
    TEST_ASSERT_TRUE_MESSAGE(BatchAdd::CommandBoundary == b.add("1", "u", cmd), cmd);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(cmd, b.render(1).c_str(), cmd);
  }
  // Not commands: a mid-sentence slash and a bare slash stay in the batch.
  MessageBatcher b;
  b.add("1", "u", "hello");
  TEST_ASSERT_TRUE(BatchAdd::Accepted == b.add("1", "u", "please run /update for me"));
  TEST_ASSERT_TRUE(BatchAdd::Accepted == b.add("1", "u", "/"));
  TEST_ASSERT_EQUAL_UINT(1, b.turnCount());
}

// A lone command, two commands in a row, and a command per chat: each its own turn.
static void test_commands_alone_and_in_a_row() {
  MessageBatcher b;
  TEST_ASSERT_TRUE(BatchAdd::CommandBoundary == b.add("1", "u", "/loops"));
  TEST_ASSERT_TRUE(BatchAdd::CommandBoundary == b.add("1", "u", "/help"));
  TEST_ASSERT_TRUE(BatchAdd::CommandBoundary == b.add("2", "v", "/help"));
  TEST_ASSERT_EQUAL_UINT(3, b.turnCount());
  TEST_ASSERT_EQUAL_STRING("/loops", b.render(0).c_str());
  TEST_ASSERT_EQUAL_STRING("/help", b.render(1).c_str());
  TEST_ASSERT_EQUAL_STRING("/help", b.render(2).c_str());
  TEST_ASSERT_EQUAL_STRING("2", b.chatIdAt(2));
}

// Caps around a command: when the bucket cap stops a command, the chat's open batch
// says more are waiting; a command's own turn is never marked, so it stays byte for
// byte even when the message after it is deferred.
static void test_caps_never_mark_a_command_turn() {
  MessageBatcher b;
  for (size_t i = 0; i + 1 < cap::kBatchMaxChats; ++i)
    b.add("c" + std::to_string(i), "u", "x");
  TEST_ASSERT_TRUE(BatchAdd::CommandBoundary == b.add("last", "u", "/help"));
  TEST_ASSERT_EQUAL_UINT(cap::kBatchMaxChats, b.turnCount());
  TEST_ASSERT_TRUE(BatchAdd::Deferred == b.add("last", "u", "after"));   // no bucket left
  TEST_ASSERT_FALSE(b.cappedAt(cap::kBatchMaxChats - 1));
  TEST_ASSERT_EQUAL_STRING("/help", b.render(cap::kBatchMaxChats - 1).c_str());
  TEST_ASSERT_TRUE(BatchAdd::Deferred == b.add("c0", "u", "/loops"));   // command deferred
  TEST_ASSERT_TRUE(b.cappedAt(0));                                       // its batch says so
  TEST_ASSERT_TRUE(has(b.render(0).c_str(), "waiting"));
}

// Contiguous mode (a page that holds a command): a message joins only the drain's
// newest bucket, so every bucket is one contiguous run of update ids and the drain can
// commit after each. Off (no command on the page), chats still fold into one turn each.
static void test_contiguous_mode_keeps_id_runs() {
  MessageBatcher b;
  b.setContiguous(true);
  b.add("A", "u", "a1", 11);
  b.add("B", "u", "b1", 12);
  b.add("A", "u", "a2", 13);
  b.add("A", "u", "a3", 14);
  TEST_ASSERT_EQUAL_UINT(3, b.turnCount());
  TEST_ASSERT_EQUAL_INT32(11, b.firstIdAt(0));
  TEST_ASSERT_EQUAL_INT32(12, b.firstIdAt(1));
  TEST_ASSERT_EQUAL_INT32(13, b.firstIdAt(2));
  TEST_ASSERT_EQUAL_UINT(2, b.messageCountAt(2));
  b.clear();   // clear() turns contiguous mode off again
  b.add("A", "u", "a1", 11);
  b.add("B", "u", "b1", 12);
  b.add("A", "u", "a2", 13);
  TEST_ASSERT_EQUAL_UINT(2, b.turnCount());
}

// ---- the burst property --------------------------------------------------------------
// Random sequences of plain texts and commands (every table verb, unknown verbs, the
// other accepted forms) from 1-3 chats, drained through the real poll cycle
// (drainCycle over the offset cursor) until Telegram is empty. For every sequence:
//   - every command is a turn of its own, rendered byte for byte as it was sent;
//   - no other turn carries a command;
//   - each chat's messages come out in the order they were sent;
//   - every update is dispatched exactly once;
//   - the extra poll cycles against the same sequence with the commands sent as plain
//     text are at most the number of commands.

struct Rng {
  uint32_t s;
  uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
  uint32_t below(uint32_t n) { return next() % n; }
};

struct Sent {
  int32_t     id;
  std::string chat;
  std::string text;
  std::string token;   // unique, appears in the rendered turn
  bool        command;
};

static std::vector<Sent> randomBurst(Rng& r, bool commandsAsText) {
  std::vector<Sent> out;
  const int chats = 1 + (int)r.below(3);
  const int n = 1 + (int)r.below(24);
  for (int i = 0; i < n; ++i) {
    Sent m;
    m.id = 500 + i;
    m.chat = std::to_string(1 + r.below((uint32_t)chats));
    m.token = "k" + std::to_string(i) + "q";
    m.command = r.below(3) == 0;
    if (m.command) {
      static const char* kForms[] = {"/%s %s", " /%s %s", "/%s@Example_bot %s", "/%s %s"};
      const uint32_t pick = r.below((uint32_t)nimbus::orch::kCommandVerbCount + 1);
      std::string verb = pick < nimbus::orch::kCommandVerbCount ? kCommandVerbs[pick].verb : "zzz";
      if (r.below(5) == 0) for (char& ch : verb) ch = (char)toupper((unsigned char)ch);
      char buf[96];
      std::snprintf(buf, sizeof buf, kForms[r.below(4)], verb.c_str(), m.token.c_str());
      m.text = buf;
      if (commandsAsText) { m.text = "say " + m.token; m.command = false; }
    } else {
      m.text = "msg " + m.token;
    }
    out.push_back(m);
  }
  return out;
}

struct Turn {
  std::string chat;
  std::string text;
  bool        command;
};

// Drain `msgs` to empty through drainCycle; returns the poll cycles it took.
static int drainAll(const std::vector<Sent>& msgs, std::vector<Turn>& turns) {
  OffsetServer srv;
  srv.limit = 10;
  for (const Sent& m : msgs) srv.all.push_back(txt(m.id, m.chat.c_str(), "u", m.text.c_str()));
  nimbus::core::TgOffsetCursor cursor;
  cursor.seed(500);
  MessageBatcher b;
  int cycles = 0;
  for (; cycles < 100; ++cycles) {
    bool any = false;
    nimbus::orch::drainCycle(
        b, cursor, [&](int32_t o) { return srv.fetch(o); }, [&](int i) { return srv.handle(i); },
        [&](size_t i) {
          any = true;
          turns.push_back({b.chatIdAt(i), b.render(i).c_str(), b.commandAt(i)});
        },
        [] {});
    if (!any) break;
  }
  return cycles;
}

static std::vector<std::string> tokensIn(const std::string& t) {
  std::vector<std::string> out;
  for (size_t p = t.find('k'); p != std::string::npos; p = t.find('k', p + 1)) {
    size_t q = p + 1;
    while (q < t.size() && isdigit((unsigned char)t[q])) ++q;
    if (q > p + 1 && q < t.size() && t[q] == 'q') out.push_back(t.substr(p, q - p + 1));
  }
  return out;
}

static void test_burst_property_commands_are_their_own_turns() {
  Rng rng{0x5eed1234u};
  int withCommands = 0;
  for (int trial = 0; trial < 3000; ++trial) {
    const uint32_t seed = rng.next();
    Rng r1{seed}, r2{seed};
    const std::vector<Sent> msgs = randomBurst(r1, false);
    const std::vector<Sent> plain = randomBurst(r2, true);
    std::vector<Turn> turns, plainTurns;
    const int cycles = drainAll(msgs, turns);
    const int baseline = drainAll(plain, plainTurns);
    int commands = 0;
    std::map<std::string, const Sent*> byToken;
    std::map<std::string, std::vector<std::string>> sentOrder, gotOrder;
    for (const Sent& m : msgs) {
      byToken[m.token] = &m;
      sentOrder[m.chat].push_back(m.token);
      commands += m.command ? 1 : 0;
    }
    withCommands += commands ? 1 : 0;
    char ctx[64];
    std::snprintf(ctx, sizeof ctx, "trial %d seed 0x%08x", trial, (unsigned)seed);
    std::map<std::string, int> seen;
    for (const Turn& t : turns) {
      const std::vector<std::string> toks = tokensIn(t.text);
      TEST_ASSERT_FALSE_MESSAGE(toks.empty(), ctx);
      for (const std::string& k : toks) {
        const Sent* m = byToken.at(k);
        seen[k]++;
        TEST_ASSERT_EQUAL_STRING_MESSAGE(m->chat.c_str(), t.chat.c_str(), ctx);
        gotOrder[t.chat].push_back(k);
        if (m->command) {   // its own turn, byte for byte
          TEST_ASSERT_TRUE_MESSAGE(t.command, ctx);
          TEST_ASSERT_EQUAL_UINT_MESSAGE(1, toks.size(), ctx);
          TEST_ASSERT_EQUAL_STRING_MESSAGE(m->text.c_str(), t.text.c_str(), ctx);
        } else {
          TEST_ASSERT_FALSE_MESSAGE(t.command, ctx);
        }
      }
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(msgs.size(), seen.size(), ctx);   // none lost
    for (const auto& kv : seen) TEST_ASSERT_EQUAL_INT_MESSAGE(1, kv.second, ctx);   // once
    for (const auto& kv : sentOrder)                                                // order
      TEST_ASSERT_TRUE_MESSAGE(gotOrder[kv.first] == kv.second, ctx);
    TEST_ASSERT_TRUE_MESSAGE(cycles <= baseline + commands, ctx);
  }
  TEST_ASSERT_TRUE(withCommands > 1500);   // the generator really exercised commands
}

// The design choice the property above pins (orchestrator ruling, 18 Oct): a command
// gets a sealed turn INSIDE the same drain. "text, /cmd, text" from one chat drains in
// ONE poll cycle as three turns. The rejected design (stop the drain before the
// command, re-serve it next cycle, then stop again after it) needs three cycles here
// and fails this test as well as the extra-cycles bound of the property.
static void test_command_mid_burst_costs_no_extra_cycle() {
  std::vector<Sent> msgs = {{700, "5", "msg k0q", "k0q", false},
                            {701, "5", "/skill approve k1q", "k1q", true},
                            {702, "5", "msg k2q", "k2q", false}};
  std::vector<Turn> turns;
  TEST_ASSERT_EQUAL_INT(1, drainAll(msgs, turns));
  TEST_ASSERT_EQUAL_UINT(3, turns.size());
  TEST_ASSERT_EQUAL_STRING("msg k0q", turns[0].text.c_str());
  TEST_ASSERT_TRUE(turns[1].command);
  TEST_ASSERT_EQUAL_STRING("/skill approve k1q", turns[1].text.c_str());
  TEST_ASSERT_EQUAL_STRING("msg k2q", turns[2].text.c_str());
}

// ---- albums and bursts as one turn (lane D) -----------------------------------

using nimbus::orch::MediaClass;
using nimbus::orch::MediaLabel;
using nimbus::orch::mediaSegment;
using nimbus::orch::SegKind;

// What the device does after the drain: give every media segment of bucket i its
// label (k/N among the turn's media) and description. Returns the labels it set.
static std::vector<std::string> describeAll(MessageBatcher& b, size_t i) {
  std::vector<std::string> out;
  const size_t n = b.mediaCountAt(i);
  for (size_t k = 0; k < n; ++k) {
    MediaLabel l;
    l.k = (int)k + 1;
    l.n = (int)n;
    char ref[17];
    std::snprintf(ref, sizeof ref, "%016x", 0xabc000 + b.mediaSlotAt(i, k));
    l.ref = ref;
    l.detail = "photo in slot " + std::to_string(b.mediaSlotAt(i, k));
    out.push_back(mediaSegment(l));
    b.setMediaText(i, k, out.back());
  }
  return out;
}

// The 15 Oct burst (photo 1 with the caption, photos 2 and 3, then a text): ONE turn,
// interleaved in arrival order, every photo numbered k/3 with its own description,
// the caption with its photo at full length, the text after the third photo.
static void test_album_and_text_render_as_one_interleaved_turn() {
  MessageBatcher b;
  TEST_ASSERT_TRUE(BatchAdd::Accepted == b.addMedia("77", "Ana", "All of this for dinner", 0, 501));
  TEST_ASSERT_TRUE(BatchAdd::Accepted == b.addMedia("77", "Ana", "", 1, 502));
  TEST_ASSERT_TRUE(BatchAdd::Accepted == b.addMedia("77", "Ana", "", 2, 503));
  TEST_ASSERT_TRUE(BatchAdd::Accepted == b.add("77", "Ana", "I sent 3 images. Did you see all?", 504));
  TEST_ASSERT_EQUAL_UINT(1, b.turnCount());
  TEST_ASSERT_EQUAL_UINT(4, b.messageCountAt(0));
  TEST_ASSERT_EQUAL_UINT(3, b.mediaCountAt(0));
  TEST_ASSERT_EQUAL_UINT(3, b.mediaInDrain());
  for (size_t k = 0; k < 3; ++k) {
    TEST_ASSERT_EQUAL_INT((int)k, b.mediaSlotAt(0, k));
    TEST_ASSERT_EQUAL_INT32(501 + (int32_t)k, b.mediaUpdateIdAt(0, k));
  }
  TEST_ASSERT_EQUAL_STRING("All of this for dinner", b.mediaCaptionAt(0, 0));
  const std::vector<std::string> labels = describeAll(b, 0);
  const std::string r = b.render(0).c_str();
  const size_t p1 = r.find("[image 1/3 ref="), p2 = r.find("[image 2/3 ref="), p3 = r.find("[image 3/3 ref=");
  const size_t cap = r.find("They wrote: All of this for dinner"), txt = r.find("I sent 3 images");
  TEST_ASSERT_TRUE(p1 != std::string::npos && p2 != std::string::npos && p3 != std::string::npos);
  TEST_ASSERT_TRUE(p1 < cap && cap < p2 && p2 < p3 && p3 < txt);   // arrival order
  TEST_ASSERT_TRUE(has(r, "Message 4 of 4 from Ana:"));
  for (const std::string& l : labels) TEST_ASSERT_TRUE(has(r, l.c_str()));
  // The turn that contains the follow-up text contains all three photo segments.
  TEST_ASSERT_TRUE(r.find("photo in slot 2") < txt);
}

// A single photo (no album, no group id, a fresh chat) renders exactly as the
// pre-batching photo turn did: its label and description, then "They wrote:".
static void test_single_photo_renders_like_before() {
  MessageBatcher b;
  TEST_ASSERT_TRUE(BatchAdd::Accepted == b.addMedia("5", "Bo", "what is this", 0, 9));
  describeAll(b, 0);
  const std::string r = b.render(0).c_str();
  TEST_ASSERT_EQUAL_STRING("[image 1/1 ref=0000000000abc000] photo in slot 0\n\nThey wrote: what is this",
                           r.c_str());
  MessageBatcher bare;
  bare.addMedia("5", "Bo", "", 0, 9);
  describeAll(bare, 0);
  TEST_ASSERT_EQUAL_STRING("[image 1/1 ref=0000000000abc000] photo in slot 0", bare.render(0).c_str());
}

// The 10/11 cap: a 10-photo album is one turn; the 11th item is Deferred (re-served
// next drain, never dropped), and the turn says more are waiting.
static void test_ten_media_one_turn_eleventh_deferred() {
  MessageBatcher b;
  for (int i = 0; i < (int)cap::kBatchMaxMediaPerChat; ++i)
    TEST_ASSERT_TRUE(BatchAdd::Deferred != b.addMedia("77", "Ana", "", i, 600 + i));
  TEST_ASSERT_EQUAL_UINT(1, b.turnCount());
  TEST_ASSERT_EQUAL_UINT(cap::kBatchMaxMediaPerChat, b.mediaCountAt(0));
  TEST_ASSERT_TRUE(BatchAdd::Deferred == b.addMedia("77", "Ana", "", 10, 610));
  TEST_ASSERT_EQUAL_UINT(cap::kBatchMaxMediaPerChat, b.mediaCountAt(0));
  TEST_ASSERT_TRUE(b.cappedAt(0));
  describeAll(b, 0);
  const std::string r = b.render(0).c_str();
  TEST_ASSERT_TRUE(has(r, "[image 10/10 ref="));
  TEST_ASSERT_TRUE(has(r, "More messages from this chat are waiting"));
}

// Counter-test for the raised media limits: the drain's slots are a hard ceiling
// across chats too. Two chats of 6 photos each: the 11th item (chat B's 5th) is
// Deferred, as is anything needing a slot after it (a voice note); text still fits.
static void test_media_slots_cap_the_drain_across_chats() {
  MessageBatcher b;
  int slot = 0;
  for (int i = 0; i < 6; ++i) TEST_ASSERT_TRUE(BatchAdd::Deferred != b.addMedia("A", "a", "", slot++, 100 + i));
  for (int i = 0; i < 4; ++i) TEST_ASSERT_TRUE(BatchAdd::Deferred != b.addMedia("B", "b", "", slot++, 200 + i));
  TEST_ASSERT_EQUAL_UINT(cap::kBatchMaxMediaPerDrain, b.mediaInDrain());
  TEST_ASSERT_TRUE(BatchAdd::Deferred == b.addMedia("B", "b", "", slot, 204));
  TEST_ASSERT_TRUE(BatchAdd::Deferred == b.addVoice("C", "c", slot, 300));
  TEST_ASSERT_TRUE(BatchAdd::Deferred != b.add("C", "c", "text needs no slot", 301));
  TEST_ASSERT_EQUAL_UINT(cap::kBatchMaxMediaPerDrain, b.mediaInDrain());
  b.clear();
  TEST_ASSERT_EQUAL_UINT(0, b.mediaInDrain());
}

// A command next to media is still a turn of its own, and the media around it keep
// their own turns in order.
static void test_command_boundary_next_to_media() {
  MessageBatcher b;
  b.addMedia("77", "Ana", "", 0, 1);
  TEST_ASSERT_TRUE(BatchAdd::CommandBoundary == b.add("77", "Ana", "/status", 2));
  b.addMedia("77", "Ana", "", 1, 3);
  TEST_ASSERT_EQUAL_UINT(3, b.turnCount());
  TEST_ASSERT_EQUAL_UINT(1, b.mediaCountAt(0));
  TEST_ASSERT_TRUE(b.commandAt(1));
  TEST_ASSERT_EQUAL_STRING("/status", b.render(1).c_str());
  TEST_ASSERT_EQUAL_UINT(1, b.mediaCountAt(2));
  TEST_ASSERT_EQUAL_INT(1, b.mediaSlotAt(2, 0));
}

// The voice boundary: photos, a voice note, a text. The voice note is a turn of its own
// between them (its transcript), so replies come in the order things were sent.
static void test_voice_boundary_next_to_media() {
  MessageBatcher b;
  b.addMedia("77", "Ana", "lunch", 0, 1);
  b.addMedia("77", "Ana", "", 1, 2);
  TEST_ASSERT_TRUE(BatchAdd::VoiceBoundary == b.addVoice("77", "Ana", 2, 3));
  TEST_ASSERT_TRUE(BatchAdd::Accepted == b.add("77", "Ana", "and a coffee", 4));
  TEST_ASSERT_EQUAL_UINT(3, b.turnCount());
  TEST_ASSERT_EQUAL_UINT(2, b.mediaCountAt(0));
  TEST_ASSERT_TRUE(b.voiceAt(1));
  TEST_ASSERT_EQUAL_INT(2, b.voiceSlotAt(1));
  TEST_ASSERT_EQUAL_UINT(0, b.mediaCountAt(1));   // a voice note is not a media segment
  b.setVoiceText(1, "it was a big salad");
  TEST_ASSERT_EQUAL_STRING("it was a big salad", b.render(1).c_str());
  TEST_ASSERT_EQUAL_STRING("and a coffee", b.render(2).c_str());
  TEST_ASSERT_EQUAL_UINT(3, b.mediaInDrain());   // the voice note holds a slot too
  // Two voice notes in a row are two turns; a voice note never joins a text bucket.
  MessageBatcher v;
  v.add("9", "x", "first");
  v.addVoice("9", "x", 0, 2);
  v.addVoice("9", "x", 1, 3);
  TEST_ASSERT_EQUAL_UINT(3, v.turnCount());
}

// Full-length captions: the old 256-byte cut is gone (a 3000-byte caption reaches the
// turn whole); counter-test: a caption past the per-chat byte budget still defers the
// item to the next drain instead of cutting or dropping it.
static void test_full_length_captions_and_their_cap() {
  MessageBatcher b;
  const std::string longCap(3000, 'c');
  TEST_ASSERT_TRUE(BatchAdd::Accepted == b.addMedia("77", "Ana", longCap, 0, 1));
  describeAll(b, 0);
  TEST_ASSERT_TRUE(has(b.render(0).c_str(), longCap.c_str()));
  const std::string big(cap::kBatchMaxBytesPerChat, 'd');
  TEST_ASSERT_TRUE(BatchAdd::Deferred == b.addMedia("77", "Ana", big, 1, 2));
  TEST_ASSERT_EQUAL_UINT(1, b.mediaCountAt(0));
  TEST_ASSERT_EQUAL_UINT(1, b.mediaInDrain());
  // Alone in a fresh drain the same caption fits (a single message is never stuck).
  MessageBatcher fresh;
  TEST_ASSERT_TRUE(BatchAdd::Deferred != fresh.addMedia("77", "Ana", big, 1, 2));
}

// A late or redelivered text carries its original send time; an on-time one is
// byte-for-byte unchanged.
static void test_late_text_carries_its_send_time() {
  MessageBatcher b;
  b.add("77", "Ana", "are you there?", 1, "09:58");
  TEST_ASSERT_EQUAL_STRING("(sent 09:58) are you there?", b.render(0).c_str());
  b.add("77", "Ana", "hello", 2);
  const std::string r = b.render(0).c_str();
  TEST_ASSERT_TRUE(has(r, "Message 1 of 2 from Ana:\n(sent 09:58) are you there?"));
  TEST_ASSERT_TRUE(has(r, "Message 2 of 2 from Ana:\nhello"));
}

// The largest turn the batcher can build stays well under the 64 KB single-string
// limit of the device's JSON writer: a full album at the description cap plus the
// per-chat caption budget.
static void test_largest_turn_stays_bounded() {
  MessageBatcher b;
  const std::string capText(cap::kBatchMaxBytesPerChat / cap::kBatchMaxMediaPerChat, 'q');
  for (int i = 0; i < (int)cap::kBatchMaxMediaPerChat; ++i) b.addMedia("77", "Somebody Long Name", capText, i, i + 1);
  for (size_t k = 0; k < b.mediaCountAt(0); ++k) {
    MediaLabel l;
    l.k = (int)k + 1;
    l.n = 10;
    l.ref = "0123456789abcdef";
    l.sentAt = "21 Oct 09:58";
    l.detail = std::string(5 * nimbus::orch::kMediaTextMax, 'z');
    b.setMediaText(0, k, mediaSegment(l));
  }
  const size_t n = b.render(0).size();
  TEST_ASSERT_TRUE(n > cap::kBatchMaxMediaPerChat * nimbus::orch::kMediaTextMax);
  TEST_ASSERT_TRUE_MESSAGE(n < 40 * 1024, "one turn must stay far below the 64 KB string cap");
}

// The drain routes media and voice actions into the batcher (addClassified); a media
// item the device had no slot for (StopNoAck) stops the drain un-acked.
static void test_drain_routes_media_and_voice() {
  FakeServer srv;
  ClassifiedUpdate m1 = txt(10, "77", "Ana", "caption");
  m1.action = ClassifiedUpdate::Action::BatchMedia;
  m1.slot = 0;
  ClassifiedUpdate m2 = txt(11, "77", "Ana", "");
  m2.action = ClassifiedUpdate::Action::BatchMedia;
  m2.slot = 1;
  ClassifiedUpdate v = txt(12, "77", "Ana", "");
  v.action = ClassifiedUpdate::Action::BatchVoice;
  v.slot = 2;
  ClassifiedUpdate stop;
  stop.updateId = 13;
  stop.action = ClassifiedUpdate::Action::StopNoAck;
  addPage(srv, {m1, m2, v, stop, txt(14, "77", "Ana", "never reached")});
  MessageBatcher b;
  const DrainResult r = runDrain(b, 10, srv);
  TEST_ASSERT_TRUE(r.stopped);
  TEST_ASSERT_EQUAL_INT32(13, r.ackOffset);   // acked through the voice note, not past the stop
  TEST_ASSERT_EQUAL_INT(3, r.messagesBatched);
  TEST_ASSERT_EQUAL_UINT(2, b.turnCount());
  TEST_ASSERT_EQUAL_UINT(2, b.mediaCountAt(0));
  TEST_ASSERT_EQUAL_STRING("caption", b.mediaCaptionAt(0, 0));
  TEST_ASSERT_TRUE(b.voiceAt(1));
  TEST_ASSERT_EQUAL_UINT(4, srv.handledIdx.size());   // never classified past the stop
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
  RUN_TEST(test_burst_splits_at_every_verb);
  RUN_TEST(test_every_command_form_is_a_boundary);
  RUN_TEST(test_commands_alone_and_in_a_row);
  RUN_TEST(test_caps_never_mark_a_command_turn);
  RUN_TEST(test_contiguous_mode_keeps_id_runs);
  RUN_TEST(test_burst_property_commands_are_their_own_turns);
  RUN_TEST(test_command_mid_burst_costs_no_extra_cycle);
  RUN_TEST(test_album_and_text_render_as_one_interleaved_turn);
  RUN_TEST(test_single_photo_renders_like_before);
  RUN_TEST(test_ten_media_one_turn_eleventh_deferred);
  RUN_TEST(test_media_slots_cap_the_drain_across_chats);
  RUN_TEST(test_command_boundary_next_to_media);
  RUN_TEST(test_voice_boundary_next_to_media);
  RUN_TEST(test_full_length_captions_and_their_cap);
  RUN_TEST(test_late_text_carries_its_send_time);
  RUN_TEST(test_largest_turn_stays_bounded);
  RUN_TEST(test_drain_routes_media_and_voice);
  return UNITY_END();
}
