#pragma once
#include <cstdint>
#include <mutex>
#include <string>

#include "nimbus/orch/provider_park.h"   // ParkKind, PauseCause - why a provider is closed

// head_switch - orch_model never moves the owner's head only because it is paused.
//
// 26 Oct 2026, Lumi on v4.6.0: Anthropic, the owner's head, was paused by its monthly
// limit and every turn already ran on Mistral, automatically and only until the limit
// lifts. The owner wrote "Hmm I thought anthropic is out of budget"; the assistant
// answered by switching the head to Mistral with orch_model, which overwrote the
// owner's choice for good: when the limit lifts, Lumi stays on Mistral.
//
// The rule: while the head provider is paused, parked or over its budget, an
// orch_model to ANOTHER provider is refused with an explanation the reply carries
// ("Anthropic is paused by your monthly limit; replies already fail over automatically
// and return to Anthropic when it lifts."). Told that, the owner can still ask for the
// switch in a LATER message that names the new provider or model, within kConfirmMs
// (or pick it under Assistant > Models): a message that names the provider only as a
// reaction to the pause - "switch to mistral since anthropic is out of budget" - is
// that first ask, and is refused. A switch inside the head's own provider, any switch
// while the head is open, and the owner's web pick are unaffected. A turn no person
// started (a routine, a wakeup) never confirms one.
//
// Portable (no Arduino), host-tested (test_orch_head_switch). Thread-safe.
namespace nimbus {
namespace orch {

// Why paid requests to a provider are held off, as the sentence a refused switch opens
// with, "" when nothing holds it off: "Anthropic is paused by your monthly limit; replies
// already fail over automatically and return to Anthropic when it lifts." Its ending
// follows the cause: "when it lifts" (a monthly or overall spending limit), "when you
// resume it" (paused by the owner), "once credit is added", "once its key is replaced",
// "when its budget resets" (over its monthly budget). `label` is the display name;
// `pauseCauses` the pause switch's bits (PauseCause); `park` its refusal park on the chat
// seam; `overBudget` the device's monthly budget check.
std::string headPauseNote(const std::string& label, uint8_t pauseCauses, ParkKind park, bool overBudget);

// What holds a provider off, as the device reads it (the pause switch, its refusal park on
// the chat seam, its monthly budget).
struct HeadPause {
  uint8_t  causes = 0;               // PauseCause bits
  ParkKind park = ParkKind::None;
  bool     overBudget = false;
};
inline std::string headPauseNote(const std::string& label, const HeadPause& p) {
  return headPauseNote(label, p.causes, p.park, p.overBudget);
}
// The same fact for the model (the per-turn line): "Anthropic, the chosen head, is paused by the
// owner's monthly limit and returns automatically when it lifts." "" when nothing holds it off.
std::string headPauseFact(const std::string& label, const HeadPause& p);
// The same fact for any provider (the catalog's provider row, CUM-821): "OpenAI is paused by the
// owner's monthly limit and returns automatically when it lifts." "" when nothing holds it off.
std::string providerPauseFact(const std::string& label, const HeadPause& p);

// The explanation for a refused switch, owner-facing and plain: the note, then
// 'To make <target> your main provider anyway, say "switch to <target>" again, or pick it
// under Assistant > Models.'
std::string headSwitchRefusal(const std::string& note, const std::string& targetLabel);

// Does the owner's message name `provider` (its slug, display name or a model family
// that only it serves: "claude", "gpt", "glm", ...) or `model` (its id, with or
// without "-latest")? Case-insensitive.
bool ownerNamesTarget(const std::string& ownerText, const std::string& provider, const std::string& model);
// Does the owner's message ASK to switch to the target: it names it (ownerNamesTarget), carries
// a switch word ("switch", "change", "move", "use", "make", "set", "pick", "choose", "go with")
// and no word that keeps or declines ("keep", "stay", "no", "not", "don't", "do not", "never")?
// Only such a message arms a refusal for confirmation, and only such a later one confirms it:
// a refusal the model drew on its own ("Hmm I thought anthropic is out of budget") leaves
// nothing to confirm, and a mention ("so mistral is answering?") confirms nothing.
bool ownerAsksSwitchTo(const std::string& ownerText, const std::string& provider, const std::string& model);

// Are a turn's words the owner's own? The owner's web, serial and voice channels, or a
// chat whose role is admin; a member, a guest or a turn no person started never is.
bool ownerChannel(const std::string& chatId, bool admin);

// The owner's message of the turn in flight: its words and its number (each owner
// message has its own; "" / 0 for a turn the owner did not write).
struct OwnerMessage {
  std::string text;
  uint32_t    number = 0;
  bool present() const { return number != 0 && !text.empty(); }
};

class HeadSwitchGuard {
 public:
  static constexpr uint32_t kConfirmMs = 30u * 60u * 1000u;

  // May orch_model move the head from `head` to `target` (`model`) now? `headClosed`
  // is headPauseNote for the head ("" = open); `owner` the owner's message this turn. True for
  // an open head or a switch within its own provider; else true only as the owner's
  // confirmation: a refusal for the same target drawn by an EARLIER owner message that asked
  // for it (another number, so the same words sent again count), under kConfirmMs ago, and
  // this message asks for it too (ownerAsksSwitchTo). A refusal is recorded for confirmation
  // only when the owner's message asked for that switch.
  bool allow(const std::string& head, const std::string& headClosed, const std::string& target,
             const std::string& model, const OwnerMessage& owner, uint32_t nowMs);
  void reset();

 private:
  std::mutex  mx_;
  bool        armed_ = false;   // a refusal the owner may confirm
  std::string target_;
  uint32_t    atMs_ = 0;
  uint32_t    msg_ = 0;         // the owner message that drew the refusal
};

// The process-wide guard (end-of-turn device[] and the mid-turn device.control share it).
HeadSwitchGuard& headSwitchGuard();

}  // namespace orch
}  // namespace nimbus
