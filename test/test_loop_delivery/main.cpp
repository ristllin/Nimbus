// CUM-505 / CUM-513 - where a routine, reminder or wakeup delivers (nimbus/orch/loop_delivery.h).
//
// The bug: a loop created from the web chat (or by the model with no chat_id) was stored
// with chatId "", which meant "the first Telegram allowlist entry" at fire time. On a
// device with no Telegram that is "" again: the turn ran, was billed, and its reply was
// handed to telegram::send and dropped. A loop that DID remember "web" (a /remind typed
// in the web chat) was paused as "chat revoked", because "web" is not on the Telegram
// allowlist. These tests sweep every channel value against every Telegram state and every
// loop kind, assert the invariants (never into "", a fallback only for the owner's own
// chat, a revoke always pauses), and keep the pre-fix rule as a counter-model that fails
// the same sweep.
#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "nimbus/orch/loop_delivery.h"

using nimbus::orch::buildFireRequest;
using nimbus::orch::CreatedBy;
using nimbus::orch::Delivery;
using nimbus::orch::DeliveryChannel;
using nimbus::orch::deliveryChannelLabel;
using nimbus::orch::deliveryChannelOf;
using nimbus::orch::deliveryFallbackSentence;
using nimbus::orch::DeliveryVerdict;
using nimbus::orch::DeliveryWorld;
using nimbus::orch::deliveryWorld;
using nimbus::orch::isDeviceChannel;
using nimbus::orch::liftDeviceChannelRevoke;
using nimbus::orch::LoopFireRequest;
using nimbus::orch::LoopRecord;
using nimbus::orch::originChat;
using nimbus::orch::PauseReason;
using nimbus::orch::resolveDelivery;
using nimbus::orch::SchedKind;

void setUp() {}
void tearDown() {}

namespace {

// One sample of every chatId routing value the device uses.
const char* const kChats[] = {"", "web", "voice", "serial", "123456789", "-1001234567890"};

// Every Telegram state a device can be in: (bot token, allowlist, owners list, is the
// loop's Telegram chat still allowed - allow-listed and its tenant role not revoked).
struct WorldCase {
  const char* name;
  const char* token;
  const char* allowlist;
  const char* owners;
  bool        allowed;
};
const WorldCase kWorlds[] = {
    {"no telegram", "", "", "", false},
    {"token only", "tok", "", "", false},
    {"bot off, owner chat kept", "", "123456789", "", true},
    {"bot off, member chat kept", "", "555,123456789", "555", true},
    {"ready, chat allowed", "tok", "123456789,555", "", true},
    {"ready, chat removed", "tok", "555", "", false},
    {"ready, tenant revoked", "tok", "123456789", "", false},
    {"ready, explicit owners", "tok", "555,123456789", "123456789", true},
};

const SchedKind kKinds[] = {SchedKind::Interval, SchedKind::Daily, SchedKind::Weekly, SchedKind::Once};
const CreatedBy kMakers[] = {CreatedBy::Owner, CreatedBy::Agent};

DeliveryWorld worldOf(const WorldCase& wc) { return deliveryWorld(wc.token, wc.allowlist, wc.owners); }

// The PRE-FIX fire rule, kept as a counter-model: "" -> first allowlist entry, and the
// fire-time check allowed a non-empty chat only if it was on the Telegram allowlist.
bool oldRuleGoesNowhere(const std::string& chat, const std::string& firstAllowed, bool allowed) {
  const bool paused = !chat.empty() && !allowed;   // "web"/"voice" are never on the allowlist
  return !paused && (chat.empty() ? firstAllowed : chat).empty();
}
bool oldRulePausesLocal(const std::string& chat) {
  return isDeviceChannel(deliveryChannelOf(chat));   // never allow-listed -> paused
}

// The invariants every (channel, world) pair must hold.
void checkInvariants(const WorldCase& wc, const char* chat) {
  const DeliveryWorld w = worldOf(wc);
  const DeliveryChannel from = deliveryChannelOf(chat);
  const bool allowed = from == DeliveryChannel::Telegram && wc.allowed;
  const Delivery d = resolveDelivery(chat, w, allowed);
  char msg[96];
  snprintf(msg, sizeof msg, "world='%s' chat='%s'", wc.name, chat);
  TEST_ASSERT_EQUAL_MESSAGE(from, d.from, msg);
  if (d.verdict == DeliveryVerdict::Revoked) {
    TEST_ASSERT_EQUAL_MESSAGE(DeliveryChannel::Telegram, from, msg);   // only Telegram revokes
    TEST_ASSERT_TRUE_MESSAGE(d.target.empty(), msg);
    TEST_ASSERT_FALSE_MESSAGE(w.botToken && allowed, msg);              // never a live, allowed chat
    return;
  }
  TEST_ASSERT_FALSE_MESSAGE(d.target.empty(), msg);   // never fire into nowhere
  if (isDeviceChannel(from)) {
    TEST_ASSERT_EQUAL_MESSAGE(DeliveryVerdict::Deliver, d.verdict, msg);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(chat, d.target.c_str(), msg);
  }
  if (d.to == DeliveryChannel::Telegram) TEST_ASSERT_TRUE_MESSAGE(w.botToken, msg);
  if (from == DeliveryChannel::OwnerDefault) {
    TEST_ASSERT_EQUAL_MESSAGE(DeliveryVerdict::Deliver, d.verdict, msg);
    // The owner default only ever reaches an OWNER chat, else the web chat.
    if (d.to == DeliveryChannel::Telegram) TEST_ASSERT_TRUE_MESSAGE(w.isOwnerChat(d.target), msg);
    else TEST_ASSERT_EQUAL_STRING_MESSAGE("web", d.target.c_str(), msg);
  }
  if (d.verdict == DeliveryVerdict::FellBack) {
    // A fallback runs as the web chat (admin): only the owner's own chat earns one.
    TEST_ASSERT_EQUAL_MESSAGE(DeliveryChannel::Telegram, from, msg);
    TEST_ASSERT_FALSE_MESSAGE(w.botToken, msg);
    TEST_ASSERT_TRUE_MESSAGE(w.isOwnerChat(chat), msg);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("web", d.target.c_str(), msg);
  }
  if (from == DeliveryChannel::Telegram && w.botToken && allowed) {
    TEST_ASSERT_EQUAL_MESSAGE(DeliveryVerdict::Deliver, d.verdict, msg);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(chat, d.target.c_str(), msg);
  }
}

LoopRecord loopFor(SchedKind kind, CreatedBy maker, const char* chat) {
  LoopRecord l;
  l.id = "lp123456";
  l.name = "Morning brief";
  l.prompt = "Remind me to check the tide table.";
  l.chatId = chat;
  l.sched.kind = kind;
  l.createdBy = maker;
  return l;
}

void checkFireRequest(const LoopRecord& l, const Delivery& d, const char* msg) {
  const LoopFireRequest r = buildFireRequest(l, d, 1791667257ull);
  TEST_ASSERT_FALSE_MESSAGE(r.chatId.empty(), msg);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(d.target.c_str(), r.chatId.c_str(), msg);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(l.id.c_str(), r.id.c_str(), msg);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(l.name.c_str(), r.name.c_str(), msg);
  TEST_ASSERT_EQUAL_UINT64_MESSAGE(1791667257ull, r.scheduledFor, msg);
  const bool once = l.sched.kind == SchedKind::Once;
  TEST_ASSERT_EQUAL_MESSAGE(once, r.once, msg);
  TEST_ASSERT_EQUAL_MESSAGE(once && l.createdBy == CreatedBy::Owner, r.ownerReminder, msg);
  TEST_ASSERT_EQUAL_MESSAGE(0, (int)r.prompt.rfind(l.prompt, 0), msg);   // the task leads
  const bool told = r.prompt.find("[DELIVERY]") != std::string::npos &&
                    r.prompt.find(deliveryFallbackSentence(d)) != std::string::npos;
  TEST_ASSERT_EQUAL_MESSAGE(d.verdict == DeliveryVerdict::FellBack, told, msg);
}

}  // namespace

// ---- channels and worlds ----------------------------------------------------------------

static void test_channel_of_every_slug() {
  TEST_ASSERT_EQUAL(DeliveryChannel::OwnerDefault, deliveryChannelOf(""));
  TEST_ASSERT_EQUAL(DeliveryChannel::Web, deliveryChannelOf("web"));
  TEST_ASSERT_EQUAL(DeliveryChannel::Web, deliveryChannelOf(nimbus::orch::kHostedWebChatId));   // CUM-542
  TEST_ASSERT_EQUAL(DeliveryChannel::Voice, deliveryChannelOf("voice"));
  TEST_ASSERT_EQUAL(DeliveryChannel::Serial, deliveryChannelOf("serial"));
  TEST_ASSERT_EQUAL(DeliveryChannel::Telegram, deliveryChannelOf("123456789"));
  TEST_ASSERT_EQUAL(DeliveryChannel::Telegram, deliveryChannelOf("-1001234567890"));
}

// Every channel has a real, printable-ASCII label (it lands in a delivered sentence).
static void test_every_channel_has_a_label() {
  for (int i = 0; i < (int)DeliveryChannel::Count; ++i) {
    const std::string label = deliveryChannelLabel(DeliveryChannel(i));
    TEST_ASSERT_FALSE(label.empty());
    TEST_ASSERT_TRUE(label != "an unknown channel");
    for (unsigned char ch : label) TEST_ASSERT_TRUE(ch >= 0x20 && ch < 0x7F);
  }
}

// Owners follow telegram::isOwner exactly.
static void test_world_owner_rule() {
  DeliveryWorld w = deliveryWorld("tok", " 111 , 222", "");
  TEST_ASSERT_TRUE(w.botToken);
  TEST_ASSERT_EQUAL_STRING("111", w.ownerTelegram.c_str());
  TEST_ASSERT_TRUE(w.isOwnerChat("111"));
  TEST_ASSERT_FALSE(w.isOwnerChat("222"));
  w = deliveryWorld("tok", "555,123", "123");   // explicit owners win over list order
  TEST_ASSERT_EQUAL_STRING("123", w.ownerTelegram.c_str());
  TEST_ASSERT_FALSE(w.isOwnerChat("555"));
  w = deliveryWorld("tok", "555", "123");       // an owner must be allow-listed
  TEST_ASSERT_TRUE(w.ownerTelegram.empty());
  w = deliveryWorld("tok", " ,123", "");        // isOwner reads the text before the first comma
  TEST_ASSERT_TRUE(w.ownerTelegram.empty());
  TEST_ASSERT_FALSE(w.isOwnerChat("123"));
  TEST_ASSERT_FALSE(w.isOwnerChat(""));
  TEST_ASSERT_FALSE(deliveryWorld("   ", "111", "").botToken);
}

// ---- the class: every channel x every Telegram state -----------------------------------

static void test_every_channel_every_world() {
  for (const WorldCase& wc : kWorlds)
    for (const char* chat : kChats) checkInvariants(wc, chat);
}

// The sweep really exercises every verdict.
static void test_sweep_reaches_every_verdict() {
  int seen[3] = {0, 0, 0};
  for (const WorldCase& wc : kWorlds)
    for (const char* chat : kChats) {
      const bool allowed = deliveryChannelOf(chat) == DeliveryChannel::Telegram && wc.allowed;
      ++seen[(int)resolveDelivery(chat, worldOf(wc), allowed).verdict];
    }
  TEST_ASSERT_TRUE(seen[(int)DeliveryVerdict::Deliver] > 0);
  TEST_ASSERT_TRUE(seen[(int)DeliveryVerdict::FellBack] > 0);
  TEST_ASSERT_TRUE(seen[(int)DeliveryVerdict::Revoked] > 0);
}

// The same sweep through the pre-fix rule: it fires into "" and pauses a local channel
// somewhere, which the new rule never does.
static void test_old_rule_fails_the_sweep() {
  int nowhere = 0, pausedLocal = 0;
  for (const WorldCase& wc : kWorlds)
    for (const char* chat : kChats) {
      const bool allowed = deliveryChannelOf(chat) == DeliveryChannel::Telegram && wc.allowed;
      nowhere += oldRuleGoesNowhere(chat, worldOf(wc).ownerTelegram, allowed);
      pausedLocal += oldRulePausesLocal(chat);
    }
  TEST_ASSERT_TRUE(nowhere > 0);       // CUM-505: the web-only reminder went nowhere
  TEST_ASSERT_TRUE(pausedLocal > 0);   // a "web" /remind was paused as chat-revoked
}

// ---- the class over loop kinds: routines, wakeups, reminders ---------------------------

static void test_fire_request_for_every_kind_channel_world() {
  for (const WorldCase& wc : kWorlds)
    for (const char* chat : kChats) {
      const bool allowed = deliveryChannelOf(chat) == DeliveryChannel::Telegram && wc.allowed;
      const Delivery d = resolveDelivery(chat, worldOf(wc), allowed);
      if (d.verdict == DeliveryVerdict::Revoked) continue;   // pauses, never fires
      for (SchedKind kind : kKinds)
        for (CreatedBy maker : kMakers) {
          char msg[128];
          snprintf(msg, sizeof msg, "kind=%d maker=%d world='%s' chat='%s'", (int)kind,
                   (int)maker, wc.name, chat);
          checkFireRequest(loopFor(kind, maker, chat), d, msg);
        }
    }
}

// The one-time repair: exactly the recurring device-channel loops the old rule paused.
static void test_lift_device_channel_revoke_over_every_case() {
  const PauseReason reasons[] = {PauseReason::None, PauseReason::Owner, PauseReason::ChatRevoked};
  for (const char* chat : kChats)
    for (SchedKind kind : kKinds)
      for (PauseReason why : reasons) {
        LoopRecord l = loopFor(kind, CreatedBy::Owner, chat);
        l.enabled = false;
        l.pauseReason = why;
        const bool want = why == PauseReason::ChatRevoked && kind != SchedKind::Once &&
                          isDeviceChannel(deliveryChannelOf(chat));
        TEST_ASSERT_EQUAL(want, liftDeviceChannelRevoke(l));
        TEST_ASSERT_EQUAL(want, l.enabled);
        if (want) TEST_ASSERT_EQUAL(PauseReason::None, l.pauseReason);
      }
  LoopRecord running = loopFor(SchedKind::Daily, CreatedBy::Owner, "web");
  TEST_ASSERT_FALSE(liftDeviceChannelRevoke(running));   // a running loop is untouched
}

// ---- the reported cases ------------------------------------------------------------------

// CUM-505: "remind me in 2 minutes" in the web chat of a device with no Telegram.
static void test_web_chat_reminder_on_a_web_only_device_reaches_the_web_chat() {
  const DeliveryWorld w = deliveryWorld("", "", "");
  const std::string stored = originChat("", "web");   // wakeup.set passes no chat_id
  TEST_ASSERT_EQUAL_STRING("web", stored.c_str());
  const Delivery d = resolveDelivery(stored, w, false);
  TEST_ASSERT_EQUAL(DeliveryVerdict::Deliver, d.verdict);
  TEST_ASSERT_EQUAL_STRING("web", d.target.c_str());
  // A wakeup stored before the fix ("") lands in the web chat too.
  TEST_ASSERT_EQUAL_STRING("web", resolveDelivery("", w, false).target.c_str());
}

// CUM-513: a routine created from the web chat, Telegram set up for the owner's chat.
static void test_web_routine_stays_in_the_web_chat_when_telegram_is_set_up() {
  const Delivery d = resolveDelivery(originChat("", "web"), deliveryWorld("tok", "555", ""), false);
  TEST_ASSERT_EQUAL(DeliveryVerdict::Deliver, d.verdict);   // never "chat revoked"
  TEST_ASSERT_EQUAL_STRING("web", d.target.c_str());
}

// A reminder asked for by voice comes back on the device screen.
static void test_voice_reminder_returns_to_the_screen() {
  const Delivery d = resolveDelivery(originChat("", "voice"), deliveryWorld("", "", ""), false);
  TEST_ASSERT_EQUAL(DeliveryVerdict::Deliver, d.verdict);
  TEST_ASSERT_EQUAL_STRING("voice", d.target.c_str());
}

// The owner's Telegram routine on a device whose bot token was removed falls back and
// says so.
static void test_owner_telegram_routine_falls_back_and_says_so() {
  const Delivery d = resolveDelivery("123456789", deliveryWorld("", "123456789", ""), true);
  TEST_ASSERT_EQUAL(DeliveryVerdict::FellBack, d.verdict);
  TEST_ASSERT_EQUAL(DeliveryChannel::Telegram, d.from);
  TEST_ASSERT_EQUAL(DeliveryChannel::Web, d.to);
  TEST_ASSERT_EQUAL_STRING("web", d.target.c_str());
  TEST_ASSERT_EQUAL_STRING("Showing this in the web chat because Telegram isn't available right now.",
                           deliveryFallbackSentence(d).c_str());
}

// A member's routine never re-homes to the (admin) web chat: it pauses instead.
static void test_member_routine_pauses_when_the_bot_is_off() {
  const Delivery d = resolveDelivery("123456789", deliveryWorld("", "555,123456789", "555"), true);
  TEST_ASSERT_EQUAL(DeliveryVerdict::Revoked, d.verdict);
}

// Emptying the allowlist is the strongest revoke, not "Telegram was never set up".
static void test_emptied_allowlist_pauses_telegram_routines() {
  TEST_ASSERT_EQUAL(DeliveryVerdict::Revoked,
                    resolveDelivery("123456789", deliveryWorld("", "", ""), false).verdict);
  TEST_ASSERT_EQUAL(DeliveryVerdict::Revoked,
                    resolveDelivery("123456789", deliveryWorld("tok", "", ""), false).verdict);
}

// The security rail is unchanged: a chat removed from a working allowlist (or a tenant
// whose role was revoked) pauses.
static void test_revoked_chat_still_pauses() {
  const Delivery d = resolveDelivery("123456789", deliveryWorld("tok", "555", ""), false);
  TEST_ASSERT_EQUAL(DeliveryVerdict::Revoked, d.verdict);
  TEST_ASSERT_TRUE(d.target.empty());
}

// The owner's own Telegram routine keeps going to Telegram (no regression), and the owner
// default honors an explicit owners list.
static void test_telegram_routine_on_a_telegram_device_is_unchanged() {
  const DeliveryWorld w = deliveryWorld("tok", "123456789", "");
  TEST_ASSERT_EQUAL_STRING("123456789", resolveDelivery(originChat("", "123456789"), w, true).target.c_str());
  TEST_ASSERT_EQUAL_STRING("123456789", resolveDelivery("", w, false).target.c_str());
  const DeliveryWorld owners = deliveryWorld("tok", "555,123456789", "123456789");
  TEST_ASSERT_EQUAL_STRING("123456789", resolveDelivery("", owners, false).target.c_str());
}

static void test_origin_chat() {
  TEST_ASSERT_EQUAL_STRING("123", originChat("123", "web").c_str());   // explicit wins
  TEST_ASSERT_EQUAL_STRING("web", originChat("", "web").c_str());
  TEST_ASSERT_EQUAL_STRING("web", originChat("  ", " web ").c_str());
  TEST_ASSERT_EQUAL_STRING("", originChat("", "").c_str());   // web Routines page: owner default
}

// The delivered sentence is plain ASCII copy: no em dash, no exclamation.
static void test_fallback_sentence_copy() {
  for (int f = 0; f < (int)DeliveryChannel::Count; ++f)
    for (int t = 0; t < (int)DeliveryChannel::Count; ++t) {
      Delivery d;
      d.from = DeliveryChannel(f);
      d.to = DeliveryChannel(t);
      const std::string s = deliveryFallbackSentence(d);
      TEST_ASSERT_TRUE(s.size() < 120);
      TEST_ASSERT_TRUE(s.find('!') == std::string::npos);
      for (unsigned char ch : s) TEST_ASSERT_TRUE(ch >= 0x20 && ch < 0x7F);
    }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_channel_of_every_slug);
  RUN_TEST(test_every_channel_has_a_label);
  RUN_TEST(test_world_owner_rule);
  RUN_TEST(test_every_channel_every_world);
  RUN_TEST(test_sweep_reaches_every_verdict);
  RUN_TEST(test_old_rule_fails_the_sweep);
  RUN_TEST(test_fire_request_for_every_kind_channel_world);
  RUN_TEST(test_lift_device_channel_revoke_over_every_case);
  RUN_TEST(test_web_chat_reminder_on_a_web_only_device_reaches_the_web_chat);
  RUN_TEST(test_web_routine_stays_in_the_web_chat_when_telegram_is_set_up);
  RUN_TEST(test_voice_reminder_returns_to_the_screen);
  RUN_TEST(test_owner_telegram_routine_falls_back_and_says_so);
  RUN_TEST(test_member_routine_pauses_when_the_bot_is_off);
  RUN_TEST(test_emptied_allowlist_pauses_telegram_routines);
  RUN_TEST(test_revoked_chat_still_pauses);
  RUN_TEST(test_telegram_routine_on_a_telegram_device_is_unchanged);
  RUN_TEST(test_origin_chat);
  RUN_TEST(test_fallback_sentence_copy);
  return UNITY_END();
}
