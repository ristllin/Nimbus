#pragma once
// claim_check - the engine's half of the claim check: one per-turn effect
// ledger, filled while the turn runs, checked twice.
//
//   1. In the head loop, as one of the final-answer checks (validateFinal):
//      the final orch_turn's own fields count as pending effects (its
//      mem_write items, spawn items, scratchpad, device actions), merged with
//      what the tools did; a reply claim nothing backs gets ONE correction
//      round, with the turn's real record in the text.
//   2. After apply, with the real outcomes (a refused memory write, a spawn the
//      queue refused, a scratchpad kept in RAM only): a claim still unbacked
//      gets one calm owner line per class. The delivered text is what the
//      conversation history keeps.
//
// Claims a provider-hosted tool could back (a connector's send, a hosted web
// search, a remote calendar change) run where the device cannot see them, so
// when such a tool really ran this turn (CUM-835: a head response listed a
// server-side tool run, or a sub-agent result's provider record names one),
// the classes it could back are not checked (precision first): a connector run
// stands for sent / changed, any run for checked and a website or app source -
// except a gap only this device could have closed (a Telegram voice message
// from its own bot: ClaimGap::deviceOnly). Everything the device does itself
// (memory, files, skills, the scratchpad, spawns, durability, figures from a
// source, unsourced sub-agent results) is always checked.
//
// RAM only, one turn long. Portable, host-tested (test_harness_claim_check).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/orch/claims.h"
#include "nimbus/orch/food_figures.h"
#include "nimbus/orch/hosted_runs.h"
#include "nimbus/orch/reply_guard.h"
#include "nimbus/orch/turn.h"
#include "nimbus/orch/turn_effects.h"

namespace agent {

// The effect class of a registered tool, for the ledger, from the shared tool
// table (orch::toolEffect). Unknown names (a connector's remote tools) read as
// a lookup: nothing the device can vouch for.
nimbus::orch::FxKind fxKindForTool(const std::string& regName);

// The tool names a result's provenance tag (" [ran: web_search x2, Notion x0]") shows
// as RUN (count above zero), never the idle x0 ones: an idle "Notion x0" must not
// satisfy "Source: Notion" (the review), but a tool that ran really was read.
std::string ranSourceNames(const std::string& provenance);

// The device-authored parts of a turn's input block, apart (CUM-824). The claim check
// never re-parses them out of the composed text: a sub-agent reply or a stored memory
// that contains a block's title must not pose as that block.
struct TurnSources {
  std::string owner;    // the owner's message ("" on a synthesis or scheduled turn)
  std::string fresh;    // the sub-agent results the turn read: provenance tags and text
  std::string memory;   // the [MEMORY RESULTS] body
  std::string ledger;   // the spawn ledger block (JobEngine::spawnLedgerText)
  // The owner's own rows from the conversation window (their messages and photos,
  // never the assistant's replies): a follow-up about the label or photo of the
  // previous message credits a source the owner really gave (the review).
  std::string histOwner;
  std::string rules;    // the owner's standing rules as this turn sees them (a daily target's source)
  std::string targets;  // the figures of the person's stored food targets
  bool webRead = false; // a result's provider record shows a web search or fetch
  nimbus::orch::HostedRuns subRuns;   // the tools those results' provider records name (CUM-835)
};

class ClaimCheck {
 public:
  // Start a turn: clears the ledger, the sources, the per-turn flags and this
  // thread's record of hosted tool runs (nimbus::orch::resetHostedRuns).
  void beginTurn();

  // The sources a figure may come from: the turn's input block (the owner's
  // text, captions, the conversation window, fresh results) and, separately,
  // the recalled memories (a source only for what the owner reported).
  // The input block also seeds this turn's cause evidence (L19-11): what the
  // records the model was given say about the owner's phone/app/connection,
  // damaged files, timeouts and the like, as orch::CauseEvidence bits.
  // CUM-824: the sources a reply may credit by name (a gauge, a label, an app, a
  // website) come from `parts`: the owner's message, the sub-agent results (one with web
  // runs names a website), the memory search results, a photo in the owner's message (it
  // may show any of them) and, for the owner's own devices, the recalled memories; never
  // from the rest of the block or the history window. The owner's numbers are kept
  // apart, for the correction; the spawn ledger is what a why-question is answered from.
  void setSources(const std::string& inputs, const std::string& recalled, const TurnSources& parts);
  // The same with only the owner's message (a caller with no results or ledger).
  void setSources(const std::string& inputs, const std::string& recalled,
                  const std::string& ownerText = std::string()) {
    TurnSources parts;
    parts.owner = ownerText;
    setSources(inputs, recalled, parts);
  }
  // CUM-824: everything the turn was given, for the quoted-record rule (a quote presented
  // as a memory, a message or a record must be words the turn saw): the composed prompt
  // (its history, standing rules, notes, recalled memories) and the input block. Both
  // outlive the turn's checks (engine-owned); null leaves the rule unchecked.
  void setQuoteContext(const std::string* instructions, const std::string* inputs) {
    instructions_ = instructions;
    inputs_ = inputs;
  }
  // What the device holds of today's food, read when a reply states food figures
  // (food_figures.h: targets need a source, the day total is the ledger's). Unset: only
  // the targets are checked.
  void setFoodFacts(std::function<nimbus::orch::FoodFacts()> facts) { foodFacts_ = std::move(facts); }
  // The owner's own earlier messages in this request: a daily target's source.
  void noteOwnerHistory(const std::string& ownerRows);
  // Lane MLQ (OIR-06): the earlier owner messages in this request that got no reply
  // (oldest first), on a turn with a real human message; scopeCheck reads them.
  void noteUnanswered(std::vector<std::string> messages);
  // A scheduled turn: `today` (YYYY-MM-DD) turns on the stale-status rule (food_figures.h).
  void setRoutineDay(const std::string& today) { routineDay_ = today; }
  // A memory about to be stored states a daily target nothing this turn holds: true, with the
  // sentence. Apply refuses it: a made-up figure must not become its own source.
  bool unsourcedFoodTarget(const std::string& text, std::string* excerpt) const;
  // A write to the food targets file with a figure nothing this turn holds: the refusal, or "".
  std::string targetWriteRefusal(const std::string& regName, const std::string& argsJson) const;
  // A write to the person's food ledger (food/food-log*.csv) of a labelled row: the
  // store-time label gate (food_label.h) - a row whose figures fail the label's own
  // arithmetic is refused and names the figure in doubt, and a brand or detail no
  // turn source holds is refused unless the row is marked [assumed]. The refusal, or "".
  std::string labelRowRefusal(const std::string& regName, const std::string& argsJson) const;
  // A head tool ran: its effect class (the shared tool table) and its result.
  // The result text joins the figure sources (bounded). `regName` (the registered
  // name) lets the label gate keep the raw text of this turn's image.* calls - the
  // head's own reads of the turn's photos, which the input block does not hold when
  // a photo rides the message as an image.
  void noteTool(nimbus::orch::FxKind kind, bool isError, const std::string& output,
                uint16_t flags = 0, const std::string& regName = std::string());
  // The arguments of the same call, right before noteTool: the words a memory or file
  // write saved join the quote rule's seen text when the write lands, so a save
  // confirmation quoting them is never corrected as an invented record (the review).
  void noteToolArgs(const std::string& regName, const std::string& argsJson);
  // The ledger flags a tool call's arguments carry (kFxPermanent for a memory
  // written or updated with permanent:true, kFxVoice for reply.telegram
  // voice:true, kFxHealth for a read of the device's own state, kFxLogRow for a
  // files.append into a .csv or .log file, kept only if its result counts a row, kFxMusic for
  // media.play, kept only if its result says the player is playing).
  static uint16_t toolCallFlags(const std::string& regName, const std::string& argsJson);
  // Sub-agent results this turn read: how many had a lookup behind them and how
  // many were live-data results with none.
  void noteFresh(int sourced, int unsourced);

  // This turn's cause evidence (L19-11): what the records the model was given
  // (its inputs, its tool results, its health reads) say about causes, as
  // orch::CauseEvidence bits. Apply reads it to refuse a mem_write that would
  // store an unbacked cause as a fact.
  uint32_t causeEvidence() const { return causeEvidence_; }

  nimbus::orch::TurnEffects& effects() { return fx_; }
  const nimbus::orch::TurnEffects& effects() const { return fx_; }

  // What stands behind a promise of later work as the turn starts (reply_guard.h).
  void setLaterWork(nimbus::orch::LaterWork behind) { laterWork_ = behind; }

  // The final-answer check (validateFinal): "" when the reply's claims are
  // backed by the ledger plus the turn's own fields, else the correction text.
  // `skipSpawned` leaves the spawn class to another check of the same chain.
  std::string finalCheck(const std::string& finalTurnJson, bool skipSpawned = false);
  bool corrected() const { return corrected_; }
  // Lane MLQ: the reply-scope check of the same chain (owner_voice.h): "" unless the
  // reply takes up an earlier owner message that got no reply and that this message does
  // not bring up; then the correction text.
  std::string scopeCheck(const std::string& finalTurnJson) const;

  // After apply: apply recorded the turn's real outcomes in effects() (each
  // memory write's result, each spawn the queue took or refused, the scratchpad
  // save, the device actions). Returns the owner tail ("" when every claim is
  // backed), one line per gap class; `skipSpawned` as above.
  std::string tailAfterApply(const std::string& reply, bool skipSpawned = false);

  // The sentence of `reply` promising later work with nothing behind it ("" = none): no
  // live sub-agent, and no sub-agent or routine in `fx`.
  std::string unbackedPromise(const std::string& reply, const nimbus::orch::TurnEffects& fx) const;

  // Pending effects a final turn carries in its own fields (exposed for tests).
  static void addPending(nimbus::orch::TurnEffects& fx, const nimbus::orch::Turn& t);
  // The effect an allowed device[] element has (apply records it); false for a
  // blocked or unknown one.
  static bool deviceActionEffect(const std::string& json, nimbus::orch::FxKind& out);

  // RAM counters for /api/state (process lifetime, never persisted).
  static uint32_t correctedCount();
  static uint32_t tailedCount();

 private:
  nimbus::orch::LaterWork laterWork_ = nimbus::orch::LaterWork::None;
  std::vector<nimbus::orch::ClaimGap> gapsFor(const std::string& reply,
                                              const nimbus::orch::TurnEffects& fx,
                                              bool skipSpawned) const;
  nimbus::orch::HostedRuns hostedRan() const;   // the head's server-side runs plus the subs'
  bool hostedMayBack(const nimbus::orch::ClaimGap& g) const;
  void recordGaps(const std::string& reply, std::vector<nimbus::orch::ClaimGap>& kept) const;
  std::string seenText(bool fixedToo) const;   // what a quote (fixedToo) or a reason may come from
  void noteToolText(const std::string& output);
  // Stash a raw image.* arg/result as one of this turn's label reads (bounded).
  void stashImageRead(const std::string& text);
  // A write tool landed: its saved words join the seen text and the record.
  void noteWriteLanded();
  // noteTool's landed-write merge and figure scan (split out for the complexity gate).
  void mergePendingCited();
  void noteToolNumbers(const std::string& output);
  // The turn's own write cited a target it did not store: the correction round's
  // text (finalCheck) and the owner-facing note (tailAfterApply) share the facts
  // read, so both take it by reference.
  std::string citedTargetCorrection(const std::string& said, nimbus::orch::FoodFacts& facts) const;
  std::string citedTargetNote(const std::string& reply, nimbus::orch::FoodFacts& facts) const;
  // tailAfterApply's log lines (split out for the complexity gate).
  void logTailChecks(const std::vector<nimbus::orch::ClaimGap>& gaps,
                     const std::vector<nimbus::orch::FoodGap>& food,
                     bool later, const std::string& tail) const;
  std::vector<nimbus::orch::FoodGap> foodGaps(const std::string& reply, nimbus::orch::FoodFacts& facts) const;
  // The correction text for a final turn: the claim fix plus the food/stale notes and
  // the later-promise line (split out of finalCheck for the complexity gate).
  std::string correctionFor(const std::vector<nimbus::orch::ClaimGap>& gaps,
                            const nimbus::orch::TurnEffects& pending,
                            const std::string& more, const std::string& later) const;

  // EFF: the normalized scan cache (claim_check.cpp ensureSeenCache) - the records
  // every check of one turn reads, normalized once instead of per check.
  struct SeenCache {
    bool ready = false;
    bool inputsHad = false;      // an inputs block was set (its words may still be empty)
    std::string inputsNorm;     // it, normalized, minus the spawn ledger's header
    size_t fromShelf = 0;       // the prompt's first shelf/history/dyn label; npos = none
    std::string instrNormAll;   // the whole prompt, normalized (the quote rule reads it all)
    std::string instrNormShelf; // the prompt from its shelf on (the reason rule's records)
  };
  mutable SeenCache seen_;
  void ensureSeenCache() const;
  nimbus::orch::TurnEffects fx_;
  std::string sources_;
  std::string recalled_;
  std::string ownerNums_;        // the numbers the owner's own message holds (CUM-824)
  std::string targetNums_;       // the numbers a daily target may come from (food_figures.h)
  bool targetsClipped_ = false;  // that list hit its cap: the target rule stands down
  // Bench F03: targets a landed targets-write cited but did not store (food_figures.h),
  // and the same scan of the write whose result is still pending. One slot: tool calls
  // are sequential, noteToolArgs fills it and the matching noteTool consumes it. The
  // pending scan's stored flags ride too: a write only clears a metric from the
  // running set when it LANDS, so a refused write never silences an earlier one.
  std::vector<nimbus::orch::CitedTarget> citedUnstored_;
  std::vector<nimbus::orch::CitedTarget> pendingCited_;
  bool pendingStoredKcal_ = false;
  bool pendingStoredProtein_ = false;
  std::function<nimbus::orch::FoodFacts()> foodFacts_;
  std::string routineDay_;       // set on a scheduled turn: the stale-status rule applies
  uint32_t sourceNames_ = 0;     // SourceEvidence bits this turn names (CUM-824)
  std::string sourceWords_;      // the words those sources hold, for a source named by name
  std::string turnInputs_;       // the turn's input block (bounded): the label reads a
                                 // ledger row is checked against (food_label.h)
  bool turnInputsClipped_ = false;  // the input block was past its cap: the label gate stands down
  std::string imageReads_;       // the raw args/results of this turn's image.* calls (bounded):
                                 // the head's own reads of the turn's photos, which the input
                                 // block holds only as "[image k/N ref=...] (attached)"
  std::string records_;          // the turn's stored vision records (settled media rows in the
                                 // owner's history rows and the recalled memories), bounded:
                                 // the second-read comparison reads them (food_label.h)
  std::string toolText_;         // the tool results in normalizedWords form (bounded)
  std::string ownerText_;        // the owner's message (bounded): a why-question's reasons
  std::vector<std::string> unanswered_;   // earlier owner messages with no reply (bounded, lane MLQ)
  std::string ownerWords_;       // the owner's own words this turn (message + their history
                                 // rows), normalizedWords form: what backs "you told me"
  std::string ledger_;           // the spawn ledger block this turn read
  bool ownerAskedHow_ = false;   // the owner asked how something works: an explained
                                 // instrument is not claimed as read (the review)
  bool ownerAskedKeep_ = false;  // the owner asked for something to be kept: "Noted." answers that
  std::string wroteText_;        // the words this turn's own memory/file writes saved: a
                                 // save confirmation quoting them is the turn's record
  std::string pendingWrote_;     // noteToolArgs' stash until noteTool lands the write
  bool clipped_ = false;         // an evidence text or store hit its cap: the source, quote and
                                 // reason rules (which assert absence) stand down
  const std::string* instructions_ = nullptr;   // setQuoteContext
  const std::string* inputs_ = nullptr;
  uint32_t causeEvidence_ = 0;   // this turn's records, as CauseEvidence bits
  nimbus::orch::HostedRuns subRuns_;   // TurnSources::subRuns
  bool corrected_ = false;
};

}  // namespace agent
