#pragma once
#include <ArduinoJson.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "nimbus/harness/channel.h"
#include "nimbus/harness/http.h"
#include "nimbus/harness/log.h"
#include "nimbus/harness/plain/chat_skills.h"
#include "nimbus/harness/plain/compact.h"
#include "nimbus/harness/plain/deliver.h"
#include "nimbus/harness/plain/engine.h"
#include "nimbus/harness/plain/guards.h"
#include "nimbus/harness/plain/head_gate.h"   // noteHeadOutcome: the shared park after a turn (E2-FIX14)
#include "nimbus/harness/plain/head_slot.h"   // slotRatesFor, ledgerUsage (lane E2-R8a)
#include "nimbus/harness/plain/hooks.h"
#include "nimbus/harness/plain/request.h"
#include "nimbus/harness/plain/resume.h"   // installResume - the crash resume (lane E2-FIX12)
#include "nimbus/harness/plain/unattended.h"   // OwnerSink, RoutineDeps (lane E2-SCHED)
#include "nimbus/harness/plain/rails.h"   // installRails - the safety rails (lane E2-FSAFE)
#include "nimbus/harness/platform.h"
#include "nimbus/orch/fallback_rules.h"   // the owner's fallback rule set (E2-P-FO)
#include "nimbus/orch/model_catalog.h"    // modelSizeClass
#include "nimbus/orch/model_price_defaults.h"
#include "nimbus/orch/rbac.h"
#include "nimbus/orch/seen_index.h"
#include "nimbus/orch/spend_guard.h"
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/budget.h"
#include "nimbus/orch/tx/digest.h"   // RecentFilesBlock::Source - the live file store's list
#include "nimbus/orch/tx/log.h"
#include "nimbus/orch/turn_spend.h"
#include "nimbus/orch/tx/migrate.h"

// plain/chat_binding.h - one chat on engine 2 on the device (lane E2-DEV). A copy of nimbusd's
// one-chat rig (nimbusd/src/plain_rig.h, PlainRig) with what a device chat needs on top:
//   - the arena sized for the head (tx::arenaBytesFor of its budget, about 1.1 MB for Sonnet),
//     not the 1,728 KB constant;
//   - compaction installed (plain::installCompaction): the summarizer is the head host's small
//     model on the same transport; the control file is the binding's TextIO (no chatsum mirror:
//     engine 2 never writes the earlier engine's fold record, lane E2-FIX2B finding P5);
//   - the suppression seam (installSuppression), the money guard (TurnSetup::cost from the head's
//     price row; no price row: boot refuses, fail closed) and one ChatBreaker;
//   - the seen index rebuilt from the filled arena after boot (RB-TG1);
//   - the safety rails (plain::installRails, RB-RL; lane E2-FSAFE): in a turn that read outside
//     content a note change goes to the binding's pending shelf (railPending) and never runs;
//     with no shelf every pending change is refused (fail closed);
//   - the shared tool registry, dispatched with the turn's Principal (plain_rig.h's dispatch).
// Lane E2-CHAT adds the owner's daily chat on top:
//   - photos: the message's image parts (ChatMessage::parts) and the photo hook (TurnSetup::photo);
//   - skills: the skills index and the context skills in the pinned block, each one whole or not
//     at all (skillRefused names one left out; chat_skills.h);
//   - commands that act on the engine: compactNow (a compaction now) and budgetView (/budget);
//   - the turn's usage handed to `usage` once per turn; the progress line (installProgress).
//   - lane E2-MIG: the earlier engine's conversation imported before the engine opens its log
//     (tx::migrate), when the device gives an oldEngine hook.
// Lane E2-FIX12: the crash resume (plain::installResume, PRD section 7) at every boot: a person's
// turn a restart cut is resumed once from what is stored (no tool run again), a second cut gives
// the plain crash notice; the seam's report is lastResume() and goes to `resumed` once boot is done.
// Lane E2-SCHED: ownerSink and routineDeps, the parts a reminder or routine fire takes from the chat
// (nimbus/harness/plain/schedule_binding.h).
// Only lib headers are included, so a host test drives the same class with a fake platform, a
// scripted transport and a fake card (test/test_plain_chat_binding). The device seam
// (src/agent/orchestrator_plain.cpp) gives it the device's parts.
//
// It adds no behavior to the loop: a turn is PlainEngine::runTurn, and what the owner sees is
// what the engine sent through `sendPart` (one already-made part per call).
namespace agent {
namespace plain {

struct ChatBindingDeps {
  const Platform* platform = nullptr;            // nowMs, delayMs, nowEpoch, allocLarge, freeLarge
  HttpTransport* transport = nullptr;            // the head and the summarizer (one TLS slot)
  nimbus::orch::tx::TxFiles* files = nullptr;    // /mem/tx (false everywhere: RAM only)
  nimbus::orch::ToolRegistry* registry = nullptr;   // the device's shared registry
  // One part the deliver function already made; true when the channel took it.
  std::function<bool(const std::string& chatId, const std::string& part)> sendPart;
  std::function<std::string()> key;              // the head's key, read at render time, never stored
  std::function<std::string(const nimbus::orch::Principal&)> pinnedNotes;   // null: none
  // E2-REP2: the owner's tool servers waiting for his approval, named in his turn's tools index. Null: none.
  std::function<std::vector<std::string>()> toolServersWaiting;
  TextIO compactCtl;                             // unset: kept in RAM
  nimbus::orch::SeenIndex* seen = nullptr;       // null: no rebuild (not a Telegram chat)
  // One line each; null: silent. A std::string, not a const char*: the tg_poll stack gate resolves
  // a std::function call to every handler of its exact signature, and the card listings' name
  // callbacks are void(const char*) (lane E2-FIX2B).
  std::function<void(const std::string& line)> log;
  // Lane E2-TOOLS: a tool whose result is outside content (web search); its outputs taint the turn
  // (ToolOutput::outside). Null: no tool reads outside content.
  std::function<bool(const std::string& tool)> outside;
  // Lane E2-TOOLS: EngineDeps::recentFiles for this chat's person (the device's file store); null: no list.
  std::function<nimbus::orch::tx::RecentFilesBlock::Source(const nimbus::orch::Principal&)> recentFiles;
  // Lane E2-CHAT.
  PhotoFn photo;                                 // the turn's photo bytes by ref; null: stubs only
  std::function<ChatSkills(const nimbus::orch::Principal&)> skills;   // null: no skills
  // A context skill that does not fit the pinned block whole: left out of it (listed instead).
  std::function<void(const std::string& id, size_t bytes)> skillRefused;
  // Once per turn, after runTurn: what the turn spent, for the owner's usage view.
  std::function<void(const TurnOutcome& out)> usage;
  // Lane E2-CMP: each paid summarizer request of this chat's compaction, for the same usage view and
  // monthly ceiling (provider slug, model, its tokens). Null: not counted.
  std::function<void(const std::string& provider, const std::string& model, const nimbus::orch::TokenUsage& u)>
      compactUsage;
  ProgressDeps progress;                         // send unset: no progress line
  // The earlier engine's conversation (lane E2-MIG): fills rows, fold, exportText and tzMinutes
  // of the migration's deps (the binding sets the rest); null, or false: no import. Called at
  // every boot, before the engine opens the log (tx/migrate.h's contract), and only for the chat's
  // own key: a key /clear started (<chat>_c<n>) is never filled.
  std::function<bool(nimbus::orch::tx::MigrateDeps&)> oldEngine;
  // True when no card is fitted at all (the chat runs from RAM by design); null: a card is
  // fitted, so a log that cannot open is an outage (EngineDeps::noCard, lane E2-STORE).
  std::function<bool()> noCard;
  // Lane E2-FSAFE: RailsDeps::pending, the approval shelf for a tainted turn's pending calls (the
  // device: the notes' pending shelf). Null: a pending call is refused, nothing kept, nothing run.
  RailPendingFn railPending;
  // Lane E2-P-SURF: RailsDeps::noted, a call the rails refused or put on the shelf (the device: the Safety tab).
  RailNotedFn railNoted;
  // Lane E2-FIX12: the plain crash notice's device name ("": Nimbus), and what the boot's crash
  // resume did when it acted (lastResume(); null: not told). Called once boot returned, never from
  // inside the seam: the device files the resumed answer or the notice where the chat's history
  // rows go and runs the resumed turn's deferred effects.
  std::string deviceName;
  std::function<void(const ResumeReport&)> resumed;
  // Lane E2-SRCH: a call the turn itself answers (search and view read the turn's own conversation,
  // plain/history.h HistoryTools::answer); true when it answered. Null: every call goes to the registry.
  std::function<bool(const ToolCall&, const TurnCtx&, ToolOutput&)> turnTool;
  // Lane E2-P-HOST1 (the hosted daemon): what a host adds to each turn's setup after the binding's own
  // fields (the head's effort, a test rig's body cap); null: nothing. Every progress event of a turn, before
  // the progress line's own rule (the Telegram typing keep-alive); null: none. The device sets neither.
  std::function<void(const TurnCtx&, TurnSetup&)> moreSetup;
  std::function<void(const ProgressEvent&)> onProgress;
  // Lane E2-FIX14: engine 1's money gate after a turn. True: the shared provider park
  // (nimbus::orch::providerPark) learns what the head answered (plain/head_gate.h noteHeadOutcome).
  bool parkHead = false;
  // The provider's own monthly limit is reached (store::providerOverBudget). Null: none. Read with parkHead
  // before the crash resume's one request (lane E2-FIX14 after E2-FIX12).
  std::function<bool(const std::string& provider)> overBudget;
  // Lane E2-P-FO: the heads a failed round may move to, in the owner's fallback order (fallbackSlots);
  // the device leaves it null with midTurnFailover off. Null: no fail-over.
  std::function<void(const FailoverAsk& ask, std::vector<HeadSlot>& out)> fallbacks;
  // The owner chose `provider` for vision: a fallback head there sees photos as pixels. Null: none did.
  std::function<bool(const std::string& provider)> visionChosen;
  // Lane E2-FIX14: what engine 1's deliver() does to a model's reply before it leaves
  // (src/agent/orchestrator_common.cpp), each the shared code engine 1 calls. replyFilter: the owner's
  // output rules for this chat (prefs::filtersForRole, orch::applyOutputFilters: the no-dash rule);
  // null: none. replyScreen: the outbound screen (moderateGate, ModGate::OutboundReply); false: it
  // blocks this reply and nothing of it is sent, as on engine 1; null: none. Both run once on the
  // whole reply, before it is split into parts; the same runs on a routine's note to the chat.
  std::function<std::string(const std::string& chatId, const std::string& text)> replyFilter;
  std::function<bool(const std::string& chatId, const std::string& text)> replyScreen;
  // Lane E2-P-CONN2: HeadEndpoint::connectors and ::bearer of the turn's head (the owner's connectors
  // and the Code sandbox, as engine 1's attach reads them). Null: none ride.
  std::function<std::vector<nimbus::orch::ConnectorInfo>()> connectors;
  std::function<std::string(const nimbus::orch::ConnectorInfo&)> bearer;
};

// The owner's line on every turn while the stored conversation could not be loaded (lane
// E2-STORE, finding S5). Owner-facing only: sent to the owner's chat after the turn, never
// stored and never shown to a model.
inline constexpr char kNotLoadedLine[] =
    "The stored conversation could not be loaded; what you say now is kept in memory only until the card is back.";
// Lane E2-FIX3 (finding R1, ruling 116): the owner's line on each turn while the history import holds
// the chat (owner-facing only, as kNotLoadedLine). Stuck: it failed the same way twice in a row.
inline constexpr char kImportStuckLine[] =
    "The earlier conversation could not be imported, so what you say now is kept in memory only. Send /clear "
    "to keep it in a new conversation; the earlier one stays stored on the device.";
inline constexpr char kImportPendingLine[] =
    "The earlier conversation is not imported yet; what you say now is kept in memory only until it is.";
// Another chat (or role) refused while this binding holds turns in RAM. Stuck import: /clear in the
// held chat gets out (E2-FIX3).
inline constexpr char kHeldElsewhereLine[] =
    "This chat cannot start while another conversation is held in memory because its earlier history could "
    "not be imported. Send /clear in that chat to store it and go on.";
// Lane E2-FIX5 (finding L2): the other held states. A restart would lose the held turns, so neither
// line advises one; each ends by itself (the card answers again, the import's next try succeeds).
inline constexpr char kHeldWaitingLine[] =
    "This chat cannot start yet: another conversation is kept in memory because the card is not storing it "
    "now. It is stored by itself when the card answers again. Send the message again later.";
inline constexpr char kHeldImportingLine[] =
    "This chat cannot start yet: another conversation is kept in memory until its earlier history is "
    "imported. Send the message again in a minute.";

// The tags of the person's day-stream row for a message (the web chat's history, the import's
// rows). "e2" only when an engine-2 turn stores the message itself (it is then engine 2's: never
// imported). A message answered with a device line instead (the history wait, a refusal, a chat
// that could not start) is left untagged, so the import brings it into the conversation once
// at the chat's next boot (lane E2-FIX2B, finding P10).
inline std::string ownerRowTags(const std::string& fromName, bool turnStoresIt) {
  std::string t = fromName.empty() ? std::string() : "from:" + fromName;
  if (turnStoresIt) t += t.empty() ? "e2" : ",e2";
  return t;
}

struct ChatBindingConfig {
  std::string chatKey;                           // the transcript key (/mem/tx/<chatKey>)
  std::string chatId;                            // the routing chat; "" = chatKey
  // Lane E2-R8a: the head on its provider slot (plain/head_slot.h; useSlot fills these). No default:
  // a binding with no provider or host does not boot.
  std::string host;                              // the head's endpoint host
  std::string provider;                          // its provider slot (budget, price, spend and park)
  std::string model;                             // the head model (cumulo: the router selector)
  Dialect dialect = Dialect::AnthropicMessages;  // the head's wire
  uint16_t port = 443;
  bool tls = true;
  std::string basePath;                          // "" = the slot's own
  bool routed = false;                           // through the Cumulo router
  bool toolsVerified = false;                    // a custom endpoint's verified tool call (HeadEndpoint's)
  std::string summarizerModel;                   // the compaction rung (the provider's small model)
  // The compaction ladder in order, each rung with its own endpoint and key (the released engine's
  // ladder across providers). Empty: one rung, the head's endpoint on summarizerModel.
  std::vector<SummarizerRung> summarizers;
  nimbus::orch::Role role = nimbus::orch::Role::Admin;
  std::string timeZone;
  uint16_t dayStartMin = kDayStartDefaultMin;
  int storedRounds = 0;                          // the stored orchLoopRnds
  uint64_t monthlyCeilingMicro = 0;              // the owner's monthly ceiling (0: none set)
  // The owner's pinned history profile (ruling 83: full unless the owner chose a smaller one).
  nimbus::orch::tx::Profile profile = nimbus::orch::tx::kDefaultProfile;
};

// The head `slot` into `cfg` (lane E2-R8a): the provider, model, wire and endpoint (its key is the
// binding's ChatBindingDeps::key).
inline void useSlot(const HeadSlot& slot, ChatBindingConfig& cfg) {
  cfg.provider = slot.provider;
  cfg.model = slot.model;
  cfg.dialect = slot.dialect;
  cfg.host = slot.endpoint.host;
  cfg.port = slot.endpoint.port;
  cfg.tls = slot.endpoint.tls;
  cfg.basePath = slot.endpoint.basePath;
  cfg.routed = slot.endpoint.routed;
  cfg.toolsVerified = slot.endpoint.toolsVerified;
}
// A summarizer rung on `slot` (its endpoint and key are the slot's).
inline SummarizerRung rungOf(const HeadSlot& slot) {
  SummarizerRung r;
  r.endpoint = slot.endpoint;
  r.host = slot.provider;   // the slot's name: caps, spend and the compaction lines
  r.model = slot.model;
  r.dialect = slot.dialect;
  return r;
}

// One message in.
struct ChatMessage {
  std::string text;
  std::string ch = "tg";                         // u.ch
  std::vector<uint64_t> src;                     // Telegram update ids
  int16_t tzMinutes = 0;
  uint64_t m = 0;                                // the day-stream row it mirrors
  std::vector<nimbus::orch::tx::Part> parts;     // its photos (Image parts "p<ref>")
  bool photosBlind = false;                      // TurnCtx::photosBlind: the head takes no images (E2-P-PHOTO)
  std::vector<std::string> stored;               // TurnInput::stored (lane E2-FIX10)
  // Lane E2-P-SUB: Owner for a person's message; AgentResult for a sub-agent's report turn (text is
  // its agent device note, outside content, so the rails gate its writes; plain/agents.h).
  Trigger trigger = Trigger::Owner;
  bool probe = false;                            // the park's re-check request (MessageGate::probe, E2-FIX14)
};

// Lane E2-P-FO: the heads a failed round may move to, as engine 1's fallbackOrder picks them
// (lib/harness/src/engine.cpp): the owner's rule set (the shipped size-class defaults over the priority
// when none is set) for the head's provider, model, size class and the error's class, up to three
// providers, each open (keyed, not parked, paused or over its budget); none by the rules, the priority
// walk. Each on its provider slot with the rule's model, else that provider's head model.
struct FallbackPlan {
  std::string rulesJson;                                           // "" = the shipped defaults
  std::vector<std::string> priority;                               // provider slugs, in order
  std::function<bool(const std::string& provider)> open;
  std::function<std::string(const std::string& provider)> model;   // the provider's head model
};
inline nimbus::orch::ErrorClass fallbackClassOf(ExchangeErr e) {
  using nimbus::orch::ErrorClass;
  switch (e) {
    case ExchangeErr::Transport: return ErrorClass::Network;
    case ExchangeErr::Server:
    case ExchangeErr::Malformed: return ErrorClass::ServerError;
    case ExchangeErr::RateLimited: return ErrorClass::RateLimited;
    case ExchangeErr::KeyRefused: return ErrorClass::Auth;
    case ExchangeErr::Cut: return ErrorClass::Timeout;
    case ExchangeErr::Other: return ErrorClass::BadRequest;
    default: return ErrorClass::None;
  }
}
// The rule set's providers for `ctx`, up to three, each `usable`; none by the rules: the priority walk.
inline std::vector<nimbus::orch::FallbackTarget> fallbackPicks(const FallbackPlan& plan, const nimbus::orch::TurnContext& ctx) {
  namespace orch = nimbus::orch;
  orch::FallbackRuleSet rs;
  if (!plan.rulesJson.empty()) orch::parseFallbackRules(plan.rulesJson, rs);
  if (rs.rules.empty()) rs = orch::defaultRuleSet(plan.priority);
  std::vector<orch::FallbackTarget> picks;
  auto usable = [&](const std::string& p, const std::string&) {
    if (p == ctx.provider || (plan.open && !plan.open(p))) return false;
    return std::none_of(picks.begin(), picks.end(), [&p](const orch::FallbackTarget& t) { return t.provider == p; });
  };
  for (int i = 0; i < 3; ++i) {
    const orch::FallbackChoice ch = orch::selectFallback(rs, ctx, usable);
    if (!ch.found) break;
    picks.push_back(ch.target);
  }
  if (picks.empty())   // never below the priority walk (engine 1's rawAlternates)
    for (const std::string& p : plan.priority)
      if (usable(p, "")) picks.push_back(orch::FallbackTarget{p, ""});
  return picks;
}
inline void fallbackSlots(const SlotSettings& set, const FallbackPlan& plan, const FailoverAsk& ask, std::vector<HeadSlot>& out) {
  namespace orch = nimbus::orch;
  orch::TurnContext ctx;
  ctx.provider = ask.provider;
  ctx.model = ask.model;
  ctx.sizeClass = orch::sizeClassWord(orch::modelSizeClass(ask.provider, ask.model));
  ctx.capability = "chat";
  ctx.errorClass = fallbackClassOf(ask.err);
  for (const orch::FallbackTarget& t : fallbackPicks(plan, ctx)) {
    HeadSlot h;
    const std::string model = t.model.empty() && plan.model ? plan.model(t.provider) : t.model;
    if (!model.empty() && headSlotFor(set, t.provider, model, h).ok()) out.push_back(std::move(h));
  }
}

// A turn's spend as the owner's usage view files it (store::recordProviderSpend): one slot of the
// head's provider and model, in the counts the ledger reads for that provider (ledgerUsage: engine 2
// reports the uncached input, a provider whose input holds its cache gets it added back; lane
// E2-R8a), one billed response per head request.
inline nimbus::orch::TurnSpend turnSpendOf(const TurnOutcome& out, const std::string& provider, const std::string& model) {
  nimbus::orch::TurnSpend s;
  if (!out.shares.empty()) {   // E2-P-FO: a switched turn, each head's requests booked to it
    for (size_t i = 0; i < out.shares.size(); ++i) {
      const HeadShare& h = out.shares[i];
      nimbus::orch::TokenUsage u;
      u.promptTokens = h.promptTokens;
      u.completionTokens = h.outputTokens;
      u.cacheReadTokens = h.cacheReadTokens;
      u.cacheWriteTokens = h.cacheWriteTokens;
      u.cacheWrite1hTokens = h.cacheWrite1hTokens;
      u.responses = h.requests;
      if (i + 1 == out.shares.size()) u.maxRequestIn = out.end.lastPromptTokens;
      s.add(h.provider.c_str(), h.model.c_str(), ledgerUsage(h.provider, h.model, u));
    }
    return s;
  }
  nimbus::orch::TokenUsage u;
  u.promptTokens = out.end.promptTokens;
  u.completionTokens = out.end.outputTokens;
  u.cacheReadTokens = out.end.cacheReadTokens;
  u.cacheWriteTokens = out.end.cacheWriteTokens;
  u.cacheWrite1hTokens = out.end.cacheWrite1hTokens;   // the ledger prices it at 2x (R3; lane E2-CACHE)
  u.responses = out.headRequests;
  u.servedModel = out.end.servedModel;   // E2-P-SURF: the row is the model that answered, not only the one asked
  u.maxRequestIn = out.end.lastPromptTokens;
  u = ledgerUsage(provider, model, u);
  s.add(provider.c_str(), model.c_str(), u);   // files nothing when nothing was billed
  return s;
}

class ChatBinding {
 public:
  // By reference: a caller that keeps both on the heap leaves no copy on its stack (E2-FIX2B).
  ChatBinding(const ChatBindingConfig& cfg, const ChatBindingDeps& d) : cfg_(cfg), d_(d) {
    if (cfg_.chatId.empty()) cfg_.chatId = cfg_.chatKey;
    who_ = nimbus::orch::principalForRole(cfg_.chatId, cfg_.role);
    channel_.send = [this](const std::string& chat, const std::string& part) {
      const bool ok = d_.sendPart && d_.sendPart(chat, part);
      ++(ok ? sent_ : refused_);
      return ok;
    };
    channel_.isAllowed = [this](const std::string& chat) { return chat == cfg_.chatId; };
    channel_.firstAllowedChat = [this] { return cfg_.chatId; };
  }
  ChatBinding(const ChatBinding&) = delete;
  ChatBinding& operator=(const ChatBinding&) = delete;

  const ChatBindingConfig& config() const { return cfg_; }
  PlainEngine* engine() { return engine_.get(); }
  const nimbus::orch::tx::TxArena* arena() const { return arena_.get(); }
  const HeadCost& cost() const { return cost_; }
  nimbus::orch::ChatBreaker& breaker() { return breaker_; }
  // Parts the channel took and refused in the last turn.
  uint32_t sentParts() const { return sent_; }
  uint32_t refusedParts() const { return refused_; }
  // The last boot's import of the earlier engine's conversation (E2-MIG): whether it ran, its
  // status and its outcome. A refused import does not stop the boot (the owner is answered); the
  // next boot runs it again, and the library resumes where it stopped.
  bool migrationRan() const { return migrationRan_; }
  const Status& migrationStatus() const { return migrationStatus_; }
  const nimbus::orch::tx::MigrationOutcome& migration() const { return migration_; }
  uint32_t migrationMs() const { return migrationMs_; }
  // Lane E2-FIX2B (finding P3, tx/migrate.h's contract): the import ran and has not answered Done,
  // so nothing is stored for this chat; it runs unloaded and the import is tried again before
  // each turn (reload). A refusal it can never get past (InvalidArgument) does not hold the chat.
  bool importPending() const { return importPending_; }
  // Lane E2-FIX3 (finding R1): the import failed the same way (error and place) on its last two
  // tries, so trying again before each turn is not likely to get past it (a marker over 8 KB, a
  // page that never decodes, a read the card always refuses). The tries go on (it ends by itself
  // when the cause is gone); the owner is told the import is the cause and that /clear gets out.
  // A card that does not answer is the card's cause (kNotLoadedLine), never the import's.
  bool importStuck() const { return importPending_ && movedTo_.empty() && cardAnswered_ && sameFailures_ >= 2; }
  // The owner's line for another chat or role refused while this binding holds turns in RAM: the
  // true cause, never a restart (E2-FIX5, L2).
  const char* elsewhereLine() const {
    if (importStuck()) return kHeldElsewhereLine;
    return importPending_ && movedTo_.empty() && cardAnswered_ ? kHeldImportingLine : kHeldWaitingLine;
  }
  // The key storeHeldAs is storing the held turns into ("" before it was asked).
  const std::string& movedTo() const { return movedTo_; }

  // Once: the head's budget and price, the arena, the log, the seams, the engine and its boot,
  // then the seen-index rebuild. A refused allocation is a status, never an abort.
  Status boot(bool afterCrash = false) {
    namespace tx = nimbus::orch::tx;
    if (engine_) return Status::fail(tx::Err::Busy, "plain::ChatBinding::boot.twice");
    if (!bound()) return Status::fail(tx::Err::InvalidArgument, "plain::ChatBinding::boot");
    const Platform* p = d_.platform;
    Status st = headFigures();
    if (!st.ok()) return st;
    try {
      std::unique_ptr<tx::TxArena> arena(new tx::TxArena());
      st = arena->init(arenaBytes_, p->allocLarge, p->freeLarge);
      if (!st.ok()) return st;
      arena_ = std::move(arena);
      log_.reset(new tx::TxLog(*d_.files, cfg_.chatKey));
      st = installSeams();
      if (!st.ok()) return fail(st);
      migrateOld();
      engine_.reset(new PlainEngine(deps()));
      resume_ = ResumeReport{};
      resumeRan_ = false;
      st = engine_->boot(BootCtx{p->nowEpoch(), afterCrash});
    } catch (const std::bad_alloc&) {
      st = Status::fail(tx::Err::TooLarge, "plain::ChatBinding::boot.memory");
    }
    if (!st.ok() && !onlyResumeFailed(st)) return fail(st);
    if (resumeRan_) afterResume();
    if (engine_->unloaded()) say("e2: the stored conversation could not be loaded: held in RAM, retried before each turn");
    rebuildSeen();
    return st;
  }

  // What the last boot's crash resume did (action None when it found nothing to do or did not run).
  const ResumeReport& lastResume() const { return resume_; }

  // The stored conversation could not be loaded and is not yet (the owner's line is due).
  bool unloaded() const { return engine_ && engine_->unloaded(); }
  // While unloaded: open the log again and store the RAM turns after the conversation
  // (PlainEngine::reopen), then rebuild the seen index. True when the chat is loaded after it.
  bool reload() {
    if (!engine_ || moved_) return !!engine_;
    if (!movedTo_.empty()) return storeHeldAs(movedTo_).ok();   // E2-FIX3: only into the new conversation
    if (!engine_->unloaded()) return true;
    if (importPending_) {   // P3: the import first; the RAM turns follow what it stored, once, in order
      migrateOld();
      if (importPending_) {
        say("e2: the earlier engine's conversation is still not imported: this turn is held in RAM");
        return false;
      }
      say("e2: the earlier engine's conversation is imported now");
    }
    const Status r = engine_->reopen();
    if (engine_->unloaded()) {
      if (!r.ok()) say("e2: the stored conversation still cannot be loaded: this turn is held in RAM");
      return false;
    }
    say("e2: the stored conversation is loaded again; the turns held in RAM are stored after it");
    rebuildSeen();
    return true;
  }

  // Lane E2-FIX3 (finding R1): /clear on a chat the history import holds. The turns held in RAM are
  // stored, in order, each once, as the start of the transcript `key` (the new conversation), and
  // nothing is written under this chat's key: its earlier history stays on the card as it is, for a
  // later import. Success: this binding is done (moved), the caller boots one on `key`. Not ok:
  // still held, and every later reload() stores into `key` only (resuming after what landed).
  Status storeHeldAs(const std::string& key) {
    namespace tx = nimbus::orch::tx;
    if (moved_ || !engine_ || !engine_->unloaded() || (!importPending_ && movedTo_.empty()) ||
        (!movedTo_.empty() && key != movedTo_))
      return Status::fail(tx::Err::InvalidArgument, "plain::ChatBinding::storeHeldAs");
    std::unique_ptr<tx::TxLog> log(new (std::nothrow) tx::TxLog(*d_.files, key));
    if (!log) return Status::fail(tx::Err::TooLarge, "plain::ChatBinding::storeHeldAs.memory");
    movedTo_ = key;
    importPending_ = false;   // the import of this key is left to a later boot; nothing more runs here
    const Status st = engine_->reopenAs(*log, key);
    if (!st.ok()) {
      say("e2: the turns held in RAM are not stored in the new conversation yet: tried again before the next turn");
      return st;
    }
    log->close();
    moved_ = true;
    say("e2: the turns held in RAM are stored in the new conversation");
    return st;
  }
  // storeHeldAs succeeded: nothing more runs on this binding.
  bool moved() const { return moved_; }

  // One message through the loop: a person's, or a sub-agent's report (msg.trigger AgentResult,
  // lane E2-P-SUB). The breaker admits every person's message (it holds back only turns no person
  // started: a report it holds is Busy with nothing stored); the spend is recorded after.
  Status turn(const ChatMessage& msg, TurnOutcome& out) {
    namespace tx = nimbus::orch::tx;
    if (!engine_ || moved_) return Status::fail(tx::Err::InvalidArgument, "plain::ChatBinding::turn.before-boot");
    if (busy_) return Status::fail(tx::Err::Busy, "plain::ChatBinding::turn.busy");
    const bool person = msg.trigger != Trigger::AgentResult;
    if (reportedBefore(msg)) return Status::success();   // E2-REP6: its turn ran before a restart
    const uint32_t now = d_.platform->nowMs();
    if (!breaker_.admit(person, now).run && !person) return Status::fail(tx::Err::Busy, "plain::ChatBinding::turn.breaker");
    busy_ = true;
    struct Idle {
      bool& busy;
      ~Idle() { busy = false; }
    } idle{busy_};
    sent_ = refused_ = 0;
    shown_.clear();
    withheld_ = false;
    if (reporter_) reporter_->open(turnCtx(), now);
    TurnInput in;
    in.ctx = turnCtx();
    in.ctx.trigger = msg.trigger;
    in.text = msg.text;
    in.ch = msg.ch;
    in.src = msg.src;
    in.tzMinutes = msg.tzMinutes;
    in.m = msg.m;
    in.parts = msg.parts;
    in.stored = msg.stored;
    in.ctx.photosBlind = msg.photosBlind;
    // Lane E2-STORE: a chat whose stored conversation could not be loaded tries again before
    // the turn, so the turn sees the whole conversation as soon as the card answers.
    if (movedTo_.empty()) reload();   // E2-FIX3: held for the new conversation, the caller stores it first
    Status st;
    try {
      st = engine_->runTurn(in, out);
    } catch (const std::bad_alloc&) {
      st = Status::fail(tx::Err::TooLarge, "plain::ChatBinding::turn.memory");
    }
    if (engine_->unloaded() && d_.sendPart) (void)d_.sendPart(cfg_.chatId, unloadedLine());
    noteHead(msg, out);   // E2-FIX14: the shared park learns what the head answered
    breaker_.spent(person, out.end.microUsd, d_.platform->nowMs());
    if (reporter_) reporter_->close(d_.platform->nowMs());
    if (d_.usage) d_.usage(out);
    return st.err == tx::Err::Busy ? Status::fail(tx::Err::Io, st.where) : st;   // Busy: nothing was stored
  }

  // A compaction of this chat now (/compact): the driver's manual run, stepped here until it has
  // answered (the driver tells the chat its one line through sendPart). Busy while one is queued.
  Status compactNow() {
    namespace tx = nimbus::orch::tx;
    if (!engine_ || !driver_) return Status::fail(tx::Err::InvalidArgument, "plain::ChatBinding::compactNow");
    if (busy_ || !driver_->requestManual()) return Status::fail(tx::Err::Busy, "plain::ChatBinding::compactNow");
    Status st;
    for (uint32_t i = 0; i < kCompactManualRequests + 2 && driver_->manualPending(); ++i) {
      IdleCtx ctx;
      ctx.now = d_.platform->nowEpoch();
      ctx.nowMs = d_.platform->nowMs();
      ctx.clockSet = ctx.now != 0;
      IdleOutcome o;
      try {
        st = engine_->idle(ctx, o);
      } catch (const std::bad_alloc&) {
        st = Status::fail(tx::Err::TooLarge, "plain::ChatBinding::compactNow.memory");
      }
      if (!st.ok()) break;
    }
    return st;
  }

  // Lane E2-P-HOST1: a compaction queued for the host's idle step (engine()->idle runs it); false while
  // one is queued or before boot. compactNow() is the device's form, stepped at once.
  bool requestCompaction() { return engine_ && driver_ && driver_->requestManual(); }

  // What /budget shows (rulings 105 and 106): the fixed part as this chat composes it now, the
  // summary and the transcript after it, the marks and the settings, in tokens.
  Status budgetView(BudgetView& out) {
    namespace tx = nimbus::orch::tx;
    if (!engine_ || !engine_->head() || !engine_->digests())
      return Status::fail(tx::Err::InvalidArgument, "plain::ChatBinding::budgetView");
    tx::HeadBudget b;
    Status st = tx::budgetFor(headKey(), cfg_.profile, b);
    if (!st.ok()) return st;
    const TurnCtx t = turnCtx();
    TurnSetup ts;
    if (!(st = setup(t, ts)).ok()) return st;
    ts.facts.turn = &t;
    SystemBlocks sys;
    std::vector<ToolSpec> tools;
    if (!(st = composeSystem(ts.facts, sys)).ok() || !(st = toolManifest(ts.facts, tools)).ok()) return st;
    BudgetViewIn in;
    in.budget = &b;
    in.system = &sys;
    in.tools = &tools;
    in.live = engine_->digests();
    in.arena = arena_.get();
    in.thru = engine_->head()->thru;
    return plain::budgetView(in, out);
  }

  // Lane E2-SCHED: this chat as the sink of a reminder or routine fire (plain::deliverHeld): its
  // channel's delivery and a routine device note appended to its transcript between its turns.
  OwnerSink ownerSink() {
    OwnerSink s;
    s.chatKey = cfg_.chatKey;
    s.chatId = cfg_.chatId;
    s.deliver = [this](const DeliverInput& in, DeliverOutcome& out) {
      const Status st = deliverReply(in, out);
      // E2-FIX14: a note the outbound screen withheld is done with, as engine 1 drops it once; never
      // retried (a retry would ask the screen again and could send it on a classifier outage).
      if (withheld_ && in.text && !in.text->empty()) out.sent = out.replyDone = true;
      return st;
    };
    s.append = [this](const std::string& note, uint64_t at) {
      if (!engine_ || busy_) return Status::fail(nimbus::orch::tx::Err::Busy, "plain::ChatBinding::ownerSink.append");
      return appendOwnerNote(*engine_, note, at);
    };
    return s;
  }

  // Lane E2-SCHED: one routine fire over this chat's parts (plain::runRoutine): the platform, the
  // transport, the card, the shared registry (runRoutine cuts it to the routine's tools), this
  // chat's dispatch and setup (pinned notes, the money guard's cost), the suppression seam, and
  // the head `model` on this chat's provider ("" = this chat's head). The caller sets held,
  // quiet, tzMinutes and head.tainted.
  RoutineDeps routineDeps(const std::string& model) {
    RoutineDeps r;
    r.platform = d_.platform;
    r.transport = d_.transport;
    r.files = d_.files;
    r.reg = d_.registry;
    r.dispatch = [this](const ToolCall& c, const TurnCtx& t) { return dispatchCall(c, t); };
    r.setup = [this](const TurnCtx& t, TurnSetup& out) { return setup(t, out); };
    r.head = turnCtx();
    r.head.trigger = Trigger::Routine;
    if (!model.empty()) {
      r.head.model = model;
      r.head.dialect = slotDialect(cfg_.provider, model, cfg_.dialect);   // E2-R8a: a selector's own upstream
    }
    r.moreHooks = [](PlainHooks& h) { (void)installSuppression(h); };
    r.owner = ownerSink();
    return r;
  }

  // The reply as it left in the last turn, after the owner's output rules ("" when no reply went
  // out, or when no rule is bound: the model's text is what left). E2-FIX14: the chat's history row.
  const std::string& shownReply() const { return shown_; }
  // The outbound screen withheld the last turn's reply (nothing of it was sent).
  bool replyWithheld() const { return withheld_; }

  // The head of a message: true when every update id of `src` was already taken into this chat
  // (a re-served update after a restart: acked, never answered again).
  bool allSeen(const std::vector<uint64_t>& src) const {
    if (!d_.seen || !d_.seen->ready() || src.empty()) return false;
    for (uint64_t id : src)
      if (!d_.seen->seen(cfg_.chatId, id)) return false;
    return true;
  }

  // E2-REP6 (review F4): the job tag a report turn's note names on its first line
  // ("[agent report] <name> id=<tag>...", agentReportNote); "" for any other text.
  static std::string reportTagOf(const std::string& note) {
    namespace dn = texts::stubs;
    if (note.rfind(dn::kAgentReport, 0) != 0) return std::string();
    const std::string head = note.substr(0, note.find('\n'));
    const size_t at = head.rfind(dn::kAgentId);
    if (at == std::string::npos) return std::string();
    const size_t from = at + std::char_traits<char>::length(dn::kAgentId);
    return head.substr(from, head.find(' ', from) - from);
  }

  // A report turn whose note the conversation holds already (logged): it is not run again.
  bool reportedBefore(const ChatMessage& msg) const {
    const std::string tag = msg.trigger == Trigger::AgentResult ? reportTagOf(msg.text) : std::string();
    if (!holdsReport(tag)) return false;
    agent::hlog::logf("plain(%s): the report of %s is in the conversation already: not run again", cfg_.chatId.c_str(),
                      tag.c_str());
    return true;
  }

  // Does the conversation in RAM hold the agent note of job `tag` already? A report turn whose answer
  // was stored before a restart, and whose job was not yet marked reported, is not run a second time
  // (the boot's resume sent its stored answer).
  bool holdsReport(const std::string& tag) const {
    namespace tx = nimbus::orch::tx;
    if (!arena_ || tag.empty()) return false;
    tx::Item it;
    for (size_t i = arena_->count(); i-- > 0;) {
      tx::ArenaEntry e;
      if (!arena_->header(i, e).ok() || e.kind != tx::ItemKind::DeviceNote || !arena_->get(i, it).ok()) continue;
      if (it.note == tx::NoteClass::Agent && reportTagOf(it.x) == tag) return true;
    }
    return false;
  }

  // Lane E2-FIX13: does the conversation in RAM hold this message as a person's item already (its
  // day-stream row `m`, or one of its update ids)? A message kept during the read-in and answered
  // before a restart is found here, so it is not answered a second time.
  bool holdsMessage(const std::vector<uint64_t>& src, uint64_t m) const {
    namespace tx = nimbus::orch::tx;
    if (!arena_) return false;
    tx::Item it;
    for (size_t i = arena_->count(); i-- > 0;) {
      tx::ArenaEntry e;
      if (!arena_->header(i, e).ok() || e.kind != tx::ItemKind::User || !arena_->get(i, it).ok()) continue;
      if (m && it.m == m) return true;
      for (uint64_t a : it.src)
        for (uint64_t b : src)
          if (a && a == b) return true;
    }
    return false;
  }

 private:
  void say(const char* line) const {
    if (d_.log) d_.log(std::string(line));
  }
  // The owner's line for an unloaded turn, naming the true cause (E2-FIX3, ruling 116).
  const char* unloadedLine() const {
    if (importStuck()) return kImportStuckLine;
    return importPending_ && cardAnswered_ ? kImportPendingLine : kNotLoadedLine;
  }
  // Every binding boot needs is there.
  bool bound() const {
    const Platform* p = d_.platform;
    const bool clock = p && p->nowMs && p->delayMs && p->nowEpoch;
    const bool memory = p && p->allocLarge && p->freeLarge;
    return clock && memory && d_.transport && d_.files && d_.registry && !cfg_.model.empty() && !cfg_.provider.empty() &&
           !cfg_.host.empty();
  }
  // E2-MIG: tx::migrate for this chat before the engine opens its log. Digest 0 is asked of the
  // compaction rung (the summarizer) on the same transport.
  void migrateOld() {
    namespace tx = nimbus::orch::tx;
    const Status before = migrationStatus_;
    migrationRan_ = false;
    importPending_ = false;
    migrationStatus_ = Status::success();
    migration_ = tx::MigrationOutcome{};
    // Only the chat's own key (generation 0) takes the earlier engine's history: a conversation
    // that /clear started (<chat>_c<n>, E2-CHAT) begins empty, and the import never fills it.
    if (!d_.oldEngine || cfg_.chatKey != cfg_.chatId) return;
    // On the heap: the import runs deep on the device's poll task (the tg_poll stack gate, E2-FIX2B).
    std::unique_ptr<tx::MigrateDeps> mp(new (std::nothrow) tx::MigrateDeps());
    if (!mp) {
      migrationRan_ = true;
      migrationStatus_ = Status::fail(tx::Err::TooLarge, "plain::ChatBinding::migrateOld.memory");
      importPending_ = !(d_.noCard && d_.noCard());
      return;
    }
    tx::MigrateDeps& m = *mp;
    m.files = d_.files;
    m.chatKey = cfg_.chatKey;
    m.head = headKey();
    m.arena = arena_.get();
    m.ramFollows = true;   // RAM turns are stored after the import (PlainEngine::reopen renumbers them)
    m.now = d_.platform->nowEpoch();
    m.dayStartMin = cfg_.dayStartMin;
    m.digest0 = digest0Asker(compactDeps());
    m.bound = boundTools();
    if (!d_.oldEngine(m)) return;
    migrationRan_ = true;
    const uint32_t t0 = d_.platform->nowMs();
    migrationStatus_ = tx::migrate(m, migration_);
    migrationMs_ = d_.platform->nowMs() - t0;
    if (migrationStatus_.ok()) {
      sameFailures_ = 0;
      return;
    }
    say("e2: the earlier engine's conversation was not imported this time");
    const bool noCard = d_.noCard && d_.noCard();   // no card at all: RAM by design, nothing to hold for
    importPending_ = !noCard && migrationStatus_.err != tx::Err::InvalidArgument;
    const bool same = !before.ok() && before.err == migrationStatus_.err &&
                      std::string(before.where ? before.where : "") == (migrationStatus_.where ? migrationStatus_.where : "");
    sameFailures_ = same ? sameFailures_ + 1 : 1;
    std::vector<std::string> names;
    cardAnswered_ = d_.files->list(tx::kTxRoot, names);   // the card answers: the cause is the import
    if (importStuck()) say("e2: the earlier engine's conversation fails to import the same way: the owner is offered /clear");
  }
  // RB-TG1: the chat's taken updates from the filled arena and head.json's high water.
  void rebuildSeen() {
    if (!d_.seen || !d_.seen->ready() || !engine_->head()) return;
    if (!nimbus::orch::rebuildSeen(*d_.seen, cfg_.chatId, *engine_->head(), *arena_).ok())
      say("e2: seen-index rebuild refused");
  }
  // The hooks and every seam the binding installs on them.
  Status installSeams() {
    hooks_.reset(new PlainHooks());
    (void)installSuppression(*hooks_);
    RailsDeps rails;
    rails.pending = d_.railPending;
    rails.noted = d_.railNoted;
    Status st = installRails(*hooks_, rails);
    if (st.ok()) {
      driver_.reset(new CompactDriver(compactDeps()));
      st = installCompaction(*hooks_, *driver_);
    }
    if (st.ok() && d_.progress.send) {
      reporter_.reset(new ProgressReporter(d_.progress));
      st = installProgress(*hooks_, *reporter_, d_.platform->nowMs);   // a copy: no lambda of the same signature wrapping it
    }
    if (st.ok()) st = installResume(*hooks_, resumeDeps());   // E2-FIX12: the device binding never installed it
    return st;
  }
  // E2-FIX12: the crash resume runs after the chat is loaded; its own failure (a send the channel
  // refused, a resumed run that failed) leaves the turn open for a later boot and never keeps the
  // chat from starting.
  bool onlyResumeFailed(const Status& st) const {
    return resumeRan_ && engine_ && st.err == resume_.status.err && st.where == resume_.status.where;
  }
  // Lane E2-FIX12: the seam's deps, read when it runs (the engine exists by then: boot runs it).
  ResumeDeps resumeDeps() {
    ResumeDeps r;
    r.log = log_.get();
    r.arena = arena_.get();
    r.head = [this] { return engine_ ? engine_->head() : nullptr; };
    r.runTurn = [this](const TurnInput& in, TurnOutcome& out) { return resumeTurn(in, out); };
    // E2-FIX14: what the resume sends passes the owner's output rules and the outbound screen.
    r.deliver = [this](const DeliverInput& in, DeliverOutcome& out) {
      const Status st = deliverReply(in, out);
      if (withheld_ && in.text && !in.text->empty()) out.sent = out.replyDone = true;   // dropped once, as engine 1
      return st;
    };
    r.context = [this](TurnCtx& out) {
      out = turnCtx();
      return Status::success();
    };
    r.now = d_.platform->nowEpoch;   // a copy: no lambda of the same signature wrapping it (the tg_poll stack gate)
    r.deviceName = d_.deviceName;
    r.done = [this](const ResumeReport& rep) {
      resume_ = rep;
      resumeRan_ = true;
    };
    return r;
  }
  // Lane E2-FIX14: after a turn, the shared park learns what the head answered (parkHead only).
  void noteHead(const ChatMessage& msg, const TurnOutcome& out) {
    if (d_.parkHead)
      noteHeadOutcome(cfg_.provider, d_.key ? d_.key() : std::string(), out, msg.probe, /*told=*/true, d_.platform->nowMs());
  }
  // Lane E2-FIX14: the resume's one request passes engine 1's money gates first. Over the limit, parked or
  // paused, nothing is sent: the resume fails as a refused run does, the turn stays open for a later boot and
  // the device keeps its crash notice. A resume never takes the park's re-check. After it, the park learns
  // what the head answered, as after any turn.
  Status resumeTurn(const TurnInput& in, TurnOutcome& out) {
    namespace tx = nimbus::orch::tx;
    if (!engine_) return Status::fail(tx::Err::InvalidArgument, "plain::ChatBinding::resume");
    const std::string key = d_.key ? d_.key() : std::string();
    std::string why;
    if (d_.parkHead && headHeld(cfg_.provider, key, d_.overBudget && d_.overBudget(cfg_.provider), d_.platform->nowMs(), why)) {
      say(("e2: boot resume held, no request: " + why).c_str());
      return Status::fail(tx::Err::Busy, "plain::ChatBinding::resume.held");
    }
    const Status st = engine_->runTurn(in, out);
    if (d_.parkHead) noteHeadOutcome(cfg_.provider, key, out, /*probe=*/false, /*told=*/true, d_.platform->nowMs());
    return st;
  }
  // After boot: one log line when the seam acted; a resumed turn's spend as any turn's; the device told.
  void afterResume() {
    namespace tx = nimbus::orch::tx;
    namespace intents = nimbus::orch::intents;
    if (resume_.action == intents::BootAction::None && resume_.status.ok()) return;
    const std::string line = "e2: boot resume: action=" + std::string(intents::bootActionSlug(resume_.action)) +
                             " turn=" + std::to_string(resume_.turn) + " answered=" + std::to_string(resume_.answered) +
                             " counted=" + std::to_string(resume_.counted ? 1 : 0) +
                             " status=" + tx::errSlug(resume_.status.err) +
                             (resume_.status.where ? std::string("@") + resume_.status.where : std::string());
    say(line.c_str());
    if (resume_.action == intents::BootAction::Resume) {
      breaker_.spent(/*person=*/true, resume_.outcome.end.microUsd, d_.platform->nowMs());
      if (d_.usage) d_.usage(resume_.outcome);
    }
    if (d_.resumed) d_.resumed(resume_);
  }
  Status fail(const Status& st) {
    engine_.reset();   // a failed boot leaves the binding unbooted, never half-booted
    return st;
  }

  // Does `block` reach the request whole as the last lines of the pinned notes in `facts`?
  static bool keptWhole(const RequestFacts& facts, const std::string& block) {
    SystemBlocks sys;
    if (!composeSystem(facts, sys).ok()) return false;
    const std::string tail = "\n" + block;
    return sys.notes.size() >= tail.size() && sys.notes.compare(sys.notes.size() - tail.size(), tail.size(), tail) == 0;
  }

  // The skills index and the context skills: each context skill rides the pinned block whole,
  // after the notes, or is listed in the index and named through skillRefused. Measured against
  // the index with every context skill listed, so a later one cannot squeeze an earlier one out.
  void skillFacts(const TurnCtx& t, RequestFacts& facts) {
    if (!d_.skills) return;
    const ChatSkills s = d_.skills(who_);
    std::string worst = s.index;
    for (const ContextSkill& c : s.context) worst += (worst.empty() ? "" : "\n") + c.indexLine;
    RequestFacts probe = facts;
    probe.turn = &t;
    probe.skillsIndex = worst;
    std::string index = s.index;
    for (const ContextSkill& c : s.context) {
      const std::string before = probe.pinnedNotes;
      probe.pinnedNotes = before.empty() ? c.block : before + "\n" + c.block;
      if (keptWhole(probe, c.block)) continue;
      probe.pinnedNotes = before;
      index += (index.empty() ? "" : "\n") + c.indexLine;
      if (d_.skillRefused) d_.skillRefused(c.id, c.block.size());
    }
    facts.pinnedNotes = probe.pinnedNotes;
    facts.skillsIndex = index;
  }

  // The head's marks, arena size and guard cost. No price row: refused (money fails closed).
  Status headFigures() {
    namespace tx = nimbus::orch::tx;
    tx::HeadBudget b;
    Status st = tx::budgetFor(headKey(), cfg_.profile, b);
    if (!st.ok()) return st;
    arenaBytes_ = tx::arenaBytesFor(b);
    HeadRates r;   // the ledger's own rows, the long-prompt tier included (E2-FIX6)
    if (!slotRatesFor(cfg_.provider, cfg_.model, r)) return Status::fail(tx::Err::NotFound, "plain::ChatBinding::boot.price");
    cost_ = guardCostFor(b, r);
    breaker_.setSpendCap(nimbus::orch::spendRateCapMicro(
        cfg_.monthlyCeilingMicro, nimbus::orch::blendedTurnMicro(cost_.warmRequestMicroUsd, cost_.coldRequestMicroUsd)));
    return Status::success();
  }

  nimbus::orch::tx::HeadKey headKey() const { return nimbus::orch::tx::HeadKey{cfg_.provider, cfg_.model, cfg_.routed}; }
  // TurnCtx::host: a direct slot's API host, else the slot's name (HeadSlot::turnHost).
  std::string turnHost() const {
    return cfg_.provider == "custom" || cfg_.provider == "cumulo" ? cfg_.provider : cfg_.host;
  }

  TurnCtx turnCtx() const {
    TurnCtx c;
    c.chatKey = cfg_.chatKey;
    c.chatId = cfg_.chatId;
    c.turn = 0;   // the engine numbers the turn
    c.trigger = Trigger::Owner;
    c.host = turnHost();
    c.model = cfg_.model;
    c.dialect = cfg_.dialect;
    c.dayStartMin = cfg_.dayStartMin;
    c.profile = cfg_.profile;
    c.who = &who_;
    return c;
  }

  HeadEndpoint endpoint() const {
    HeadEndpoint e;
    e.host = cfg_.host;
    e.port = cfg_.port;
    e.tls = cfg_.tls;
    e.basePath = cfg_.basePath;
    e.routed = cfg_.routed;
    e.toolsVerified = cfg_.toolsVerified;
    e.key = d_.key ? d_.key : [] { return std::string(); };
    return e;
  }

  Status setup(const TurnCtx& t, TurnSetup& out) {
    out.facts.reg = d_.registry;
    out.facts.admin = who_.owner;
    out.facts.timeZone = cfg_.timeZone;
    if (d_.pinnedNotes) out.facts.pinnedNotes = d_.pinnedNotes(who_);
    if (d_.toolServersWaiting) out.facts.toolServersWaiting = d_.toolServersWaiting();
    skillFacts(t, out.facts);
    out.photo = d_.photo;
    out.dayStartMin = cfg_.dayStartMin;
    out.head = endpoint();
    out.head.effort = headEffortFor(cfg_.provider, t.model);   // E2-FIX14: as nimbusd sends for the same model
    out.head.connectors = d_.connectors;   // lane E2-P-CONN2: the turn's requests only, never the summarizer's
    out.head.bearer = d_.bearer;
    out.wire = nullptr;   // wireFor(the head's dialect)
    out.storedRounds = cfg_.storedRounds;
    out.cost = cost_;
    if (d_.moreSetup) d_.moreSetup(t, out);
    return Status::success();
  }

  // plain_rig.h's dispatch: the registry's answer as the model sees it. Arguments that are not a
  // JSON object reach the handler as an empty object.
  ToolOutput dispatchCall(const ToolCall& c, const TurnCtx& turn) {
    ToolOutput own;
    if (d_.turnTool && d_.turnTool(c, turn, own)) return own;   // E2-SRCH: search, view
    JsonDocument args;
    (void)deserializeJson(args, c.argsJson, DeserializationOption::NestingLimit(64));
    // A direct call spelled with '_' (mcp_x_y) reaches the registry's dotted name, as one through `more` does (E2-REP2).
    const std::string regName = d_.registry->resolveName(c.name);
    const std::string& name = regName.empty() ? c.name : regName;
    const nimbus::orch::ToolResult r = d_.registry->dispatch(name, args.as<JsonObjectConst>(), turn.who ? *turn.who : who_);
    ToolOutput o;
    o.id = c.id;
    o.name = c.name;
    o.isError = !r.success;
    o.text = r.success ? r.output : r.error;
    o.outside = d_.outside && d_.outside(name);
    if (r.success) (void)deferredFromCode(r.code, o.deferred, o.deferredArg);   // E2-P-TOOLS: restart, head switch
    return o;
  }

  // E2-FIX5: the tools a stub, an omission line or an import note may name (registered now).
  nimbus::orch::tx::BoundTools boundTools() const { return plain::boundTools(d_.registry); }
  CompactDeps compactDeps() {
    CompactDeps c;
    c.platform = d_.platform;
    c.transport = d_.transport;
    c.log = log_.get();
    c.arena = arena_.get();
    c.files = d_.files;
    c.chatKey = cfg_.chatKey;
    c.chatId = cfg_.chatId;
    c.head = [this] { return engine_ ? engine_->head() : nullptr; };
    c.digests = [this] { return engine_ ? engine_->digests() : nullptr; };
    c.omittedThru = [this] { return engine_ ? engine_->omittedThru() : uint64_t(0); };
    c.rungs = [this](std::vector<SummarizerRung>& out) {
      out = cfg_.summarizers;
      if (!out.empty()) return;
      SummarizerRung r;
      r.endpoint = endpoint();
      r.host = cfg_.provider;
      r.model = cfg_.summarizerModel.empty() ? cfg_.model : cfg_.summarizerModel;
      r.dialect = slotDialect(cfg_.provider, r.model, cfg_.dialect);
      out.push_back(r);
    };
    c.jobRunning = [](uint32_t) { return false; };   // no agent jobs on engine 2 yet
    c.park = [this](const SummarizerRung&) { say("e2: compaction rung refused its key or credit"); };
    c.spent = [this](const SummarizerRung& r, const nimbus::orch::TokenUsage& u) {
      if (d_.compactUsage) d_.compactUsage(r.host, r.model, ledgerUsage(r.host, r.model, u));
    };
    c.tell = [this](const std::string& chat, const std::string& text) {
      if (d_.sendPart) (void)d_.sendPart(chat, text);
    };
    c.ctl = d_.compactCtl;
    c.headKey = headKey();
    c.dayStartMin = cfg_.dayStartMin;
    c.searchBound = boundTools().search;
    return c;
  }

  EngineDeps deps() {
    EngineDeps e;
    e.platform = d_.platform;
    e.transport = d_.transport;
    e.channel = &channel_;
    e.hooks = hooks_.get();
    e.log = log_.get();
    e.arena = arena_.get();
    e.files = d_.files;
    e.chatKey = cfg_.chatKey;
    e.dispatch = [this](const ToolCall& c, const TurnCtx& t) { return dispatchCall(c, t); };
    e.setup = [this](const TurnCtx& t, TurnSetup& out) { return setup(t, out); };
    if (d_.recentFiles) e.recentFiles = d_.recentFiles(who_);
    e.stablePrefix = true;   // lane E2-CACHE: a person's chat keeps its system part across switches
    if (d_.progress.send || d_.onProgress)
      e.progress = [this](const ProgressEvent& ev) {
        if (d_.onProgress) d_.onProgress(ev);
        if (reporter_) (void)reporter_->tick(ev);
      };
    e.noCard = d_.noCard;
    e.holdUnloaded = [this] { return importPending_; };
    if (d_.fallbacks)
      e.failover = [this](const FailoverAsk& ask, std::vector<FailoverTarget>& out) { failoverTargets(ask, out); };
    if (d_.replyFilter || d_.replyScreen)
      e.deliver = [this](const DeliverInput& in, DeliverOutcome& out) { return deliverReply(in, out); };
    return e;
  }

  // Lane E2-P-FO: the binding's fallback slots as heads the engine can move a round to, each with its own
  // key, effort and rates; a slot nothing prices is left out (money fails closed).
  void failoverTargets(const FailoverAsk& ask, std::vector<FailoverTarget>& out) {
    namespace tx = nimbus::orch::tx;
    std::vector<HeadSlot> slots;
    d_.fallbacks(ask, slots);
    for (HeadSlot& h : slots) {
      tx::HeadBudget b;
      HeadRates r;
      if (!tx::budgetFor(tx::HeadKey{h.provider, h.model, h.endpoint.routed}, cfg_.profile, b).ok() ||
          !slotRatesFor(h.provider, h.model, r))
        continue;
      FailoverTarget t;
      t.provider = h.provider;
      t.host = h.turnHost();
      t.model = h.model;
      t.dialect = h.dialect;
      t.head = std::move(h.endpoint);
      t.head.effort = headEffortFor(h.provider, h.model);
      t.cost = guardCostFor(b, r);
      t.seesPhotos = d_.visionChosen && d_.visionChosen(h.provider);
      out.push_back(std::move(t));
    }
  }

  // E2-FIX14: one delivery as engine 1's deliver() makes it: the owner's output rules on the model's
  // whole reply, then the outbound screen, once per reply (a resend after a refused allocation, from
  // a later part, keeps the first verdict), then the portable deliver function over the channel. A
  // withheld reply sends nothing of itself; the status line, the harness's own words, still goes.
  Status deliverReply(const DeliverInput& in, DeliverOutcome& out) {
    if (!d_.replyFilter && !d_.replyScreen) return deliverTurn(DeliverDeps{&channel_, nullptr, nullptr}, in, out);
    DeliverInput go = in;
    if (in.text && !in.text->empty() && !out.replyDone && in.turn) {
      if (in.fromPart == 0) {
        shown_ = d_.replyFilter ? d_.replyFilter(in.turn->chatId, *in.text) : *in.text;
        withheld_ = d_.replyScreen && !d_.replyScreen(in.turn->chatId, shown_);
        if (withheld_) say("e2: the outbound screen withheld a reply");
      }
      go.text = withheld_ ? nullptr : &shown_;
    }
    const Status st = deliverTurn(DeliverDeps{&channel_, nullptr, nullptr}, go, out);
    if (withheld_ || !out.sent) shown_.clear();
    return st;
  }

  ChatBindingConfig cfg_;
  ChatBindingDeps d_;
  nimbus::orch::Principal who_;
  Channel channel_;
  HeadCost cost_;
  size_t arenaBytes_ = 0;
  nimbus::orch::ChatBreaker breaker_;
  uint32_t sent_ = 0, refused_ = 0;
  std::string shown_;           // the reply as it left (E2-FIX14)
  bool withheld_ = false;       // the outbound screen withheld it
  bool busy_ = false;
  bool migrationRan_ = false;
  bool importPending_ = false;
  uint32_t sameFailures_ = 0;   // the import's failures in a row with the same error and place (E2-FIX3)
  bool cardAnswered_ = false;   // the card listed /mem/tx after the import's last failure
  std::string movedTo_;         // storeHeldAs's key: the held turns go there only
  bool moved_ = false;          // storeHeldAs succeeded: the binding is done
  uint32_t migrationMs_ = 0;
  Status migrationStatus_;
  nimbus::orch::tx::MigrationOutcome migration_;
  std::unique_ptr<nimbus::orch::tx::TxArena> arena_;
  std::unique_ptr<nimbus::orch::tx::TxLog> log_;
  std::unique_ptr<PlainHooks> hooks_;
  std::unique_ptr<CompactDriver> driver_;        // the hooks call it: it outlives no hook
  std::unique_ptr<ProgressReporter> reporter_;
  std::unique_ptr<PlainEngine> engine_;
  ResumeReport resume_;      // the last boot's crash resume (E2-FIX12)
  bool resumeRan_ = false;   // the seam ran in the last boot (the chat was loaded first)
};

}  // namespace plain
}  // namespace agent
