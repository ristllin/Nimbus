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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_notice_has_a_route);
  RUN_TEST(test_tags);
  return UNITY_END();
}
