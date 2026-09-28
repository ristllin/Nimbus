#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/sfx_map.h"
#include "nimbus/stt_result.h"   // SttResult + the transport error vocabulary
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
static_assert(int(Outcome::NoReply) + 1 == kOutcomeCount,
              "kOutcomeCount must track the Outcome enum (the property tests iterate it)");

// The ring cue a phase/outcome shows. Listening and Processing use the theme
// accent; Alert and Offline use the theme's alert color; None hands the ring back
// to the normal status composition.
enum class Cue : uint8_t { None = 0, Listening, Processing, Alert, Offline };

// How a cue drives an LED ring (a self-animating driver pattern: the loop is
// blocked through recording and speech-to-text, so only the driver can keep an LED
// ring moving) - in the theme accent, or the theme alert color. The on-screen ring
// of a ringless board draws cueFrame() instead: the same colors, but its listening
// frame is STEADY (it is painted once, then the loop blocks while recording).
enum class LedMotion : uint8_t { Off = 0, Solid, Pulse, Spinner };
struct LedCue {
  LedMotion motion = LedMotion::Off;
  bool alert = false;   // alert color (else the theme accent)
};
LedCue ledCueFor(Cue c);

// Status-line color tone.
enum class Tone : uint8_t { Accent = 0, Alert = 1, Calm = 2 };

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

// Why a hold could not start at all (shown before recording, on the reply screen).
enum class Block : uint8_t { Updating, NoKey };
// NoKey is also the refusal line the speech-to-text adapter reports for a keyless
// device, so the two paths cannot disagree.
std::string blockedLine(Block b);

Line lineFor(Outcome o, const SttResult& r);
Line lineFor(Outcome o);                  // outcomes that carry no transport detail
std::string sentence(const Line& l);      // "Title. Detail" for logs and serial
Cue cueFor(Outcome o);
// The sound for an outcome (false = silent, e.g. EmptyTranscript: the capture
// sound already played and nothing went wrong).
bool sfxFor(Outcome o, sfx::Ev& out);
// "Mistral" / "OpenAI" / "Cumulo" for a provider slug ("Speech-to-text" if unknown).
std::string providerName(const std::string& slug);
// Stable lowercase names for serial / log seams ("transcribing", "no_network").
const char* phaseName(Phase p);
const char* outcomeName(Outcome o);

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

// Timing. A Notice holds this long (or until a touch). Thinking ends when the
// device reports the voice message finished; these are the backstops: no turn
// running for kQuietMs (the message was lost), or kMaxThinkMs overall - the
// engine's longest loop deadline (3600 s, owner-tunable) plus its stuck-turn
// reaper margin (120 s), so a healthy long turn is never called "No reply".
constexpr uint32_t kNoticeHoldMs = 15000;
constexpr uint32_t kQuietMs = 60000;
constexpr uint32_t kMaxThinkMs = (3600 + 120) * 1000;

class Flow {
 public:
  // Each transition returns true when it changed the phase. A call that does not
  // apply to the current phase is ignored (returns false) - so a repeated or late
  // event can never make the state oscillate.
  bool press(uint32_t now);                                  // Idle|Notice -> Recording
  bool release(uint32_t now);                                // Recording -> Transcribing
  bool transcribed(const std::string& heard, uint32_t now);  // Transcribing -> Thinking
  bool fail(Outcome o, const Line& line, uint32_t now);      // Transcribing|Thinking -> Notice
  // A reply for the voice chat was shown. It never ends processing by itself - a
  // mid-turn fallback notice or a stray sub-agent result is not the answer - it
  // only lets a backstop end quietly (the answer is on screen) instead of saying
  // "No reply".
  void replyLanded(uint32_t now);
  bool turnEnded(bool ok, uint32_t now);                     // Thinking -> Idle | Notice
  bool tick(bool turnInFlight, uint32_t now);                // backstops + Notice expiry
  bool dismiss(uint32_t now);                                // Notice -> Idle

  // What the device's loop saw this pass while a turn may be running (the order of
  // the reads matters on the device: turnInFlight BEFORE the end counter, because
  // the engine bumps the counter before it clears in-flight).
  struct TurnSignals {
    bool turnInFlight = false;   // a turn is running now
    bool turnEnded = false;      // the device finished OUR voice message
    bool turnOk = false;         // ... and its turn answered
    bool replyLanded = false;    // a reply for the voice channel was shown this pass
  };
  // One loop pass: note a shown reply, end on our message's own end, then the
  // backstops. Returns true when the phase changed.
  bool step(const TurnSignals& s, uint32_t now);

  Phase phase() const { return phase_; }
  Outcome outcome() const { return outcome_; }
  Cue cue() const;
  bool ownsRing() const { return cue() != Cue::None; }
  bool canPress() const { return phase_ == Phase::Idle || phase_ == Phase::Notice; }
  bool busy() const { return phase_ == Phase::Transcribing || phase_ == Phase::Thinking; }
  bool replyShown() const { return replyShown_; }
  // Color tone for the status line: accent while listening / processing, alert
  // for a failed outcome, calm for an outcome that is not a failure.
  Tone tone() const;
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
// fail fast with no link, then transcribe, then send the turn. Returns the outcome:
// None when the turn was sent (the flow is Thinking) - or when the flow was not
// recording, so there was nothing to run.
Outcome afterRelease(Flow& f, Port& p);

}  // namespace nimbus::voice
