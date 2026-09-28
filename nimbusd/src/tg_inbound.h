#pragma once
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/tg_updates.h"
#include "tg_access.h"

// tg_inbound - one getUpdates batch through the device trust model (CUM-459).
//
// The device classifies each update on its poll task (telegram.cpp handleUpdate):
// an allow-listed chat's text becomes a turn; an unlisted chat is queued for the
// owner's one-tap approval and gets NO turn. This is the same routing, kept out of
// main.cpp so it is host-tested with fakes. The only hosted addition is the reply
// to a refused chat: one polite message, at most once per cooldown per chat,
// telling them the owner has to approve them (a hosted bot has no screen where
// the owner would notice the knock).
namespace nimbusd {

struct TgInboundIo {
  // Run one turn for an approved chat (the daemon posts it to the engine thread).
  std::function<void(const std::string& chat, const std::string& text)> turn;
  // Send a Telegram message (the refusal).
  std::function<void(const std::string& chat, const std::string& text)> send;
};

struct TgRouteStats {
  int turns = 0;        // messages handed to the engine
  int refused = 0;      // updates from unlisted or revoked chats
  int refusalsSent = 0; // refusal messages actually sent (rate-limited)
};

// The refusal copy. The chat id is included so the sender can hand it to the owner,
// who can approve them by id from the web app.
inline std::string tgRefusalText(TelegramAccess::Admit why, const std::string& chatId) {
  if (why == TelegramAccess::Admit::Revoked)
    return "Your access to this assistant has been removed, so it has not read your "
           "message. Ask its owner if you think this is a mistake.";
  return "This assistant only answers people its owner has approved, so it has not read "
         "your message. The owner can approve you in the Nimbus web app. Your chat ID is " +
         chatId + ".";
}

inline TgRouteStats routeTelegramUpdates(const std::vector<nimbus::tg::Update>& ups,
                                         TelegramAccess& access, const TgInboundIo& io,
                                         time_t now) {
  TgRouteStats st;
  for (const auto& u : ups) {
    if (!TelegramAccess::isChatId(u.chatId)) continue;   // no chat, nothing to gate on
    const TelegramAccess::Admit why = access.admit(u.chatId);
    if (why == TelegramAccess::Admit::Serve) {
      // Text (or a caption) is a turn; a bare voice note or file stays unhandled on a
      // hosted instance, as before.
      if (!u.text.empty() && io.turn) {
        io.turn(u.chatId, u.text);
        st.turns++;
      }
      continue;
    }
    st.refused++;
    if (why == TelegramAccess::Admit::Unlisted) access.notePending(u.chatId, u.from, u.text);
    if (io.send && access.takeRefusal(u.chatId, now)) {
      io.send(u.chatId, tgRefusalText(why, u.chatId));
      st.refusalsSent++;
    }
  }
  return st;
}

}  // namespace nimbusd
