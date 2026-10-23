#include "voice_reply.h"

#include <LittleFS.h>

#include <vector>

#include "../sys/agent_log.h"
#include "adapters/audio_tts.h"
#include "nimbus/orch/voice_notes.h"   // the split, the caps and the tool's words (host-tested)
#include "telegram.h"

namespace agent {
namespace voicereply {

namespace {

using nimbus::orch::VoiceNoteSent;

constexpr const char* kTempPrefix = "tgv-";

// A clip a restart left behind mid-upload is removed before the next one is made, so
// the flash never fills with voice notes nobody will send.
void sweepStale() {
  File dir = LittleFS.open("/");
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }
  std::vector<String> stale;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    const char* nm = f.name();
    const char* base = strrchr(nm, '/');
    base = base ? base + 1 : nm;
    if (!f.isDirectory() && strncmp(base, kTempPrefix, strlen(kTempPrefix)) == 0)
      stale.push_back(String("/") + base);
    f.close();
  }
  dir.close();
  for (const String& p : stale) LittleFS.remove(p);
  if (!stale.empty()) alogf("voice: removed %u leftover clip(s)", (unsigned)stale.size());
}

// A file name no other clip uses: the speaker's /reply.mp3 and a pending media send
// keep their own files.
String tempPath() {
  static uint16_t s_seq = 0;
  char p[40];
  snprintf(p, sizeof p, "/%s%08lx-%u.mp3", kTempPrefix, (unsigned long)millis(), (unsigned)++s_seq);
  return String(p);
}

// One note: synthesize, upload, delete. False with `why` when it did not arrive.
bool sendNote(const String& chat, const std::string& note, size_t k, size_t n, VoiceNoteSent& sent,
              String& why) {
  const String path = tempPath();
  String ttsWhy;
  const size_t clip = tts::synthesizeToFile(String(note.c_str()), path.c_str(), "mp3", nullptr, &ttsWhy);
  if (!clip) {
    LittleFS.remove(path);   // synthesizeToFile removes a cut clip; this covers the rest
    why = String("text-to-speech failed: ") + (ttsWhy.length() ? ttsWhy : String("no audio came back"));
    return false;
  }
  char title[40];
  if (n > 1) snprintf(title, sizeof title, "Voice reply (%u of %u)", (unsigned)k, (unsigned)n);
  else snprintf(title, sizeof title, "Voice reply");
  telegram::MediaOptions opt;
  opt.uploadName = "voice-reply.mp3";
  opt.title = title;
  const telegram::MediaReceipt r = telegram::sendMediaNow(chat, "audio", path.c_str(), opt);
  LittleFS.remove(path);   // a delivered clip was archived before sendMediaNow returned
  if (!r.ok) {
    why = String("Telegram did not accept it (") + r.error + ")";
    return false;
  }
  sent.messageId = r.messageId;
  sent.bytes = r.bytes;
  return true;
}

}  // namespace

Outcome deliver(const String& chatId, const String& text) {
  Outcome o;
  const nimbus::orch::VoicePlan plan = nimbus::orch::planVoiceNotes(std::string(text.c_str()));
  String refusal = plan.refusal.c_str();
  if (refusal.length() == 0 && !telegram::onTurnTask())
    refusal = "a voice reply can be sent only from inside a turn";
  else if (refusal.length() == 0 && !telegram::enabled())
    refusal = "the Telegram bot is not running";
  if (refusal.length()) {
    o.toolText = nimbus::orch::voiceFailedText({}, 1, std::string(refusal.c_str()));
    alogf("voice: not sent (%s)", refusal.c_str());
    return o;
  }
  sweepStale();
  std::vector<VoiceNoteSent> sent;
  const uint32_t started = millis();
  for (size_t i = 0; i < plan.notes.size(); i++) {
    VoiceNoteSent one;
    String why;
    const size_t chars = nimbus::orch::textChars(plan.notes[i]);
    const bool fits = nimbus::orch::voiceNoteFits(millis() - started, i, chars);
    if (!fits) why = nimbus::orch::voiceBudgetText().c_str();
    if (!fits || !sendNote(chatId, plan.notes[i], i + 1, plan.notes.size(), one, why)) {
      o.toolText = nimbus::orch::voiceFailedText(sent, plan.notes.size(), std::string(why.c_str()));
      alogf("voice: %u of %u note(s) reached Telegram; stopped: %s", (unsigned)sent.size(),
            (unsigned)plan.notes.size(), why.c_str());
      return o;
    }
    sent.push_back(one);
    o.notesSent++;
    if (o.spokenText.length()) o.spokenText += " ";
    o.spokenText += plan.notes[i].c_str();
  }
  o.delivered = true;
  o.toolText = nimbus::orch::voiceDeliveredText(sent);
  alogf("voice: %s", o.toolText.c_str());
  return o;
}

}  // namespace voicereply
}  // namespace agent
