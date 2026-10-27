#include "nimbus/orch/head_switch.h"

#include <cctype>

#include "nimbus/orch/model_catalog.h"   // isLatestAlias

namespace nimbus {
namespace orch {

namespace {
// One cause: why the provider is held off and when it comes back, in the owner's words and in
// the model's (the per-turn line).
struct PauseWords {
  const char* ownerWhy;
  const char* ownerUntil;
  const char* modelWhy;
  const char* modelUntil;
};
const PauseWords* pauseWords(uint8_t causes, ParkKind park, bool overBudget) {
  static const PauseWords kLimit{" is paused by your monthly limit", "when it lifts",
                                 " is paused by the owner's monthly limit", "when it lifts"};
  static const PauseWords kCeiling{" is paused by your overall spending limit", "when it lifts",
                                   " is paused by the owner's overall spending limit", "when it lifts"};
  static const PauseWords kOwner{" is paused by you", "when you resume it", " is paused by the owner",
                                 "when the owner resumes it"};
  static const PauseWords kCredit{" is out of credit", "once credit is added", " is out of credit",
                                  "once credit is added"};
  static const PauseWords kKey{" rejected its key", "once its key is replaced", " rejected its key",
                               "once its key is replaced"};
  static const PauseWords kBudget{" is over its monthly budget", "when its budget resets",
                                  " is over its monthly budget", "when its budget resets"};
  if (causes & kPauseProviderLimit) return &kLimit;
  if (causes & kPauseSpendCeiling) return &kCeiling;
  if (causes & kPauseOwner) return &kOwner;
  if (park == ParkKind::Billing) return &kCredit;
  if (park == ParkKind::Auth) return &kKey;
  return overBudget ? &kBudget : nullptr;
}
}  // namespace

std::string headPauseNote(const std::string& label, uint8_t pauseCauses, ParkKind park, bool overBudget) {
  const PauseWords* w = pauseWords(pauseCauses, park, overBudget);
  if (!w) return std::string();
  return label + w->ownerWhy + "; replies already fail over automatically and return to " + label + " " +
         w->ownerUntil + ".";
}

std::string headPauseFact(const std::string& label, const HeadPause& p) {
  const PauseWords* w = pauseWords(p.causes, p.park, p.overBudget);
  if (!w) return std::string();
  return label + ", the chosen head," + w->modelWhy + " and returns automatically " + w->modelUntil + ".";
}

std::string headSwitchRefusal(const std::string& note, const std::string& targetLabel) {
  return note + " To make " + targetLabel + " your main provider anyway, say \"switch to " + targetLabel +
         "\" again, or pick it under Assistant > Models.";
}

bool ownerChannel(const std::string& chatId, bool admin) {
  return admin || chatId == "web" || chatId == "serial" || chatId == "voice";
}

namespace {

std::string lower(const std::string& s) {
  std::string out(s);
  for (char& c : out) c = (char)std::tolower((unsigned char)c);
  return out;
}

bool has(const std::string& hay, const std::string& needle) {
  return !needle.empty() && hay.find(needle) != std::string::npos;
}

// The words that name a provider in a message: its slug, its display name and the
// model families only it serves.
bool namesProvider(const std::string& text, const std::string& provider) {
  static const struct {
    const char* slug;
    const char* words[6];
  } kNames[] = {
      {"openai", {"openai", "open ai", "gpt", "chatgpt", nullptr}},
      {"anthropic", {"anthropic", "claude", "opus", "sonnet", "haiku", nullptr}},
      {"mistral", {"mistral", "magistral", nullptr}},
      {"zai", {"z.ai", "zai", "glm", "zhipu", nullptr}},
      {"cumulo", {"cumulo", nullptr}},
  };
  for (const auto& n : kNames) {
    if (provider != n.slug) continue;
    for (int i = 0; n.words[i]; ++i)
      if (has(text, n.words[i])) return true;
  }
  return false;
}

}  // namespace

bool ownerNamesTarget(const std::string& ownerText, const std::string& provider, const std::string& model) {
  const std::string text = lower(ownerText);
  if (text.empty()) return false;
  if (namesProvider(text, lower(provider))) return true;
  std::string m = lower(model);
  const size_t slash = m.rfind('/');   // a router id names its upstream's model
  if (slash != std::string::npos) m = m.substr(slash + 1);
  if (has(text, m)) return true;
  return isLatestAlias(m) && has(text, m.substr(0, m.size() - 7));   // the id without "-latest"
}

namespace {
// `w` as a whole word of `text` (lowercase): not part of a longer word.
bool hasWord(const std::string& text, const std::string& w) {
  for (size_t at = text.find(w); at != std::string::npos; at = text.find(w, at + 1)) {
    const bool startOk = at == 0 || !std::isalnum((unsigned char)text[at - 1]);
    const size_t end = at + w.size();
    const bool endOk = end >= text.size() || !std::isalnum((unsigned char)text[end]);
    if (startOk && endOk) return true;
  }
  return false;
}
}  // namespace

bool ownerAsksSwitchTo(const std::string& ownerText, const std::string& provider, const std::string& model) {
  if (!ownerNamesTarget(ownerText, provider, model)) return false;
  const std::string text = lower(ownerText);
  static const char* const kSwitch[] = {"switch", "change", "move", "use", "make", "set", "pick", "choose", "go with"};
  static const char* const kDecline[] = {"keep", "stay", "no", "not", "don't", "dont", "do not", "never"};
  bool asks = false;
  for (const char* w : kSwitch) asks = asks || hasWord(text, w);
  for (const char* w : kDecline)
    if (hasWord(text, w)) return false;
  return asks;
}

bool HeadSwitchGuard::allow(const std::string& head, const std::string& headClosed, const std::string& target,
                            const std::string& model, const OwnerMessage& owner, uint32_t nowMs) {
  if (headClosed.empty() || target == head) return true;
  if (!owner.present()) return false;   // nobody to confirm with: refused, nothing recorded
  const bool asks = ownerAsksSwitchTo(owner.text, target, model);
  std::lock_guard<std::mutex> lk(mx_);
  const bool confirmed = asks && armed_ && target_ == target && (uint32_t)(nowMs - atMs_) < kConfirmMs &&
                         owner.number != msg_;
  if (confirmed) {
    armed_ = false;
    return true;
  }
  if (!asks) return false;   // a refusal the owner did not ask for: nothing to confirm later
  armed_ = true;
  target_ = target;
  atMs_ = nowMs;
  msg_ = owner.number;
  return false;
}

void HeadSwitchGuard::reset() {
  std::lock_guard<std::mutex> lk(mx_);
  armed_ = false;
  target_.clear();
  atMs_ = 0;
  msg_ = 0;
}

HeadSwitchGuard& headSwitchGuard() {
  static HeadSwitchGuard g;
  return g;
}

}  // namespace orch
}  // namespace nimbus
