#pragma once
#include <Arduino.h>

#include <string>

// voice_reply - a Telegram voice reply that either reaches the chat, with Telegram's
// receipt, or says why it did not.
//
// On 22 Oct a reply.telegram voice:true call played on the device speaker instead
// (Telegram was only the speaker's fallback), the speaker failed silently, and the
// tool said "sent to Telegram": nothing arrived. A Telegram voice request now always
// goes to Telegram and never to the speaker: the text is cut into voice notes
// (nimbus/orch/voice_notes.h), each note is synthesized to its own flash file,
// uploaded at once on this task (telegram::sendMediaNow) and deleted, and the result
// names Telegram's message id for every note that arrived.
namespace agent {
namespace voicereply {

struct Outcome {
  bool        delivered = false;   // every note reached Telegram
  size_t      notesSent = 0;       // how many did (they stay delivered when a later one fails)
  String      spokenText;          // the text of the notes that arrived, for the history row
  std::string toolText;            // reply.telegram's result: the receipt, or why not
  std::string why;                 // not delivered: the short reason, no instructions to the model
  std::string receipt;             // delivered: Telegram's message id of the first note (digits)
};

// Voice `text` into `chatId` (the caller has checked the chat against the allowlist).
// Runs on the poll task inside a turn; stops at the first note that does not arrive.
Outcome deliver(const String& chatId, const String& text);

#if defined(NIMBUS_TEST)
// Test console only (VOICETEXT): the next voice delivery voices this text instead of the
// model's, once - so a bench leg can force a long reply the model cannot shorten and prove
// the split into several receipted voice messages. An empty text clears it.
void setTestTextOverride(const String& text);
#endif

}  // namespace voicereply
}  // namespace agent
