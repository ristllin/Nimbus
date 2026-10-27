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
// when the head had hosted tools this turn, the sent / checked / changed
// classes are not checked (precision first) - except a gap only this device
// could have closed (a Telegram voice message from its own bot:
// ClaimGap::deviceOnly). Everything the device does itself
// (memory, files, skills, the scratchpad, spawns, durability, figures from a
// source, unsourced sub-agent results) is always checked.
//
// RAM only, one turn long. Portable, host-tested (test_harness_claim_check).

#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/claims.h"
#include "nimbus/orch/turn.h"
#include "nimbus/orch/turn_effects.h"

namespace agent {

// The effect class of a registered tool, for the ledger, from the shared tool
// table (orch::toolEffect). Unknown names (a connector's remote tools) read as
// a lookup: nothing the device can vouch for.
nimbus::orch::FxKind fxKindForTool(const std::string& regName);

// The device-authored parts of a turn's input block, apart (CUM-824). The claim check
// never re-parses them out of the composed text: a sub-agent reply or a stored memory
// that contains a block's title must not pose as that block.
struct TurnSources {
  std::string owner;    // the owner's message ("" on a synthesis or scheduled turn)
  std::string fresh;    // the sub-agent results the turn read: provenance tags and text
  std::string memory;   // the [MEMORY RESULTS] body
  std::string ledger;   // the spawn ledger block (JobEngine::spawnLedgerText)
  bool webRead = false; // a result's provider record shows a web search or fetch
};

class ClaimCheck {
 public:
  // Start a turn: clears the ledger, the sources and the per-turn flags.
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
  // A head tool ran: its effect class (the shared tool table) and its result.
  // The result text joins the figure sources (bounded).
  void noteTool(nimbus::orch::FxKind kind, bool isError, const std::string& output,
                uint8_t flags = 0);
  // The ledger flags a tool call's arguments carry (kFxPermanent for a memory
  // written or updated with permanent:true, kFxVoice for reply.telegram
  // voice:true, kFxHealth for a read of the device's own state, kFxLogRow for a
  // files.append into a .csv or .log file, kept only if its result counts a row).
  static uint8_t toolCallFlags(const std::string& regName, const std::string& argsJson);
  // Sub-agent results this turn read: how many had a lookup behind them and how
  // many were live-data results with none.
  void noteFresh(int sourced, int unsourced);
  // The head had provider-hosted tools this turn (connectors, hosted search).
  void setHostedTools(bool on) { hosted_ = on; }

  // This turn's cause evidence (L19-11): what the records the model was given
  // (its inputs, its tool results, its health reads) say about causes, as
  // orch::CauseEvidence bits. Apply reads it to refuse a mem_write that would
  // store an unbacked cause as a fact.
  uint32_t causeEvidence() const { return causeEvidence_; }

  nimbus::orch::TurnEffects& effects() { return fx_; }
  const nimbus::orch::TurnEffects& effects() const { return fx_; }

  // The final-answer check (validateFinal): "" when the reply's claims are
  // backed by the ledger plus the turn's own fields, else the correction text.
  // `skipSpawned` leaves the spawn class to another check of the same chain.
  std::string finalCheck(const std::string& finalTurnJson, bool skipSpawned = false);
  bool corrected() const { return corrected_; }

  // After apply: apply recorded the turn's real outcomes in effects() (each
  // memory write's result, each spawn the queue took or refused, the scratchpad
  // save, the device actions). Returns the owner tail ("" when every claim is
  // backed), one line per gap class; `skipSpawned` as above.
  std::string tailAfterApply(const std::string& reply, bool skipSpawned = false);

  // Pending effects a final turn carries in its own fields (exposed for tests).
  static void addPending(nimbus::orch::TurnEffects& fx, const nimbus::orch::Turn& t);
  // The effect an allowed device[] element has (apply records it); false for a
  // blocked or unknown one.
  static bool deviceActionEffect(const std::string& json, nimbus::orch::FxKind& out);

  // RAM counters for /api/state (process lifetime, never persisted).
  static uint32_t correctedCount();
  static uint32_t tailedCount();

 private:
  std::vector<nimbus::orch::ClaimGap> gapsFor(const std::string& reply,
                                              const nimbus::orch::TurnEffects& fx,
                                              bool skipSpawned) const;
  bool hostedMayBack(const nimbus::orch::ClaimGap& g) const;
  void recordGaps(const std::string& reply, std::vector<nimbus::orch::ClaimGap>& kept) const;
  std::string seenText(bool fixedToo) const;   // what a quote (fixedToo) or a reason may come from
  void noteToolText(const std::string& output);

  nimbus::orch::TurnEffects fx_;
  std::string sources_;
  std::string recalled_;
  std::string ownerNums_;        // the numbers the owner's own message holds (CUM-824)
  uint32_t sourceNames_ = 0;     // SourceEvidence bits this turn names (CUM-824)
  std::string sourceWords_;      // the words those sources hold, for a source named by name
  std::string toolText_;         // the tool results in normalizedWords form (bounded)
  std::string ownerText_;        // the owner's message (bounded): a why-question's reasons
  std::string ledger_;           // the spawn ledger block this turn read
  bool clipped_ = false;         // an evidence text or store hit its cap: the source, quote and
                                 // reason rules (which assert absence) stand down
  const std::string* instructions_ = nullptr;   // setQuoteContext
  const std::string* inputs_ = nullptr;
  uint32_t causeEvidence_ = 0;   // this turn's records, as CauseEvidence bits
  bool hosted_ = false;
  bool corrected_ = false;
};

}  // namespace agent
