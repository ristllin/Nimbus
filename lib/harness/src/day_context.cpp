// day_context - the fold's day and (RB-Q3a) the chat's recent actions in the per-turn block.
// TurnEngine members and their free helpers (engine.h), kept apart from engine.cpp so
// the turn core stays readable. (The speculative wire guard that cut history before any request,
// fitHistoryToWire, is gone - lane H of the history fix: history leaves a request
// only after a real refusal of its body, in the give-up order, turn_fit.h.)
#include "nimbus/harness/compose.h"   // renderRecentActions
#include "nimbus/harness/engine.h"
#include "nimbus/orch/food_ledger.h"

namespace agent {

namespace {

bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// A chat with a person behind it (the ledger belongs to that person).
bool personChat(const std::string& chatId) { return !chatId.empty() && chatId != "system"; }

}  // namespace

bool isSummaryMessage(const HistMsg& m) { return startsWith(m.text, kSummaryUserHead) || m.text == kSummaryAck; }

// RB-Q3a: no keyword playbook and no per-turn data block of one use case ride a prompt
// (ruling 41). What rides every turn is this chat's recent actions: the newest effect
// receipts from its effects ring, as data, so a correction can name what it changes without
// a read. Anything else is read with a tool; the owner's own skills from the [SKILLS] shelf.
void TurnEngine::applyRecentActions(ComposeInputs& ci) const {
  ci.recentActions.clear();
  if (!d_.recentActions || curChat_.empty()) return;
  const OffsetAt zoneAt = d_.utcOffsetAt ? d_.utcOffsetAt : fixedZone(d_.utcOffsetMin ? d_.utcOffsetMin() : 0);
  ci.recentActions = renderRecentActions(*d_.recentActions, curChat_, zoneAt);
}

std::string TurnEngine::foldDayContext(const std::string& chatId) const {
  if (!d_.foodLedger || !personChat(chatId)) return std::string();
  return nimbus::orch::renderFoldDay(d_.foodLedger(chatId));
}

}  // namespace agent
