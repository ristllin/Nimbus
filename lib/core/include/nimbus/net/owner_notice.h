#pragma once
#include <cstdint>
#include <string>

// owner_notice - where a device notice raised on the Telegram poll task goes: the
// settings-storage and file-store notices, and the storm breakers' notices (a repeat
// skipped, the hourly ceiling, work that crashed the device twice).
//
// With a Telegram owner chat (telegram::ownerChat: the first allow-listed owner, else the
// first allow-list entry) the notice is queued to that chat on Telegram and captured as
// its assistant row, which the web chat shows too. A device with no Telegram owner (a
// web-only setup, the usual first one) gets them in the web chat as an assistant row
// tagged as a device event, the same row the memory-card and spend-ceiling notices
// write. (Those two also refresh an already open page at once; these show on the page's
// next history read, which the poll task cannot trigger: the web chat's live channel is
// outside it.) Every kind has a route (pinned by test/test_owner_notice over every kind).
namespace nimbus::net {

enum class OwnerNotice : uint8_t { Storage, FileStore, Guard };
// Every kind, for the tests that walk them all. A new kind goes here too; the switch
// below has no default, so the compiler flags a kind it does not route.
constexpr OwnerNotice kOwnerNoticeAll[] = {OwnerNotice::Storage, OwnerNotice::FileStore, OwnerNotice::Guard};

struct NoticeRoute {
  std::string chat;        // the chat whose history gets the assistant row
  bool telegram = false;   // also queue it to that chat on Telegram
  const char* tag = "";    // the row's tag ("" for the owner chat's row, as before)
};

// The device-event tag of a notice's web chat row.
inline const char* ownerNoticeTag(OwnerNotice k) {
  switch (k) {
    case OwnerNotice::Storage:
    case OwnerNotice::FileStore: return "ev:storage";
    case OwnerNotice::Guard: return "ev:guard";
  }
  return "ev:notice";
}

// The route of a notice of `kind` for the owner chat `ownerChat` ("" when there is none).
inline NoticeRoute noticeRouteOf(const std::string& ownerChat, OwnerNotice kind) {
  NoticeRoute r;
  if (!ownerChat.empty()) {
    r.chat = ownerChat;
    r.telegram = true;
    return r;
  }
  r.chat = "web";
  r.tag = ownerNoticeTag(kind);
  return r;
}

}  // namespace nimbus::net
