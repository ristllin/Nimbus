#pragma once
#include <cstdint>
#include <mutex>
#include <string>

// embed_check - the live check behind the web app's embedding settings (CUM-540).
//
// Verify and Save make one real embeddings call for the candidate model: it proves
// the model answers on this instance's keys and reads the width it really returns.
// That call can take up to the embed timeout (30 s), and the daemon serves every
// request (/healthz included) on ONE HTTP thread, so the call runs on the engine
// thread and the HTTP thread waits for it only a short bound. A check that has not
// settled by then answers "still checking", and its result is kept here, so the
// next Verify or Save of the same candidate answers at once from it. Thread-safe:
// the engine thread starts and settles a check, the HTTP thread reads and queues
// them. A check queued behind other engine work is never presumed lost while it
// waits (the lost window runs from when it starts), and a check another one has
// replaced makes no call when its turn comes, so one press never pays twice.
namespace nimbusd {

// What one check is of: the provider, the model and the width asked for (0 = the
// model's own), under the key set it ran with (a key change makes a result stale).
struct EmbedCandidate {
  std::string provider, model;
  int dims = 0;
  uint32_t keyGen = 0;
  bool operator==(const EmbedCandidate& o) const {
    return provider == o.provider && model == o.model && dims == o.dims && keyGen == o.keyGen;
  }
};

struct EmbedCheck {
  enum class State { None, Checking, Ok, Failed };
  State state = State::None;
  EmbedCandidate cand;
  int width = 0;           // the width the model returned (0 = it did not answer)
  std::string error;       // why it failed
  int64_t at = 0;          // when it was queued or started (Checking) or settled, in steady seconds
  bool started = false;    // Checking: the engine thread has begun the call (else it is queued)
  uint32_t gen = 0;        // which check this is: one replaced by a newer one never runs
  bool reported = false;   // a failure is answered once; asking again runs a new check
};

class EmbedCheckSlot {
 public:
  static constexpr int64_t kOkFreshS = 600;     // a working check is reused this long
  static constexpr int64_t kLostCheckS = 90;    // past the 30 s call: a check running this long is presumed lost
  // A check still queued this long is presumed dropped; one queued behind turns is
  // not, and if it does run after all, it was replaced and makes no call.
  static constexpr int64_t kLostQueuedS = 600;

  enum class Next { Run, Checking, Settled };
  // What a Verify or Save of `c` does now, decided atomically:
  //   Settled  - `out` is a result for `c` (a fresh working one, or a failure not
  //              yet answered, which this call now answers);
  //   Checking - a check is queued or running (of `c` or of another candidate): `out`;
  //   Run      - no usable result: `c` is marked checking (queued, generation
  //              out.gen) and the caller queues it, then calls start() as it runs.
  Next begin(const EmbedCandidate& c, int64_t now, EmbedCheck& out) {
    std::lock_guard<std::mutex> lk(mu_);
    const int64_t lostAfter = s_.started ? kLostCheckS : kLostQueuedS;
    if (s_.state == EmbedCheck::State::Checking && now - s_.at < lostAfter) {
      out = s_;
      return Next::Checking;
    }
    const bool same = s_.cand == c;
    const bool fresh = s_.state == EmbedCheck::State::Ok && now - s_.at < kOkFreshS;
    const bool unanswered = s_.state == EmbedCheck::State::Failed && !s_.reported;
    if (same && (fresh || unanswered)) {
      s_.reported = true;
      out = s_;
      return Next::Settled;
    }
    s_ = EmbedCheck{};
    s_.state = EmbedCheck::State::Checking;
    s_.cand = c;
    s_.at = now;
    s_.gen = ++gen_;
    out = s_;
    return Next::Run;
  }
  // The queued check `gen` begins (engine thread): true = make the call now, and the
  // lost window runs from here; false = a newer check replaced it, so make none.
  bool start(uint32_t gen, int64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    if (s_.gen != gen || s_.state != EmbedCheck::State::Checking || s_.started) return false;
    s_.started = true;
    s_.at = now;
    return true;
  }
  // The outcome of check `gen` (dropped unless it is the one that started).
  void settle(uint32_t gen, int width, const std::string& error, int64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    if (s_.gen != gen || s_.state != EmbedCheck::State::Checking || !s_.started) return;
    s_.state = error.empty() ? EmbedCheck::State::Ok : EmbedCheck::State::Failed;
    s_.width = width;
    s_.error = error;
    s_.at = now;
  }
  // The result a waiter just answered with (a failure is not answered twice).
  void markReported() {
    std::lock_guard<std::mutex> lk(mu_);
    if (s_.state != EmbedCheck::State::Checking) s_.reported = true;
  }
  EmbedCheck get() const {
    std::lock_guard<std::mutex> lk(mu_);
    return s_;
  }

 private:
  mutable std::mutex mu_;
  EmbedCheck s_;
  uint32_t gen_ = 0;
};

}  // namespace nimbusd
