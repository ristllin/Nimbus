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

class ClaimCheck {
 public:
  // Start a turn: clears the ledger, the sources and the per-turn flags.
  void beginTurn();

  // The sources a figure may come from: the turn's input block (the owner's
  // text, captions, the conversation window, fresh results) and, separately,
  // the recalled memories (a source only for what the owner reported).
  void setSources(const std::string& inputs, const std::string& recalled);
  // A head tool ran: its effect class (the shared tool table) and its result.
  // The result text joins the figure sources (bounded).
  void noteTool(nimbus::orch::FxKind kind, bool isError, const std::string& output,
                uint8_t flags = 0);
  // The ledger flags a tool call's arguments carry (kFxPermanent for a memory
  // written or updated with permanent:true, kFxVoice for reply.telegram voice:true).
  static uint8_t toolCallFlags(const std::string& regName, const std::string& argsJson);
  // Sub-agent results this turn read: how many had a lookup behind them and how
  // many were live-data results with none.
  void noteFresh(int sourced, int unsourced);
  // The head had provider-hosted tools this turn (connectors, hosted search).
  void setHostedTools(bool on) { hosted_ = on; }

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

  nimbus::orch::TurnEffects fx_;
  std::string sources_;
  std::string recalled_;
  bool hosted_ = false;
  bool corrected_ = false;
};

}  // namespace agent
