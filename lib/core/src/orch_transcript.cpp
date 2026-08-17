#include "nimbus/orch/transcript.h"

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

size_t Transcript::trimToolOutputs(size_t maxTotalToolBytes) {
  if (!maxTotalToolBytes || toolBytes_ <= maxTotalToolBytes) return 0;
  size_t freed = 0;
  // Oldest-first: stub the payload but KEEP the entry, so every ToolUse stays
  // answered by its ToolResult (the pairing invariant).
  for (auto& it : e_) {
    if (toolBytes_ <= maxTotalToolBytes) break;
    if (it.kind != TranscriptItem::Kind::ToolResult) continue;
    if (it.text.empty()) continue;
    const size_t was = it.text.size();
    std::string stub = "[trimmed " + std::to_string((unsigned)was) + " B]";
    if (stub.size() >= was) continue;   // already tiny - trimming would not help
    it.text = stub;
    toolBytes_ -= (was - stub.size());
    freed += was - stub.size();
  }
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
