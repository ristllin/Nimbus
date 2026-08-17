// Ported and adapted from Nuage-Solide src/agent/device_actions.{h,cpp}
// (Head Orchestrator v2). VALIDATION HALF ONLY - see device_actions.h.
//
// The dispatch order (led > lights > tts > reboot > config > unknown) mirrors the
// device's executeDeviceActions() if/else chain verbatim, so a validated action
// and an executed action always classify a given element the same way.

#include "nimbus/orch/device_actions.h"

#include <cstring>

namespace nimbus {
namespace orch {

// Protected config keys - see the rationale block in device_actions.h. Order is
// (secrets first) password/token/connector, then the auth gate + routing.
const char* const kProtectedConfigKeys[] = {
    "password", "token", "connector",     // secrets - never model-settable
    "allowlist",                          // the device's own auth gate
    "orchHost", "fabric",                 // provider / sub-agent routing
    "loops",                              // Local Loops blob - only loop.* tools write it
    // Human-only surface, blocked BY NAME so a smuggle attempt refuses the whole
    // action loudly instead of being silently ignored (2026-07-16, alongside the
    // owner-knob expansion): the orchestrator-HOST priority list under every alias
    // the web surface knows, the Telegram credential, the owner directive, and the
    // loop/budget governors. `priority` (no prefix) stays allowed - that is the
    // SUB-AGENT preference knob, deliberately model-tunable.
    "providerPriority", "provPrio", "orchPriority",
    "tgToken", "tgAllow", "sysPrompt",
    "orchLoop", "budget", "tlsSlots",
    // Provider API keys BY NAME (2026-07-17, caught by a smuggle test: "antKey"
    // rode alongside a benign knob and was silently IGNORED instead of loudly
    // refused - no write-through existed, but the contract is refuse-as-a-unit).
    "oaiKey", "antKey", "mistralKey", "custKey", "tavilyKey",
    nullptr,
};

bool isProtectedConfigKey(const char* key) {
  if (!key) return false;
  for (int i = 0; kProtectedConfigKeys[i]; ++i)
    if (!std::strcmp(key, kProtectedConfigKeys[i])) return true;
  // Generic backstop: ANY key ending in "Key" is credential-shaped - future
  // provider keys get refused without someone remembering to extend the list.
  const size_t n = std::strlen(key);
  if (n >= 3) {   // case-insensitive: "Key"/"key"/"KEY" are all credential-shaped
    const char* s = key + n - 3;
    if ((s[0]=='K'||s[0]=='k') && (s[1]=='E'||s[1]=='e') && (s[2]=='Y'||s[2]=='y'))
      return true;
  }
  return false;
}

namespace {

// Clamp helper (no <algorithm> needed; keeps the include surface tiny).
uint8_t clamp255(long v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return uint8_t(v);
}

LedMode modeFromStr(const char* s) {
  if (!s) return LedMode::Solid;
  if (!std::strcmp(s, "spinner")) return LedMode::Spinner;
  if (!std::strcmp(s, "pulse"))   return LedMode::Pulse;
  if (!std::strcmp(s, "flash"))   return LedMode::Flash;
  if (!std::strcmp(s, "rainbow")) return LedMode::Rainbow;
  return LedMode::Solid;
}

// Every real led mode is a LIT pattern - there is no "off" pattern. But after the
// model paints the ring with `led`, the natural lever it reaches for to undo it is
// ANOTHER led with mode "off"/"none"/"clear"/"dark" (field bug, owner 2026-07-18:
// "said off but they weren't; asked again and it did it" - the retry finally used
// `lights:off`). The OLD validator coerced that unknown mode to a LIT Solid and
// returned allowed=true, so the ring stayed on while the model reported success.
// Treat these as an explicit OFF: the caller rewrites the action to Lights-off,
// which is the ONE path that both darkens the ring AND clears the led override.
bool isLedOffWord(const char* s) {
  if (!s || !s[0]) return false;
  return !std::strcmp(s, "off") || !std::strcmp(s, "none") ||
         !std::strcmp(s, "clear") || !std::strcmp(s, "dark") ||
         !std::strcmp(s, "black");
}

// Talk-to-configure slug maps. Return -1 for an unknown slug: the knob is then
// IGNORED (hasX stays false) rather than misapplied - a model typo must never
// flip the ring level to a value it didn't mean.
int postureFromStr(const char* s) {
  if (!s) return -1;
  if (!std::strcmp(s, "dark")) return 0;
  if (!std::strcmp(s, "calm")) return 1;
  if (!std::strcmp(s, "full")) return 2;
  return -1;
}
int profileFromStr(const char* s) {
  if (!s) return -1;
  if (!std::strcmp(s, "battery_saver")) return 0;
  if (!std::strcmp(s, "balanced"))      return 1;
  if (!std::strcmp(s, "desk"))          return 2;
  return -1;
}
int32_t clampAttnHold(long v) {   // Param::AttnHoldMs meta range: 30 s .. 30 min
  if (v < 30000)   return 30000;
  if (v > 1800000) return 1800000;
  return int32_t(v);
}

// Shared config-knob reader for the legacy nested object and the flat
// discriminated shape - ONE implementation so the two wire forms can't drift.
void readConfigKnobs(ArduinoJson::JsonVariantConst c, ValidatedAction& a) {
  if (!c["ledBrightness"].isNull()) {
    a.hasLedBrightness = true;
    a.ledBrightness = clamp255((long)(c["ledBrightness"] | 128));
  }
  if (!c["priority"].isNull()) {
    a.hasPriority = true;
    a.priority = (const char*)(c["priority"] | "");
  }
  if (!c["posture"].isNull()) {
    int p = postureFromStr(c["posture"] | (const char*)nullptr);
    if (p >= 0) { a.hasPosture = true; a.posture = uint8_t(p); }
  }
  if (!c["profile"].isNull()) {
    int p = profileFromStr(c["profile"] | (const char*)nullptr);
    if (p >= 0) { a.hasProfile = true; a.profile = uint8_t(p); }
  }
  if (!c["theme"].isNull()) {
    const char* t = c["theme"] | "";
    if (t[0]) { a.hasTheme = true; a.theme = t; }   // device validates the slug
  }
  if (!c["attnHoldMs"].isNull()) {
    a.hasAttnHoldMs = true;
    a.attnHoldMs = clampAttnHold((long)(c["attnHoldMs"] | 300000));
  }
  if (!c["ttsVoice"].isNull()) {
    a.hasTtsVoice = true;                       // "" is valid: provider default
    a.ttsVoice = (const char*)(c["ttsVoice"] | "");
  }
  if (!c["ttsProv"].isNull()) {
    const char* p = c["ttsProv"] | "";
    if (!std::strcmp(p, "mistral") || !std::strcmp(p, "openai")) {
      a.hasTtsProv = true;
      a.ttsProv = p;
    }                                           // unknown provider: knob ignored
  }
  // Owner-serviceable knobs round 2 (2026-07-16): each clamps/whitelists exactly
  // like its applyOrchField web setter, so the model can never store an
  // out-of-range value the human surface would have refused.
  if (c["ttsOn"].is<bool>()) {
    a.hasTtsOn = true;
    a.ttsOn = c["ttsOn"].as<bool>();
  }
  // Risk-carrying protection overrides: plain bools, but the schema description
  // obliges the model to weigh + state the risk (panel heat / deep discharge).
  if (c["sleepOvr"].is<bool>()) {
    a.hasSleepOvr = true;
    a.sleepOvr = c["sleepOvr"].as<bool>();
  }
  if (c["brightOvr"].is<bool>()) {
    a.hasBrightOvr = true;
    a.brightOvr = c["brightOvr"].as<bool>();
  }
  if (!c["sttProv"].isNull()) {
    const char* p = c["sttProv"] | "";
    if (!std::strcmp(p, "mistral") || !std::strcmp(p, "openai")) {
      a.hasSttProv = true;
      a.sttProv = p;
    }                                           // unknown provider: knob ignored
  }
  if (!c["sfxLvlN"].isNull()) {
    long v = (long)(c["sfxLvlN"] | 0);
    a.hasSfxLvlN = true;
    a.sfxLvlN = uint8_t(v < 0 ? 0 : (v > 3 ? 3 : v));
  }
  if (!c["sfxLvlO"].isNull()) {
    long v = (long)(c["sfxLvlO"] | 0);
    a.hasSfxLvlO = true;
    a.sfxLvlO = uint8_t(v < 0 ? 0 : (v > 3 ? 3 : v));
  }
  if (!c["sfxVol"].isNull()) {
    long v = (long)(c["sfxVol"] | 50);
    a.hasSfxVol = true;
    a.sfxVol = uint8_t(v < 0 ? 0 : (v > 100 ? 100 : v));
  }
  if (!c["sfxTheme"].isNull()) {
    const char* t = c["sfxTheme"] | "";
    if (!std::strcmp(t, "pulse")) {
      a.hasSfxTheme = true;
      a.sfxTheme = t;
    }                                           // unknown theme: knob ignored
  }
  if (!c["devName"].isNull()) {
    const char* n = c["devName"] | "";
    std::string clean;
    for (const char* p = n; *p && clean.size() < 24; ++p)
      if (*p >= 0x20 && *p < 0x7F) clean.push_back(*p);   // printable ASCII, <=24
    a.hasDevName = true;                        // "" is valid: clears the name
    a.devName = clean;
  }
}

}  // namespace

ValidatedAction validateAction(ArduinoJson::JsonVariantConst el) {
  ValidatedAction a;

  // ---- flattened discriminated union (the schema shape) ----------------------
  // {"type":"led"|"lights"|"tts"|"reboot"|"config", ...fields at top level}.
  // This is what a strict structured-output provider emits (ORCH_SCHEMA_BODY);
  // the legacy keyed shape below is kept for migration/tolerant parses.
  if (el["type"].is<const char*>()) {
    const char* t = el["type"] | "";
    if (!std::strcmp(t, "led")) {
      // "led off" (or none/clear/dark/black) means turn the ring OFF - route it to
      // the Lights-off path (which clears the led override AND darkens); a lit Solid
      // was the old silent-failure.
      if (isLedOffWord(el["mode"] | "")) {
        a.kind = ActionKind::Lights; a.lightsOff = true; a.allowed = true; return a;
      }
      a.kind = ActionKind::Led;
      a.mode = modeFromStr(el["mode"] | "solid");
      a.r = clamp255((long)(el["r"] | 0));
      a.g = clamp255((long)(el["g"] | 120));
      a.b = clamp255((long)(el["b"] | 255));
      if (!el["brightness"].isNull()) {   // null = model left it unset
        a.hasBrightness = true;
        a.brightness = clamp255((long)(el["brightness"] | 128));
      }
      a.allowed = true;
      return a;
    }
    if (!std::strcmp(t, "lights")) {
      a.kind = ActionKind::Lights;
      a.lightsOff = (std::strcmp(el["value"] | "full", "off") == 0);
      a.allowed = true;
      return a;
    }
    if (!std::strcmp(t, "tts")) {
      a.kind = ActionKind::Tts;
      a.text = (const char*)(el["text"] | "");
      a.allowed = true;
      return a;
    }
    if (!std::strcmp(t, "reboot")) {
      a.kind = ActionKind::Reboot;
      a.allowed = true;
      return a;
    }
    if (!std::strcmp(t, "config")) {
      a.kind = ActionKind::Config;
      // SECURITY: same whole-action refusal as the legacy shape - any protected
      // key smuggled alongside a benign knob rejects the action as a unit.
      // (Strict providers can't emit extras, but the validator must not rely on
      // the wire being strict - defense in depth.)
      for (ArduinoJson::JsonPairConst kv : el.as<ArduinoJson::JsonObjectConst>()) {
        if (isProtectedConfigKey(kv.key().c_str())) {
          a.allowed = false;
          a.reason = "protected-BLOCKED";
          return a;
        }
      }
      readConfigKnobs(el, a);
      a.allowed = true;
      return a;
    }
    if (!std::strcmp(t, "orch_model")) {
      // The agent switching its OWN host+model is a DELIBERATE exception to the
      // protected-key rule (host/model only; keys + priority stay owner-only). The
      // device re-validates model-in-choices before applying (choices live device-
      // side); here we just require a well-formed provider+model.
      a.kind = ActionKind::OrchModel;
      a.hasOrchModel = true;
      a.orchProvider = (const char*)(el["provider"] | "");
      a.orchModel = (const char*)(el["model"] | "");
      a.allowed = !a.orchProvider.empty() && !a.orchModel.empty();
      if (!a.allowed) a.reason = "orch_model needs provider+model";
      return a;
    }
    // Unknown discriminator: fall through to the legacy branches (an object
    // could carry both a stray "type" and a legacy action key), then Unknown.
  }

  // ---- led ------------------------------------------------------------------
  if (!el["led"].isNull()) {
    ArduinoJson::JsonVariantConst l = el["led"];
    if (isLedOffWord(l["mode"] | "")) {   // "led off" == lights off (see isLedOffWord)
      a.kind = ActionKind::Lights; a.lightsOff = true; a.allowed = true; return a;
    }
    a.kind = ActionKind::Led;
    a.mode = modeFromStr(l["mode"] | "solid");
    a.r = clamp255((long)(l["r"] | 0));
    a.g = clamp255((long)(l["g"] | 120));
    a.b = clamp255((long)(l["b"] | 255));
    if (!l["brightness"].isNull()) {
      a.hasBrightness = true;
      a.brightness = clamp255((long)(l["brightness"] | 128));
    }
    a.allowed = true;
    return a;
  }

  // ---- lights ---------------------------------------------------------------
  if (!el["lights"].isNull()) {
    a.kind = ActionKind::Lights;
    const char* m = el["lights"] | "full";
    a.lightsOff = (std::strcmp(m, "off") == 0);
    a.allowed = true;
    return a;
  }

  // ---- tts ------------------------------------------------------------------
  if (!el["tts"].isNull()) {
    a.kind = ActionKind::Tts;
    a.text = (const char*)(el["tts"] | "");
    a.allowed = true;
    return a;
  }

  // ---- reboot ---------------------------------------------------------------
  if (el["reboot"] | false) {
    a.kind = ActionKind::Reboot;
    a.allowed = true;
    return a;
  }

  // ---- config ---------------------------------------------------------------
  if (!el["config"].isNull()) {
    ArduinoJson::JsonVariantConst c = el["config"];
    a.kind = ActionKind::Config;

    // SECURITY: if ANY protected key is present, refuse the WHOLE action. We
    // check this BEFORE reading any benign knob so a config that smuggles a
    // secret alongside a legit ledBrightness is still rejected as a unit. The
    // reason reports only the policy tag - never the offending key's value.
    for (ArduinoJson::JsonPairConst kv : c.as<ArduinoJson::JsonObjectConst>()) {
      if (isProtectedConfigKey(kv.key().c_str())) {
        a.allowed = false;
        a.reason = "protected-BLOCKED";
        return a;
      }
    }

    // Benign, owner-friendly knobs the model MAY tune (shared reader - same
    // set as the flat shape, so the two wire forms can't drift).
    readConfigKnobs(c, a);
    a.allowed = true;
    return a;
  }

  // ---- unknown --------------------------------------------------------------
  // volume/beep etc. have no device API yet; unknown actions are OPEN in v2, not
  // errors - the device logs "skip" and moves on.
  a.kind = ActionKind::Unknown;
  a.allowed = false;
  a.reason = "unsupported";
  return a;
}

ActionResult validateAction(ValidatedAction& a) {
  switch (a.kind) {
    case ActionKind::Led:
      a.r = clamp255(a.r);
      a.g = clamp255(a.g);
      a.b = clamp255(a.b);
      if (a.hasBrightness) a.brightness = clamp255(a.brightness);
      a.allowed = true;
      a.reason.clear();
      break;

    case ActionKind::Lights:
    case ActionKind::Tts:
    case ActionKind::Reboot:
      a.allowed = true;
      a.reason.clear();
      break;

    case ActionKind::Config:
      // A pre-built config action can only carry the allow-listed knobs (the
      // struct has no field for a protected key), but guard the priority string
      // in case a caller stuffed a protected NAME into it by mistake, and clamp
      // every ranged knob.
      if (a.hasLedBrightness) a.ledBrightness = clamp255(a.ledBrightness);
      if (a.hasPosture && a.posture > 2)  a.posture = 2;
      if (a.hasProfile && a.profile > 2)  a.profile = 2;
      if (a.hasAttnHoldMs) a.attnHoldMs = clampAttnHold(a.attnHoldMs);
      if (a.hasPriority && isProtectedConfigKey(a.priority.c_str())) {
        a.allowed = false;
        a.reason = "protected-BLOCKED";
      } else {
        a.allowed = true;
        a.reason.clear();
      }
      break;

    case ActionKind::Unknown:
    default:
      a.allowed = false;
      if (a.reason.empty()) a.reason = "unsupported";
      break;
  }
  return ActionResult{a.allowed, a.reason};
}

}  // namespace orch
}  // namespace nimbus
