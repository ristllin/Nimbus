#include <unity.h>

#include <string>

#include "nimbus/net/owner_notice.h"

// Where a device notice raised on the Telegram poll task goes
// (nimbus/net/owner_notice.h). The class rule: every notice kind reaches someone. With a
// Telegram owner chat it goes there on Telegram (and its history row); with none (a
// web-only device, the usual first setup) it is an assistant row of the web chat,
// tagged as a device event, never dropped.

using nimbus::net::kOwnerNoticeAll;
using nimbus::net::NoticeRoute;
using nimbus::net::noticeRouteOf;
using nimbus::net::OwnerNotice;

void setUp() {}
void tearDown() {}

// Every kind, with and without an owner chat: never an empty chat, Telegram only for a
// real owner chat, a device-event tag only on the web chat's row.
static void test_every_notice_has_a_route() {
  for (const OwnerNotice kind : kOwnerNoticeAll) {
    const NoticeRoute owned = noticeRouteOf("6098532481", kind);
    TEST_ASSERT_EQUAL_STRING("6098532481", owned.chat.c_str());
    TEST_ASSERT_TRUE(owned.telegram);
    TEST_ASSERT_EQUAL_STRING("", owned.tag);
    const NoticeRoute web = noticeRouteOf("", kind);
    TEST_ASSERT_EQUAL_STRING("web", web.chat.c_str());
    TEST_ASSERT_FALSE(web.telegram);
    TEST_ASSERT_EQUAL_INT(0, std::string(web.tag).rfind("ev:", 0));
  }
}

// The storage notices share the storage tag the DoD and the web siblings use
// ("ev:sd", "ev:spend", now "ev:storage"); the storm breakers' notices have their own.
static void test_tags() {
  TEST_ASSERT_EQUAL_STRING("ev:storage", noticeRouteOf("", OwnerNotice::Storage).tag);
  TEST_ASSERT_EQUAL_STRING("ev:storage", noticeRouteOf("", OwnerNotice::FileStore).tag);
  TEST_ASSERT_EQUAL_STRING("ev:guard", noticeRouteOf("", OwnerNotice::Guard).tag);
}

// Once per episode on the web route: a row of the last day that already carries the
// notice (its tag among the row's tags, the same words) means it is not written again;
// another notice under the same tag, or a near-miss tag, does not count.
static void test_same_notice_row() {
  using nimbus::net::sameNoticeRow;
  const std::string text = "Nimbus: settings storage is almost full, so some changes may not be saved.";
  TEST_ASSERT_TRUE(sameNoticeRow("ev:storage", text, "ev:storage", text));
  TEST_ASSERT_TRUE(sameNoticeRow("from:x,ev:storage", text, "ev:storage", text));
  TEST_ASSERT_FALSE(sameNoticeRow("ev:storagex", text, "ev:storage", text));
  TEST_ASSERT_FALSE(sameNoticeRow("ev:storage", "Nimbus: the file store is full.", "ev:storage", text));
  TEST_ASSERT_FALSE(sameNoticeRow("", text, "ev:storage", text));
  TEST_ASSERT_TRUE(nimbus::net::kWebNoticeRepeatHours >= 24);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_notice_has_a_route);
  RUN_TEST(test_tags);
  RUN_TEST(test_same_notice_row);
  return UNITY_END();
}
