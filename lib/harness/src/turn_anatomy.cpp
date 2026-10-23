#include "nimbus/harness/turn_anatomy.h"

#include "nimbus/harness/hooks.h"
#include "nimbus/harness/prompt_layout.h"
#include "nimbus/orch/failure_chain.h"   // FailureChain - why a turn failed (lane T)

namespace agent {

namespace {

constexpr size_t npos = std::string::npos;

// [b, e) of one part of the composed prompt, found without copying it (the
// prompt is tens of KB and this runs on the turn task).
struct Region {
  size_t b = 0, e = 0;
};
struct Regions {
  bool split = false;
  Region shelf, hist, dyn;
};
Regions regionsOf(const std::string& s) {
  Regions r;
  const size_t a = s.find(layout::kPartSplit);
  if (a == npos) {
    r.dyn = {0, s.size()};   // a legacy single string: search all of it
    r.shelf = r.hist = r.dyn;
    return r;
  }
  r.split = true;
  // static | shelf | history | dynamic | cache settings (prompt_layout.h)
  const size_t b = s.find(layout::kPartSplit, a + 1);
  const size_t c = s.find(layout::kPartSplit, b == npos ? a + 1 : b + 1);
  const size_t d = s.find(layout::kPartSplit, c == npos ? a + 1 : c + 1);
  r.shelf = {a + 1, b == npos ? s.size() : b};
  r.hist = {b == npos ? s.size() : b + 1, c == npos ? s.size() : c};
  r.dyn = {c == npos ? s.size() : c + 1, d == npos ? s.size() : d};
  return r;
}

bool lineStart(const std::string& s, size_t p, size_t regionBegin) {
  return p == regionBegin || s[p - 1] == '\n';
}

// The "## <name>" section inside `in`: from its heading to the next "## "
// heading (or the region end). {npos, npos} when absent.
Region sectionIn(const std::string& s, Region in, const char* name) {
  const std::string head = std::string("## ") + name;
  for (size_t p = s.find(head, in.b); p != npos && p < in.e; p = s.find(head, p + 1)) {
    if (!lineStart(s, p, in.b)) continue;
    size_t end = s.find("\n## ", p + 1);
    end = (end == npos || end >= in.e) ? in.e : end + 1;
    return {p, end};
  }
  return {npos, npos};
}

// Rows of a section: lines that start "- " (the window renders a message's
// continuation lines indented, so a list inside a message is not a row).
int rowsIn(const std::string& s, Region sec) {
  int n = 0;
  for (size_t p = s.find("- ", sec.b); p != npos && p < sec.e; p = s.find("- ", p + 1))
    if (p > sec.b && s[p - 1] == '\n') n++;
  return n;
}

// The block a heading at `at` sits in, padded for the legend column.
const char* partAt(size_t at, size_t shelfAt, size_t histAt, size_t dynAt) {
  if (at > dynAt) return "per-turn  ";
  if (histAt != npos && at > histAt) return "history   ";
  if (shelfAt != npos && at > shelfAt) return "shelf     ";
  return "static    ";
}

std::string plural(long n, const char* one, const char* many) {
  return std::to_string(n) + " " + (n == 1 ? one : many);
}

std::string chainLine(const TurnDebugEv& ev) {
  if (ev.loopTurn) return "none (stateless loop)";
  if (ev.convContinued) return "CONTINUED - the provider held history from prior turns";
  return "none (single shot)";
}

std::string historyLine(const HistoryFacts& h) {
  return "summary " + std::to_string((unsigned long)h.summaryBytes) + " B (clipped " +
         (h.summaryClipped ? "yes" : "no") + "), recent window " + plural(h.windowRows, "row", "rows") +
         " / " + std::to_string((unsigned long)h.windowBytes) + " bytes, running memory " +
         std::to_string((unsigned long)h.runningMemoryBytes) + " B, recalled " +
         plural(h.recalledItems, "item", "items");
}

// histV2: the history part's messages (one per kMsgSplit, 'u' or 'a' then its
// text) - their count, their total bytes, and the summary pair's bytes when it
// rides first.
void measureHistoryMessages(const std::string& s, const Region& hist, HistoryFacts& h) {
  int msgs = 0;
  size_t bytes = 0;
  bool first = true;
  for (size_t p = s.find(layout::kMsgSplit, hist.b);
       p != npos && p + 1 < hist.e; p = s.find(layout::kMsgSplit, p + 1)) {
    const size_t next = s.find(layout::kMsgSplit, p + 1);
    const size_t end = (next == npos || next > hist.e) ? hist.e : next;
    if (end > p + 2) {
      msgs++;
      bytes += end - (p + 2);
      if (first && s.compare(p + 2, 21, "[CONVERSATION SUMMARY") == 0)
        h.summaryBytes = end - (p + 2);
    }
    first = false;
    if (next == npos) break;
  }
  h.windowRows = msgs;
  h.windowBytes = bytes;
}

}  // namespace

HistoryFacts measureHistory(const std::string& instructions, const std::string& heldSummary,
                            const std::string& runningMemory) {
  HistoryFacts h;
  const Regions r = regionsOf(instructions);
  if (r.split) {
    measureHistoryMessages(instructions, r.hist, h);
  } else {
    const Region sec = sectionIn(instructions, r.hist, "CONVERSATION SUMMARY");
    if (sec.b != npos) h.summaryBytes = sec.e - sec.b;
  }
  // histV1 keeps the text window in the per-turn block: it is the window then.
  const Region win = sectionIn(instructions, r.dyn, "RECENT CONVERSATION");
  if (win.b != npos) {
    h.windowBytes = win.e - win.b;
    h.windowRows = rowsIn(instructions, win);
  }
  // Sent whole, the block the device held appears verbatim; cut or dropped, it does not.
  h.summaryClipped = !heldSummary.empty() && instructions.find(heldSummary) == npos;
  if (!runningMemory.empty() && instructions.find("[RUNNING MEMORY]\n" + runningMemory) != npos)
    h.runningMemoryBytes = runningMemory.size();
  const Region rec = sectionIn(instructions, r.dyn, "RELEVANT MEMORIES");
  if (rec.b != npos) h.recalledItems = rowsIn(instructions, rec);
  return h;
}

bool providerChainHeld(const std::string& convId) {
  return !convId.empty() && convId != "messages" && convId != "chat";
}

std::vector<std::string> promptHeadings(const std::string& prompt) {
  std::vector<std::string> out;
  for (size_t p = prompt.find("## "); p != npos; p = prompt.find("## ", p + 1)) {
    if (p > 0 && prompt[p - 1] != '\n') continue;
    const size_t eol = prompt.find('\n', p);
    out.push_back(prompt.substr(p + 3, eol == npos ? npos : eol - p - 3));
  }
  return out;
}

std::string sectionLegend(const std::string& prompt) {
  const std::vector<std::string> heads = promptHeadings(prompt);
  if (heads.empty()) return "Sections: none (this prompt has no \"## \" headings).\n";
  const size_t shelfAt = prompt.find(layout::kDisplayShelfLabel);
  const size_t histAt = prompt.find(layout::kDisplayHistoryLabel);
  const size_t dynAt = prompt.find(layout::kDisplayDynLabel);
  const bool split = dynAt != npos;
  std::string out = split ? "Sections (every \"## \" heading below, in order, with the block that carries it):\n"
                          : "Sections (every \"## \" heading below, in order):\n";
  size_t from = 0;
  for (const std::string& h : heads) {
    const size_t at = prompt.find("## " + h, from);
    from = at == npos ? from : at + 1;
    const char* part = split ? partAt(at, shelfAt, histAt, dynAt) : "";
    out += std::string("  ") + part + "## " + h + "\n";
  }
  out += split ? "Bracketed blocks such as [HOW YOU RUN], the catalogs and [RUNNING MEMORY] sit inside these "
                 "blocks; the history rides as real messages and the per-turn block starts at the "
                 "\"cached blocks end\" line.\n"
               : "Bracketed blocks such as [HOW YOU RUN] and [RUNNING MEMORY] sit between these sections.\n";
  return out;
}

// The result line: "ok", or a failed turn's causes, first cause first (lane T).
std::string resultLine(const TurnDebugEv& ev) {
  if (ev.ok) return "ok";
  if (ev.failure && !ev.failure->empty()) return "FAILED - " + ev.failure->render(240);
  return "FAILED";
}

std::string turnAnatomyHeader(const TurnDebugEv& ev) {
  static const std::string kNone;
  const std::string& prompt = ev.instructions ? *ev.instructions : kNone;
  return "===== NIMBUS TURN ANATOMY - the last completed turn =====\n"
         "host: " + ev.host + "\n"
         "provider chain: " + chainLine(ev) + "\n"
         "device history this turn: " + historyLine(ev.history) + "\n"
         "result: " + resultLine(ev) + "\n"
         "sections: 1 system prompt \xC2\xB7 2 per-turn input \xC2\xB7 3 raw model output \xC2\xB7\n"
         "4 tool-loop transcript (the mid-turn rounds, when the turn used tools).\n"
         "\n===== 1. INSTRUCTIONS - the system prompt sent this turn =====\n" +
         sectionLegend(prompt) + "\n";
}

}  // namespace agent
