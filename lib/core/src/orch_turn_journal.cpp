#include "nimbus/orch/turn_journal.h"

#include <ArduinoJson.h>

#include "nimbus/net/tg_dup_guard.h"   // kPoisonMaxTries: crash resets before the poison guard skips a request
#include "nimbus/sys/log_window.h"

namespace nimbus::orch {

namespace {

bool startsWith(const std::string& s, const char* p) {
  size_t i = 0;
  for (; p[i]; ++i)
    if (i >= s.size() || s[i] != p[i]) return false;
  return true;
}

std::string capped(const std::string& s, size_t cap) { return s.size() > cap ? s.substr(0, cap) : s; }

// The noun the notice uses for an owner request, or "" for a background kind.
const char* requestNoun(const std::string& kind) {
  if (kind == "photo") return "photo";
  if (kind == "file") return "file";
  if (kind == "voice") return "voice message";
  if (kind == "message") return "message";
  if (kind == "console") return "console request";
  return "";
}

}  // namespace

std::string turnKindFor(int source, const std::string& chat, const std::string& textHead) {
  if (source == kTurnSourceLoop) return "routine";
  if (source == kTurnSourceSynthesis) return "background";
  if (source == kTurnSourceSerial) return "console";
  if (startsWith(textHead, "[image]")) return "photo";
  if (startsWith(textHead, "[file]")) return "file";
  if (chat == "voice") return "voice";
  return "message";
}

static const char* const kKinds[] = {"photo", "file", "voice", "message", "routine", "background", "console"};

bool journalWriteAllowed(uint32_t intFree, uint32_t intLargest) {
  return intFree >= nimbus::sys::kForensicMinFree && intLargest >= nimbus::sys::kForensicMinLargest;
}

std::string turnMark(const std::string& kind, int round) {
  std::string m = capped(kind, 12);
  if (round > 0) m += "/r" + std::to_string(round);
  return m;
}

std::string kindFromTurnMark(const std::string& mark) {
  const std::string kind = mark.substr(0, mark.find('/'));
  for (const char* k : kKinds)
    if (kind == k) return kind;
  return {};
}

std::string renderTurnJournal(const TurnJournal& j) {
  JsonDocument d;
  d["v"] = 1;
  d["chat"] = capped(j.chat, 32);
  d["kind"] = capped(j.kind, 12);
  d["at"] = j.at;
  d["up"] = j.upMs;
  d["round"] = j.round;
  std::string out;
  serializeJson(d, out);
  return out;
}

bool parseTurnJournal(const std::string& json, TurnJournal& out) {
  if (json.empty() || json.size() > 512) return false;
  JsonDocument d;
  if (deserializeJson(d, json)) return false;
  if ((d["v"] | 0) != 1) return false;
  const char* kind = d["kind"] | "";
  if (!kind[0]) return false;
  TurnJournal j;
  j.chat = capped(d["chat"] | "", 32);
  j.kind = capped(kind, 12);
  j.at = d["at"] | (uint64_t)0;
  j.upMs = d["up"] | (uint32_t)0;
  j.round = d["round"] | 0;
  out = j;
  return true;
}

std::string crashNotice(const std::string& deviceName, const TurnJournal* j,
                        const std::string& hhmm, const std::string& cause) {
  const std::string name = deviceName.empty() ? std::string("Nimbus") : deviceName;
  std::string msg = name + " restarted after a crash";
  if (!cause.empty()) msg += " (" + cause + ")";
  if (!j) return msg + ". Nothing was in progress, so nothing was lost.";
  const std::string from = hhmm.empty() ? std::string() : " from " + hhmm;
  const char* noun = requestNoun(j->kind);
  if (noun[0]) {
    // v4.6.3 (the 29 Oct boba mechanism): a crashed MESSAGE turn was already
    // received and may have partly done its work (a row may be logged) before
    // the crash took the reply. Telling the owner to send it again is what
    // logged the same meal twice; the receipt says what may already be done.
    if (j->kind == "message")
      return msg + " while answering your " + noun + from +
             ". The reply was lost; the message arrived and its work may be done - check "
             "before re-sending.";
    return msg + " while answering your " + noun + from +
           ". That reply was lost, so please send it again if you still need it.";
  }
  if (j->kind == "routine")
    return msg + " while running a routine" + from + ". That run did not finish.";
  return msg + " during a background task" + from + ". That task did not finish.";
}

bool isTelegramChatId(const std::string& chat) {
  const size_t from = (!chat.empty() && chat[0] == '-') ? 1 : 0;
  if (chat.size() <= from) return false;
  for (size_t i = from; i < chat.size(); ++i)
    if (chat[i] < '0' || chat[i] > '9') return false;
  return true;
}

bool lostTurnRunsAgain(const TurnJournal& j, const StallReplay& r) {
  return isTelegramChatId(j.chat) && r.telegramPolling && r.poisonTries < nimbus::net::kPoisonMaxTries;
}

std::string stallNotice(const std::string& deviceName, const TurnJournal* j, const std::string& hhmm,
                        const StallReplay& r) {
  const std::string name = deviceName.empty() ? std::string("Nimbus") : deviceName;
  const std::string stopped = name + " stopped answering";
  if (!j) {
    // Telegram keeps what was sent meanwhile on its server and serves it now; the web page
    // and on-device voice queue in RAM, which the restart emptied.
    if (r.telegramPolling && r.poisonTries < nimbus::net::kPoisonMaxTries)
      return stopped + " for a few minutes, so it restarted itself. Messages sent here meanwhile are being "
                       "answered now; anything typed in the web page or said to it in that time was not kept, "
                       "so please send that again.";
    return stopped + " for a few minutes, so it restarted itself. If a reply you were waiting for does not come "
                     "in a few minutes, please send that message again.";
  }
  const std::string from = hhmm.empty() ? std::string() : " from " + hhmm;
  const char* noun = requestNoun(j->kind);
  if (noun[0]) {
    const std::string msg = stopped + " while working on your " + noun + from + ", so it restarted itself.";
    // A message that was logging something may have saved its row before it stuck (the
    // 29 Oct boba mechanism): a retry could save it again, a re-send too.
    if (lostTurnRunsAgain(*j, r)) {
      if (j->kind == "message")
        return msg + " It tries that message again on its own; if it was logging something, check it was not "
                     "saved twice.";
      return msg + " It tries that " + noun + " again on its own; if no reply comes in a few minutes, send it again.";
    }
    if (j->kind == "message")
      return msg + " That reply was lost and the message is not tried again; if it was logging something, check "
                   "whether it was saved before you send it again.";
    return msg + " That reply was lost, so please send it again if you still need it.";
  }
  if (j->kind == "routine")
    return stopped + " during a routine" + from + ", so it restarted itself. That run did not finish.";
  return stopped + " during a background task" + from + ", so it restarted itself. That task did not finish.";
}

}  // namespace nimbus::orch
