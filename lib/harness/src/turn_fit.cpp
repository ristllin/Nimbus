#include "nimbus/harness/turn_fit.h"

#include <algorithm>

#include "nimbus/harness/head_tools.h"
#include "nimbus/harness/log.h"
#include "nimbus/mem_cap.h"   // utf8CapLen - every clip stays valid UTF-8

namespace agent {

namespace {

// A clipped block keeps its start and its end (the newest words, a results.get handle).
constexpr size_t kBlockFloor = 1024;
constexpr size_t kTailKeep = 240;
constexpr size_t kInputFloor = 6000;
constexpr size_t kInputTailKeep = 3000;

// `text` with its middle cut and marked, `keep` bytes in all. UTF-8 safe.
std::string clipMiddle(const std::string& text, size_t keep, size_t tailKeep, const char* what) {
  if (text.size() <= keep) return text;
  const std::string lead = "\n[cut to fit this request: ";
  const std::string rest = std::string(" B of ") + what + " left out]\n";
  const size_t marker = lead.size() + rest.size() + 6;   // 6 = the count's digits
  const size_t room = keep > marker ? keep - marker : 0;
  const size_t tail = std::min(tailKeep, room / 2);
  const int head = nimbus::utf8CapLen(text.c_str(), (int)text.size(), (int)(room - tail));
  size_t from = text.size() - tail;
  while (from < text.size() && ((unsigned char)text[from] & 0xC0) == 0x80) from++;
  return text.substr(0, (size_t)head) + lead + std::to_string((unsigned)(from - (size_t)head)) + rest + text.substr(from);
}

// The next block header in a turn input at or after `from`: a line opening "[UPPER".
size_t nextBlockAt(const std::string& s, size_t from) {
  for (size_t at = s.find("\n[", from); at != std::string::npos; at = s.find("\n[", at + 1)) {
    const char c = at + 2 < s.size() ? s[at + 2] : '\0';
    if (c >= 'A' && c <= 'Z') return at;
  }
  return s.size();
}

}  // namespace

TurnFit::TurnFit(ComposeInputs& ci, const std::string& inputs, HeadTools* tools)
    : ci_(ci), tools_(tools), input_(inputs) {}

size_t TurnFit::dropOlderHistory(size_t want) {
  auto& h = ci_.history;
  // The summary pair stays (it carries what was folded), and so does the newest turn.
  const size_t first = (h.size() >= 2 && isSummaryMessage(h[0])) ? 2 : 0;
  size_t drop = 0, freed = 0;
  while (first + drop + 2 < h.size() && freed < want) {
    freed += h[first + drop].text.size() + h[first + drop + 1].text.size();
    drop += 2;
  }
  if (!drop) return 0;
  h.erase(h.begin() + (long)first, h.begin() + (long)(first + drop));
  historyDropped_ += (int)drop;
  historyBytesDropped_ += freed;
  return freed;
}

size_t TurnFit::clipResults(size_t want) {
  size_t freed = 0;
  for (const char* title : {"[FRESH RESULTS]", "[MEMORY RESULTS]"}) {
    if (freed >= want) break;
    const size_t at = input_.find(title);
    if (at == std::string::npos) continue;
    const size_t end = nextBlockAt(input_, at + 1);
    const size_t len = end - at;
    if (len <= kBlockFloor) continue;
    const size_t cutGoal = want - freed;
    const size_t keep = len > cutGoal + kBlockFloor ? len - cutGoal : kBlockFloor;
    const std::string kept = clipMiddle(input_.substr(at, len), keep, kTailKeep, "these results");
    if (kept.size() >= len) continue;
    freed += len - kept.size();
    resultBytesCut_ += len - kept.size();
    resultBlocksClipped_++;
    input_.replace(at, len, kept);
  }
  return freed;
}

size_t TurnFit::dropRecalled() {
  size_t freed = 0;
  for (const auto& r : ci_.recalled) freed += r.size();
  recalledDropped_ += (int)ci_.recalled.size();
  ci_.recalled.clear();
  return freed;
}

size_t TurnFit::dropPlaybook() {
  const size_t freed = ci_.foodPlaybook.size();
  if (freed) playbookDropped_ = true;
  ci_.foodPlaybook.clear();
  return freed;
}

size_t TurnFit::dropNewestHistory(size_t want) {
  auto& h = ci_.history;
  size_t freed = 0;
  // The wire writes a quoted text at up to twice its size, so this rung stops at what it can
  // measure and the next body build says whether it was enough.
  // Past turns go whole, as user and assistant pairs (a history that opens on an assistant
  // message is a refusal on some wires), the oldest of them first; the summary pair last.
  const size_t first = (h.size() >= 2 && isSummaryMessage(h[0])) ? 2 : 0;
  while (h.size() >= first + 2 && freed < want) {
    freed += h[first].text.size() + h[first + 1].text.size();
    h.erase(h.begin() + (long)first, h.begin() + (long)first + 2);
    newestDropped_ += 2;
  }
  if (freed < want && !h.empty()) {
    for (const auto& m : h) freed += m.text.size();
    newestDropped_ += (int)h.size();
    h.clear();
  }
  historyBytesDropped_ += freed;
  return freed;
}

size_t TurnFit::dropShelf() {
  const size_t freed = ci_.skillsShelf.size() + ci_.standingPrefs.size();
  if (freed) shelfDropped_ = true;
  ci_.skillsShelf.clear();
  ci_.standingPrefs.clear();
  return freed;
}

size_t TurnFit::clipInput(size_t want) {
  if (input_.size() <= kInputFloor) return 0;
  const size_t keep = input_.size() > want + kInputFloor ? input_.size() - want : kInputFloor;
  const std::string kept = clipMiddle(input_, keep, kInputTailKeep, "this turn's input");
  if (kept.size() >= input_.size()) return 0;
  const size_t freed = input_.size() - kept.size();
  inputBytesCut_ += freed;
  input_ = kept;
  return freed;
}

size_t TurnFit::dropTools() {
  if (!tools_ || tools_->specs.empty()) return 0;
  size_t freed = 0;
  for (const auto& s : tools_->specs) freed += s.name.size() + s.description.size() + s.schemaJson.size();
  tools_->specs.clear();
  ci_.tools.clear();
  toolsDropped_ = true;
  return freed;
}

namespace {
enum Rung : int { kRecalled, kPlaybook, kResults, kNewest, kInput, kTools, kShelf };
// What a tool can read back goes first. Then a turn with nobody's message in it (a synthesis,
// a routine) exists for its results: the past turns go before they do. An owner's turn is the
// other way round: "its pumpkin soup" means nothing without the last exchange, and a clipped
// result can be fetched again.
constexpr int kOwnerOrder[] = {kRecalled, kPlaybook, kResults, kNewest, kInput, kTools, kShelf};
constexpr int kUnattendedOrder[] = {kRecalled, kPlaybook, kNewest, kResults, kInput, kTools, kShelf};
}  // namespace

size_t TurnFit::runRung(int rung, size_t want) {
  switch (rung) {
    case kRecalled: return dropRecalled();
    case kPlaybook: return dropPlaybook();
    case kResults:  return clipResults(want);
    case kNewest:   return dropNewestHistory(want);
    case kInput:    return clipInput(want);
    case kTools:    return dropTools();
    case kShelf:    return dropShelf();
    default:        return 0;
  }
}

size_t TurnFit::walkLadder(size_t want, bool whole) {
  const int* order = ci_.speakerPresent ? kOwnerOrder : kUnattendedOrder;
  size_t freed = 0;
  for (size_t i = 0; i < sizeof(kOwnerOrder) / sizeof(kOwnerOrder[0]); i++) {
    freed += runRung(order[i], want > freed ? want - freed : 0);
    if (whole ? freed >= want : freed > 0) break;
  }
  return freed;
}

size_t TurnFit::shrink(size_t want, nimbus::orch::FitScope scope) {
  using nimbus::orch::FitScope;
  const size_t freed = scope == FitScope::OlderHistory ? dropOlderHistory(want)
                                                       : walkLadder(want, scope == FitScope::Ladder);
  if (!freed) return 0;
  trimmed_ = true;
  instructions_ = composeInstructions(ci_);
  hlog::logf("turn fit: left out %s (asked for %u B, gave %u B)", note().c_str(), (unsigned)want, (unsigned)freed);
  return freed;
}

namespace {
void addPart(std::string& s, const std::string& part) { s += (s.empty() ? "" : "; ") + part; }
}  // namespace

std::string TurnFit::note() const {
  std::string s;
  if (historyDropped_) addPart(s, std::to_string(historyDropped_) + " older conversation message(s)");
  if (resultBlocksClipped_) addPart(s, std::to_string((unsigned)resultBytesCut_) + " B of the results below");
  if (recalledDropped_) addPart(s, std::to_string(recalledDropped_) + " recalled memory item(s)");
  if (playbookDropped_) addPart(s, "the attached food playbook");
  if (newestDropped_) addPart(s, std::to_string(newestDropped_) + " of the newest conversation message(s)");
  if (shelfDropped_) addPart(s, "the skills list and standing preferences");
  if (inputBytesCut_) addPart(s, std::to_string((unsigned)inputBytesCut_) + " B of this turn's input");
  if (toolsDropped_) addPart(s, "every tool");
  return s;
}

std::string TurnFit::inputs() const {
  const std::string what = note();
  if (what.empty()) return input_;
  std::string head = "[LEFT OUT TO FIT THIS REQUEST] " + what + ". ";
  head += toolsDropped_
              ? "Your tools did not fit, so answer now from what you have and say plainly what you could not do.\n"
              : "If your answer depends on any of it, read it back with your tools, or say what you could not see.\n";
  return head + input_;
}

std::string TurnFit::ownerLine() const {
  std::string s;
  if (resultBlocksClipped_) addPart(s, "part of the results");
  if (newestDropped_ && ci_.speakerPresent) addPart(s, "the latest messages");
  if (inputBytesCut_) addPart(s, "part of this message");
  if (toolsDropped_) addPart(s, "its tools");
  if (s.empty()) return s;
  return "This answer was written without " + s + ", which did not fit the request. Ask again for anything missing.";
}

}  // namespace agent
