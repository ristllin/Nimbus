#include "nimbus/orch/gradient.h"

#include <algorithm>

#include "nimbus/mem_cap.h"  // utf8CapLen - every clip must be UTF-8-safe

namespace nimbus {
namespace orch {

namespace {

// Collapse whitespace runs to single spaces so a folded line stays one line.
std::string oneLine(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  bool ws = false;
  for (char c : s) {
    if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
      ws = true;
      continue;
    }
    if (ws && !o.empty()) o += ' ';
    ws = false;
    o += c;
  }
  return o;
}

std::string clipUtf8(const std::string& s, size_t maxBytes) {
  if (s.size() <= maxBytes) return s;
  int keep = utf8CapLen(s.c_str(), (int)s.size(), (int)maxBytes);
  return s.substr(0, (size_t)keep) + "…";
}

}  // namespace

std::string foldLine(const std::string& label, const std::string& text, size_t lineMax) {
  std::string body = oneLine(text);
  std::string line = label.empty() ? body : (label + ": " + body);
  return clipUtf8(line, lineMax);
}

size_t transcriptBytes(const std::vector<TranscriptItem>& items) {
  size_t n = 0;
  for (const auto& it : items) n += it.text.size();
  return n;
}

std::vector<TranscriptItem> gradientTrim(const std::vector<TranscriptItem>& in,
                                         const GradientPolicy& pol) {
  if (pol.triggerBytes && transcriptBytes(in) <= pol.triggerBytes) return in;

  // The newest keepRounds distinct rounds stay verbatim. Round -1 (non-loop
  // items: the seeded user message, prose) is never a fold candidate by round.
  int8_t maxRound = -1;
  for (const auto& it : in) maxRound = std::max(maxRound, it.round);
  const int foldBelow = (int)maxRound - (pol.keepRounds - 1);  // rounds < this fold
  if (maxRound < 0 || foldBelow <= 0) return in;

  std::vector<TranscriptItem> out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); i++) {
    const TranscriptItem& it = in[i];
    const bool oldRound = it.round >= 0 && (int)it.round < foldBelow;
    if (!oldRound || it.pinned) {
      out.push_back(it);
      continue;
    }
    switch (it.kind) {
      case TranscriptItem::Kind::ToolUse: {
        // Fold ONLY as a complete pair: find this call's ToolResult. An orphan
        // (result not present - mid-round state) is kept verbatim so the
        // pairing invariant can never break here.
        size_t ri = 0;
        bool found = false;
        for (size_t j = i + 1; j < in.size(); j++) {
          if (in[j].kind == TranscriptItem::Kind::ToolResult && in[j].id == it.id) {
            ri = j;
            found = true;
            break;
          }
        }
        if (!found) {
          out.push_back(it);
          break;
        }
        const TranscriptItem& r = in[ri];
        TranscriptItem folded;
        folded.kind = TranscriptItem::Kind::User;  // renders as plain history text
        folded.round = it.round;
        std::string label = "[earlier round " + std::to_string((int)it.round) + "] " + it.name;
        std::string gist = r.text;
        // Keep an actionable results.get pointer intact if the clamp left one -
        // the fold must not orphan the fetch handle.
        folded.text = foldLine(label, gist, pol.lineMax) +
                      (r.isError ? " (errored)" : "") +
                      " (" + std::to_string((unsigned)r.text.size()) + " B)";
        out.push_back(std::move(folded));
        break;
      }
      case TranscriptItem::Kind::ToolResult: {
        // Skip: folded together with its ToolUse above. A result whose call is
        // MISSING from the vector (shouldn't happen) is kept verbatim.
        bool hasCall = false;
        for (size_t j = 0; j < i; j++) {
          if (in[j].kind == TranscriptItem::Kind::ToolUse && in[j].id == it.id) {
            hasCall = true;
            break;
          }
        }
        if (!hasCall) out.push_back(it);
        break;
      }
      case TranscriptItem::Kind::AssistantText:
      case TranscriptItem::Kind::User:
        out.push_back(it);  // prose is preserved (it is small and load-bearing)
        break;
    }
  }
  return out;
}

}  // namespace orch
}  // namespace nimbus
