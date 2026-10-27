#include "nimbus/orch/transcript.h"

#include <algorithm>

#include "nimbus/mem_cap.h"  // utf8CapLen - every clip is UTF-8-safe

namespace nimbus {
namespace orch {

namespace {
void setProvider(TranscriptItem& it, const char* p) {
  if (!p) return;
  size_t n = 0;
  while (p[n] && n + 1 < sizeof(it.provider)) { it.provider[n] = p[n]; n++; }
  it.provider[n] = '\0';
}
}  // namespace

void Transcript::addUser(std::string text) {
  TranscriptItem it;
  it.kind = TranscriptItem::Kind::User;
  it.text = std::move(text);
  it.round = -1;
  it.pinned = true;   // the seeded turn input is never folded away
  e_.push_back(std::move(it));
}

void Transcript::setSeedText(std::string text) {
  for (TranscriptItem& it : e_)
    if (it.kind == TranscriptItem::Kind::User && it.round == -1) {
      it.text = std::move(text);
      return;
    }
}

void Transcript::addAssistantText(std::string text, int round, const char* provider) {
  if (text.empty()) return;
  TranscriptItem it;
  it.kind = TranscriptItem::Kind::AssistantText;
  it.text = std::move(text);
  it.round = (int8_t)round;
  setProvider(it, provider);
  replayBytes_ += it.text.size();
  e_.push_back(std::move(it));
}

void Transcript::addToolCall(const HeadToolCall& c, int round, const char* provider) {
  TranscriptItem it;
  it.kind = TranscriptItem::Kind::ToolUse;
  it.id = c.id;
  it.name = c.name;
  it.text = c.argsJson;
  it.round = (int8_t)round;
  setProvider(it, provider);
  replayBytes_ += it.text.size();
  e_.push_back(std::move(it));
}

void Transcript::addToolResult(const HeadToolResult& r, int round) {
  TranscriptItem it;
  it.kind = TranscriptItem::Kind::ToolResult;
  it.id = r.id;
  it.name = r.name;
  it.text = r.output;
  it.isError = r.isError;
  it.round = (int8_t)round;
  replayBytes_ += r.output.size();
  e_.push_back(std::move(it));
}

void Transcript::attachMeta(int round, std::string meta) {
  if (meta.empty()) return;
  for (auto& it : e_) {
    if (it.kind == TranscriptItem::Kind::User) continue;
    if (it.round != round) continue;
    it.meta = std::move(meta);   // first item of the round carries the payload
    return;
  }
}

namespace {
// The newest result clipped to `room` bytes: its head plus a visible "[cut: ...]"
// marker, or a "[not shown: ...]" note when too little room is left for a useful
// head. The whole replacement fits `room` whenever a head is kept.
std::string clipToRoom(const std::string& text, size_t room) {
  const std::string cut = "\n[cut: this turn's tool-output budget is spent - showing the first ";
  const std::string tail = " of " + std::to_string((unsigned)text.size()) +
                           " B. Ask for a smaller slice, or finish with what you have.]";
  const size_t overhead = cut.size() + tail.size() + 6;   // 6 = the count's digits
  if (room > overhead + 256) {
    const int keep = utf8CapLen(text.c_str(), (int)text.size(), (int)(room - overhead));
    return text.substr(0, (size_t)keep) + cut + std::to_string(keep) + tail;
  }
  return "[not shown: this turn's tool-output budget is spent (" + std::to_string((unsigned)text.size()) +
         " B). Ask for a smaller slice, or finish with what you have.]";
}
}  // namespace

const char Transcript::kFoldNote[] =
    "[EARLIER TOOL ROUNDS OF THIS TURN - folded to one line each to save room; call the "
    "tool again if you need a full result back]";

int Transcript::newestRound() const {
  int newest = -1;
  for (const auto& it : e_)
    if (it.kind != TranscriptItem::Kind::User && it.round > newest) newest = it.round;
  return newest;
}

namespace {
// One folded round: each call with the gist of its result, or the gist of the
// prose of a round that called nothing.
std::string foldRoundLine(const std::vector<const TranscriptItem*>& items, int round) {
  std::string out;
  std::string prose;
  for (const TranscriptItem* it : items) {
    if (it->kind == TranscriptItem::Kind::AssistantText) prose += (prose.empty() ? "" : " ") + it->text;
    if (it->kind != TranscriptItem::Kind::ToolUse) continue;
    std::string result;
    size_t bytes = 0;
    bool err = false;
    for (const TranscriptItem* r : items)
      if (r->kind == TranscriptItem::Kind::ToolResult && r->id == it->id) {
        result = r->text;
        bytes = r->text.size();
        err = r->isError;
      }
    const std::string label = "- round " + std::to_string(round) + ": " + it->name + " " +
                              foldLine("", it->text, 60) + (err ? " -> error" : " ->");
    out += foldLine(label, result, 200) + " (" + std::to_string((unsigned)bytes) + " B)\n";
  }
  if (out.empty() && !prose.empty())
    out = foldLine("- round " + std::to_string(round) + ": (said)", prose, 200) + "\n";
  return out;
}
}  // namespace

void Transcript::foldOldRounds(int newest, size_t lowWater) {
  std::string lines;
  for (int round = 0; round < newest && replayBytes_ > lowWater; round++) {
    std::vector<const TranscriptItem*> items;
    for (const auto& it : e_)
      if (it.kind != TranscriptItem::Kind::User && it.round == round) items.push_back(&it);
    if (items.empty()) continue;
    lines += foldRoundLine(items, round);
    size_t freed = 0;
    for (const TranscriptItem* it : items) freed += it->text.size();
    e_.erase(std::remove_if(e_.begin(), e_.end(),
                            [round](const TranscriptItem& it) {
                              return it.kind != TranscriptItem::Kind::User && it.round == round;
                            }),
             e_.end());
    replayBytes_ -= freed;
  }
  if (lines.empty()) return;
  for (auto& it : e_) {
    if (it.kind != TranscriptItem::Kind::User) continue;
    if (it.text.find(kFoldNote) == std::string::npos) it.text += std::string("\n\n") + kFoldNote + "\n";
    it.text += lines;
    break;
  }
}

void Transcript::clipNewest(int newest, size_t cap) {
  size_t newestResults = 0;
  for (const auto& it : e_)
    if (it.kind == TranscriptItem::Kind::ToolResult && it.round == newest) newestResults += it.text.size();
  const size_t others = replayBytes_ - newestResults;
  size_t room = cap > others ? cap - others : 0;
  for (auto& it : e_) {
    if (it.kind != TranscriptItem::Kind::ToolResult || it.round != newest) continue;
    const size_t was = it.text.size();
    if (was > room) {
      std::string next = clipToRoom(it.text, room);
      if (next.size() < was) {
        replayBytes_ -= was - next.size();
        it.text = std::move(next);
      }
    }
    room -= std::min(room, it.text.size());
  }
}

size_t Transcript::stubArguments(size_t minBytes) {
  size_t freed = 0;
  for (auto& it : e_) {
    if (it.kind != TranscriptItem::Kind::ToolUse || it.name == "orch_turn" || it.text.size() <= minBytes)
      continue;
    std::string stub = "{\"omitted\":\"" + std::to_string((unsigned)it.text.size()) + " bytes; this call already ran\"}";
    if (stub.size() >= it.text.size()) continue;
    freed += it.text.size() - stub.size();
    it.text = std::move(stub);
  }
  replayBytes_ -= freed;
  return freed;
}

size_t Transcript::trimToolOutputs(size_t maxTotalToolBytes) {
  if (!maxTotalToolBytes || replayBytes_ <= maxTotalToolBytes) return 0;
  const size_t before = replayBytes_;
  const int newest = newestRound();
  // Hysteresis: fold down to half the cap, so the next fold is about cap/2
  // bytes of new rounds away - the request is rewritten that rarely, and only
  // here, never round after round.
  foldOldRounds(newest, maxTotalToolBytes / 2);
  if (replayBytes_ > maxTotalToolBytes) clipNewest(newest, maxTotalToolBytes);
  return before - replayBytes_;
}

std::string Transcript::renderBrief(size_t maxBytes) const {
  std::string out;
  size_t omitted = 0;
  for (const auto& it : e_) {
    std::string line;
    switch (it.kind) {
      case TranscriptItem::Kind::User:          line = "[user] " + it.text; break;
      case TranscriptItem::Kind::AssistantText: line = "[assistant] " + it.text; break;
      case TranscriptItem::Kind::ToolUse:       line = "[tool] " + it.name + " " + it.text; break;
      case TranscriptItem::Kind::ToolResult:
        line = std::string("[result] ") + it.name + (it.isError ? " (error) " : " ") + it.text;
        break;
    }
    line = foldLine("", line, 200);
    if (out.size() + line.size() + 1 > maxBytes) { omitted++; continue; }
    out += line;
    out += "\n";
  }
  if (omitted) out += "(" + std::to_string((unsigned)omitted) + " earlier entries omitted)\n";
  return out;
}

}  // namespace orch
}  // namespace nimbus
