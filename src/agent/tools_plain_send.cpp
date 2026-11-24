// tools_plain_send.cpp - the device binding of engine 2's send and speak (lane E2-P-OUT1; the rules are
// tools_plain_send.h). Every effect is engine 1's delivery code: files::sendStored (files.send),
// orchestrator::sendToChat (reply.telegram, voice through voicereply::deliver), speakOnDevice
// (reply.speak); every text passes orchestrator::screenReply (deliver()'s rules) first. Engine 2 only
// (tools/engine/engine2_only.txt); tools_plain.cpp calls registerSend from the family hook.
#include "tools_plain_send.h"

#include <Arduino.h>

#include <cctype>
#include <string>

#include "files_subsystem.h"           // files::sendStored - engine 1's files.send
#include "memory_subsystem.h"          // memory::Lock, haveSd, dataFs - a stored photo by ref
#include "nimbus/orch/action_rules.h"  // filesSendTarget - engine 1's default file target
#include "nimbus/orch/blob_store.h"    // blobPath
#include "orchestrator.h"              // turnInFlight, currentChat, isChatAllowed, sendToChat, speakOnDevice
#include "orchestrator_common.h"       // screenReply, chatAllowed
#include "store.h"                     // ttsEnabled, sfxVolume
#include "telegram.h"                  // onTurnTask, ownerChat, sendMediaSd
#include "voice_reply.h"               // voicereply::Outcome

namespace agent {
namespace tools {

namespace {

namespace ps = agent::plainsend;
namespace orch = nimbus::orch;

std::string str(const String& s) { return std::string(s.c_str(), s.length()); }

bool localChat(const std::string& c) { return c.empty() || c == "web" || c == "voice" || c == "serial"; }

// The chat a send goes to, as engine 1 chose it: files.send's default (the turn's Telegram chat, else
// the owner's chat for the owner) and allowlist; reply.telegram's (the turn's chat or an allowlisted one).
std::string target(const std::string& asked, const orch::Principal& who, bool file) {
  const String cur = orchestrator::currentChat();
  std::string cid = asked;
  if (cid.empty())
    cid = file ? orch::filesSendTarget(str(cur), orchestrator::isChatAllowed(cur), who.perms().manageTenants,
                                       str(telegram::ownerChat()))
               : str(cur);
  if (localChat(cid)) return std::string();
  const String c(cid.c_str());
  const bool allowed = file ? orchestrator::isChatAllowed(c) : (c == cur || orchestrator::chatAllowed(c));
  return allowed ? cid : std::string();
}

ps::FileSent fileSent(files::StoredSend r) {
  switch (r) {
    case files::StoredSend::Queued: return ps::FileSent::Queued;
    case files::StoredSend::NotImage: return ps::FileSent::NotImage;
    case files::StoredSend::NoTarget: return ps::FileSent::NoTarget;
    case files::StoredSend::Busy: return ps::FileSent::Busy;
    default: return ps::FileSent::NotFound;
  }
}

// A photo of this conversation by its ref (p + the blob's hex hash, as view takes it): its stored copy
// on the card, sent on the same media lane as files.send.
ps::FileSent sendPhotoRef(const std::string& ref, const std::string& chat, const std::string& caption) {
  if (ref.size() < 9 || ref.size() > 65 || ref[0] != 'p' || !memory::haveSd()) return ps::FileSent::NotFound;
  const std::string hash = ref.substr(1);
  for (char ch : hash)
    if (!std::isxdigit((unsigned char)ch)) return ps::FileSent::NotFound;
  std::string path;
  {
    memory::Lock lk;
    for (const char* ext : {"jpg", "bin"}) {
      const std::string p = orch::blobPath("/mem/blobs", hash, ext);
      if (memory::dataFs().exists(p.c_str())) {
        path = p;
        break;
      }
    }
  }
  if (path.empty()) return ps::FileSent::NotFound;
  if (!telegram::sendMediaSd(String(chat.c_str()), "photo", path.c_str(), String(caption.c_str())))
    return ps::FileSent::Busy;
  return ps::FileSent::Queued;
}

ps::SendDeps deviceDeps() {
  ps::SendDeps d;
  d.inTurn = [] { return orchestrator::turnInFlight() && telegram::onTurnTask(); };
  d.target = target;
  d.turnChat = [] { return str(orchestrator::currentChat()); };
  d.voiceOn = [] { return store::ttsEnabled(); };
  d.speakerMuted = [] { return store::sfxVolume() == 0; };
  d.screen = [](const std::string& chat, std::string& text) {
    String body;
    if (!orchestrator::screenReply(String(chat.c_str()), String(text.c_str()), /*system=*/false, body)) return false;
    text = str(body);
    return true;
  };
  d.sendFile = [](const std::string& project, const std::string& name, const std::string& chat,
                  const orch::Principal& who, bool photoOnly, const std::string& caption) {
    return fileSent(
        files::sendStored(project, name, String(chat.c_str()), who, photoOnly, String(caption.c_str())));
  };
  d.sendPhotoRef = sendPhotoRef;
  d.sendText = [](const std::string& chat, const std::string& text) {
    return orchestrator::sendToChat(String(chat.c_str()), String(text.c_str()), /*asVoice=*/false);
  };
  d.sendVoice = [](const std::string& chat, const std::string& text) {
    voicereply::Outcome o;
    (void)orchestrator::sendToChat(String(chat.c_str()), String(text.c_str()), /*asVoice=*/true, nullptr, &o);
    ps::VoiceSent v;
    v.delivered = o.delivered;
    v.notesSent = o.notesSent;
    v.receipt = o.receipt;
    v.why = o.why;
    return v;
  };
  d.speak = [](const std::string& text, std::string& why) {
    String w;
    const bool ok = orchestrator::speakOnDevice(String(text.c_str()), /*capture=*/true, &w);
    why = str(w);
    return ok;
  };
  return d;
}

}  // namespace

void registerSend(nimbus::orch::ToolRegistry& reg) { (void)ps::registerSendTools(reg, deviceDeps()); }

}  // namespace tools
}  // namespace agent
