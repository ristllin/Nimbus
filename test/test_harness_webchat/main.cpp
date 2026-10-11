#include <unity.h>

#include <ArduinoJson.h>

#include <set>
#include <string>
#include <vector>

#include "nimbus/harness/web_chat.h"
#include "nimbus/orch/msg_batch.h"

// CUM-482 / CUM-507 suite - per-turn web chat reply pairing, host-tested.
//
// The field bug: GET /api/chat served ONE slot that every delivery to chat "web"
// overwrote. A research turn's synthesis (delivered minutes later, between turns)
// was handed to the NEXT turn's first poll; a failover notice or an "On it." ack
// took the reply's place; off by one from then on. These tests drive the device's
// real pairing book and the real /api/chat route bodies (the device handlers are
// thin adapters over agent::chatPostJson / agent::chatGetJson).

using agent::WebChatBook;
using State = agent::WebChatBook::State;

void setUp() {}
void tearDown() {}

// ---- the device flow, as the book sees it -----------------------------------
// POST books + injects; tg_poll begins the turn with the (possibly batched)
// text, the turn delivers, the turn ends. Deliveries outside begin/end are async.
static std::string post(WebChatBook& b, const std::string& text, uint32_t now = 1000) {
  return b.open(text, now);
}

static JsonDocument parse(const std::string& body) {
  JsonDocument d;
  TEST_ASSERT_TRUE_MESSAGE(deserializeJson(d, body) == DeserializationError::Ok, body.c_str());
  return d;
}

// GET /api/chat?turn=<id> through the real route body, wired to the book.
static JsonDocument getTurn(WebChatBook& b, const std::string& id, uint32_t now = 2000) {
  agent::ChatGetRequest rq;
  rq.hasTurn = true;
  rq.turn = id;
  agent::ChatGetDeps deps;
  deps.view = [&b, now](const std::string& t) { return b.view(t, now); };
  deps.activity = [&b](uint32_t& a, int& busy) { a = b.asyncSeq(); busy = 0; };
  return parse(agent::chatGetJson(rq, deps));
}

// ---- the DoD harness test ---------------------------------------------------

// "A delivery between two turns is not returned for the second turn" - the exact
// CUM-482 field sequence: a research turn replies at once, its synthesis lands
// minutes later with nobody polling, then the follow-up is sent and polled.
static void test_delivery_between_two_turns_is_not_the_second_turns_reply() {
  WebChatBook b(7);
  const std::string t1 = post(b, "research EU timber-frame fire rules, cite sources");
  TEST_ASSERT_EQUAL(1, b.begin("research EU timber-frame fire rules, cite sources"));
  b.deliver("I've started a multi-source review.");
  b.end();
  TEST_ASSERT_EQUAL_STRING("I've started a multi-source review.",
                           getTurn(b, t1)["reply"].as<std::string>().c_str());

  const uint32_t asyncBefore = b.asyncSeq();
  b.deliver("## EU timber-frame fire rules - 3-storey residential ...");   // the synthesis
  TEST_ASSERT_EQUAL_UINT32(asyncBefore + 1, b.asyncSeq());

  const std::string t2 = post(b, "On the Germany row: which document sets that 7 m threshold?");
  // The first poll (1.5 s after POST, the turn not yet started): pending, and
  // NEVER the synthesis that is sitting in the old slot.
  JsonDocument first = getTurn(b, t2);
  TEST_ASSERT_TRUE(first["pending"].as<bool>());
  TEST_ASSERT_EQUAL_STRING("", first["reply"].as<std::string>().c_str());
  TEST_ASSERT_EQUAL(1, b.begin("On the Germany row: which document sets that 7 m threshold?"));
  JsonDocument mid = getTurn(b, t2);
  TEST_ASSERT_TRUE(mid["pending"].as<bool>());
  b.deliver("The 7 m threshold comes from section 2(3) of the Musterbauordnung.");
  b.end();
  JsonDocument ans = getTurn(b, t2);
  TEST_ASSERT_FALSE(ans["pending"].as<bool>());
  TEST_ASSERT_TRUE(ans["done"].as<bool>());
  TEST_ASSERT_EQUAL_STRING("The 7 m threshold comes from section 2(3) of the Musterbauordnung.",
                           ans["reply"].as<std::string>().c_str());
  // ...and the first turn's answer is untouched by everything after it.
  TEST_ASSERT_EQUAL_STRING("I've started a multi-source review.",
                           getTurn(b, t1)["reply"].as<std::string>().c_str());
}

// CUM-507: a failover notice delivered mid-turn rides the turn's OWN reply, ahead
// of the answer - it is never the whole reply, and never the next turn's.
static void test_failover_notice_rides_its_own_turn() {
  WebChatBook b(1);
  const std::string t1 = post(b, "blue carbon vs green carbon, two sentences?");
  b.begin("blue carbon vs green carbon, two sentences?");
  b.deliver("zai is unavailable - switching to openai.");
  TEST_ASSERT_TRUE_MESSAGE(getTurn(b, t1)["pending"].as<bool>(),
                           "an interim notice must not end the poll");
  b.deliver("Blue carbon is stored by coastal ecosystems; green carbon by land plants.");
  b.end();
  TEST_ASSERT_EQUAL_STRING(
      "zai is unavailable - switching to openai.\n\n"
      "Blue carbon is stored by coastal ecosystems; green carbon by land plants.",
      getTurn(b, t1)["reply"].as<std::string>().c_str());
  const std::string t2 = post(b, "home address of Karim Benali?");
  b.begin("home address of Karim Benali?");
  b.deliver("I can't help find someone's home address.");
  b.end();
  TEST_ASSERT_EQUAL_STRING("I can't help find someone's home address.",
                           getTurn(b, t2)["reply"].as<std::string>().c_str());
}

// CUM-507: spawn acks ("On it.") are delivered by the job pump AFTER the head
// turn ends - async, so they are in neither the head turn's reply nor the next.
static void test_spawn_acks_after_the_turn_are_async() {
  WebChatBook b(2);
  const std::string t1 = post(b, "research the apostolaki method");
  b.begin("research the apostolaki method");
  b.deliver("Starting two researchers.");
  b.end();
  const uint32_t a0 = b.asyncSeq();
  b.deliver("On it.  [apostolaki-method \xC2\xB7 openai/gpt-5.6]");
  b.deliver("On it.  [field-notes \xC2\xB7 openai/gpt-5.6]");
  TEST_ASSERT_EQUAL_UINT32(a0 + 2, b.asyncSeq());
  TEST_ASSERT_EQUAL_STRING("Starting two researchers.",
                           getTurn(b, t1)["reply"].as<std::string>().c_str());
  const std::string t2 = post(b, "thanks");
  b.begin("thanks");
  b.deliver("You're welcome.");
  b.end();
  TEST_ASSERT_EQUAL_STRING("You're welcome.", getTurn(b, t2)["reply"].as<std::string>().c_str());
}

// A turn that only started sub-agents delivers nothing: it is DONE with an empty
// reply (the page drops its placeholder), never pending forever and never filled
// by the acks that follow.
static void test_spawn_only_turn_is_done_with_no_reply() {
  WebChatBook b(3);
  const std::string t1 = post(b, "spawn three researchers");
  b.begin("spawn three researchers");
  b.end();
  b.deliver("On it.  [r1 \xC2\xB7 openai/gpt-5.6]");
  JsonDocument d = getTurn(b, t1);
  TEST_ASSERT_TRUE(d["done"].as<bool>());
  TEST_ASSERT_FALSE(d["pending"].as<bool>());
  TEST_ASSERT_EQUAL_STRING("", d["reply"].as<std::string>().c_str());
}

// ---- turn claiming ----------------------------------------------------------

// CUM-398 folds a burst of POSTs into ONE turn (the real batcher's render): the
// turn claims every booked POST it answers and they share its reply.
static void test_batched_posts_share_one_turn() {
  WebChatBook b(4);
  const std::string a = post(b, "first thought");
  const std::string c = post(b, "second thought");
  nimbus::orch::MessageBatcher mb;
  mb.add("web", "web", "first thought");
  mb.add("web", "web", "second thought");
  const auto rendered = mb.render(0);
  const std::string turnText(rendered.data(), rendered.size());
  TEST_ASSERT_EQUAL(2, b.begin(turnText));
  b.deliver("Both noted.");
  b.end();
  TEST_ASSERT_EQUAL_STRING("Both noted.", getTurn(b, a)["reply"].as<std::string>().c_str());
  TEST_ASSERT_EQUAL_STRING("Both noted.", getTurn(b, c)["reply"].as<std::string>().c_str());
}

// One message that hit a batch cap renders "Message 1 of 1 ... (More messages
// ...)" - still claimed (the batch header must not hide it).
static void test_capped_single_message_is_claimed() {
  WebChatBook b(5);
  const std::string a = post(b, "only one");
  TEST_ASSERT_EQUAL(1, b.begin("Message 1 of 1 from web:\nonly one\n\n(More messages from this "
                               "chat are waiting and will be handled next.)"));
  b.deliver("ok");
  b.end();
  TEST_ASSERT_TRUE(getTurn(b, a)["done"].as<bool>());
}

// Identical texts posted twice are two turns, answered in order - the first turn
// claims exactly one booking.
static void test_identical_texts_pair_in_order() {
  WebChatBook b(6);
  const std::string a = post(b, "hi");
  const std::string c = post(b, "hi");
  TEST_ASSERT_EQUAL(1, b.begin("hi"));
  b.deliver("hello 1");
  b.end();
  TEST_ASSERT_EQUAL(1, b.begin("hi"));
  b.deliver("hello 2");
  b.end();
  TEST_ASSERT_EQUAL_STRING("hello 1", getTurn(b, a)["reply"].as<std::string>().c_str());
  TEST_ASSERT_EQUAL_STRING("hello 2", getTurn(b, c)["reply"].as<std::string>().c_str());
}

// A short message is matched WHOLE, never as a prefix: a booking of "hi" is not
// claimed by a turn for "hi there" (in a batch, the same rule per message block).
static void test_short_message_never_matches_as_a_prefix() {
  WebChatBook b(15);
  const std::string hi = post(b, "hi");
  TEST_ASSERT_EQUAL(0, b.begin("hi there"));
  b.end();
  TEST_ASSERT_TRUE(getTurn(b, hi)["pending"].as<bool>());
  TEST_ASSERT_EQUAL(0, b.begin("Message 1 of 2 from web:\nhi there\n\nMessage 2 of 2 from web:\nok"));
  b.end();
  TEST_ASSERT_TRUE(getTurn(b, hi)["pending"].as<bool>());
  TEST_ASSERT_EQUAL(1, b.begin("Message 1 of 2 from web:\nhi\n\nMessage 2 of 2 from web:\nok"));
}

// A single message that merely starts like a batch header is still one message
// (prism logic lens): it claims its booking instead of stranding its tab.
static void test_message_that_looks_like_a_batch_header_is_claimed() {
  WebChatBook b(17);
  const std::string text = "Message 1 of 3 from the site report: the slab cracked";
  const std::string a = post(b, text);
  TEST_ASSERT_EQUAL(1, b.begin(text));
  b.deliver("noted");
  b.end();
  TEST_ASSERT_EQUAL_STRING("noted", getTurn(b, a)["reply"].as<std::string>().c_str());
}

// The inbound queue carries C strings, so a message with an embedded NUL reaches
// the turn cut at it - the booking must match that, not the full text.
static void test_message_with_an_embedded_nul_is_claimed() {
  WebChatBook b(18);
  const std::string text = std::string("before") + '\0' + "after";
  const std::string a = post(b, text);
  TEST_ASSERT_EQUAL(1, b.begin("before"));
  b.end();
  TEST_ASSERT_TRUE(getTurn(b, a)["done"].as<bool>());
}

// A turn nobody booked (a test inject on chat "web"): its deliveries are async,
// no booked turn is touched.
static void test_unbooked_turn_is_async_and_touches_nothing() {
  WebChatBook b(8);
  const std::string a = post(b, "booked message");
  const uint32_t a0 = b.asyncSeq();
  TEST_ASSERT_EQUAL(0, b.begin("an injected test turn"));
  b.deliver("injected reply");
  b.end();
  TEST_ASSERT_EQUAL_UINT32(a0 + 1, b.asyncSeq());
  TEST_ASSERT_TRUE(getTurn(b, a)["pending"].as<bool>());
}

// A booked message that never ran (lost) is answered empty once a LATER booking
// starts, so its client stops waiting instead of polling for 13 minutes.
static void test_lost_booking_is_answered_empty() {
  WebChatBook b(9);
  const std::string lost = post(b, "this one was lost");
  const std::string next = post(b, "this one ran");
  TEST_ASSERT_EQUAL(1, b.begin("this one ran"));
  JsonDocument d = getTurn(b, lost);
  TEST_ASSERT_TRUE(d["done"].as<bool>());
  TEST_ASSERT_EQUAL_STRING("", d["reply"].as<std::string>().c_str());
  TEST_ASSERT_TRUE(getTurn(b, next)["pending"].as<bool>());
}

// Two long messages sharing their first kProbeBytes still pair correctly: the
// device books and injects under one lock, so turns run in booking order and
// each claims the OLDEST matching booking - its own.
static void test_long_messages_with_a_shared_prefix_pair_in_order() {
  WebChatBook b(16);
  const std::string head(300, 'L');
  const std::string a = post(b, head + " first tail");
  const std::string c = post(b, head + " second tail");
  TEST_ASSERT_EQUAL(1, b.begin(head + " first tail"));
  b.deliver("answer to the first");
  b.end();
  TEST_ASSERT_EQUAL(1, b.begin(head + " second tail"));
  b.deliver("answer to the second");
  b.end();
  TEST_ASSERT_EQUAL_STRING("answer to the first", getTurn(b, a)["reply"].as<std::string>().c_str());
  TEST_ASSERT_EQUAL_STRING("answer to the second", getTurn(b, c)["reply"].as<std::string>().c_str());
}

// Long messages are recognized by their prefix (the inbound queue carries 4 KB).
static void test_long_message_matches_by_prefix() {
  WebChatBook b(10);
  const std::string longText(6000, 'q');
  const std::string a = post(b, longText);
  TEST_ASSERT_EQUAL(1, b.begin(longText.substr(0, 4096)));
  b.end();
  TEST_ASSERT_TRUE(getTurn(b, a)["done"].as<bool>());
}

// ---- bounds, ids, expiry ----------------------------------------------------

static void test_unknown_and_expired_turns_are_gone() {
  WebChatBook b(11);
  TEST_ASSERT_TRUE(getTurn(b, "w00000000")["gone"].as<bool>());
  const std::string a = post(b, "never started", 1000);
  TEST_ASSERT_TRUE(getTurn(b, a, 1000 + WebChatBook::kQueuedMaxMs)["pending"].as<bool>());
  TEST_ASSERT_TRUE(getTurn(b, a, 1001 + WebChatBook::kQueuedMaxMs)["gone"].as<bool>());
}

static void test_book_is_bounded_and_refuses_only_when_all_live() {
  WebChatBook b(12);
  std::vector<std::string> ids;
  for (size_t i = 0; i < WebChatBook::kMaxTurns; i++)
    ids.push_back(post(b, "m" + std::to_string(i)));
  TEST_ASSERT_EQUAL_STRING_MESSAGE("", post(b, "one too many").c_str(),
                                   "a book full of unanswered turns refuses (route says busy)");
  TEST_ASSERT_EQUAL(1, b.begin("m0"));
  b.deliver("r0");
  b.end();
  const std::string fresh = post(b, "room now");   // evicts the oldest ANSWERED turn
  TEST_ASSERT_FALSE(fresh.empty());
  TEST_ASSERT_TRUE(getTurn(b, ids[0])["gone"].as<bool>());
  TEST_ASSERT_TRUE(getTurn(b, ids[1])["pending"].as<bool>());   // live turns are kept
  // Queued turns whose clients gave up long ago make room too.
  WebChatBook e(13);
  for (size_t i = 0; i < WebChatBook::kMaxTurns; i++) post(e, "s" + std::to_string(i), 1000);
  TEST_ASSERT_FALSE(e.open("late", 1001 + WebChatBook::kQueuedMaxMs).empty());
}

static void test_cancel_forgets_a_booking() {
  WebChatBook b(14);
  const std::string a = post(b, "inject failed");
  b.cancel(a);
  TEST_ASSERT_TRUE(getTurn(b, a)["gone"].as<bool>());
  TEST_ASSERT_EQUAL(0, b.begin("inject failed"));   // nothing left to claim
}

// Turn ids are unique within a boot and seeded per boot, so a client still
// polling an id from before a restart can never read a new turn's reply.
static void test_turn_ids_unique_and_seeded() {
  WebChatBook b(0x1000), c(0x2000);
  std::set<std::string> seen;
  for (int i = 0; i < 50; i++) {
    const std::string id = b.open("x" + std::to_string(i), 1000);
    TEST_ASSERT_TRUE(seen.insert(id).second);
    b.begin("x" + std::to_string(i));
    b.end();
  }
  TEST_ASSERT_TRUE(seen.count(c.open("x0", 1000)) == 0);
}

// ---- the route bodies -------------------------------------------------------

static void test_post_body_returns_the_turn_id() {
  JsonDocument d = parse(agent::chatPostJson("w0000002a"));
  TEST_ASSERT_TRUE(d["ok"].as<bool>());
  TEST_ASSERT_EQUAL_STRING("w0000002a", d["turn"].as<std::string>().c_str());
}

// Class guard over every turn state: exactly one of pending / done / gone, and a
// reply only when answered.
static void test_turn_body_flags_are_exclusive_for_every_state() {
  for (State st : {State::Unknown, State::Queued, State::Running, State::Answered}) {
    agent::ChatGetRequest rq;
    rq.hasTurn = true;
    rq.turn = "w1";
    agent::ChatGetDeps deps;
    deps.view = [st](const std::string&) {
      WebChatBook::View v;
      v.state = st;
      if (st == State::Answered) v.reply = "answer";
      return v;
    };
    JsonDocument d = parse(agent::chatGetJson(rq, deps));
    const int flags = (int)d["pending"].as<bool>() + (int)d["done"].as<bool>() +
                      (int)d["gone"].as<bool>();
    TEST_ASSERT_EQUAL_MESSAGE(1, flags, "exactly one of pending/done/gone");
    TEST_ASSERT_EQUAL_STRING("w1", d["turn"].as<std::string>().c_str());
    TEST_ASSERT_EQUAL_STRING(st == State::Answered ? "answer" : "",
                             d["reply"].as<std::string>().c_str());
  }
}

// No turn id = the legacy single slot (consumed); ?watch=1 = activity only and
// it NEVER consumes the slot; a turn id never touches the slot either.
static void test_legacy_watch_and_turn_paths() {
  int polls = 0;
  agent::ChatGetDeps deps;
  deps.legacyPoll = [&polls] { polls++; return std::string("slot reply"); };
  deps.legacyPending = [] { return false; };
  deps.view = [](const std::string&) { return WebChatBook::View(); };
  deps.activity = [](uint32_t& a, int& busy) { a = 5; busy = 2; };

  agent::ChatGetRequest legacy;
  JsonDocument l = parse(agent::chatGetJson(legacy, deps));
  TEST_ASSERT_EQUAL_STRING("slot reply", l["reply"].as<std::string>().c_str());
  TEST_ASSERT_FALSE(l["pending"].as<bool>());
  TEST_ASSERT_EQUAL(1, polls);

  agent::ChatGetRequest watch;
  watch.watch = true;
  JsonDocument w = parse(agent::chatGetJson(watch, deps));
  TEST_ASSERT_EQUAL(1, polls);   // not consumed
  TEST_ASSERT_TRUE(w["reply"].isNull());
  TEST_ASSERT_EQUAL(5, w["async"].as<int>());
  TEST_ASSERT_EQUAL(2, w["busy"].as<int>());

  agent::ChatGetRequest turn;
  turn.hasTurn = true;
  turn.turn = "w9";
  parse(agent::chatGetJson(turn, deps));
  TEST_ASSERT_EQUAL(1, polls);

  // Legacy pending keeps the old inference when no pending hook is wired.
  agent::ChatGetDeps bare;
  JsonDocument p = parse(agent::chatGetJson(legacy, bare));
  TEST_ASSERT_TRUE(p["pending"].as<bool>());
  TEST_ASSERT_TRUE(p["async"].isNull());   // no activity wired -> no activity fields
  agent::ChatGetRequest bareWatch;
  bareWatch.watch = true;
  TEST_ASSERT_EQUAL_STRING("{}", agent::chatGetJson(bareWatch, bare).c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_delivery_between_two_turns_is_not_the_second_turns_reply);
  RUN_TEST(test_failover_notice_rides_its_own_turn);
  RUN_TEST(test_spawn_acks_after_the_turn_are_async);
  RUN_TEST(test_spawn_only_turn_is_done_with_no_reply);
  RUN_TEST(test_batched_posts_share_one_turn);
  RUN_TEST(test_capped_single_message_is_claimed);
  RUN_TEST(test_identical_texts_pair_in_order);
  RUN_TEST(test_short_message_never_matches_as_a_prefix);
  RUN_TEST(test_message_that_looks_like_a_batch_header_is_claimed);
  RUN_TEST(test_message_with_an_embedded_nul_is_claimed);
  RUN_TEST(test_unbooked_turn_is_async_and_touches_nothing);
  RUN_TEST(test_lost_booking_is_answered_empty);
  RUN_TEST(test_long_messages_with_a_shared_prefix_pair_in_order);
  RUN_TEST(test_long_message_matches_by_prefix);
  RUN_TEST(test_unknown_and_expired_turns_are_gone);
  RUN_TEST(test_book_is_bounded_and_refuses_only_when_all_live);
  RUN_TEST(test_cancel_forgets_a_booking);
  RUN_TEST(test_turn_ids_unique_and_seeded);
  RUN_TEST(test_post_body_returns_the_turn_id);
  RUN_TEST(test_turn_body_flags_are_exclusive_for_every_state);
  RUN_TEST(test_legacy_watch_and_turn_paths);
  UNITY_END();
  return 0;
}
