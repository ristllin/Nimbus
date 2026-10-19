#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// failure_chain - why a turn failed, kept from the moment each provider says so
// until every surface that reports the turn (the turn-end row, the error timeline
// row, the dream row, the routine record, device.status, the owner notice).
//
// Before this, only the LAST error of a turn survived: a turn that died on an
// Anthropic 400 ("invalid unicode code point ...") and then on the two hosts it
// failed over to was recorded as the last host's bare "chat HTTP 400", and the head
// asked to explain the failure had nothing true to read (18 Oct dream).
//
// One chain per turn, RAM only, no NVS. Bounded: at most kMaxSteps entries (the
// fabric speaks three hosts), each the FIRST error text seen on that run of
// attempts, at most kTextMax bytes, control characters removed and always valid
// UTF-8 (a provider body or a byte-cut log line can carry torn characters, and
// these texts are written back into rows the next prompt reads).
//
// A "run" is consecutive failed attempts on one host (a same-host retry adds an
// attempt to the run, it does not replace its text). When the chain is full a new
// run replaces the last entry, so first() is always the first failure and last()
// the host that failed last. Device-side exits (a budget refusal, low memory, a
// reply that could not be parsed) note a synthetic step on host "device", so every
// failed turn has at least one cause. Portable (NO Arduino), host-tested.

namespace nimbus {
namespace orch {

struct FailStep {
  std::string host;       // provider id, or "device"
  std::string text;       // the first error of this run: sanitized, <= kTextMax bytes
  uint8_t     attempts = 0;   // failed attempts in this run (saturates at 255)
};

class FailureChain {
 public:
  static constexpr size_t kMaxSteps = 3;
  static constexpr size_t kTextMax = 160;
  // The smallest excerpt render() keeps of the first cause before it drops later
  // entries (the first cause is the one that explains the turn).
  static constexpr size_t kMinHeadText = 40;

  void clear();
  // One failed attempt on `host` with its error text.
  void note(const std::string& host, const std::string& err);
  // A device-side exit: a step on host "device" ("budget", "low memory", "parse").
  void noteDevice(const char* cause) { note(kDeviceHost, cause ? cause : ""); }

  size_t size() const { return n_; }
  bool   empty() const { return n_ == 0; }
  const FailStep* first() const { return n_ ? &s_[0] : nullptr; }
  const FailStep* last() const { return n_ ? &s_[n_ - 1] : nullptr; }
  const FailStep& at(size_t i) const { return s_[i < n_ ? i : 0]; }
  // Whether any entry's text contains `needle` (case-sensitive).
  bool anyTextHas(const char* needle) const;

  // The chain as one line of at most `cap` bytes, valid UTF-8:
  //   "anthropic (x2): messages HTTP 400: The request body ... > openai 400 (x2) > mistral 400 (x2)"
  // The first entry keeps its text; later ones keep their host and a short cause.
  // When it does not fit, the first text shrinks to kMinHeadText, then later
  // entries are dropped from the end (" > +N more"), then the first text shrinks
  // further. Names the first host whenever `cap` holds it. "" when empty.
  std::string render(size_t cap) const;
  // The failing hosts in order: "anthropic -> openai -> mistral".
  std::string path() const;
  // One entry, short: "anthropic 400", "device parse".
  std::string brief(size_t i) const;

  static constexpr const char* kDeviceHost = "device";

 private:
  FailStep s_[kMaxSteps];
  uint8_t  n_ = 0;
};

// Text made safe to store and to resend: invalid UTF-8 sequences become '?',
// control characters become spaces, runs of spaces collapse, the result is trimmed
// and cut on a character boundary at `maxBytes`.
std::string sanitizeFailText(const char* s, size_t len, size_t maxBytes);

// The short cause in an error line: the HTTP status ("400"), else a known class
// ("timeout", "low memory", "network", "budget", "parse"), else the text up to its
// first ':' (at most 24 bytes). "" for an empty text.
std::string shortFailCause(const std::string& text);

}  // namespace orch
}  // namespace nimbus
