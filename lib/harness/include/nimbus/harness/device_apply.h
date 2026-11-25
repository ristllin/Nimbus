#pragma once
// device_apply - one device action (config, led, lights, tts, reboot, orch_model): validate,
// check who asked, strip what that caller may not set, execute through injected deps.
// Lifted byte-identical out of apply.cpp (lane E2-P-TOOLS) so engine 2's device_control links
// it without the old engine's turn machinery; apply.cpp's device[] loop and engine 1's
// device.control call the same function. Every policy decision is here, host-tested
// (test_harness_apply); the deps are execution only.
#include <cstdint>
#include <functional>
#include <string>

#include "nimbus/orch/device_actions.h"
#include "nimbus/orch/head_switch.h"     // OwnerMessage, HeadPause
#include "nimbus/orch/speak_outcome.h"   // how a spoken reply ended
#include "nimbus/orch/tool_registry.h"   // Principal

namespace agent {

// The summary token's lead of a tts that was not voiced (apply.cpp reads it back).
inline constexpr char kTtsNotSpoken[] = "tts(not-spoken: ";

struct DeviceApplyDeps {
  // Device-action execution (no policy inside):
  //   execConfig: apply the inline NVS knobs still flagged on the (already
  //   policy-stripped) action + live sfx refresh. Returns whether a stored
  //   value CHANGED (before != after): false is a no-op action (23 Oct: a
  //   model invented a config field, nothing switched, the reply said it had)
  //   and the device action then reports "no change", so the claim ledger backs
  //   no "switched" reply. stageDevice: hand a validated action to the main
  //   loop via the DeviceSink (led/lights/reboot + the posture/profile/theme/
  //   attnHold config knobs).
  std::function<bool(const nimbus::orch::ValidatedAction&)> execConfig;
  std::function<void(const nimbus::orch::ValidatedAction&)> stageDevice;  // null => (noop) paths
  // Voice `text` (whole characters, at most kTtsActionMaxBytes bytes) for the turn in
  // `chatId` and say how it ended: played on the device speaker, delivered as a Telegram
  // voice message (with its message id) to that chat when it is a Telegram chat, else to the
  // owner chat (telegram::ownerChat: the first allowlisted owner), or not voiced (with why). The
  // turn records and reports exactly that, never the request. null => tts(no-sink).
  std::function<nimbus::orch::SpeakOutcome(const std::string& chatId, const std::string& text)> speak;
  // The speaker volume now (0..100), read when a tts action runs: at 0 the speaker plays
  // silence, so the action is refused as not spoken. null => unknown, never refuses.
  std::function<int()> speakerVolume;
  bool ttsEnabled = false;

  // orch_model validation + write (validation data lives device-side).
  std::function<bool(const std::string& prov, const std::string& model)> modelIsValid;
  std::function<bool(const std::string& prov)> providerHasKey;
  // True when both the head provider and its model were stored (E2-REP1: a refused write is told).
  std::function<bool(const std::string& prov, const std::string& model)> setOrchHostModel;
  // An orch_model the provider's cached list does not hold: the device asks for
  // one list refresh (at most one per provider per hour, never a connection here)
  // and returns the head's answer, "not in <provider> list cached <date>; refresh
  // queued" (models_tool.h modelMissText). Null => "not in <provider> list".
  std::function<std::string(const std::string& prov, const std::string& model)> modelMiss;
  // A pause never moves the head (nimbus/orch/head_switch.h). headHost: the head the
  // owner chose (before any failover). headPause: what holds paid requests to `prov` off
  // now (nothing set = open). ownerMessage: the owner's message this turn and
  // its number (empty for a turn the owner did not write: a member, a guest, a routine).
  // nowMs: the device clock. Null headPause => no guard.
  std::function<std::string()> headHost;
  std::function<nimbus::orch::HeadPause(const std::string& prov)> headPause;
  std::function<nimbus::orch::OwnerMessage()> ownerMessage;
  std::function<uint32_t()> nowMs;

  // Record what was voiced in the chat's history (null: not recorded).
  std::function<void(const std::string& chatId, const std::string& text)> captureAssistant;
};

// What one device element did.
struct DeviceOutcome {
  std::string summary;      // the token(s): "cfg ", "reboot-refused(scheduled) ", ...
  bool refusedWhole = false;   // the caller rule refused the element as a whole
  std::string refusals;     // every part the caller rule refused, in plain words ("" = none)
  std::string replyNote;    // a note that only rides a message the owner already gets (speech cut short)
};

// Validate + check who asked + policy-strip + execute ONE device[] element (raw
// JSON). The shared engine behind applyTurn's device[] loop AND the mid-turn
// `device.control` registry tool - one security envelope for both. `who` is the
// caller's Principal (the turn's chat, or the tool dispatch's), `unattended` the
// turn's flag (routine, wakeup, dream, results synthesis, system turn); the rule
// table (nimbus/orch/action_rules.h) decides each action and config knob. Appends
// any owner-visible note to *riskNote when non-null.
DeviceOutcome applyDeviceAction(const std::string& json, const nimbus::orch::Principal& who,
                                bool unattended, const std::string& chatId, const DeviceApplyDeps& d,
                                std::string* riskNote);

}  // namespace agent
