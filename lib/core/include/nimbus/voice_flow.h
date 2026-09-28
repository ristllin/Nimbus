#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/sfx_map.h"
#include "solide/ring.h"

// voice_flow - the portable hold-to-talk state machine and its error taxonomy
// (CUM-456). The device seam (src/main.cpp) owns the mic, the speech-to-text call
// and the panel; everything that decides WHAT the owner sees, and in which order,
// lives here so it is host-tested with no hardware.
//
// The contract this module exists to pin:
//   1. Releasing the mic ends recording at once. The processing state is shown
//      BEFORE any network I/O (afterRelease() renders, then checks the link, then
//      transcribes) - a network timeout can never hold the listening frame.
//   2. Processing stays visible from release until the reply lands (Transcribing,
//      then Thinking while the turn runs).
//   3. Errors are honest and specific: no link / unreachable says "No network",
//      a provider HTTP error names the provider and status, and "Didn't catch
//      that" is reserved for an EMPTY transcript returned by a reachable provider.
//
// Single-task by design (AGENTS.md "No on-device concurrency"): nothing here spawns
// work or reorders the network calls; it only orders the STATE updates around them.
namespace nimbus::voice {

// Where a hold-to-talk interaction is. Idle -> Recording (mic held) -> Transcribing
// (released; speech-to-text runs) -> Thinking (turn sent; waiting for the reply) ->
// Idle, or -> Notice (an outcome line held on the home screen) -> Idle.
enum class Phase : uint8_t { Idle = 0, Recording, Transcribing, Thinking, Notice };

// Why an interaction ended early. APPEND-ONLY (host tests iterate the range).
enum class Outcome : uint8_t {
  None = 0,         // still running, or ended with a reply
  NoNetwork,        // Wi-Fi not joined, or the speech-to-text host could not be reached
  SttHttp,          // the speech-to-text provider answered an HTTP error (or not at all)
  SttRefused,       // provider or router refusal with a known reason (credit, rate limit)
  SttBadReply,      // the provider answered, but the reply could not be read
  Busy,             // the device is busy with another request (TLS slot, turn queue)
  NoAudio,          // the mic recorded nothing
  EmptyTranscript,  // reachable provider, readable reply, nothing was said
  TurnError,        // the turn ended without an answer (its own reply names the cause)
  NoReply,          // nothing came back within the wait window
};
constexpr int kOutcomeCount = 10;

// The ring cue a phase/outcome shows. Listening and Processing use the theme
// accent; Alert and Offline use the theme's alert color; None hands the ring back
// to the normal status composition.
enum class Cue : uint8_t { None = 0, Listening, Processing, Alert, Offline };

// What the device's speech-to-text call came back with (built by the device seam
// from the transport result; host tests build it directly).
struct SttResult {
  enum class Kind : uint8_t { Ok = 0, NoNetwork, Http, Refused, BadReply, Busy, NoAudio };
  Kind kind = Kind::Ok;
  int http = 0;             // Http: the status code (0 = no answer before the deadline)
  std::string provider;     // effective provider slug: "mistral" | "openai" | "cumulo"
  std::string refusal;      // Refused: the honest one-line refusal status
  std::string text;         // Ok: the transcript (may be empty)
};

// Map the multipart transport's error string (agent::httpmp::post `err`) to a
// result kind. `httpOut` receives the status for "HTTP <n>" (0 otherwise).
// "connect failed" -> NoNetwork, "tls arbiter busy" -> Busy, "file open failed"
// -> NoAudio, "HTTP <n>" -> Http; anything else -> Http with status 0.
SttResult::Kind sttKindForError(const std::string& err, int* httpOut);

// The outcome a speech-to-text result leads to: None for a usable transcript.
Outcome classify(const SttResult& r);

// A status line: a short title (fits the ring center: <= kTitleMaxChars) and a
// detail sentence (wraps to <= kDetailMaxLines lines of kTitleMaxChars). Printable
// ASCII, sentence case, no em dash and no " - " (AGENTS.md section 6).
struct Line {
  std::string title;
  std::string detail;
};
constexpr size_t kTitleMaxChars = 17;   // ring-center width on the 2.8" panel
constexpr size_t kDetailMaxLines = 4;

Line lineFor(Outcome o, const SttResult& r);
Line lineFor(Outcome o);                  // outcomes that carry no transport detail
std::string sentence(const Line& l);      // "Title. Detail" for logs and serial
Cue cueFor(Outcome o);
// The sound for an outcome (false = silent, e.g. EmptyTranscript: the capture
// sound already played and nothing went wrong).
bool sfxFor(Outcome o, sfx::Ev& out);
// "Mistral" / "OpenAI" / "Cumulo" for a provider slug ("Speech-to-text" if unknown).
std::string providerName(const std::string& slug);

// Greedy word wrap into at most `maxLines` lines of `maxChars`; a word longer than
// a line is hard-split, and overflow ends the last line with "...".
std::vector<std::string> wrap(const std::string& s, size_t maxChars, size_t maxLines);

// The on-screen ring frame for a cue (boards with no LED ring draw the ring on the
// panel). `elapsedMs` is the time since the cue began. Listening and Alert are
// steady; Offline breathes; Processing is a comet that sweeps once per kSpinMs.
constexpr uint32_t kSpinMs = 1200;
constexpr uint32_t kOfflineBreatheMs = 2000;
void cueFrame(Cue c, uint32_t elapsedMs, solide::ring::RGB accent, solide::ring::RGB alert,
              solide::ring::RGB* out, int n);

// Timing. A Notice holds this long (or until a touch). Thinking ends on the
// turn's own end; these are the backstops: no turn running for kQuietMs (the turn
// never started, or ended without a record), or kMaxThinkMs overall.
constexpr uint32_t kNoticeHoldMs = 15000;
constexpr uint32_t kQuietMs = 60000;
constexpr uint32_t kMaxThinkMs = 180000;

class Flow {
 public:
  // Each transition returns true when it changed the phase. A call that does not
  // apply to the current phase is ignored (returns false) - so a repeated or late
  // event can never make the state oscillate.
  bool press(uint32_t now);                                  // Idle|Notice -> Recording
  bool release(uint32_t now);                                // Recording -> Transcribing
  bool transcribed(const std::string& heard, uint32_t now);  // Transcribing -> Thinking
  bool fail(Outcome o, const Line& line, uint32_t now);      // Transcribing|Thinking -> Notice
  // A reply for the voice chat landed. While the turn is still running the flow
  // keeps processing (a fallback notice is not the answer); otherwise it ends.
  bool replyLanded(bool turnInFlight, uint32_t now);
  bool turnEnded(bool ok, uint32_t now);                     // Thinking -> Idle | Notice
  bool tick(bool turnInFlight, uint32_t now);                // backstops + Notice expiry
  bool dismiss(uint32_t now);                                // Notice -> Idle

  Phase phase() const { return phase_; }
  Outcome outcome() const { return outcome_; }
  Cue cue() const;
  bool ownsRing() const { return cue() != Cue::None; }
  bool canPress() const { return phase_ == Phase::Idle || phase_ == Phase::Notice; }
  bool busy() const { return phase_ == Phase::Transcribing || phase_ == Phase::Thinking; }
  bool replyShown() const { return replyShown_; }
  uint32_t since() const { return since_; }
  uint32_t transitions() const { return transitions_; }
  // The status line for the current phase ("Listening", "Transcribing", "Thinking"
  // + what was heard, or the Notice outcome). Empty title in Idle.
  const Line& line() const { return line_; }

 private:
  bool go(Phase p, uint32_t now);

  Phase phase_ = Phase::Idle;
  Outcome outcome_ = Outcome::None;
  Line line_;
  uint32_t since_ = 0;
  uint32_t lastBusy_ = 0;
  uint32_t transitions_ = 0;
  bool replyShown_ = false;
};

// The device side of the release path. Every method but transcribe() must be free
// of network I/O - that is exactly what the seam test counts.
class Port {
 public:
  virtual ~Port() = default;
  virtual uint32_t now() = 0;
  virtual void show(const Flow& f) = 0;         // paint ring + screen for the state
  virtual bool linkUp() = 0;                    // cached Wi-Fi association state
  virtual SttResult transcribe() = 0;           // THE network call (speech-to-text)
  virtual bool sendTurn(const std::string& transcript) = 0;   // enqueue the turn
  virtual void sound(sfx::Ev e) = 0;
};

// Everything after the mic is released: leave Recording and SHOW processing, then
// fail fast with no link, then transcribe, then send the turn. Returns the outcome
// (None = the turn was sent and the flow is Thinking).
Outcome afterRelease(Flow& f, Port& p);

}  // namespace nimbus::voice
