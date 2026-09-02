#pragma once
#include <cstring>

// Danger-zone confirm phrases + the identity-preservation policy - the ONE source
// of truth the web UI, the endpoints, and the tests all agree on (CUM-15). Pure +
// host-tested; the device glue (NVS erase, SD erase/format) lives in src/.
//
// Each destructive action has its OWN typed confirm phrase so one confirm can never
// trigger a heavier action than the owner meant. Factory Reset SCRAPS EVERYTHING the
// owner set - keys, token, bonds, config, AND the user-visible device name - so a
// reset unit comes back as a clean, re-onboardable device with a fresh auto-generated
// name + mDNS (CUM-230). Only the board's PHYSICAL identity (config/identity_keys.h:
// panel model, mount flip, touch cal, OTA slug) survives, so it still boots on the
// right driver instead of a white screen it cannot correct from its own controls.

namespace nimbus {
namespace orch {

inline constexpr const char* kConfirmFactoryReset = "FACTORY RESET";
inline constexpr const char* kConfirmEraseStorage = "ERASE STORAGE";
inline constexpr const char* kConfirmFormatCard   = "FORMAT CARD";

// Exact, case-sensitive, non-empty match. A missing or wrong phrase is rejected.
inline bool confirmOk(const char* expected, const char* got) {
  return expected && got && got[0] && std::strcmp(expected, got) == 0;
}

// ---- The destructive-action matrix (CUM-15 v2) -------------------------------
// Every danger-zone action, its machine slug, its OWN typed confirm phrase, and
// WHEN its control may be offered - in ONE table, so the guard class is tested as a
// whole rather than one instance at a time. The `Count` sentinel makes a new action
// that lands without a matrix row fail the build (the static_assert below), instead
// of silently shipping an ungated or dishonest wipe. This mirrors the CUM-290
// route/lifecycle-matrix precedent: every cell has a defined honest outcome, and a
// new cell must be filled in before it can ship.
enum class DangerAction { FactoryReset = 0, EraseStorage, FormatCard, Count };

// When a control may appear in the owner UI. A control the owner can see but that
// can only ever fail is the CUM-290 lying-knob class; NeedsCard hides such a control
// when there is nothing for it to act on.
enum class OfferRule {
  Always,     // always shown; if it cannot act it REFUSES LOUDLY (never a silent no-op)
  NeedsCard,  // shown only when a card is physically present, else it could only fail
};

struct DangerActionSpec {
  DangerAction action;
  const char*  slug;     // machine slug (frozen; matches the /api route + HIL)
  const char*  confirm;  // the exact typed confirm phrase (frozen wire)
  OfferRule    offer;    // when the control may be offered
};

// Factory Reset and Erase Storage are always offered: with no card they refuse
// loudly ("storage not available"), which is an honest outcome. Full-card Format is
// offered only when a card is physically present - a corrupt or too-small card is
// exactly what Format is for, but with NO card the control could only fail.
inline constexpr DangerActionSpec kDangerActions[] = {
  { DangerAction::FactoryReset, "factory-reset", kConfirmFactoryReset, OfferRule::Always    },
  { DangerAction::EraseStorage, "sdreset",       kConfirmEraseStorage, OfferRule::Always    },
  { DangerAction::FormatCard,   "sdformat",      kConfirmFormatCard,   OfferRule::NeedsCard },
};
static_assert(sizeof(kDangerActions) / sizeof(kDangerActions[0]) ==
                  static_cast<int>(DangerAction::Count),
              "every DangerAction needs exactly one matrix row - add the new action's "
              "slug, confirm phrase, and offer rule so it cannot ship ungated");

// The confirm phrase a given action requires ("" if the action is out of range).
inline const char* confirmPhraseFor(DangerAction a) {
  for (const auto& s : kDangerActions)
    if (s.action == a) return s.confirm;
  return "";
}

// The offer rule for a given action (Always if the action is out of range).
inline OfferRule offerRuleFor(DangerAction a) {
  for (const auto& s : kDangerActions)
    if (s.action == a) return s.offer;
  return OfferRule::Always;
}

// Whether a control under `rule` should be offered given the live storage truth.
inline bool shouldOffer(OfferRule rule, bool cardPresent) {
  return rule == OfferRule::Always || cardPresent;
}

// Whether to advertise the full-card Format control (webui state.files.canFormat).
// Requires BOTH the firmware to have the format primitive wired (hookWired - a build
// without it must never show the control) AND a card to be physically present, so
// the Format knob is shown exactly when it can actually act. `cardPresent` is a
// nonzero raw card capacity, which the driver still reports for a card whose
// filesystem is corrupt or below the usable minimum - the cases Format exists for.
inline bool offerFormatCard(bool hookWired, bool cardPresent) {
  return hookWired && shouldOffer(offerRuleFor(DangerAction::FormatCard), cardPresent);
}

// The NVS keys a factory reset PRESERVES beyond the board's physical identity.
// Intentionally EMPTY (CUM-230 ruling): nothing user-facing survives - not even the
// device name, which is user state tied to the OLD identity and would otherwise keep
// a reset unit advertising its pre-reset mDNS name (CUM-233). The mechanism stays so
// re-adding a keep-key is a one-line change with a matching test. The lone sentinel
// keeps the array a valid (non-zero-length) declaration; kFactoryKeepKeyCount is
// authoritative and iterators must stop at it.
inline constexpr const char* kFactoryKeepKeys[] = {nullptr};
inline constexpr int kFactoryKeepKeyCount = 0;

// After the whole-partition wipe, factory reset SEEDS the operating mode so the device
// boots into the Wi-Fi setup AP and the onboarding wizard is reachable. Leaving the
// mode key merely cleared defaults to Notifier - Wi-Fi off, no AP, no wizard, no way to
// onboard (the CUM-230 field bug). Mirrors sys::Mode::Orchestrator (1); the reset path
// static_asserts the two agree.
inline constexpr int kFactoryResetSeedMode = 1;

}  // namespace orch
}  // namespace nimbus
