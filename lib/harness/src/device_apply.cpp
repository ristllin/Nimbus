#include "nimbus/harness/device_apply.h"

#include <ArduinoJson.h>

#include "nimbus/orch/action_rules.h"    // who may run each action (CUM-813)
#include "nimbus/orch/models_tool.h"     // the "not in <provider> list" answers
#include "nimbus/orch/provider_slots.h"  // the orch_model provider enum
#include "nimbus/orch/reply_markup.h"    // cleanReplyMarkup
#include "nimbus/orch/voice_notes.h"     // cutForSpeech: the tts action's bound

// Lifted from apply.cpp (lane E2-P-TOOLS), itself lifted from src/agent/orchestrator.cpp
// applyTurn (Stage E): every summary token, refusal string and risk note BYTE-IDENTICAL
// (test_harness_apply pins the security-relevant ones).

namespace agent {

using nimbus::orch::ActionKind;
using nimbus::orch::ValidatedAction;

// A switch away from a head that is paused, parked or over its budget, refused because
// the failover is already automatic and temporary (head_switch.h): the plain
// explanation, or "" when the switch may go ahead (an open head, the same provider, or
// the owner's confirmation in a later message).
static std::string headPauseRefusal(const ValidatedAction& va, const DeviceApplyDeps& d) {
  if (!d.headPause || !d.headHost) return std::string();
  const std::string head = d.headHost();
  if (head.empty() || head == va.orchProvider) return std::string();
  const nimbus::orch::ProviderSlot* h = nimbus::orch::findProviderSlot(head.c_str());
  const std::string headLabel = h ? h->label : head;
  const std::string closed = nimbus::orch::headPauseNote(headLabel, d.headPause(head));
  const nimbus::orch::OwnerMessage owner = d.ownerMessage ? d.ownerMessage() : nimbus::orch::OwnerMessage();
  const uint32_t now = d.nowMs ? d.nowMs() : 0;
  if (nimbus::orch::headSwitchGuard().allow(head, closed, va.orchProvider, va.orchModel, owner, now))
    return std::string();
  const nimbus::orch::ProviderSlot* t = nimbus::orch::findProviderSlot(va.orchProvider.c_str());
  // A turn the owner did not write learns no more than that the head stays.
  if (!owner.present()) return headLabel + " stays the main provider: only the owner can move it.";
  return nimbus::orch::headSwitchRefusal(closed, t ? t->label : va.orchProvider);
}

// A reason as it rides a summary token and the log: printable ASCII, one line, bounded.
static std::string oneLine(const std::string& s) {
  constexpr size_t kMax = 120;
  std::string out;
  for (char c : s) {
    if (out.size() >= kMax) break;
    const unsigned char u = (unsigned char)c;
    out += (u >= 0x20 && u < 0x7F) ? c : ' ';
  }
  const size_t e = out.find_last_not_of(' ');
  return e == std::string::npos ? std::string() : out.substr(0, e + 1);
}

static const char kVolumeZeroWhy[] = "the speaker volume is 0, so speech would play as silence";

static std::string ttsNotSpoken(const std::string& why) {
  return kTtsNotSpoken + oneLine(why.empty() ? std::string("no reason was given") : why) + ") ";
}

// Why the sink could not voice it, in the words of the summary. A Telegram fallback that went
// out unconfirmed opens with the fixed lead the ledger reads (it may have arrived).
static std::string notSpokenWhy(const nimbus::orch::SpeakOutcome& o) {
  if (o.via == nimbus::orch::SpeakOutcome::Via::Telegram) return "Telegram gave no message id";
  if (o.unconfirmed) return std::string(nimbus::orch::kTtsUnconfirmedLead) + "; " + o.why;
  return o.why;
}

// The summary token of a tts that cannot run at all, or "" when it can: voice replies
// off (the owner's switch: a silent no-op), no sink, or a speaker that would play silence.
static std::string ttsRefusal(const ValidatedAction& va, const DeviceApplyDeps& d) {
  if (!d.ttsEnabled) return "tts(voice-off) ";
  if (!d.speak || va.text.empty()) return "tts(no-sink) ";
  if (d.speakerVolume && d.speakerVolume() == 0) return ttsNotSpoken(kVolumeZeroWhy);
  return std::string();
}

// The tts device action: voice the text and report what REALLY happened (summary token
// byte-identical for a spoken reply, "tts "). Never "spoken" on the strength of the
// request: the sink answers with the speaker, a Telegram receipt, or why not. A text
// over the bound is cut on a character boundary and the reply says so (shortNote: it rides
// a reply, never a message of its own).
static std::string applyTts(const ValidatedAction& va, const std::string& chatId,
                            const DeviceApplyDeps& d, std::string* shortNote) {
  using nimbus::orch::SpeakOutcome;
  const std::string refused = ttsRefusal(va, d);
  if (!refused.empty()) return refused;
  // The speaker is a reply seam too: markup is cleaned, every sentence is voiced (the reply
  // guard's sentence drops went in v5.2.0, lane RB-Q1c).
  const std::string spoken = nimbus::orch::cleanReplyMarkup(va.text);
  const nimbus::orch::SpeechCut cut = nimbus::orch::cutForSpeech(spoken);
  if (cut.text.empty()) return ttsNotSpoken("there was nothing to say");
  const SpeakOutcome o = d.speak(chatId, cut.text);
  if (!o.ok()) return ttsNotSpoken(notSpokenWhy(o));
  const bool viaTelegram = o.via == SpeakOutcome::Via::Telegram;
  // Capture what was voiced to history so "what did you just say?" works even when the
  // turn had no text reply - and only what was voiced.
  if (d.captureAssistant) d.captureAssistant(chatId, o.said.empty() ? cut.text : o.said);
  if (cut.shortened && shortNote) *shortNote += "\n\n" + nimbus::orch::speechShortenedNote(cut);
  return viaTelegram ? "tts(telegram message_id " + o.receipt + ") " : std::string("tts ");
}

// Why an orch_model the device refused names no model it can switch to, in the
// words the head reads (the mid-turn device.control answer and the next turn's
// [MEMORY RESULTS]). L19-39: a router id on a direct provider is refused as such,
// with the bare id when that provider lists it; any other miss asks for a list
// refresh through the device (modelMiss).
static std::string orchModelMiss(const ValidatedAction& va, const DeviceApplyDeps& d) {
  const std::string& p = va.orchProvider;
  const std::string& m = va.orchModel;
  if (!nimbus::orch::isProviderSlug(p.c_str()))
    return "'" + p + "' is not a provider (openai, anthropic, mistral, zai or cumulo)";
  if (d.providerHasKey && !d.providerHasKey(p))
    return p + " has no API key on this device, so it has no model list; only the owner adds keys";
  if (nimbus::orch::routerIdOnDirectProvider(p, m)) {
    const std::string bare = m.substr(m.find('/') + 1);
    const bool listed = d.modelIsValid && d.modelIsValid(p, bare);
    return nimbus::orch::routerIdMissText(p, m, listed ? bare : std::string());
  }
  return d.modelMiss ? d.modelMiss(p, m) : "not in " + p + " list";
}

namespace {

using nimbus::orch::ActionCaller;

// Why the caller rule refused a key, as the summary tag: "not-owner" for a member,
// a guest or the LAN endpoint, else the owner's unattended turn. The four knobs and
// reboot the scheduled-turn rails refused before the rule table keep the tag their
// tokens always carried (test_harness_apply pins them).
std::string refusedTag(const std::string& key, const ActionCaller& c) {
  if (c.lan) return "lan";
  if (!c.owner()) return "not-owner";
  if (key == "device.reboot" || key == "device.config.ttsOn" || key == "device.config.devName")
    return "scheduled";
  return "unattended";
}

// A refused part: its plain reason for the head (the next turn's [DEVICE] line or
// the device.control answer) and one line on the reply: a member or a guest hears that
// only the owner can, and the owner hears what an automatic turn left as it was.
void noteRefusal(DeviceOutcome& out, const std::string& key, const ActionCaller& c, std::string* riskNote) {
  const std::string why = nimbus::orch::actionRefusal(key, c);
  if (!out.refusals.empty()) out.refusals += "; ";
  out.refusals += why;
  if (!riskNote || c.lan) return;
  const nimbus::orch::ActionRuleRow* row = nimbus::orch::actionRuleRow(key);
  const std::string what = row ? row->what : "do that";
  std::string line;
  if (!c.owner()) line = "\nOnly the device's owner can " + what + ".";
  else if (key == "device.orch_model")
    line = "\nThe main model stays as it is: only the owner can change it, in a message they write.";
  else line = "\nAn automatic turn cannot " + what + ", so it was left as it was.";
  if (riskNote->find(line) == std::string::npos) *riskNote += line;
}

// The config element's knobs the caller may not set, stripped from a copy (each says
// why). The owner writing now strips nothing.
ValidatedAction stripRefusedKnobs(const ValidatedAction& va, const ActionCaller& c, std::string* riskNote,
                                  DeviceOutcome& out) {
  ValidatedAction ex = va;
  size_t nk = 0;
  const nimbus::orch::ConfigKnob* knobs = nimbus::orch::configKnobTable(nk);
  for (size_t i = 0; i < nk; i++) {
    if (!(va.*(knobs[i].has))) continue;
    const std::string key = nimbus::orch::configKnobKey(knobs[i].knob);
    if (nimbus::orch::actionAllowed(key, c)) continue;
    ex.*(knobs[i].has) = false;
    out.summary += std::string(knobs[i].knob) + "-refused(" + refusedTag(key, c) + ") ";
    noteRefusal(out, key, c, riskNote);
  }
  return ex;
}

// The tokens of the knobs that carry a voice, a risk or a restart, and the
// owner-visible risk notes. Owner-visible, NOT model-optional: the log line alone left
// zero trace unless the owner went looking (review) - the note rides the delivered
// reply itself, whatever the model chose to say.
void describeRiskKnobs(const ValidatedAction& ex, std::string* riskNote, DeviceOutcome& out) {
  if (ex.hasTtsOn) out.summary += ex.ttsOn ? "ttsOn " : "ttsOff ";
  if (ex.hasSleepOvr) {
    out.summary += ex.sleepOvr ? "sleepOvr:ON(deep-discharge-risk) " : "sleepOvr:off ";
    if (riskNote)
      *riskNote += ex.sleepOvr
          ? "\n⚠ low-battery sleep OVERRIDDEN - the pack can deep-discharge (resets at reboot)"
          : "\n✓ low-battery sleep protection re-enabled";
  }
  if (ex.hasBrightOvr) {
    out.summary += ex.brightOvr ? "brightOvr:ON(heat-risk) " : "brightOvr:off ";
    if (riskNote)
      *riskNote += ex.brightOvr
          ? "\n⚠ LED cap OVERRIDDEN to 100% - sustained heat risk to the panel (resets at reboot)"
          : "\n✓ LED 60% safety cap re-enabled";
  }
  if (ex.hasDevName) out.summary += "devName(reboot-to-apply) ";
}

// The config element: strip what the caller may not set, then execute what is left.
void applyConfig(const ValidatedAction& va, const ActionCaller& c, const DeviceApplyDeps& d, std::string* riskNote,
                 DeviceOutcome& out) {
  const ValidatedAction ex = stripRefusedKnobs(va, c, riskNote, out);
  describeRiskKnobs(ex, riskNote, out);
  // Talk-to-configure knobs (posture/profile/theme/attnHoldMs) mutate the
  // main-task-owned Config/theme, so they route through the DeviceSink like
  // led/lights - the main loop applies + persists them (same pipeline as
  // the menu/web UI). NVS-only knobs above stay inline (any-task safe).
  const bool stagedKnobs = ex.hasPosture || ex.hasProfile || ex.hasTheme || ex.hasAttnHoldMs;
  const bool changed = d.execConfig ? d.execConfig(ex) : false;
  if (d.stageDevice && stagedKnobs) d.stageDevice(ex);
  // 23 Oct: "cfg" is said only when a stored value changed (or a knob was
  // handed to the main loop); an action that stored nothing reports "no
  // change", and the claim check holds a "switched" reply against it.
  out.summary += (changed || (d.stageDevice && stagedKnobs)) ? "cfg " : "cfg(no change) ";
}

void applyOrchModel(const ValidatedAction& va, const DeviceApplyDeps& d, std::string* riskNote, DeviceOutcome& out) {
  std::string paused;   // why a pause keeps the head where it is ("" = it does not)
  // The agent reroutes its OWN provider host + model (owner: "switch to opus").
  // Validate the model against that provider's choice list, then set BOTH host
  // and model - NVS-only, any-task safe. Keys + provider PRIORITY stay owner-only
  // (never settable here); this host/model switch is the one deliberate exception.
  // Refuse a switch to a provider with NO API key configured - modelIsValid only
  // checks the static choice list, so without this the agent could reroute its own
  // host to a keyless provider and strand every subsequent turn. Keys are
  // owner-only (web UI); the agent can only move between providers already keyed.
  // The provider must be one of the registry slots (the schema enum), and a
  // refusal says why in the head's words after the pinned token.
  if (!nimbus::orch::isProviderSlug(va.orchProvider.c_str()) || !d.modelIsValid ||
      !d.modelIsValid(va.orchProvider, va.orchModel)) {
    out.summary += "orch_model(invalid) " + va.orchModel + " on " + va.orchProvider + ": " +
                   orchModelMiss(va, d) + " ";
  } else if (!d.providerHasKey || !d.providerHasKey(va.orchProvider)) {
    out.summary += "orch_model(no-key) ";
  } else if (!(paused = headPauseRefusal(va, d)).empty()) {
    // A pause is no reason to move the owner's head: nothing changes, and the reply
    // carries why (whatever the model chose to say).
    out.summary += "orch_model(no change) " + va.orchProvider + "/" + va.orchModel + ": " + paused + " ";
    if (riskNote) *riskNote += "\n" + paused;
  } else if (!d.setOrchHostModel) {
    out.summary += "orch_model(no change) " + va.orchProvider + "/" + va.orchModel +
                   ": this host cannot change its model ";
  } else {
    d.setOrchHostModel(va.orchProvider, va.orchModel);
    out.summary += "orch_model(" + va.orchProvider + "/" + va.orchModel + ") ";
  }
}

// led / lights / reboot execute through the DeviceSink: main.cpp stages the
// validated action thread-safely and the main loop (which owns the ring +
// power) applies it. Without a sink installed they remain logged no-ops.
void applyStaged(const ValidatedAction& va, const char* name, const DeviceApplyDeps& d, DeviceOutcome& out) {
  if (!d.stageDevice) { out.summary += std::string(name) + "(noop) "; return; }
  d.stageDevice(va);
  out.summary += std::string(name) + " ";
}

}  // namespace

DeviceOutcome applyDeviceAction(const std::string& json, const nimbus::orch::Principal& who,
                                bool unattended, const std::string& chatId, const DeviceApplyDeps& d,
                                std::string* riskNote) {
  // One device[] element: validate + check WHO asked + policy-strip + execute.
  // Shared by the end-of-turn device[] loop below AND the mid-turn
  // `device.control` registry tool, so both paths carry the IDENTICAL security
  // envelope (protected-key block, the caller rule table, risk notes). Every
  // summary token is the pre-lift byte-identical literal (test_harness_apply
  // pins them).
  DeviceOutcome out;
  JsonDocument dd;
  if (deserializeJson(dd, json)) { out.summary = "skip "; return out; }
  ValidatedAction va =
      nimbus::orch::validateAction(dd.as<ArduinoJson::JsonVariantConst>());
  if (!va.allowed) {
    // Blocked/unknown: log the policy reason ("protected-BLOCKED"/"unsupported"),
    // never the offending value (the portable validator never puts a secret here).
    out.summary = va.reason.empty() ? std::string("skip ") : va.reason + " ";
    return out;
  }
  // WHO asked (CUM-813): the action as a whole first (a member's config element, an
  // automatic turn's restart or model switch), before any key or model is checked,
  // so a refused caller learns nothing about the device's keys or lists.
  const ActionCaller c = nimbus::orch::actionCaller(who, unattended, chatId);
  const std::string key = nimbus::orch::deviceActionKey(va.kind);
  if (!key.empty() && !nimbus::orch::actionAllowed(key, c)) {
    out.refusedWhole = true;
    noteRefusal(out, key, c, riskNote);
    if (va.kind == ActionKind::OrchModel) out.summary = "orch_model(refused) " + out.refusals + " ";
    else
      out.summary = (va.kind == ActionKind::Config ? std::string("config") : key.substr(sizeof("device.") - 1)) +
                    "-refused(" + refusedTag(key, c) + ") ";
    return out;
  }
  switch (va.kind) {
    case ActionKind::Config:    applyConfig(va, c, d, riskNote, out); break;
    // EXPLICIT tts action - but ONLY when the owner's "Voice replies" toggle is ON (default
    // ON for a speaker board, N12). Field bug (P2.5): the model reliably chose to speak
    // whenever the owner seemed nearby (voice input), and this gate bypass meant there was
    // NO way to mute it. When OFF the action is a silent no-op - the text reply still
    // delivers on its own channel.
    case ActionKind::Tts:       out.summary += applyTts(va, chatId, d, &out.replyNote); break;
    case ActionKind::Led:       applyStaged(va, "led", d, out); break;
    case ActionKind::Lights:    applyStaged(va, "lights", d, out); break;
    // Reboot reaches here only on the owner's attended turn: a scheduled loop must
    // NOT reboot - a reboot in the boot-relative clock window would reset the daily
    // cost ceiling (the rule table's device.reboot row).
    case ActionKind::Reboot:    applyStaged(va, "reboot", d, out); break;
    case ActionKind::OrchModel: applyOrchModel(va, d, riskNote, out); break;
    default:                    out.summary += "skip "; break;
  }
  return out;
}

}  // namespace agent
