#pragma once
#include <string>

// stt_result - what a speech-to-text call came back with, and the transport's
// failure vocabulary it is classified from (CUM-456). Kept apart from the voice
// flow so the transport adapter (src/agent/adapters/audio_stt) and the multipart
// uploader need only this, not the UI state machine.
//
// The uploader (agent::httpmp::post) PRODUCES the error strings below and
// sttKindForError() CLASSIFIES them; both use these constants, so a reworded
// transport error cannot silently fall through to the generic "no answer" line.
namespace nimbus::voice {

constexpr const char* kErrConnectFailed = "connect failed";   // DNS / TCP / TLS connect
constexpr const char* kErrSlotBusy = "tls arbiter busy";      // the single TLS slot is taken
constexpr const char* kErrFileOpen = "file open failed";      // the capture file is missing
constexpr const char* kErrHttpPrefix = "HTTP ";               // "HTTP <status>" (0 = no answer)

struct SttResult {
  enum class Kind : unsigned char { Ok = 0, NoNetwork, Http, Refused, BadReply, Busy, NoAudio };
  Kind kind = Kind::Ok;
  int http = 0;             // Http: the status code (0 = no answer before the deadline)
  std::string provider;     // effective provider slug: "mistral" | "openai" | "cumulo"
  std::string refusal;      // Refused: the honest one-line refusal status
  std::string text;         // Ok: the transcript (may be empty)
};

// Map the uploader's error string to a result kind. `httpOut` receives the status
// for "HTTP <n>" (0 otherwise). Anything unrecognised reads as Http with status 0
// ("no answer") - never as an empty transcript.
SttResult::Kind sttKindForError(const std::string& err, int* httpOut);

}  // namespace nimbus::voice
