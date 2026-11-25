#pragma once
// tools_plain_send - engine 2's send and speak (lane E2-P-OUT1; inventory parity-modalities-media rows 11
// and 12): the tool rules here, header-only and Arduino-free so the native tests run them with a scripted
// head; every effect is engine 1's own delivery code, bound by tools_plain_send.cpp:
//   - send kind file or photo: files::sendStored (engine 1's files.send: the reader's access, an image as a
//     photo, the media lane), or a stored photo by ref to this chat;
//   - send kind text (a second message, here or to an allowed chat) and kind voice: orchestrator::sendToChat
//     (engine 1's reply.telegram; voice through voicereply::deliver, with Telegram's receipt);
//   - speak (behind more, the media group): orchestrator::speakOnDevice (engine 1's reply.speak).
// Every text that leaves (a message, a voice note's words, a caption, speech) first passes the shared
// screen, orchestrator::screenReply: deliver()'s owner output rules and outbound screen.
// The owner's "Voice replies" setting off: a voice send goes as text (never silent), speak says so.
#include <ArduinoJson.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "nimbus/harness/plain/request.h"   // toolDecl, declSchema
#include "nimbus/harness/plain/texts.h"     // tool_results::kSend*, kSpeak*
#include "nimbus/orch/tool_registry.h"

namespace agent {
namespace plainsend {

// files::sendStored's answer (engine 1's files.send), mirrored so this header stays portable.
enum class FileSent : uint8_t { Queued, NotFound, NotImage, NoTarget, Busy };

// What a voice note delivery did (voicereply::Outcome, the fields the result names).
struct VoiceSent {
  bool delivered = false;
  size_t notesSent = 0;
  std::string receipt;   // Telegram's message id of the first note
  std::string why;
};

struct SendDeps {
  std::function<bool()> inTurn;                    // a turn runs, on the turn task
  // The Telegram chat a send goes to: `asked` (the chat argument) or, empty, the turn's chat (a file:
  // engine 1's filesSendTarget, the owner's chat for the owner on a local turn); "" when that chat is
  // local or not allowed.
  std::function<std::string(const std::string& asked, const nimbus::orch::Principal& who, bool file)> target;
  std::function<std::string()> turnChat;           // the running turn's chat ("" outside one)
  std::function<bool()> voiceOn;                   // the owner's "Voice replies" setting
  std::function<bool()> speakerMuted;              // volume 0: speech would play as silence
  // The shared screen (orchestrator::screenReply): `text` becomes what may leave; false: suppressed.
  std::function<bool(const std::string& chat, std::string& text)> screen;
  std::function<FileSent(const std::string& project, const std::string& name, const std::string& chat,
                         const nimbus::orch::Principal& who, bool photoOnly, const std::string& caption)>
      sendFile;
  std::function<FileSent(const std::string& ref, const std::string& chat, const std::string& caption)> sendPhotoRef;
  std::function<bool(const std::string& chat, const std::string& text)> sendText;   // queued
  std::function<VoiceSent(const std::string& chat, const std::string& text)> sendVoice;
  std::function<bool(const std::string& text, std::string& why)> speak;
};

namespace detail {

namespace tr = plain::texts::tool_results;
using nimbus::orch::ToolResult;

inline std::string arg(ArduinoJson::JsonObjectConst a, const char* key) {
  return a[key].is<const char*>() ? std::string(a[key].as<const char*>()) : std::string();
}

// project/name: exactly two non-empty parts.
inline bool splitPath(const std::string& path, std::string& project, std::string& name) {
  const size_t k = path.find('/');
  if (k == std::string::npos || k == 0 || k + 1 >= path.size() || path.find('/', k + 1) != std::string::npos)
    return false;
  project = path.substr(0, k);
  name = path.substr(k + 1);
  return true;
}

inline ToolResult fileResult(FileSent r, const std::string& chat) {
  switch (r) {
    case FileSent::Queued: return ToolResult::ok(tr::kSendQueued + chat);
    case FileSent::NotImage: return ToolResult::fail(tr::kSendNotImage);
    case FileSent::NoTarget: return ToolResult::fail(tr::kSendNoChat);
    case FileSent::Busy: return ToolResult::fail(tr::kSendBusy);
    default: return ToolResult::fail(tr::kSendNotFound);
  }
}

inline ToolResult sendMedia(const SendDeps& d, ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who,
                            bool photo) {
  const std::string path = arg(a, "path"), ref = photo ? arg(a, "ref") : std::string();
  std::string project, name;
  if (ref.empty() && !splitPath(path, project, name)) return ToolResult::fail(tr::kSendArgs);
  const std::string chat = d.target(arg(a, "chat"), who, ref.empty());
  if (chat.empty()) return ToolResult::fail(tr::kSendNoChat);
  std::string caption = arg(a, "caption");
  if (caption.empty() && ref.empty()) caption = path;
  if (!caption.empty() && !d.screen(chat, caption)) return ToolResult::fail(tr::kSendBlocked);
  if (!ref.empty()) {   // a photo of this conversation, to this chat only
    if (chat != d.turnChat()) return ToolResult::fail(tr::kSendNoChat);
    return fileResult(d.sendPhotoRef(ref, chat, caption), chat);
  }
  return fileResult(d.sendFile(project, name, chat, who, photo, caption), chat);
}

inline ToolResult sendWords(const SendDeps& d, ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who,
                            bool voice) {
  std::string text = arg(a, "text");
  if (text.empty()) return ToolResult::fail(tr::kSendArgs);
  const std::string chat = d.target(arg(a, "chat"), who, false);
  if (chat.empty()) return ToolResult::fail(tr::kSendNoChat);
  if (!d.screen(chat, text)) return ToolResult::fail(tr::kSendBlocked);
  if (!voice || !d.voiceOn()) {   // the setting off: the words still arrive, as text
    if (!d.sendText(chat, text)) return ToolResult::fail(tr::kSendBusy);
    return ToolResult::ok((voice ? std::string(tr::kSendVoiceOff) : std::string()) + tr::kSendQueued + chat);
  }
  const VoiceSent v = d.sendVoice(chat, text);
  if (v.delivered) return ToolResult::ok(tr::kSendVoiceDone + v.receipt);
  std::string out = tr::kSendVoiceFailed + v.why;
  if (v.notesSent) out += tr::kSendVoiceParts + std::to_string((unsigned long long)v.notesSent);
  return ToolResult::fail(out);
}

}  // namespace detail

inline nimbus::orch::ToolResult send(const SendDeps& d, ArduinoJson::JsonObjectConst a,
                                     const nimbus::orch::Principal& who) {
  namespace tr = plain::texts::tool_results;
  if (!d.inTurn()) return nimbus::orch::ToolResult::fail(tr::kSendNotInTurn);
  const std::string kind = detail::arg(a, "kind");
  if (kind == "file" || kind == "photo") return detail::sendMedia(d, a, who, kind == "photo");
  if (kind == "text" || kind == "voice") return detail::sendWords(d, a, who, kind == "voice");
  return nimbus::orch::ToolResult::fail(tr::kSendArgs);
}

inline nimbus::orch::ToolResult speak(const SendDeps& d, ArduinoJson::JsonObjectConst a) {
  namespace tr = plain::texts::tool_results;
  using nimbus::orch::ToolResult;
  if (!d.inTurn()) return ToolResult::fail(tr::kSendNotInTurn);
  std::string text = detail::arg(a, "text");
  if (text.empty()) return ToolResult::fail(tr::kSendArgs);
  if (!d.voiceOn()) return ToolResult::fail(tr::kSpeakOff);
  if (d.speakerMuted()) return ToolResult::fail(tr::kSpeakMuted);
  if (!d.screen(d.turnChat(), text)) return ToolResult::fail(tr::kSendBlocked);
  std::string why;
  if (!d.speak(text, why)) return ToolResult::fail(tr::kSpeakFailed + why);
  return ToolResult::ok(tr::kSpeakDone);
}

// Register send (always on) and speak (behind more) with the manifest's description and schema.
// False when a declaration is missing (nothing registered then).
inline bool registerSendTools(nimbus::orch::ToolRegistry& reg, const SendDeps& deps) {
  const auto* s = plain::toolDecl("send");
  const auto* k = plain::toolDecl("speak");
  if (!s || !k) return false;
  auto d = std::make_shared<SendDeps>(deps);
  reg.add(
      s->name, s->description,
      [d](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) { return send(*d, a, who); },
      plain::declSchema(*s, false));
  reg.add(
      k->name, k->description,
      [d](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) { return speak(*d, a); },
      plain::declSchema(*k, false));
  return true;
}

}  // namespace plainsend

namespace tools {
// The device binding (tools_plain_send.cpp, engine 2 only): send and speak over engine 1's delivery code.
void registerSend(nimbus::orch::ToolRegistry& reg);
}  // namespace tools
}  // namespace agent
