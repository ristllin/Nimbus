#pragma once
// Ported and adapted from Nuage-Solide src/agent/device_actions.{h,cpp}
// (Head Orchestrator v2).
//
// PORTABLE HALF ONLY: validation, not execution. The device today does both in
// agent::executeDeviceActions(); here the portable core answers exactly one
// question - "is this device[] action allowed, and with what clamped params?"
// No LED / audio / reboot / NVS calls live in lib/core. The device executor
// (src/agent/device_actions.cpp) consumes the validated ValidatedAction vector
// and only acts on items with allowed==true.
//
// Vocabulary (orchestrator-v2-spec §5): each device[] element is exactly one of
//   led | lights | tts | reboot | config
// with the per-action params below. Anything else is Unknown (open in v2, not an
// error). Session ops (cancel/spawn/status) are NOT device actions - they go
// through the turn loop + journal (spawn/await fields), never device[].
//
// SECURITY CORE - secrets & protected config are BLOCKED unconditionally. An
// LLM-issued chat action must never set a credential or rewrite the device's own
// auth / routing. See the deny rationale on kProtectedConfigKeys below.
//
// Arduino-free + host-tested under [env:native]. ArduinoJson v7 is the one
// dependency (parses a single device[] element); the struct-based validateAction
// overload has no dependency at all.

#include <cstdint>
#include <string>

#include <ArduinoJson.h>  // JsonVariantConst - the one lib/core dependency (v7)

namespace nimbus {
namespace orch {

// ---- action kinds -----------------------------------------------------------

enum class ActionKind : uint8_t {
  Unknown = 0,  // unsupported / no recognized key - allowed==false, "unsupported"
  Led,          // ring pattern + colour + brightness
  Lights,       // ring on/off shorthand
  Tts,          // speak text
  Reboot,       // restart the device
  Config,       // benign, owner-friendly knobs (allow-listed keys only)
  OrchModel,    // switch the orchestrator's OWN provider host + model (self-reroute)
};

// LED pattern mirrors the device leds::Pattern set; parsed from the "mode" field.
enum class LedMode : uint8_t { Solid = 0, Spinner, Pulse, Flash, Rainbow };

// A single validated device action: the kind, its clamped params, and the
// allow/deny verdict. The device executor switches on `kind` and acts only when
// `allowed` is true. Params are pre-clamped to their device ranges so the
// executor never has to re-validate.
//
// NOTE ON NAMING: the turn contract (nimbus/orch/turn.h) has its own
// `DeviceAction` - the RAW, unvalidated JSON slice of one device[] element. This
// is the VALIDATED form that slice turns into, hence `ValidatedAction`. Keeping
// the two names distinct lets a TU include both the turn parser and this
// validator without an ODR clash.
struct ValidatedAction {
  ActionKind kind = ActionKind::Unknown;
  bool       allowed = false;
  std::string reason;  // deny/skip reason; "" when allowed. NEVER a secret value.

  // led / lights
  LedMode mode = LedMode::Solid;
  uint8_t r = 0, g = 120, b = 255;   // defaults match the device (r0 g120 b255)
  uint8_t brightness = 128;
  bool    hasBrightness = false;     // led: only touch brightness if the model set it
  bool    lightsOff = false;         // lights: "off" vs "full"

  // tts
  std::string text;

  // config (validated, allow-listed)
  bool    hasLedBrightness = false;
  uint8_t ledBrightness = 128;
  bool    hasPriority = false;
  std::string priority;
  // Talk-to-configure knobs (owner-friendly; the device applies them through the
  // same Config/Param pipeline as the menu/web UI). posture/profile arrive as
  // slugs and are mapped to their enums here; unknown slugs leave hasX false
  // (knob ignored, never an error). attnHoldMs clamps to the Param's range.
  bool    hasPosture = false;
  uint8_t posture = 0;        // nimbus::Posture as int (0 dark / 1 calm / 2 full)
  bool    hasProfile = false;
  uint8_t profile = 0;        // nimbus::ProfileId as int (0 saver / 1 balanced / 2 desk)
  bool    hasTheme = false;
  std::string theme;          // LED theme slug; device validates against themeList()
  bool    hasAttnHoldMs = false;
  int32_t attnHoldMs = 300000;  // clamped 30 s .. 30 min (Param::AttnHoldMs range)
  // Voice: the model may retune its OWN voice (provider + voice id/slug) - an
  // owner-facing preference, not a credential. Voice slugs are provider-specific
  // free-form ids (validated on use by the TTS adapter, like the web UI's field).
  bool    hasTtsVoice = false;
  std::string ttsVoice;       // "" clears back to the provider default
  bool    hasTtsProv = false;
  std::string ttsProv;        // "mistral" | "openai" only; unknown = ignored
  // Owner-serviceable knobs round 2 (2026-07-16 - "the agent should be able to
  // enable most configs"): voice-replies master switch, STT provider, SFX ladder,
  // device name. All owner-facing preferences; secrets/routing stay blocked.
  bool    hasTtsOn = false;
  bool    ttsOn = false;      // voice-replies master switch (owner-request-only by prompt)
  // Battery/LED protection overrides (owner feature 2026-07-17). BOTH carry real
  // physical risk - the schema docs REQUIRE the model to state the risk to the
  // owner when setting them (brightOvr: >60% LED heat can damage the panel/shell;
  // sleepOvr: skipping the low-batt deep sleep risks deep-discharging the pack).
  bool    hasSleepOvr = false;
  bool    sleepOvr = false;
  bool    hasBrightOvr = false;
  bool    brightOvr = false;
  bool    hasSttProv = false;
  std::string sttProv;        // "mistral" | "openai" only; unknown = ignored
  bool    hasSfxLvlN = false;
  uint8_t sfxLvlN = 0;        // clamped 0-3
  bool    hasSfxLvlO = false;
  uint8_t sfxLvlO = 0;        // clamped 0-3
  bool    hasSfxVol = false;
  uint8_t sfxVol = 50;        // clamped 0-100
  bool    hasSfxTheme = false;
  std::string sfxTheme;       // terran|protoss|zerg only; unknown = ignored
  bool    hasDevName = false;
  std::string devName;        // <=24 printable ASCII; "" clears; reboot-to-apply

  // orch_model: the agent may switch its OWN provider host + model at runtime
  // (owner: "running mistral, ask it to switch to opus, agent tool-calls to change
  // itself"). A DELIBERATE exception to the protected-key rule for host/model ONLY
  // - API keys, tokens, provider PRIORITY and fabric routing stay owner-only. The
  // device validates `model` against the provider's choice list before applying.
  bool    hasOrchModel = false;
  std::string orchProvider;   // "openai" | "anthropic" | "mistral"
  std::string orchModel;      // a model id from that provider's choices
};

// The pure allow/deny verdict, decoupled from the parsed params, for callers /
// tests that only care whether an action ran. `reason` is a short device-log
// tag, never the offending value.
struct ActionResult {
  bool        allowed = false;
  std::string reason;
};

// ---- deny policy (the security core) ----------------------------------------

// config keys the model MAY tune. Everything else in a config action is either
// ignored (unknown) or BLOCKED (protected, below).
//   ledBrightness - 0..255 brightness knob
//   priority      - provider-priority string (routing *preference*, not a host)
//
// PROTECTED config keys - rejected outright, and if ANY is present the WHOLE
// config action is refused (reason "protected-BLOCKED"). Rationale (ported):
//   password / token / connector  - SECRETS. An LLM-issued chat action must
//       never set a credential. Passwords are blocked entirely; secrets require
//       explicit owner confirmation via web UI / Telegram, never a model turn.
//   allowlist  - the device's own auth gate; letting the model rewrite it would
//       let it grant itself access.
//   orchHost / fabric - provider routing; letting the model rewrite them would
//       let it redirect its own brain / sub-agent backends.
// The reason string reports only the policy tag ("protected-BLOCKED"), never the
// blocked key's value, so a secret can never leak into the log / next-turn ctx.
extern const char* const kProtectedConfigKeys[];  // nullptr-terminated
bool isProtectedConfigKey(const char* key);

// ---- validators -------------------------------------------------------------

// Validate one device[] element (an ArduinoJson object). Recognizes exactly one
// action key; unknown keys yield ActionKind::Unknown, allowed==false. A config
// action carrying any protected key is refused whole. Params are clamped to
// device ranges (0..255 colour/brightness, mode enum, lights off/full).
ValidatedAction validateAction(ArduinoJson::JsonVariantConst el);

// Struct-in / verdict-out overload for callers that already have a typed action
// (or for host tests that build one directly). Fills `.allowed`/`.reason` on the
// passed action in place and returns the same verdict as an ActionResult.
ActionResult validateAction(ValidatedAction& a);

}  // namespace orch
}  // namespace nimbus
