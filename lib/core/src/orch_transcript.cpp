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

void Transcript::addAssistantText(std::string text, int round, const char* provider) {
  if (text.empty()) return;
  TranscriptItem it;
  it.kind = TranscriptItem::Kind::AssistantText;
  it.text = std::move(text);
  it.round = (int8_t)round;
  setProvider(it, provider);
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
  toolBytes_ += r.output.size();
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

size_t Transcript::trimToolOutputs(size_t maxTotalToolBytes) {
  if (!maxTotalToolBytes || toolBytes_ <= maxTotalToolBytes) return 0;
  // Append-only (prompt_layout.h): a result an earlier round already SENT keeps
  // its bytes, so every round's request starts with the previous round's request
  // and reads it from the provider cache. Only the newest round's results - not
  // on any wire yet - are clipped, to the room the older ones leave. The entry is
  // never removed, so every ToolUse stays answered by its ToolResult.
  int newest = -1;
  for (const auto& it : e_)
    if (it.kind == TranscriptItem::Kind::ToolResult && it.round > newest) newest = it.round;
  size_t older = 0;
  for (const auto& it : e_)
    if (it.kind == TranscriptItem::Kind::ToolResult && it.round != newest) older += it.text.size();
  size_t room = maxTotalToolBytes > older ? maxTotalToolBytes - older : 0;
  size_t freed = 0;
  for (auto& it : e_) {
    if (it.kind != TranscriptItem::Kind::ToolResult || it.round != newest) continue;
    const size_t was = it.text.size();
    if (was > room) {
      std::string next = clipToRoom(it.text, room);
      if (next.size() < was) {
        freed += was - next.size();
        it.text = std::move(next);
      }
    }
    room -= std::min(room, it.text.size());
  }
  toolBytes_ -= freed;
  return freed;
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
