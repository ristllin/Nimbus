#pragma once
#include <functional>
#include <mutex>
#include <string>

#include "../../src/agent/tools_plain_send.h"   // the device's send rules (header-only, Arduino-free)
#include "nimbus/orch/action_rules.h"           // filesSendTarget: engine 1's default file target
#include "nimbus/orch/blob_store.h"             // blobPath
#include "nimbus/orch/media_label.h"            // validRef
#include "nimbus/orch/path_rules.h"             // storePathFor
#include "nimbus/orch/turn_images.h"            // sniffImageMime
#include "posix_files.h"
#include "posix_fs.h"

// hosted_send - engine 2's `send` on a Virtual Nimbus (lane E2-REP3; the device's binding is
// src/agent/tools_plain_send.cpp). The rules are the device's (agent::plainsend, one header); the effects
// are the daemon's own delivery:
//   - kind text: a second message through the daemon's deliver function (the web chat ring, or Telegram for
//     an approved chat), to this chat or an approved Telegram chat;
//   - kind file or photo: the stored file, read by the person's read rule, uploaded to Telegram (sendPhoto
//     for an image, else sendDocument); a photo of this conversation by its ref from the instance's photo
//     store. With no Telegram chat to take it (a web turn of a member, no bot) the device's "no allowed
//     Telegram chat" line;
//   - kind voice and speak: the instance has no speech path, so its "Voice replies" setting is off: a voice
//     send arrives as text (the device's line says so) and speak says voice replies are off.
// The instance keeps no output rules of its own, so every text leaves as the model wrote it.
namespace nimbusd {

struct HostedSendParts {
  std::function<std::string()> turnChat;                       // the running turn's chat ("" outside a turn)
  std::string webChat;                                         // the web chat's id (the daemon's ring)
  std::function<bool(const std::string& chat)> allowed;        // an approved Telegram chat
  std::function<std::string()> ownerChat;                      // the owner's Telegram chat ("" none)
  std::function<void(const std::string& chat, const std::string& text)> deliver;
  // One upload to Telegram: kind "photo" or "document"; false when it did not go (no bot, refused).
  std::function<bool(const std::string& chat, const std::string& kind, const std::string& name,
                     const std::string& bytes, const std::string& caption)>
      media;
  PosixFiles* files = nullptr;
  std::string photoDir;                                        // <hash>.bin of each photo kept
};

namespace hostedsend {

namespace ps = agent::plainsend;

inline bool approved(const HostedSendParts& p, const std::string& chat) { return p.allowed && p.allowed(chat); }

// The chat a send names none for: this chat for words; for a file engine 1's filesSendTarget (this Telegram chat
// when approved, else the owner's Telegram chat for the owner).
inline std::string defaultTarget(const HostedSendParts& p, const std::string& cur, const nimbus::orch::Principal& who,
                                 bool file) {
  if (!file) return cur;
  const bool curTelegram = !cur.empty() && cur != p.webChat;
  return nimbus::orch::filesSendTarget(curTelegram ? cur : std::string(), curTelegram && approved(p, cur),
                                       who.perms().manageTenants, p.ownerChat ? p.ownerChat() : std::string());
}

inline std::string target(const HostedSendParts& p, const std::string& asked, const nimbus::orch::Principal& who,
                          bool file) {
  const std::string cur = p.turnChat ? p.turnChat() : std::string();
  const std::string cid = asked.empty() ? defaultTarget(p, cur, who, file) : asked;
  if (cid.empty()) return std::string();
  if (cid == p.webChat) return !file && cur == p.webChat ? cid : std::string();   // the web ring takes words only
  return approved(p, cid) || (!file && cid == cur) ? cid : std::string();
}

inline ps::FileSent upload(const HostedSendParts& p, const std::string& chat, const std::string& name,
                           const std::string& bytes, bool photoOnly, const std::string& caption) {
  const bool image = *nimbus::orch::sniffImageMime((const uint8_t*)bytes.data(), bytes.size()) != '\0';
  if (photoOnly && !image) return ps::FileSent::NotImage;
  if (!p.media || !p.media(chat, image ? "photo" : "document", name, bytes, caption)) return ps::FileSent::Busy;
  return ps::FileSent::Queued;
}

// A stored file the person may read (the store's read rule), uploaded whole.
inline ps::FileSent sendFile(const HostedSendParts& p, const std::string& path, const std::string& chat,
                             const nimbus::orch::Principal& who, bool photoOnly, const std::string& caption) {
  if (!p.files) return ps::FileSent::NotFound;
  const nimbus::orch::StorePath sp = nimbus::orch::storePathFor(path);
  if (!sp.ok()) return ps::FileSent::NotFound;
  std::string bytes;
  {
    std::lock_guard<std::recursive_mutex> g(p.files->mutex());
    const nimbus::orch::FileEntry* e = p.files->toolsIndex().find(sp.project, sp.name);
    const bool shared = who.owner || who.perms().readShared;
    if (!e || !p.files->toolsIndex().readableBy(*e, who.ns, who.owner, shared)) return ps::FileSent::NotFound;
    if (!p.files->fetch(sp.project, sp.name, bytes)) return ps::FileSent::NotFound;
  }
  return upload(p, chat, sp.name, bytes, photoOnly, caption);
}

// A photo of this conversation by its ref (p + the hash), from the instance's photo store.
inline ps::FileSent sendPhotoRef(const HostedSendParts& p, const std::string& ref, const std::string& chat,
                                 const std::string& caption) {
  const std::string hash = ref.size() > 1 && ref[0] == 'p' ? ref.substr(1) : std::string();
  if (p.photoDir.empty() || !nimbus::orch::validRef(hash)) return ps::FileSent::NotFound;
  std::string bytes;
  if (!fsutil::readFile(nimbus::orch::blobPath(p.photoDir, hash, "bin"), bytes) || bytes.empty())
    return ps::FileSent::NotFound;
  return upload(p, chat, "photo.jpg", bytes, true, caption);
}

inline ps::SendDeps deps(const HostedSendParts& p) {
  ps::SendDeps d;
  d.inTurn = [p] { return p.turnChat && !p.turnChat().empty(); };
  d.target = [p](const std::string& asked, const nimbus::orch::Principal& who, bool file) {
    return target(p, asked, who, file);
  };
  d.turnChat = [p] { return p.turnChat ? p.turnChat() : std::string(); };
  d.voiceOn = [] { return false; };       // no speech path on an instance
  d.speakerMuted = [] { return true; };   // and no speaker
  d.screen = [](const std::string&, std::string&) { return true; };
  d.sendFile = [p](const std::string& project, const std::string& name, const std::string& chat,
                   const nimbus::orch::Principal& who, bool photoOnly, const std::string& caption) {
    return sendFile(p, project + "/" + name, chat, who, photoOnly, caption);
  };
  d.sendPhotoRef = [p](const std::string& ref, const std::string& chat, const std::string& caption) {
    return sendPhotoRef(p, ref, chat, caption);
  };
  d.sendText = [p](const std::string& chat, const std::string& text) {
    if (!p.deliver) return false;
    p.deliver(chat, text);
    return true;
  };
  d.sendVoice = [](const std::string&, const std::string&) { return ps::VoiceSent{}; };   // never reached: voice is off
  d.speak = [](const std::string&, std::string&) { return false; };                     // never reached: voice is off
  return d;
}

}  // namespace hostedsend

// send (and speak, behind more) on `reg` over the daemon's delivery.
inline bool registerHostedSend(nimbus::orch::ToolRegistry& reg, const HostedSendParts& p) {
  return agent::plainsend::registerSendTools(reg, hostedsend::deps(p));
}

}  // namespace nimbusd
