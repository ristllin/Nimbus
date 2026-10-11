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
// the engine thread settles a check, the HTTP thread reads and starts them.
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
  int64_t at = 0;          // when it started (Checking) or settled, in steady seconds
  bool reported = false;   // a failure is answered once; asking again runs a new check
};

class EmbedCheckSlot {
 public:
  static constexpr int64_t kOkFreshS = 600;     // a working check is reused this long
  static constexpr int64_t kLostCheckS = 90;    // past the 30 s call: a check this old is presumed lost

  enum class Next { Run, Checking, Settled };
  // What a Verify or Save of `c` does now, decided atomically:
  //   Settled  - `out` is a result for `c` (a fresh working one, or a failure not
  //              yet answered, which this call now answers);
  //   Checking - a check is still running (of `c` or of another candidate): `out`;
  //   Run      - no usable result: `c` is marked checking and the caller runs it.
  Next begin(const EmbedCandidate& c, int64_t now, EmbedCheck& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (s_.state == EmbedCheck::State::Checking && now - s_.at < kLostCheckS) {
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
    out = s_;
    return Next::Run;
  }
  // The outcome of the check of `c` (dropped if the slot moved on to another).
  void settle(const EmbedCandidate& c, int width, const std::string& error, int64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!(s_.cand == c) || s_.state != EmbedCheck::State::Checking) return;
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
};

}  // namespace nimbusd
