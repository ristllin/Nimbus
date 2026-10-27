#pragma once
#include <string>

#include "nimbus/orch/food_ledger.h"     // FoodLedger
#include "nimbus/orch/tool_registry.h"   // Principal

// food_ledger_device - the device side of the daily food ledger (lane CTX): today's
// rows and totals of one person's ledger file on the card, streamed through the
// portable FoodLedgerScan (nimbus/orch/food_ledger.h), for the per-turn block and
// the fold. Runs on the turn task; holds the card lock only while it reads.
namespace agent::foodledger {

// Today's ledger for `who` (the turn's person, chat `chatId`): their canonical file
// (food/food-log.csv for the owner, food/food-log-<chat id>.csv for a member), read
// under the file store's read boundary. No card or no file = no ledger; a failed read
// is an error the block states (never a total).
nimbus::orch::FoodLedger today(const std::string& chatId, const nimbus::orch::Principal& who);

}  // namespace agent::foodledger
