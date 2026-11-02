#pragma once
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "hosted_photos.h"
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
  // A turn that brings photos: its caption and the photos (an album arrives together).
  std::function<void(const std::string& chat, const std::string& text, const std::vector<PhotoIn>& photos)> photoTurn;
  // Download one Telegram file. False leaves the reason in `err`.
  std::function<bool(const std::string& fileId, std::string& bytes, std::string& err)> fetchFile;
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

// What the hosted bot says to a voice note, video or file: it takes text and photos.
// Never silence: the sender would wait for an answer that never comes.
constexpr const char* kTgNotTaken = "This assistant reads text and photos here. Send it as text, or add it under Files on its page.";

// The words of a message that came with something this bot cannot read: they run as a
// turn, and the head is told what did not reach it, so it never answers as if it had read
// it. Constant words: a file name is the sender's text and stays out of the framing.
inline const char* tgAttachmentWord(nimbus::tg::Attachment::Kind k) {
  using K = nimbus::tg::Attachment::Kind;
  switch (k) {
    case K::Document: return "file";
    case K::Voice: return "voice note";
    case K::Video:
    case K::VideoNote: return "video";
    case K::Sticker: return "sticker";
    case K::Audio: return "audio clip";
    case K::Animation: return "animation";
    default: return "attachment";
  }
}
inline std::string tgCaptionTurn(const nimbus::tg::Update& u) {
  if (u.attachment.kind == nimbus::tg::Attachment::Kind::None) return u.text;
  return u.text + "\n\n[The sender also attached a " + tgAttachmentWord(u.attachment.kind) +
         " to this message. It did not reach you: this assistant reads only text and photos here. "
         "Say so, and do not guess what it holds.]";
}

// A photo the head can take: a Telegram photo, or an image sent as a file.
inline bool isTgPhoto(const nimbus::tg::Attachment& a) {
  using K = nimbus::tg::Attachment::Kind;
  if (a.kind == K::Photo) return true;
  return a.kind == K::Document && a.mime.rfind("image/", 0) == 0;
}

// One approved update's photo: fetched now (this thread owns the poll transport); a
// fetch that failed still reaches the turn, as its reason.
inline PhotoIn fetchTgPhoto(const nimbus::tg::Attachment& a, const TgInboundIo& io) {
  PhotoIn p;
  static constexpr const char* kTooMany = "too many photos are waiting for their turn; send it again in a while";
  if (!photoBudget().hasRoom()) {   // not even downloaded
    p.why = kTooMany;
    return p;
  }
  std::string err, bytes;
  if (!io.fetchFile || !io.fetchFile(a.fileId, bytes, err)) {
    p.why = err.empty() ? "the download failed" : err;
    return p;
  }
  p.bytes = photoBudget().hold(std::move(bytes));
  if (!p.bytes) p.why = kTooMany;
  return p;
}

// The photos of one message, or of its whole album (Telegram sends an album's items
// as neighbouring updates of one media group), and their shared caption. Returns the
// index of the last update it took.
inline size_t collectTgPhotos(const std::vector<nimbus::tg::Update>& ups, size_t first, const TgInboundIo& io,
                              std::string& caption, std::vector<PhotoIn>& photos) {
  size_t i = first;
  const nimbus::tg::Update& head = ups[first];
  for (; i < ups.size(); i++) {
    const nimbus::tg::Update& u = ups[i];
    const bool same = i == first || (!head.mediaGroupId.empty() && u.mediaGroupId == head.mediaGroupId && u.chatId == head.chatId);
    if (!same || !isTgPhoto(u.attachment) || photos.size() >= nimbus::orch::kTurnImageMax) break;
    if (caption.empty()) caption = u.text;
    photos.push_back(fetchTgPhoto(u.attachment, io));
  }
  return i - 1;
}

// One admitted update: a photo turn (its album collected), a text turn (with a note
// when an unreadable attachment came with the words), or the plain "not taken" line.
// Returns the index of the last update consumed.
inline size_t serveOne(const std::vector<nimbus::tg::Update>& ups, size_t i, const TgInboundIo& io,
                       TgRouteStats& st) {
  const auto& u = ups[i];
  if (isTgPhoto(u.attachment) && io.photoTurn) {
    std::string caption;
    std::vector<PhotoIn> photos;
    const size_t last = collectTgPhotos(ups, i, io, caption, photos);
    io.photoTurn(u.chatId, caption, photos);
    st.turns++;
    return last;
  }
  const bool attached = u.attachment.kind != nimbus::tg::Attachment::Kind::None && !u.edited;
  if (!u.text.empty() && io.turn) {
    if (attached && io.send) io.send(u.chatId, kTgNotTaken);   // said at once: the turn may wait behind others
    io.turn(u.chatId, tgCaptionTurn(u));
    st.turns++;
  } else if (attached && io.send) {
    io.send(u.chatId, kTgNotTaken);
  }
  return i;
}

inline TgRouteStats routeTelegramUpdates(const std::vector<nimbus::tg::Update>& ups,
                                         TelegramAccess& access, const TgInboundIo& io,
                                         time_t now) {
  TgRouteStats st;
  for (size_t i = 0; i < ups.size(); i++) {
    const auto& u = ups[i];
    if (!TelegramAccess::isChatId(u.chatId)) continue;   // no chat, nothing to gate on
    const TelegramAccess::Admit why = access.admit(u.chatId);
    if (why == TelegramAccess::Admit::Serve) {
      i = serveOne(ups, i, io, st);
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
