// day_context - lane CTX's part of the turn: today's food ledger in the per-turn
// block, the food playbooks on a food turn and the fold's day. TurnEngine members
// and their free helpers (engine.h), kept apart from engine.cpp so the turn core
// stays readable. (The speculative wire guard that cut history before any request,
// fitHistoryToWire, is gone - lane H of the history fix: history leaves a request
// only after a real refusal of its body, in the give-up order, turn_fit.h.)
#include <algorithm>

#include "nimbus/harness/builtin_skills.h"   // kFoodLogSkillId, kFoodCoachSkillId, builtinSkillBody
#include "nimbus/harness/engine.h"
#include "nimbus/harness/skill_attach.h"     // kSkillAttachCap
#include "nimbus/harness/skill_md.h"         // skillUtf8Prefix
#include "nimbus/orch/food_ledger.h"

namespace agent {

namespace {

bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// The newest part of a text (a long reply's end is what the next message answers).
std::string tail(const std::string& s, size_t n) { return s.size() <= n ? s : s.substr(s.size() - n); }

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

bool isSummaryMessage(const HistMsg& m) { return startsWith(m.text, kSummaryUserHead) || m.text == kSummaryAck; }

void TurnEngine::loadFoodLedger(const std::string& chatId) {
  ledger_ = nimbus::orch::FoodLedger();
  if (d_.foodLedger && personChat(chatId)) ledger_ = d_.foodLedger(chatId);
}

// The owner's own method for a meal rides a logging turn (1 Nov: "Dont you have a skill guiding
// you?" - it was on the shelf and never opened). The first approved skill about food, unless the
// chat already holds it; its tail stays one skill.get away.
std::string TurnEngine::ownerFoodSkillBlock(bool asks) const {
  if (asks || !d_.foodSkills || !d_.skillBody) return std::string();
  for (const std::string& id : d_.foodSkills()) {
    if (std::any_of(attachPicks_.begin(), attachPicks_.end(), [&id](const AttachPick& a) { return a.id == id; }))
      return std::string();
    const std::string body = d_.skillBody(id);
    if (body.empty()) continue;
    std::string out = "[SKILL " + id + " - the owner's approved method, follow it for this meal]\n" +
                      skillUtf8Prefix(body, kSkillAttachCap);
    if (body.size() > kSkillAttachCap) out += "\n[the rest: skill.get " + id + "]";
    return out + "\n";
  }
  return std::string();
}

std::string TurnEngine::foodRecallBlock(const std::string& userText) const {
  return d_.foodRecall && personChat(curChat_) ? d_.foodRecall(curChat_, userText) : std::string();
}

void TurnEngine::applyFoodContext(ComposeInputs& ci, const std::string& userText) const {
  using nimbus::orch::FoodIntent;
  // A food turn: the message is about food, or follows a food exchange, or asks
  // about today's ledger (its total or an earlier estimate) while it has rows.
  const bool food = nimbus::orch::looksFoodRelated(userText) ||
                    (!userText.empty() && (lastExchangeAboutFood(ci) ||
                                           (ledger_.hasRows() && nimbus::orch::mentionsLedger(userText))));
  ci.foodLedger = nimbus::orch::renderLedgerBlock(ledger_, food);
  // Lane UPSERT, 4 Nov: the owner disputes a total - the day's history rows (rows that
  // changed or disappeared today, kept as #replaced: rows) ride the turn, so the
  // answer names what the day held before instead of re-adding from memory.
  if (nimbus::orch::disputesLedger(userText)) ci.foodLedger += nimbus::orch::renderLedgerDispute(ledger_);
  ci.foodPlaybook.clear();
  if (!food) return;
  ci.foodLedger += foodRecallBlock(userText);
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
  ci.foodPlaybook += ownerFoodSkillBlock(asks);
  // The message's plain intent, last, nearest the message (eaten: log it; a request: no row).
  if (!ci.foodPlaybook.empty()) ci.foodPlaybook += nimbus::orch::renderFoodIntentHint(intent);
}

std::string TurnEngine::foldDayContext(const std::string& chatId) const {
  if (!d_.foodLedger || !personChat(chatId)) return std::string();
  return nimbus::orch::renderFoldDay(d_.foodLedger(chatId));
}

}  // namespace agent
