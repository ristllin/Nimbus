#include "nimbus/harness/claim_check.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "nimbus/harness/jobs.h"   // kLedgerRanLead: how a ledger line names what a sub ran
#include "nimbus/harness/log.h"
#include "nimbus/harness/prompt_layout.h"   // the display labels: where the fixed instructions end
#include "nimbus/orch/device_actions.h"
#include "nimbus/orch/tool_taint.h"

namespace agent {

namespace orch = nimbus::orch;

namespace {

uint32_t g_corrected = 0;   // RAM only: /api/state claimsCorrected
uint32_t g_tailed = 0;      // RAM only: /api/state claimsTailed

constexpr size_t kSourceNumbersMax = 4096;
constexpr size_t kSourceWordsMax = 8192;   // the words a named source is checked against
constexpr size_t kToolTextMax = 32768;     // the tool results a quoted record is checked against
constexpr size_t kOwnerTextMax = 1024;     // the owner's message a why-question is read from
constexpr size_t kEvidenceTextMax = 32768; // a text longer than this is read only in part (orch_claims)

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

// A photo in the owner's message that a model could read (attached, described or
// transcribed): it may show a label, a receipt, a watch or a gauge.
bool readablePhoto(const std::string& ownerText) {
  for (size_t at = ownerText.find("[image"); at != std::string::npos; at = ownerText.find("[image", at + 1)) {
    const size_t close = ownerText.find(']', at);
    if (close == std::string::npos) return false;
    const std::string after = ownerText.substr(close + 1, 40);
    if (after.find("(not read") == std::string::npos && after.find("(could not be fetched") == std::string::npos &&
        after.find("(couldn't be described") == std::string::npos)
      return true;
  }
  return false;
}

// What this turn names as sources (CUM-824): see setSources.
uint32_t namedSources(const TurnSources& parts, const std::string& recalled) {
  uint32_t bits = orch::sourceEvidenceOf(parts.owner) | orch::sourceEvidenceOf(parts.fresh) |
                  orch::sourceEvidenceOf(parts.memory) | (orch::sourceEvidenceOf(recalled) & orch::kSrcOwnerDevices);
  if (parts.webRead) bits |= orch::kSrcWebRead;   // a sub read the web (its provider's record)
  if (readablePhoto(parts.owner)) bits |= orch::kSrcAll;
  return bits;
}

// The tool-name parts of a ledger line's "it ran ..." list that name a tool ("notion_search
// x2, web_search x0" -> notion), not an action or a count.
std::string toolParts(const std::string& line) {
  static const char* const kGeneric[] = {"search", "fetch", "query", "lookup", "list", "read", "tool"};
  const size_t ran = line.find(kLedgerRanLead);
  if (ran == std::string::npos) return std::string();
  const size_t from = ran + std::strlen(kLedgerRanLead);
  const std::string ns = orch::normalizedWords(line.substr(from, line.find(" (", from) - from)) + " ";
  std::string out;
  for (size_t b = 0, e; (e = ns.find(' ', b)) != std::string::npos; b = e + 1) {
    const std::string w = ns.substr(b, e - b);
    bool generic = w.size() < 4 || std::isdigit((unsigned char)w[0]) || (w[0] == 'x' && w.size() > 1 && std::isdigit((unsigned char)w[1]));
    for (const char* g : kGeneric) generic = generic || w == g;
    if (!generic) out += " " + w;
  }
  return out;
}

// CUM-824: what makes a sentence about a past sub-agent: the spawn ledger's job tags and
// the tool and connector names its lines list ("notion_search x2" -> notion), normalized,
// and the words for a sub-agent. A job's own name is left out: the head names jobs by their
// topic ("weather-trains"), and a reason about the weather is not one about the job. ""
// when there is no ledger.
std::string ledgerSubjects(const std::string& ledger) {
  if (ledger.empty()) return std::string();
  std::string out = "sub agent agents subagent subagents lookup";
  for (size_t at = 0; (at = ledger.find("\n- ", at)) != std::string::npos;) {
    at += 3;
    const std::string line = ledger.substr(at, ledger.find('\n', at) - at);
    const size_t paren = line.find(" (");   // "- name (tag, provider/model): ..."
    if (paren != std::string::npos) out += " " + orch::normalizedWords(line.substr(paren + 2, line.find(',', paren) - paren - 2));
    out += toolParts(line);
  }
  return out;
}

// The text names one of the subject words.
bool namesASubject(const std::string& text, const std::string& subjects) {
  const std::string pad = " " + orch::normalizedWords(text) + " ", list = subjects + " ";
  for (size_t b = 0, e; (e = list.find(' ', b)) != std::string::npos; b = e + 1)
    if (e > b && pad.find(" " + list.substr(b, e - b) + " ") != std::string::npos) return true;
  return false;
}

// CUM-782: a files.append whose file is a log (.csv or .log); its result decides
// whether a row landed (recordToolEffect).
bool appendsToLog(const std::string& argsJson) {
  JsonDocument d;
  if (deserializeJson(d, argsJson) != DeserializationError::Ok) return false;
  std::string name = d["name"] | "";
  for (char& c : name) c = (char)std::tolower((unsigned char)c);
  auto endsWith = [&](const char* ext) {
    const size_t n = std::strlen(ext);
    return name.size() > n && name.compare(name.size() - n, n, ext) == 0;
  };
  return endsWith(".csv") || endsWith(".log");
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
  ownerNums_.clear();
  sourceNames_ = 0;
  sourceWords_.clear();
  toolText_.clear();
  ownerText_.clear();
  ledger_.clear();
  clipped_ = false;
  instructions_ = inputs_ = nullptr;
  causeEvidence_ = 0;
  subRuns_ = orch::HostedRuns();
  orch::resetHostedRuns();   // CUM-835: the head adapters record this turn's server-side runs
  corrected_ = false;
}

void ClaimCheck::setSources(const std::string& inputs, const std::string& recalled, const TurnSources& parts) {
  sources_ = orch::figureNumbers(inputs, kSourceNumbersMax);
  recalled_ = orch::figureNumbers(recalled, kSourceNumbersMax);
  causeEvidence_ = orch::causeEvidenceOf(inputs);
  ownerNums_ = orch::figureNumbers(parts.owner, kSourceNumbersMax);
  ownerText_ = parts.owner.substr(0, kOwnerTextMax);
  ledger_ = parts.ledger;
  subRuns_ = parts.subRuns;
  sourceNames_ = namedSources(parts, recalled);
  bool clipped = false;
  sourceWords_ = orch::sourceWordsOf(parts.owner + "\n" + parts.fresh + "\n" + parts.memory, kSourceWordsMax, &clipped);
  clipped = clipped || parts.fresh.size() > kEvidenceTextMax || parts.memory.size() > kEvidenceTextMax;
  clipped_ = clipped_ || clipped;
}

// CUM-824: a tool result is a source text: the sources it names, its words (a named
// source), its words in order (a quoted record). A store that hits its cap marks the turn
// clipped, and the rules that assert absence then stand down.
void ClaimCheck::noteToolText(const std::string& output) {
  sourceNames_ |= orch::sourceEvidenceOf(output);
  bool clipped = sourceWords_.size() >= kSourceWordsMax || output.size() > kEvidenceTextMax;
  if (!clipped) sourceWords_ += orch::sourceWordsOf(output, kSourceWordsMax - sourceWords_.size(), &clipped);
  const std::string words = orch::normalizedWords(output);
  if (toolText_.size() + words.size() + 1 > kToolTextMax) clipped = true;
  else toolText_ += " " + words;
  clipped_ = clipped_ || clipped;
}

uint8_t ClaimCheck::toolCallFlags(const std::string& regName, const std::string& argsJson) {
  // A read of the device's own state (L19-11): the only thing that can confirm
  // "it is working" (kEvHealthRead comes from the ledger, never from text).
  if (regName == "device.status" || regName == "system.health") return orch::kFxHealth;
  if (regName == "files.append") return appendsToLog(argsJson) ? orch::kFxLogRow : 0;
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
  if (!isError) noteToolText(output);
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

orch::HostedRuns ClaimCheck::hostedRan() const {
  orch::HostedRuns ran = orch::hostedRuns();
  ran.add(subRuns_);
  return ran;
}

// CUM-835: a gap a provider-hosted tool that really ran this turn may have closed
// where the device cannot see it. A web search never sent an email or changed a
// calendar: those classes need a connector run.
bool ClaimCheck::hostedMayBack(const orch::ClaimGap& g) const {
  const orch::HostedRuns ran = hostedRan();
  if (!ran.any()) return false;
  // A hosted tool may have sent an email, but never this device's Telegram voice
  // message: a device-only gap is checked whatever tools ran.
  switch (g.cls) {
    case orch::ClaimClass::Sent:
    case orch::ClaimClass::Changed: return ran.connector > 0 && !g.deviceOnly;
    case orch::ClaimClass::Checked: return !g.deviceOnly;
    // A connector or a hosted search may have read a website, an app or a receipt
    // the device never saw; a gauge, a label or a watch it cannot have read (CUM-824).
    case orch::ClaimClass::Source: return (g.source & orch::kSrcHostable) || !g.name.empty();
    default: return false;
  }
}

// CUM-824: the rules that read what the turn was given, not the cue table: a quote
// presented as a memory or a record the turn never saw, and (asked why) a reason about a
// past sub-agent that is not on record.
void ClaimCheck::recordGaps(const std::string& reply, std::vector<orch::ClaimGap>& kept) const {
  if (clipped_) return;   // a tool result past the cap may hold the words
  orch::ClaimGap g;
  if ((instructions_ || inputs_) && orch::hasRecordQuote(reply) &&
      orch::recordQuoteGap(reply, seenText(/*fixedToo=*/true), &g, hostedRan().any()))
    kept.push_back(g);
  // Asked why about a past sub-agent: the question names one ("Why notion?").
  std::string subjects = orch::asksWhy(ownerText_) ? ledgerSubjects(ledger_) : std::string();
  if (!namesASubject(ownerText_, subjects)) subjects.clear();
  if (!subjects.empty() && orch::unrecordedReasonGap(reply, seenText(/*fixedToo=*/false), ownerText_, subjects, &g))
    kept.push_back(g);
}

std::vector<orch::ClaimGap> ClaimCheck::gapsFor(const std::string& reply,
                                                const orch::TurnEffects& fx,
                                                bool skipSpawned) const {
  orch::ClaimEvidence ev;
  ev.fx = &fx;
  ev.grounding = &sources_;
  ev.recalled = &recalled_;
  ev.causeEvidence = causeEvidence_;
  ev.sources = &sourceNames_;
  ev.owner = &ownerNums_;
  ev.sourceWords = &sourceWords_;
  const orch::ClaimScan scan = orch::scanClaims(reply);
  if (scan.skippedNonEnglish) hlog::log("claims: skipped (non-English)");
  std::vector<orch::ClaimGap> gaps = orch::claimGaps(reply, scan, ev);
  std::vector<orch::ClaimGap> kept;
  for (orch::ClaimGap& g : gaps) {
    if ((skipSpawned && g.cls == orch::ClaimClass::Spawned) || hostedMayBack(g)) continue;
    if (clipped_ && g.cls == orch::ClaimClass::Source) continue;   // the words may be past a cap
    kept.push_back(std::move(g));
  }
  recordGaps(reply, kept);
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
  std::string fix = orch::claimCorrection(gaps, pending);
  // A reason about a past sub-agent: the ledger lines (device-authored) are what the
  // records show.
  const size_t nl = ledger_.find('\n');
  const bool reason = std::any_of(gaps.begin(), gaps.end(), [](const orch::ClaimGap& g) { return orch::isUnrecordedReason(g); });
  if (reason && nl != std::string::npos) fix += "\nThe spawn ledger records only:\n" + ledger_.substr(nl + 1);
  return fix;
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

// Everything the turn was given, normalized: the tool results, the input block and the
// prompt. Without `fixedToo`, only the prompt from its shelf on (standing preferences,
// skills index, history, the dynamic context): the records a reason may come from.
std::string ClaimCheck::seenText(bool fixedToo) const {
  std::string out = toolText_;
  if (inputs_) out += " " + orch::normalizedWords(*inputs_);
  if (!instructions_) return out;
  size_t from = fixedToo ? 0 : std::string::npos;
  for (const char* label : {layout::kDisplayShelfLabel, layout::kDisplayHistoryLabel, layout::kDisplayDynLabel}) {
    const size_t at = instructions_->find(label);
    if (at < from) from = at;
  }
  return from == std::string::npos ? out : out + " " + orch::normalizedWords(instructions_->substr(from));
}

uint32_t ClaimCheck::correctedCount() { return g_corrected; }
uint32_t ClaimCheck::tailedCount() { return g_tailed; }

}  // namespace agent
