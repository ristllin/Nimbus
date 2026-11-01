#pragma once
// speak_outcome - how a spoken reply really ended.
//
// The end-of-turn `tts` device action used to be fire-and-forget: the sink returned
// nothing, so the turn recorded "spoken" and wrote the text into history whether the
// speaker played it, played silence (volume 0), failed, or the Telegram fallback was
// refused (the owner's "it said it got a confirmation and I got nothing" family, 22
// Oct). The sink now answers with this: spoken on the device speaker, delivered to
// Telegram as a voice message with Telegram's message id, or not voiced with the
// reason. The apply layer (lib/harness/src/apply.cpp) records and reports exactly that.
//
// Portable and host-tested (test_harness_apply, test_orch_claims).

#include <cstdint>
#include <string>
#include <utility>

namespace nimbus {
namespace orch {

// The words of the tts action's summary token: apply.cpp builds it from the outcome, the claim
// ledger (orch_turn_effects.cpp recordSpeakSummary) reads it back.
//   "tts "                                     played on the speaker
//   "tts(telegram message_id N) "              a Telegram voice message with its id
//   "tts(not-spoken: <why>) "                  not voiced
//   "tts(not-spoken: <lead>; <details>) "      the Telegram fallback went out unconfirmed
inline constexpr char kTtsSpoken[] = "tts ";
inline constexpr char kTtsTelegramLead[] = "tts(telegram message_id ";
inline constexpr char kTtsNotSpokenLead[] = "tts(not-spoken: ";
// Leads the reason of an unconfirmed fallback, ahead of any length cap on the summary.
inline constexpr char kTtsUnconfirmedLead[] = "the Telegram voice message may not have arrived";

struct SpeakOutcome {
  enum class Via : uint8_t {
    None = 0,   // nothing was voiced: `why` says why
    Speaker,    // the device speaker played it
    Telegram,   // a Telegram voice message arrived: `receipt` holds its message id
  };
  Via         via = Via::None;
  std::string receipt;   // Via::Telegram: Telegram's message id, digits only
  std::string said;      // the text that was voiced; "" means the text asked for, whole
  std::string why;       // Via::None: one short printable reason
  // Via::None only: the Telegram fallback went out but Telegram never confirmed it (no status
  // line, an unreadable answer), so the voice message may have arrived. The turn then says so
  // instead of "nothing was delivered".
  bool unconfirmed = false;

  // A Telegram delivery without a message id is not a delivery.
  bool ok() const { return via == Via::Speaker || (via == Via::Telegram && !receipt.empty()); }

  static SpeakOutcome spoke() {
    SpeakOutcome o;
    o.via = Via::Speaker;
    return o;
  }
  static SpeakOutcome delivered(std::string receipt, std::string said = std::string()) {
    SpeakOutcome o;
    o.via = Via::Telegram;
    o.receipt = std::move(receipt);
    o.said = std::move(said);
    return o;
  }
  static SpeakOutcome failed(std::string why, bool unconfirmed = false) {
    SpeakOutcome o;
    o.why = std::move(why);
    o.unconfirmed = unconfirmed;
    return o;
  }
};

}  // namespace orch
}  // namespace nimbus
