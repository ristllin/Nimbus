#include "nimbus/harness/claim_check.h"

#include <ArduinoJson.h>

#include "nimbus/harness/log.h"
#include "nimbus/orch/device_actions.h"
#include "nimbus/orch/tool_taint.h"

namespace agent {

namespace orch = nimbus::orch;

namespace {

uint32_t g_corrected = 0;   // RAM only: /api/state claimsCorrected
uint32_t g_tailed = 0;      // RAM only: /api/state claimsTailed

constexpr size_t kSourceNumbersMax = 4096;

// What an allowed device action does, by the device's own validator (a blocked
// or unknown element backs nothing).
bool deviceEffect(const std::string& json, orch::FxKind& out) {
  JsonDocument d;
  if (deserializeJson(d, json) != DeserializationError::Ok) return false;
  const orch::ValidatedAction va = orch::validateAction(d.as<JsonVariantConst>());
  if (!va.allowed) return false;
  switch (va.kind) {
    case orch::ActionKind::Tts: out = orch::FxKind::Speak; return true;
    case orch::ActionKind::Led:
    case orch::ActionKind::Lights:
    case orch::ActionKind::Config:
    case orch::ActionKind::OrchModel:
    case orch::ActionKind::Reboot: out = orch::FxKind::Config; return true;
    default: return false;
  }
}

// A hosted tool (a connector, a hosted search) runs where the device cannot
// see it, so a claim of these classes may be true without a ledger entry.
bool hostedClass(orch::ClaimClass c) {
  return c == orch::ClaimClass::Sent || c == orch::ClaimClass::Checked ||
         c == orch::ClaimClass::Changed;
}

}  // namespace

// From the one shared tool table (orch::toolEffect, lane L), so the ledger and
// the other rails that read the table can never disagree about a tool. A spawn
// called as a tool joins the turn's spawn items and apply records whether the
// job queue took it, so the call itself vouches for nothing (Read).
orch::FxKind fxKindForTool(const std::string& n) {
  using orch::FxKind;
  static const FxKind kByEffect[] = {
      FxKind::Read,       // Read
      FxKind::MemWrite,   // MemWrite
      FxKind::MemUpdate,  // MemUpdate
      FxKind::MemPin,     // MemPin
      FxKind::MemDelete,  // MemDelete
      FxKind::Scratch,    // Scratch
      FxKind::FileWrite,  // FileWrite
      FxKind::Send,       // Share
      FxKind::SkillSave,  // SkillSave
      FxKind::Read,       // Spawn
      FxKind::Config,     // Cancel
      FxKind::Send,       // Send
      FxKind::Speak,      // Speak
      FxKind::Schedule,   // Schedule
      FxKind::Config,     // Config
      FxKind::Config,     // Admin
      FxKind::Config,     // Media
  };
  static_assert(sizeof(kByEffect) / sizeof(kByEffect[0]) == (size_t)orch::ToolEffect::Media + 1,
                "every ToolEffect needs a ledger kind");
  const size_t e = (size_t)orch::toolEffect(n);
  return e < sizeof(kByEffect) / sizeof(kByEffect[0]) ? kByEffect[e] : FxKind::Read;
}

void ClaimCheck::beginTurn() {
  fx_.reset();
  sources_.clear();
  recalled_.clear();
  causeEvidence_ = 0;
  hosted_ = false;
  corrected_ = false;
}

void ClaimCheck::setSources(const std::string& inputs, const std::string& recalled) {
  sources_ = orch::figureNumbers(inputs, kSourceNumbersMax);
  recalled_ = orch::figureNumbers(recalled, kSourceNumbersMax);
  causeEvidence_ = orch::causeEvidenceOf(inputs);
}

uint8_t ClaimCheck::toolCallFlags(const std::string& regName, const std::string& argsJson) {
  // A read of the device's own state (L19-11): the only thing that can confirm
  // "it is working" (kEvHealthRead comes from the ledger, never from text).
  if (regName == "device.status" || regName == "system.health") return orch::kFxHealth;
  const bool memory = regName == "memory.write" || regName == "memory.update";
  const bool telegram = regName == "reply.telegram" || regName == "files.send";
  if (!memory && !telegram) return 0;
  JsonDocument d;
  if (deserializeJson(d, argsJson) != DeserializationError::Ok) return 0;
  if (telegram) {
    // 22 Oct: a Telegram send (text, document, photo, voice) is backed only by
    // Telegram's own receipt, never by the send tool's success text.
    return (d["voice"] | false) ? (uint8_t)(orch::kFxTelegram | orch::kFxVoice)
                                : orch::kFxTelegram;
  }
  return (d["permanent"] | false) ? orch::kFxPermanent : 0;
}

void ClaimCheck::noteTool(orch::FxKind kind, bool isError, const std::string& output, uint8_t flags) {
  orch::recordToolEffect(fx_, kind, isError, output, flags);
  causeEvidence_ |= orch::causeEvidenceOf(output);
  if (!isError && (flags & orch::kFxHealth)) causeEvidence_ |= orch::kEvHealthRead;
  if (isError || sources_.size() >= kSourceNumbersMax) return;
  const std::string more = orch::figureNumbers(output, kSourceNumbersMax - sources_.size());
  if (more.empty()) return;
  if (!sources_.empty()) sources_ += ' ';
  sources_ += more;
}

void ClaimCheck::noteFresh(int sourced, int unsourced) {
  if (sourced > 0) fx_.add(orch::FxKind::SubSourced, true, "", (uint8_t)(sourced > 255 ? 255 : sourced));
  if (unsourced > 0)
    fx_.add(orch::FxKind::SubUnsourced, true, "", (uint8_t)(unsourced > 255 ? 255 : unsourced));
}

bool ClaimCheck::deviceActionEffect(const std::string& json, orch::FxKind& out) {
  return deviceEffect(json, out);
}

void ClaimCheck::addPending(orch::TurnEffects& fx, const orch::Turn& t) {
  using orch::FxKind;
  const uint8_t p = orch::kFxPending;
  for (const orch::MemWrite& w : t.mem_write)
    fx.add(FxKind::MemWrite, true, "", 1, (uint8_t)(p | (w.permanent ? orch::kFxPermanent : 0)));
  int spawns = 0;
  for (const orch::Spawn& s : t.spawn) spawns += s.task.empty() ? 0 : 1;
  for (const orch::SessionOp& op : t.session_ops)
    if (op.op == "spawn" && !(op.task.empty() && op.message.empty())) spawns++;
  if (spawns) fx.add(FxKind::Spawn, true, "", (uint8_t)(spawns > 255 ? 255 : spawns), p);
  if (t.scratchpad.present) fx.add(FxKind::Scratch, true, "", 1, p);
  for (const orch::DeviceAction& da : t.device) {
    FxKind k;
    if (deviceEffect(da.json, k)) fx.add(k, true, "", 1, p);
  }
}

std::vector<orch::ClaimGap> ClaimCheck::gapsFor(const std::string& reply,
                                                const orch::TurnEffects& fx,
                                                bool skipSpawned) const {
  orch::ClaimEvidence ev;
  ev.fx = &fx;
  ev.grounding = &sources_;
  ev.recalled = &recalled_;
  ev.causeEvidence = causeEvidence_;
  const orch::ClaimScan scan = orch::scanClaims(reply);
  if (scan.skippedNonEnglish) hlog::log("claims: skipped (non-English)");
  std::vector<orch::ClaimGap> gaps = orch::claimGaps(reply, scan, ev);
  std::vector<orch::ClaimGap> kept;
  for (orch::ClaimGap& g : gaps) {
    if (skipSpawned && g.cls == orch::ClaimClass::Spawned) continue;
    // A hosted tool may have sent an email, but never this device's Telegram voice
    // message: a device-only gap is checked whatever tools the head had.
    if (hosted_ && hostedClass(g.cls) && !g.deviceOnly) continue;
    kept.push_back(std::move(g));
  }
  orch::ClaimGap unsourced;
  bool already = false;
  for (const orch::ClaimGap& g : kept) already = already || g.cls == orch::ClaimClass::Checked;
  if (!already && orch::unsourcedResultGap(reply, fx, &unsourced)) kept.push_back(unsourced);
  return kept;
}

std::string ClaimCheck::finalCheck(const std::string& finalTurnJson, bool skipSpawned) {
  orch::Turn t;
  orch::ParseError err;
  if (!orch::parseTurn(finalTurnJson, t, err)) return std::string();   // the shape check's case
  orch::TurnEffects pending = fx_;
  addPending(pending, t);
  const std::string said = t.ask.empty() ? t.reply : t.reply + "\n\n" + t.ask;   // one message
  const std::vector<orch::ClaimGap> gaps = gapsFor(said, pending, skipSpawned);
  if (gaps.empty()) {
    hlog::logf("claims: ok n=%u", (unsigned)orch::scanClaims(said).claims.size());
    return std::string();
  }
  corrected_ = true;
  g_corrected++;
  hlog::logf("claims: %s unbacked -> correction round", orch::claimClassSlug(gaps[0].cls));
  return orch::claimCorrection(gaps, pending);
}

std::string ClaimCheck::tailAfterApply(const std::string& reply, bool skipSpawned) {
  if (reply.find_first_not_of(" \t\r\n") == std::string::npos) return std::string();
  const std::vector<orch::ClaimGap> gaps = gapsFor(reply, fx_, skipSpawned);
  if (gaps.empty()) return std::string();
  const std::string tail = orch::claimTail(gaps);
  if (tail.empty()) return tail;
  g_tailed++;
  for (const orch::ClaimGap& g : gaps) hlog::logf("claims: tail %s", orch::claimClassSlug(g.cls));
  return tail;
}

uint32_t ClaimCheck::correctedCount() { return g_corrected; }
uint32_t ClaimCheck::tailedCount() { return g_tailed; }

}  // namespace agent
