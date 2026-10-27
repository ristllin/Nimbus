// day_context - lane CTX's part of the turn: today's food ledger in the per-turn
// block, the food playbooks on a food turn, the fold's day, and the wire guard that
// keeps a whole day of history inside the request body budget. TurnEngine members
// and their free helpers (engine.h), kept apart from engine.cpp so the turn core
// stays readable.
#include <algorithm>

#include "nimbus/harness/builtin_skills.h"   // kFoodLogSkillId, kFoodCoachSkillId, builtinSkillBody
#include "nimbus/harness/engine.h"
#include "nimbus/harness/log.h"              // hlog
#include "nimbus/harness/providers.h"        // providers::kWireBodyMax - the request body budget
#include "nimbus/orch/food_ledger.h"
#include "nimbus/orch/orch_schema.h"         // ORCH_SCHEMA_BODY - the final-answer tool

namespace agent {

namespace {

bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// The newest part of a text (a long reply's end is what the next message answers).
std::string tail(const std::string& s, size_t n) { return s.size() <= n ? s : s.substr(s.size() - n); }

bool isSummaryMessage(const HistMsg& m) { return startsWith(m.text, kSummaryUserHead) || m.text == kSummaryAck; }

// Was the exchange just before this message about food? A short follow-up ("brine",
// "I think your calc is off") answers the question the last reply asked.
bool lastExchangeAboutFood(const ComposeInputs& ci) {
  const size_t n = ci.history.size();
  for (size_t i = n >= 2 ? n - 2 : 0; i < n; i++)
    if (!isSummaryMessage(ci.history[i]) && nimbus::orch::looksFoodRelated(tail(ci.history[i].text, 1500)))
      return true;
  return !ci.recentConversation.empty() && nimbus::orch::looksFoodRelated(tail(ci.recentConversation, 1500));
}

// A chat with a person behind it (the ledger belongs to that person).
bool personChat(const std::string& chatId) { return !chatId.empty() && chatId != "system"; }

}  // namespace

std::string TurnEngine::loadFoodLedger(const std::string& chatId) {
  ledger_ = nimbus::orch::FoodLedger();
  if (d_.foodLedger && personChat(chatId)) ledger_ = d_.foodLedger(chatId);
  return nimbus::orch::renderLedgerBlock(ledger_, /*foodTurn=*/false);
}

void TurnEngine::applyFoodContext(ComposeInputs& ci, const std::string& userText) const {
  using nimbus::orch::FoodIntent;
  // A food turn: the message is about food, or follows a food exchange, or asks
  // about today's ledger (its total or an earlier estimate) while it has rows.
  const bool food = nimbus::orch::looksFoodRelated(userText) ||
                    (!userText.empty() && (lastExchangeAboutFood(ci) ||
                                           (ledger_.hasRows() && nimbus::orch::mentionsLedger(userText))));
  ci.foodLedger = nimbus::orch::renderLedgerBlock(ledger_, food);
  ci.foodPlaybook.clear();
  if (!food) return;
  // Which playbook: logging when the owner reports food, the coach when they ask (or
  // complain), both when the words do not say. One already in [ACTIVE SKILLS] is left
  // out. Not noted in the row's skills: tag: the playbooks are device context like the
  // ledger, and a weaker head copied the history's "(skills: ...)" suffix into replies.
  const FoodIntent intent = nimbus::orch::foodIntentOf(userText);
  const bool asks = intent == FoodIntent::Ask || intent == FoodIntent::Complaint;
  const struct {
    std::string id;
    bool wanted;
  } playbooks[] = {{kFoodLogSkillId, !asks}, {kFoodCoachSkillId, intent != FoodIntent::Eaten}};
  for (const auto& p : playbooks) {
    if (!p.wanted || std::any_of(attachPicks_.begin(), attachPicks_.end(),
                                 [&p](const AttachPick& a) { return a.id == p.id; }))
      continue;
    std::string body = d_.skillBody ? d_.skillBody(p.id) : std::string();   // an owner's approved edit wins
    if (body.empty()) body = builtinSkillBody(p.id, turnTelegram_, turnFileSend_);
    ci.foodPlaybook += body;
  }
  // The message's plain intent, last, nearest the message (eaten: log it; a request: no row).
  if (!ci.foodPlaybook.empty()) ci.foodPlaybook += nimbus::orch::renderFoodIntentHint(intent);
}

std::string TurnEngine::foldDayContext(const std::string& chatId) const {
  if (!d_.foodLedger || !personChat(chatId)) return std::string();
  return nimbus::orch::renderFoldDay(d_.foodLedger(chatId));
}

size_t wireToolBytes(const std::vector<nimbus::orch::ToolRegistry::Spec>& specs, bool loopOn) {
  size_t n = sizeof(nimbus::orch::ORCH_SCHEMA_BODY) + 256;
  if (!loopOn) return n;
  for (const auto& s : specs)
    if (!loopToolHidden(s.name)) n += s.name.size() + s.description.size() + s.schemaJson.size() + 64;
  return n;
}

size_t fitHistoryToWire(std::vector<HistMsg>& history, size_t instructionBytes, size_t toolBytes,
                        size_t inputBytes, bool loopOn) {
  // JSON escaping grows a text by a few percent; each history message (its text is
  // in instructionBytes) adds its own envelope, and the request one more.
  const auto escaped = [](size_t b) { return b + b / 16; };
  constexpr size_t kEnvelope = 48;
  // Room the tool rounds need on top: round 0 and a round or two must fit as sent
  // (past that the loop folds older rounds itself); a single-shot turn has none.
  const size_t reserve = loopOn ? 8000 : 2000;
  const size_t need = escaped(instructionBytes) + (history.size() + 1) * kEnvelope + toolBytes +
                      escaped(inputBytes) + reserve;
  if (need <= providers::kWireBodyMax) return 0;
  size_t excess = need - providers::kWireBodyMax;
  // The summary pair stays (it carries what was folded), and so does the newest turn.
  const size_t first = (history.size() >= 2 && isSummaryMessage(history[0])) ? 2 : 0;
  size_t drop = 0;
  while (first + drop + 2 < history.size() && excess > 0) {
    const size_t pair = escaped(history[first + drop].text.size()) + escaped(history[first + drop + 1].text.size()) +
                        2 * kEnvelope;
    excess = pair >= excess ? 0 : excess - pair;
    drop += 2;
  }
  if (!drop) return 0;
  history.erase(history.begin() + (long)first, history.begin() + (long)(first + drop));
  hlog::logf("history: %u message(s) left out to keep the request under %u B", (unsigned)drop,
             (unsigned)providers::kWireBodyMax);
  return drop;
}

}  // namespace agent
