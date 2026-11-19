// test_tg_reply_drain - the reply queue's send rules (nimbus/net/tg_reply_drain.h), with a
// fake Telegram: a scripted answer per POST and the chat as Telegram would hold it.
//
// The class under test: whatever Telegram answers and however producers fill the queue,
// a part reaches the chat at most kTgUnconfirmedCopies times, the parts reach it in queue
// order, a part is only ever dropped after kTgSendTries attempts Telegram refused or could
// not take (or after kTgUnconfirmedCopies POSTs whose answers were all lost), and a
// reply's parts are queued all or none. The counter-tests run the drain
// as it was before v5.2.0 (plain resend after any failure, six tries, no lost-answer
// bound, take-then-put-back) on the same scripts and show the checks catch it.
#include <unity.h>

#include <deque>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "nimbus/net/tg_reply_drain.h"

using namespace nimbus::net;

void setUp() {}
void tearDown() {}

namespace {

struct Item {
  std::string text;
  uint8_t tries = 0;   // the old drain's counter (the new one keeps TgPartTries beside the queue)
};

// Telegram as the test sees it: what each POST is answered, and what reached the chat.
// An Unconfirmed POST may or may not have landed; `lostLands` says which.
struct FakeTelegram {
  std::vector<TgSend> script;   // per POST, in order; past the end: Ok
  size_t posts = 0;
  bool lostLands = true;
  std::vector<std::string> chat;          // what the owner sees, in order
  std::vector<std::pair<std::string, bool>> postsLog;   // (text, html)
  std::function<void()> duringPost;       // a producer acting while the POST is in flight

  TgSend post(const std::string& text, bool html) {
    postsLog.push_back({text, html});
    if (duringPost) duringPost();
    const TgSend r = posts < script.size() ? script[posts] : TgSend::Ok;
    ++posts;
    if (r == TgSend::Ok || (r == TgSend::Unconfirmed && lostLands)) chat.push_back(text);
    return r;
  }
};

struct Queue {
  std::deque<Item> q;
  size_t depth = 16;
  bool push(const Item& it) {
    if (q.size() >= depth) return false;
    q.push_back(it);
    return true;
  }
  bool pushFront(const Item& it) {
    if (q.size() >= depth) return false;
    q.push_front(it);
    return true;
  }
};

struct Drained {
  std::vector<std::pair<std::string, TgDrainStep>> notes;
};

// The v5.2.0 drain over the fake queue and Telegram.
size_t drainNew(Queue& q, FakeTelegram& tg, TgPartTries& tries, Drained* d = nullptr) {
  Item slot;
  return drainTgReplies(
      slot, tries,
      [&](Item& out) {
        if (q.q.empty()) return false;
        out = q.q.front();
        return true;
      },
      [&] { q.q.pop_front(); },
      [&](const Item& it) { return sendTgPart([&](bool html) { return tg.post(it.text, html); }); },
      [&](const Item& it, TgDrainStep s, const TgPartTries&) {
        if (d) d->notes.push_back({it.text, s});
      });
}

// The drain as it was on rb-main 6d0280e7: take the part, HTML then plain on any
// failure, at most six tries, back to the front on a failure (which can fail on a
// queue a producer refilled meanwhile).
size_t drainOld(Queue& q, FakeTelegram& tg) {
  size_t sent = 0;
  while (!q.q.empty()) {
    Item it = q.q.front();
    q.q.pop_front();
    bool ok = tg.post(it.text, true) == TgSend::Ok;
    if (!ok) ok = tg.post(it.text, false) == TgSend::Ok;
    if (ok) {
      ++sent;
      continue;
    }
    if (++it.tries < 6) {
      q.pushFront(it);
      break;
    }
  }
  return sent;
}

std::vector<Item> parts(int n, const char* tag = "p") {
  std::vector<Item> out;
  for (int i = 0; i < n; ++i) out.push_back({std::string(tag) + std::to_string(i)});
  return out;
}

// The class checks over one run: the chat against the queued parts.
std::string problems(const std::vector<Item>& queued, const FakeTelegram& tg, const std::vector<std::string>& dropped) {
  std::map<std::string, int> copies;
  for (const auto& t : tg.chat) copies[t]++;
  for (const auto& kv : copies)
    if (kv.second > kTgUnconfirmedCopies) return kv.first + " reached the chat " + std::to_string(kv.second) + " times";
  // First appearances in queue order.
  size_t at = 0;
  std::map<std::string, bool> seen;
  for (const auto& t : tg.chat) {
    if (seen[t]) continue;
    seen[t] = true;
    while (at < queued.size() && queued[at].text != t) ++at;
    if (at == queued.size()) return t + " reached the chat out of order";
  }
  for (const auto& it : queued) {
    bool wasDropped = false;
    for (const auto& d : dropped) wasDropped |= d == it.text;
    if (!copies.count(it.text) && !wasDropped) return it.text + " never reached the chat and was not given up";
  }
  return "";
}

}  // namespace

// ---- one attempt ----------------------------------------------------------------------

static void test_classify_reads_what_the_post_proved() {
  TEST_ASSERT_EQUAL(TgSend::NotSent, classifyTgSend({false, false, 0}));
  TEST_ASSERT_EQUAL(TgSend::NotSent, classifyTgSend({true, false, 0}));   // a short write
  TEST_ASSERT_EQUAL(TgSend::Unconfirmed, classifyTgSend({true, true, 0}));
  TEST_ASSERT_EQUAL(TgSend::Ok, classifyTgSend({true, true, 200}));
  TEST_ASSERT_EQUAL(TgSend::Refused, classifyTgSend({true, true, 400}));
  TEST_ASSERT_EQUAL(TgSend::Refused, classifyTgSend({true, true, 403}));
  TEST_ASSERT_EQUAL(TgSend::NotSent, classifyTgSend({true, true, 429}));
  TEST_ASSERT_EQUAL(TgSend::NotSent, classifyTgSend({true, true, 502}));
}

static void test_plain_follows_only_a_refused_html() {
  for (TgSend first : {TgSend::Ok, TgSend::Refused, TgSend::NotSent, TgSend::Unconfirmed}) {
    FakeTelegram tg;
    tg.script = {first, TgSend::Ok};
    const TgSend r = sendTgPart([&](bool html) { return tg.post("x", html); });
    const size_t want = first == TgSend::Refused ? 2 : 1;
    TEST_ASSERT_EQUAL(want, tg.postsLog.size());
    TEST_ASSERT_TRUE(tg.postsLog[0].second);
    if (want == 2) TEST_ASSERT_FALSE(tg.postsLog[1].second);
    TEST_ASSERT_EQUAL(first == TgSend::Refused ? TgSend::Ok : first, r);
  }
}

static void test_a_lost_answer_is_resent_once_at_most() {
  Queue q;
  for (auto& it : parts(3)) q.push(it);
  FakeTelegram tg;
  tg.script = std::vector<TgSend>(20, TgSend::Unconfirmed);   // every answer lost
  TgPartTries tries;
  Drained d;
  for (int i = 0; i < 10 && !q.q.empty(); ++i) drainNew(q, tg, tries, &d);
  TEST_ASSERT_TRUE(q.q.empty());
  TEST_ASSERT_EQUAL(6, tg.posts);   // two POSTs per part, never a plain one
  for (const auto& p : tg.postsLog) TEST_ASSERT_TRUE(p.second);
  TEST_ASSERT_EQUAL(TgDrainStep::LeaveAsSent, d.notes.back().second);
}

static void test_counter_old_drain_sends_a_lost_part_a_dozen_times() {
  Queue q;
  for (auto& it : parts(1)) q.push(it);
  FakeTelegram tg;
  tg.script = std::vector<TgSend>(40, TgSend::Unconfirmed);
  for (int i = 0; i < 10 && !q.q.empty(); ++i) drainOld(q, tg);
  TEST_ASSERT_EQUAL(12, tg.posts);
  TEST_ASSERT_FALSE(problems(parts(1), tg, {}).empty());   // the check sees it
}

static void test_not_sent_is_retried_in_front_then_given_up_after_six() {
  Queue q;
  for (auto& it : parts(2)) q.push(it);
  FakeTelegram tg;
  tg.script = std::vector<TgSend>(6, TgSend::NotSent);
  TgPartTries tries;
  Drained d;
  int drains = 0;
  while (!q.q.empty() && drains < 20) {
    drainNew(q, tg, tries, &d);
    ++drains;
  }
  TEST_ASSERT_EQUAL(6, drains);   // five retries, then p0 given up and p1 out in the same drain
  TEST_ASSERT_EQUAL(1, tg.chat.size());
  TEST_ASSERT_EQUAL_STRING("p1", tg.chat[0].c_str());
  TEST_ASSERT_EQUAL(TgDrainStep::GiveUp, d.notes.back().second);
  for (int i = 0; i < 6; ++i) TEST_ASSERT_EQUAL_STRING("p0", tg.postsLog[i].first.c_str());
}

// The part being sent stays in the queue: a producer that fills the queue during its
// POST cannot push it out, and it still goes before the parts queued after it.
static void test_a_failed_part_keeps_its_place_when_the_queue_fills_meanwhile() {
  Queue q;
  q.depth = 4;
  for (auto& it : parts(2)) q.push(it);
  FakeTelegram tg;
  tg.script = {TgSend::NotSent};
  int added = 0;
  tg.duringPost = [&] {
    while (q.push(Item{"late" + std::to_string(added)})) ++added;
  };
  TgPartTries tries;
  drainNew(q, tg, tries);
  tg.duringPost = nullptr;
  while (!q.q.empty()) drainNew(q, tg, tries);
  TEST_ASSERT_EQUAL_STRING("p0", tg.chat[0].c_str());
  TEST_ASSERT_EQUAL_STRING("p1", tg.chat[1].c_str());
  TEST_ASSERT_EQUAL(4, tg.chat.size());
}

static void test_counter_old_put_back_loses_the_part_on_a_refilled_queue() {
  Queue q;
  q.depth = 4;
  for (auto& it : parts(2)) q.push(it);
  FakeTelegram tg;
  tg.script = {TgSend::NotSent, TgSend::NotSent};   // the HTML POST and its plain resend
  int added = 0;
  tg.duringPost = [&] {
    while (q.push(Item{"late" + std::to_string(added)})) ++added;
  };
  drainOld(q, tg);
  tg.duringPost = nullptr;
  while (!q.q.empty()) drainOld(q, tg);
  bool p0 = false;
  for (const auto& t : tg.chat) p0 |= t == "p0";
  TEST_ASSERT_FALSE(p0);   // dropped without a log line or a give-up
}

// The property over random answers and lost-answer outcomes: the class checks hold for
// the v5.2.0 drain on every script, and fail for the old one on some.
static void test_property_random_answers() {
  std::mt19937 rng(20261119);
  int oldBroken = 0;
  for (int run = 0; run < 400; ++run) {
    std::vector<TgSend> script;
    for (int i = 0; i < 40; ++i) {
      const int r = (int)(rng() % 10);
      script.push_back(r < 5 ? TgSend::Ok : r < 7 ? TgSend::Unconfirmed : r < 8 ? TgSend::Refused : TgSend::NotSent);
    }
    const bool lands = rng() % 2;
    const std::vector<Item> queued = parts(1 + (int)(rng() % 6));
    for (int which = 0; which < 2; ++which) {
      Queue q;
      for (const auto& it : queued) q.push(it);
      FakeTelegram tg;
      tg.script = script;
      tg.lostLands = lands;
      TgPartTries tries;
      Drained d;
      for (int i = 0; i < 60 && !q.q.empty(); ++i) {
        if (which == 0) drainNew(q, tg, tries, &d);
        else drainOld(q, tg);
      }
      TEST_ASSERT_TRUE(q.q.empty());
      std::vector<std::string> dropped;
      for (const auto& n : d.notes)
        // Given up, or left as probably delivered after two lost answers (when those
        // POSTs really were lost, the part is not in the chat: the bound's price).
        if (n.second == TgDrainStep::GiveUp || n.second == TgDrainStep::LeaveAsSent) dropped.push_back(n.first);
      const std::string why = problems(queued, tg, dropped);
      if (which == 0) TEST_ASSERT_TRUE_MESSAGE(why.empty(), why.c_str());
      else if (!why.empty()) ++oldBroken;
    }
  }
  TEST_ASSERT_TRUE(oldBroken > 0);
}

// ---- queuing a reply --------------------------------------------------------------------

static void test_parts_are_queued_all_or_none() {
  Queue q;
  for (auto& it : parts(13, "old")) q.push(it);   // 3 slots left
  const std::vector<Item> reply = parts(5, "r");
  int waits = 0;
  const bool ok = queueTgParts(
      reply.size(), [&] { return q.depth - q.q.size(); }, [&](size_t i) { return q.push(reply[i]); },
      [&] { return ++waits < 3; });
  TEST_ASSERT_FALSE(ok);
  TEST_ASSERT_EQUAL(13, q.q.size());   // none of the five
  TEST_ASSERT_EQUAL(3, waits);
  // Room made while it waits (the consumer took parts out): all five go in.
  const bool ok2 = queueTgParts(
      reply.size(), [&] { return q.depth - q.q.size(); }, [&](size_t i) { return q.push(reply[i]); },
      [&] {
        q.q.pop_front();
        q.q.pop_front();
        return true;
      });
  TEST_ASSERT_TRUE(ok2);
  TEST_ASSERT_EQUAL(16, q.q.size());
  TEST_ASSERT_EQUAL_STRING("r0", q.q[11].text.c_str());
  TEST_ASSERT_EQUAL_STRING("r4", q.q[15].text.c_str());
  TEST_ASSERT_TRUE(queueTgParts(0, [] { return (size_t)0; }, [](size_t) { return false; }, [] { return false; }));
}

static void test_counter_part_by_part_queuing_keeps_a_torn_reply() {
  Queue q;
  for (auto& it : parts(13, "old")) q.push(it);
  size_t queued = 0;
  for (const auto& it : parts(5, "r"))
    if (q.push(it)) ++queued;
  TEST_ASSERT_EQUAL(3, queued);   // the old send(): three parts in, false returned
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_classify_reads_what_the_post_proved);
  RUN_TEST(test_plain_follows_only_a_refused_html);
  RUN_TEST(test_a_lost_answer_is_resent_once_at_most);
  RUN_TEST(test_counter_old_drain_sends_a_lost_part_a_dozen_times);
  RUN_TEST(test_not_sent_is_retried_in_front_then_given_up_after_six);
  RUN_TEST(test_a_failed_part_keeps_its_place_when_the_queue_fills_meanwhile);
  RUN_TEST(test_counter_old_put_back_loses_the_part_on_a_refilled_queue);
  RUN_TEST(test_property_random_answers);
  RUN_TEST(test_parts_are_queued_all_or_none);
  RUN_TEST(test_counter_part_by_part_queuing_keeps_a_torn_reply);
  return UNITY_END();
}
