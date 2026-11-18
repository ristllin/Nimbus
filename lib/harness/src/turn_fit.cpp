#include "nimbus/harness/turn_fit.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>
#include <cstdio>

#include "nimbus/harness/head_tools.h"
#include "nimbus/harness/log.h"
#include "nimbus/mem_cap.h"         // utf8CapLen - every clip stays valid UTF-8

namespace agent {

namespace {

// A clipped block keeps its start and its end (the newest words, a results.get handle).
constexpr size_t kBlockFloor = 1024;
constexpr size_t kTailKeep = 240;
constexpr size_t kInputFloor = 6000;
constexpr size_t kInputTailKeep = 3000;
constexpr const char kSkillsHeader[] = "[ACTIVE SKILLS]";

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

void addPart(std::string& s, const std::string& part) { s += (s.empty() ? "" : "; ") + part; }

}  // namespace

TurnFit::TurnFit(ComposeInputs& ci, const std::string& inputs, HeadTools* tools, const FitContext& ctx)
    : ci_(ci), tools_(tools), ctx_(ctx), input_(inputs) {}

// The request as the wire sends it, less its fixed envelope: the composed prompt (its
// history messages included), the input with this note, and the tools array.
size_t TurnFit::rendered() const {
  size_t n = composeInstructions(ci_).size() + inputs().size();
  if (tools_)
    for (const auto& t : tools_->specs) n += t.name.size() + t.description.size() + t.schemaJson.size();
  return n;
}

void TurnFit::dropRecalled() {
  cut_.recalledDropped += (int)ci_.recalled.size();
  ci_.recalled.clear();
}

void TurnFit::dropSkills() {
  const size_t at = input_.find(kSkillsHeader);
  if (at == std::string::npos) return;
  const size_t end = nextBlockAt(input_, at + 1);
  input_.erase(at, end - at);
  cut_.skillsDropped = true;
}

bool TurnFit::isToday(size_t i) const {
  return ctx_.todayLocalDay > 0 && (int32_t)ci_.history[i].localDay == ctx_.todayLocalDay;
}

// Live history, oldest whole turns first (a history that opens on an assistant message is a
// refusal on some wires): before today on rung 5, today's on rung 6. The summary pair and the
// newest exchange are never dropped.
void TurnFit::dropHistory(bool today, size_t want) {
  auto& h = ci_.history;
  const size_t first = (h.size() >= 2 && isSummaryMessage(h[0])) ? 2 : 0;
  const size_t live = floorNow_ ? liveHistoryBytes() : 0;
  size_t drop = 0, freed = 0;
  // Rung 5 stops at today's first turn; rung 6 starts there; the newest exchange stays, and
  // under OlderHistory the live history never goes under the keep floor.
  while (first + drop + 2 < h.size() && freed < want && isToday(first + drop) == today) {
    const size_t pair = h[first + drop].text.size() + h[first + drop + 1].text.size();
    if (floorNow_ && live < freed + pair + floorNow_) break;
    freed += pair;
    drop += 2;
  }
  if (!drop) return;
  noteHistoryCut(first, drop);
  h.erase(h.begin() + (long)first, h.begin() + (long)(first + drop));
  (today ? cut_.todayDropped : cut_.olderDropped) += (int)drop;
  cut_.historyBytesDropped += freed;
}

// What the note names of a history cut: its rows, its time range, the first row kept.
void TurnFit::noteHistoryCut(size_t first, size_t drop) {
  const auto& h = ci_.history;
  for (size_t i = first; i < first + drop; i++) {
    const HistMsg& m = h[i];
    if (cut_.cutFirstId.empty() && !m.firstId.empty()) cut_.cutFirstId = m.firstId;
    if (!m.lastId.empty()) cut_.cutLastId = m.lastId;
    if (m.atUnix && !cut_.cutFromUnix) cut_.cutFromUnix = m.atUnix;
    if (m.atUnix) cut_.cutToUnix = m.atUnix;
    cut_.historyRowsDropped += m.rows;
  }
  cut_.keptFirstId = first + drop < h.size() ? h[first + drop].firstId : std::string();
}

void TurnFit::clipResults(size_t want) {
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
    cut_.resultBytesCut += len - kept.size();
    cut_.resultBlocksClipped++;
    input_.replace(at, len, kept);
  }
}

void TurnFit::clipInput(size_t want) {
  if (input_.size() <= kInputFloor) return;
  const size_t keep = input_.size() > want + kInputFloor ? input_.size() - want : kInputFloor;
  const std::string kept = clipMiddle(input_, keep, kInputTailKeep, "this turn's input");
  if (kept.size() >= input_.size()) return;
  cut_.inputBytesCut += input_.size() - kept.size();
  input_ = kept;
}

void TurnFit::dropTools() {
  if (!tools_ || tools_->specs.empty()) return;
  tools_->specs.clear();
  ci_.tools.clear();
  cut_.toolsDropped = true;
}

void TurnFit::dropShelf() {
  if (ci_.skillsShelf.empty() && ci_.standingPrefs.empty()) return;
  cut_.shelfDropped = true;
  ci_.skillsShelf.clear();
  ci_.standingPrefs.clear();
}

// One rung applied; it returns what puts back exactly what it changed.
std::function<void()> TurnFit::applyRung(int rung, size_t want) {
  switch (rung) {
    case kRecalled: {
      auto was = std::make_shared<std::vector<std::string>>(ci_.recalled);
      dropRecalled();
      return [this, was] { ci_.recalled = std::move(*was); };
    }
    case kSkills: {
      auto was = std::make_shared<std::string>(input_);
      dropSkills();
      return [this, was] { input_ = std::move(*was); };
    }
    case kOlderDays:
    case kToday: {
      auto was = std::make_shared<std::vector<HistMsg>>(ci_.history);
      dropHistory(rung == kToday, want);
      return [this, was] { ci_.history = std::move(*was); };
    }
    case kResults:
    case kInput: {
      auto was = std::make_shared<std::string>(input_);
      if (rung == kResults) clipResults(want);
      else clipInput(want);
      return [this, was] { input_ = std::move(*was); };
    }
    case kTools: return dropToolsUndoable();
    case kShelf: {
      auto was = std::make_shared<std::pair<std::string, std::string>>(ci_.skillsShelf, ci_.standingPrefs);
      dropShelf();
      return [this, was] { ci_.skillsShelf = std::move(was->first); ci_.standingPrefs = std::move(was->second); };
    }
    default: return std::function<void()>();
  }
}

std::function<void()> TurnFit::dropToolsUndoable() {
  auto specs = std::make_shared<std::vector<nimbus::orch::ToolRegistry::Spec>>();
  if (tools_) *specs = tools_->specs;
  auto infos = std::make_shared<std::vector<nimbus::orch::ToolInfo>>(ci_.tools);
  dropTools();
  return [this, specs, infos] {
    if (tools_) tools_->specs = std::move(*specs);
    ci_.tools = std::move(*infos);
  };
}

// One rung, measured on the rendered request with this note: a rung that does not make it
// smaller is undone and reports 0 (no note, no trim). Only what the rung may change is kept.
size_t TurnFit::runRung(int rung, size_t want) {
  const size_t before = rendered();
  const Cuts cutWas = cut_;
  const std::function<void()> undo = applyRung(rung, want);
  if (!undo) return 0;
  const size_t after = rendered();
  if (after < before) return before - after;
  undo();   // nothing gained: as it was, and no note
  cut_ = cutWas;
  return 0;
}

size_t TurnFit::walk(int from, int to, size_t want, bool whole) {
  size_t freed = 0;
  for (int r = from; r <= to; r++) {
    freed += runRung(r, want > freed ? want - freed : 1);
    if (whole ? freed >= want : freed > 0) break;
  }
  return freed;
}

// What the newest message holds, in the order's own sequence: recall, the skills, the
// results blocks, the input's middle (never history, tools or the shelf).
size_t TurnFit::walkNewestMessage(size_t want) {
  size_t freed = 0;
  for (int r : {kRecalled, kSkills, kResults, kInput}) {
    freed += runRung(r, want > freed ? want - freed : 1);
    if (freed >= want) break;
  }
  return freed;
}

size_t TurnFit::liveHistoryBytes() const {
  const auto& h = ci_.history;
  size_t n = 0;
  for (size_t i = (h.size() >= 2 && isSummaryMessage(h[0])) ? 2 : 0; i < h.size(); i++) n += h[i].text.size();
  return n;
}

// The history rungs down to the keep floor, remembering what was there before the first cut.
size_t TurnFit::shedOlderHistory(size_t want) {
  const bool first = !shed_;
  if (first) shed_.reset(new HistoryShed{ci_.history, cut_});
  floorNow_ = keepFloor_;
  const size_t freed = walk(kOlderDays, kToday, want, true);
  floorNow_ = 0;
  if (!freed && first) shed_.reset();
  return freed;
}

size_t TurnFit::giveBackHistory() {
  if (!shed_) return 0;
  const size_t before = rendered();
  ci_.history = std::move(shed_->history);
  const Cuts& was = shed_->cut;
  cut_.olderDropped = was.olderDropped;
  cut_.todayDropped = was.todayDropped;
  cut_.historyBytesDropped = was.historyBytesDropped;
  cut_.historyRowsDropped = was.historyRowsDropped;
  cut_.cutFirstId = was.cutFirstId;
  cut_.cutLastId = was.cutLastId;
  cut_.keptFirstId = was.keptFirstId;
  cut_.cutFromUnix = was.cutFromUnix;
  cut_.cutToUnix = was.cutToUnix;
  shed_.reset();
  instructions_ = composeInstructions(ci_);
  trimmed_ = !note().empty();
  const size_t after = rendered();
  hlog::logf("turn fit: put back %u B of history (its photos went as text)", (unsigned)(after - std::min(after, before)));
  return after > before ? after - before : 1;
}

size_t TurnFit::shrink(size_t want, nimbus::orch::FitScope scope) {
  using nimbus::orch::FitScope;
  if (scope == FitScope::GiveBackHistory) return giveBackHistory();
  const size_t historyWas = ci_.history.size();
  size_t freed = 0;
  switch (scope) {
    case FitScope::OlderHistory:  freed = shedOlderHistory(want); break;
    case FitScope::Context:       freed = walk(kRecalled, kToday, want, false); break;
    case FitScope::NextRung:      freed = walk(kRecalled, kShelf, want, false); break;
    case FitScope::Ladder:        freed = walk(kRecalled, kShelf, want, true); break;
    case FitScope::NewestMessage: freed = walkNewestMessage(want); break;
    case FitScope::GiveBackHistory: break;
  }
  // History another scope cut stays cut: what it was given up for still holds.
  if (scope != FitScope::OlderHistory && ci_.history.size() != historyWas) shed_.reset();
  if (!freed) return 0;
  trimmed_ = true;
  instructions_ = composeInstructions(ci_);
  hlog::logf("turn fit: left out %s (asked for %u B, gave %u B)", note().c_str(), (unsigned)want, (unsigned)freed);
  return freed;
}

std::string TurnFit::historyPart() const {
  const int n = cut_.olderDropped + cut_.todayDropped;
  if (!n) return std::string();
  std::string s = std::to_string(n) + (cut_.todayDropped && !cut_.olderDropped ? " of today's earlier" : " older") +
                  " conversation message(s)";
  const std::string from = localTimeLabel(cut_.cutFromUnix, ctx_.utcOffsetMin);
  const std::string to = localTimeLabel(cut_.cutToUnix, ctx_.utcOffsetMin);
  if (!from.empty()) s += " from " + from + (to.empty() || to == from ? std::string() : " to " + to);
  if (!cut_.cutFirstId.empty()) s += " (rows " + cut_.cutFirstId + " to " + cut_.cutLastId + ")";
  s += ", read them back with memory_episodic {\"kind\":\"message\"";
  if (!cut_.keptFirstId.empty()) s += ",\"before\":\"" + cut_.keptFirstId + "\"";
  return s + "}";
}

std::string TurnFit::note() const {
  std::string s;
  const std::string history = historyPart();
  if (!history.empty()) addPart(s, history);
  if (cut_.resultBlocksClipped) addPart(s, std::to_string((unsigned)cut_.resultBytesCut) + " B of the results below");
  if (cut_.recalledDropped) addPart(s, std::to_string(cut_.recalledDropped) + " recalled memory item(s)");
  if (cut_.skillsDropped) addPart(s, "the attached skills (skill_get reads them)");
  if (cut_.shelfDropped) addPart(s, "the skills list and standing preferences");
  if (cut_.inputBytesCut) addPart(s, std::to_string((unsigned)cut_.inputBytesCut) + " B of this turn's input");
  if (cut_.toolsDropped) addPart(s, "every tool");
  return s;
}

std::string TurnFit::inputs() const {
  const std::string what = note();
  if (what.empty()) return input_;
  std::string head = "[LEFT OUT TO FIT THIS REQUEST] " + what + ". ";
  head += cut_.toolsDropped
              ? "Your tools did not fit, so answer now from what you have and say plainly what you could not do.\n"
              : "If your answer depends on any of it, read it back with your tools, or say what you could not see.\n";
  return head + input_;
}

std::string TurnFit::ownerLine() const {
  std::string s;
  if (cut_.resultBlocksClipped) addPart(s, "part of the results");
  if (cut_.todayDropped && ci_.speakerPresent) addPart(s, "some of today's earlier messages");
  if (cut_.inputBytesCut) addPart(s, "part of this message");
  if (cut_.toolsDropped) addPart(s, "its tools");
  if (s.empty()) return s;
  return "This answer was written without " + s + ", which did not fit the request. Ask again for anything missing.";
}

}  // namespace agent
