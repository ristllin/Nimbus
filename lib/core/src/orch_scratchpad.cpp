#include "nimbus/orch/scratchpad.h"

#include "nimbus/mem_cap.h"  // utf8CapLen - UTF-8-safe byte cap

namespace nimbus {
namespace orch {

namespace {
// Trim ASCII whitespace both ends (portable; the model's items are text lines).
std::string trimmed(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) b--;
  return s.substr(a, b - a);
}
// Cap a string UTF-8-safe to maxBytes; also strips embedded newlines so one item
// can never break the line-based serialization or the prompt block layout.
std::string capItem(const std::string& in, int maxBytes) {
  std::string s = trimmed(in);
  for (char& c : s) if (c == '\n' || c == '\r') c = ' ';
  int keep = utf8CapLen(s.c_str(), (int)s.size(), maxBytes);
  return s.substr(0, keep);
}
}  // namespace

std::vector<std::string>& Scratchpad::tier(Tier t) {
  return t == Tier::Short ? short_ : t == Tier::Mid ? mid_ : long_;
}
const std::vector<std::string>& Scratchpad::tierC(Tier t) const {
  return t == Tier::Short ? short_ : t == Tier::Mid ? mid_ : long_;
}
const std::vector<std::string>& Scratchpad::items(Tier t) const { return tierC(t); }

bool Scratchpad::setActiveTask(const std::string& v) {
  std::string s = trimmed(v);
  bool fit = (int)s.size() <= kScratchActiveMax;
  active_ = capItem(s, kScratchActiveMax);
  return fit;
}

bool Scratchpad::add(Tier t, const std::string& item) {
  std::string s = capItem(item, kScratchItemMax);
  if (s.empty()) return false;
  auto& v = tier(t);
  if ((int)v.size() >= kScratchTierItems) return false;  // full - refuse
  v.push_back(s);
  return true;
}

int Scratchpad::replace(Tier t, const std::vector<std::string>& items) {
  auto& v = tier(t);
  v.clear();
  for (const auto& it : items) {
    if ((int)v.size() >= kScratchTierItems) break;  // count cap
    std::string s = capItem(it, kScratchItemMax);
    if (!s.empty()) v.push_back(s);
  }
  return (int)v.size();
}

void Scratchpad::clear(Tier t) { tier(t).clear(); }

bool Scratchpad::empty() const {
  return active_.empty() && short_.empty() && mid_.empty() && long_.empty();
}
void Scratchpad::clearAll() {
  active_.clear();
  short_.clear();
  mid_.clear();
  long_.clear();
}

void Scratchpad::appendPromptBlock(std::string& out) const {
  if (empty()) return;
  out += "\n## SCRATCHPAD (your own working notes)\n";
  if (!active_.empty()) out += "Now: " + active_ + "\n";
  const char* labels[3] = {"Short-term", "Mid-term", "Long-term"};
  const std::vector<std::string>* tiers[3] = {&short_, &mid_, &long_};
  for (int i = 0; i < 3; i++) {
    if (tiers[i]->empty()) continue;
    out += labels[i];
    out += ":\n";
    for (const auto& it : *tiers[i]) out += "- " + it + "\n";
  }
}

// ---- serialization -----------------------------------------------------------
// Line format, one record per line, tier tag then payload:
//   A<active>
//   S<short item>   (repeated)
//   M<mid item>
//   L<long item>
// Items are newline-free (capItem strips them), so a plain line split is safe.
std::string Scratchpad::serialize() const {
  std::string out;
  if (!active_.empty()) out += "A" + active_ + "\n";
  const std::vector<std::string>* tiers[3] = {&short_, &mid_, &long_};
  const char tags[3] = {'S', 'M', 'L'};
  for (int i = 0; i < 3; i++)
    for (const auto& it : *tiers[i]) { out += tags[i]; out += it; out += "\n"; }
  return out;
}

bool Scratchpad::deserialize(const std::string& blob) {
  clearAll();
  size_t start = 0;
  while (start < blob.size()) {
    size_t nl = blob.find('\n', start);
    if (nl == std::string::npos) nl = blob.size();
    if (nl > start) {
      char tag = blob[start];
      std::string payload = blob.substr(start + 1, nl - start - 1);
      switch (tag) {
        case 'A': setActiveTask(payload); break;
        case 'S': add(Tier::Short, payload); break;
        case 'M': add(Tier::Mid, payload); break;
        case 'L': add(Tier::Long, payload); break;
        default: break;  // tolerant: skip unknown/garbage lines
      }
    }
    start = nl + 1;
  }
  return true;
}

}  // namespace orch
}  // namespace nimbus
