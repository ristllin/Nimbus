#include "nimbus/orch/head_switch.h"

#include <cctype>

#include "nimbus/orch/model_catalog.h"   // isLatestAlias

namespace nimbus {
namespace orch {

std::string headPauseNote(const std::string& label, uint8_t pauseCauses, ParkKind park, bool overBudget) {
  const char* why = nullptr;
  const char* until = "when it lifts";
  if (pauseCauses & kPauseProviderLimit) {
    why = " is paused by your monthly limit";
  } else if (pauseCauses & kPauseSpendCeiling) {
    why = " is paused by your overall spending limit";
  } else if (pauseCauses & kPauseOwner) {
    why = " is paused by you";
    until = "when you resume it";
  } else if (park == ParkKind::Billing) {
    why = " is out of credit";
    until = "once credit is added";
  } else if (park == ParkKind::Auth) {
    why = " rejected its key";
    until = "once its key is replaced";
  } else if (overBudget) {
    why = " is over its monthly budget";
    until = "when its budget resets";
  }
  if (!why) return std::string();
  return label + why + "; replies already fail over automatically and return to " + label + " " + until + ".";
}

std::string headSwitchRefusal(const std::string& note, const std::string& targetLabel) {
  return note + " To make " + targetLabel + " your main provider anyway, ask again or pick it under Assistant > Models.";
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

bool HeadSwitchGuard::allow(const std::string& head, const std::string& headClosed, const std::string& target,
                            const std::string& model, const OwnerMessage& owner, uint32_t nowMs) {
  if (headClosed.empty() || target == head) return true;
  if (!owner.present()) return false;   // nobody to confirm with: refused, nothing recorded
  std::lock_guard<std::mutex> lk(mx_);
  const bool confirmed = armed_ && target_ == target && (uint32_t)(nowMs - atMs_) < kConfirmMs &&
                         owner.number != msg_ && ownerNamesTarget(owner.text, target, model);
  if (confirmed) {
    armed_ = false;
    return true;
  }
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
