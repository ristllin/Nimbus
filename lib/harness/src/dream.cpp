#include "nimbus/harness/dream.h"
#include "nimbus/harness/conversation.h"   // isClearBoundary - a /clear marker is not conversation
#include "nimbus/harness/jobs.h"           // isJobDelivery - captured job rows are not conversation
#include "nimbus/mem_cap.h"     // utf8CapLen - UTF-8-safe row clip (prism B)
#include "nimbus/orch/world.h"  // kConvOmittedNote - shared with assembleContext

#include <cstdio>

namespace agent {
namespace dream {

using nimbus::orch::EpisodicMessage;
using nimbus::orch::LoopRecord;
using nimbus::orch::SchedKind;

// ---- reserved identity ------------------------------------------------------

bool isReserved(const std::string& id) { return id == kLoopId; }

std::string cancelRefusal(const std::string& id) {
  if (!isReserved(id)) return "";
  return "the 'dream' loop is reserved firmware maintenance and cannot be "
         "deleted; the owner can pause it instead (web Loops tab, or "
         "/loop off dream)";
}

LoopRecord reservedLoopRecord() {
  LoopRecord l;
  l.id = kLoopId;
  l.name = "Dream";
  l.prompt =
      "Nightly maintenance + reflection: memory decay/prune/dedup, then distill "
      "yesterday into durable memories. Inputs are built fresh each night; "
      "pause with /loop off dream.";
  l.chatId = "";  // owner default channel
  l.sched.kind = SchedKind::Daily;
  l.sched.minuteOfDay = kDefaultMinuteOfDay;  // 03:30 local
  l.sched.weekMask = 0x7F;
  l.createdBy = nimbus::orch::CreatedBy::Owner;
  l.enabled = true;
  l.approved = true;  // owner-shipped firmware behavior, not an agent creation
  return l;
}

// ---- idle gate --------------------------------------------------------------

GateResult evaluateGate(const DreamGate& g, const GateInputs& in) {
  // Unsigned subtraction is millis()-wraparound-safe; lastTurnEndMs==0 makes
  // the elapsed time "since boot", so a freshly booted device waits out one
  // quiet window before dreaming.
  if ((uint32_t)(in.nowMs - in.lastTurnEndMs) < g.minQuietMs)
    return {g.deferSec, "recent-turn"};
  if (g.requireNoJobs && in.activeJobs > 0) return {g.deferSec, "active-jobs"};
  if (in.freeHeap < g.minHeap) return {g.deferSec, "low-heap"};
  return {};
}

// ---- prompt assembly --------------------------------------------------------

bool skipReflection(bool digestEmpty, uint64_t scratchHash, uint64_t lastHash,
                    bool force) {
  // Skip the PAID stage-2 turn only when there is provably nothing to reflect
  // on: no owner conversation in 24 h AND the scratchpad is byte-identical to
  // where the LAST dream left it. lastHash==0 = no baseline yet (first night,
  // or a pre-feature device) -> run. force = the console DREAM drill -> run.
  return !force && digestEmpty && lastHash != 0 && scratchHash == lastHash;
}

namespace {
std::string previewRow(const EpisodicMessage& m);   // defined with the window helpers below
}  // namespace

Yesterday readYesterday(const nimbus::orch::EpisodicStore& store, uint32_t nowHours,
                        const std::vector<std::string>& sessions) {
  using nimbus::orch::MsgKind;
  nimbus::orch::MsgQuery q;
  q.sinceHours = nowHours > 24 ? nowHours - 24 : 0;
  q.limit = kYesterdayQueryRows;
  q.sessionAllow = sessions;
  q.haveKind = true;
  q.kind = MsgKind::Message;
  q.alsoKinds = {MsgKind::Image, MsgKind::File, MsgKind::Audio};
  std::vector<EpisodicMessage> rows;
  for (EpisodicMessage& m : store.query(q)) {
    if (agent::isJobDelivery(m.tags) || agent::isClearBoundary(m)) continue;
    if ((int)rows.size() >= kYesterdayRows) break;
    rows.push_back(std::move(m));
  }
  Yesterday y;
  y.empty = rows.empty();
  y.digest = buildEpisodicDigest(rows);
  return y;
}

std::string buildEpisodicDigest(const std::vector<EpisodicMessage>& msgs,
                                size_t capBytes) {
  if (msgs.empty()) return "(no episodic messages captured in the last day)\n";
  // Collect newest-first (the query order) so the budget clips the OLDEST
  // lines, then emit oldest-first for narrative order.
  std::vector<std::string> lines;
  size_t total = 0;
  bool clipped = false;
  for (const EpisodicMessage& m : msgs) {
    std::string line = previewRow(m);   // one flattened line, 300 B UTF-8-safe clip
    if (total + line.size() > capBytes) {
      clipped = true;
      break;
    }
    total += line.size();
    lines.push_back(std::move(line));
  }
  std::string out;
  out.reserve(total + 48);
  if (clipped) out += nimbus::orch::kConvOmittedNote;
  for (auto it = lines.rbegin(); it != lines.rend(); ++it) out += *it;
  return out;
}

namespace {
const char* roleOf(const EpisodicMessage& m) { return m.role.empty() ? "?" : m.role.c_str(); }

unsigned char byteAt(const std::string& t, size_t i) {
  return i < t.size() ? (unsigned char)t[i] : 0;
}

// NEL (C2 85), LINE SEPARATOR (E2 80 A8), PARAGRAPH SEPARATOR (E2 80 A9).
size_t unicodeBreakAt(const std::string& t, size_t i) {
  if (byteAt(t, i) == 0xC2 && byteAt(t, i + 1) == 0x85) return 2;
  if (byteAt(t, i) != 0xE2 || byteAt(t, i + 1) != 0x80) return 0;
  return (byteAt(t, i + 2) == 0xA8 || byteAt(t, i + 2) == 0xA9) ? 3 : 0;
}

// Bytes of a line break at t[i], or 0. Covers what a model may render as a new
// line, not just LF: CR / CRLF, VT, FF, FS..RS, NEL (U+0085) and the Unicode
// LINE / PARAGRAPH SEPARATORs (U+2028 / U+2029) - a message must not be able to
// start a fresh "- assistant:" row or a "## " header with any of them (prism).
size_t lineBreakAt(const std::string& t, size_t i) {
  const unsigned char c = byteAt(t, i);
  if (c == '\r') return byteAt(t, i + 1) == '\n' ? 2 : 1;
  if (c == '\n' || c == '\v' || c == '\f' || (c >= 0x1C && c <= 0x1E)) return 1;
  return unicodeBreakAt(t, i);
}

// THE one break normalizer every rendered row goes through (whole rows, the
// window's previews, the digest - so the brief, the fold and the nightly digest
// too): each lineBreakAt() break becomes `sep`. A row-renderer that flattened
// only LF/CR let U+2028 + "- owner: ..." through on the preview path (gate W2).
std::string replaceBreaks(const std::string& t, const char* sep) {
  std::string out;
  out.reserve(t.size() + 16);
  for (size_t i = 0; i < t.size();) {
    const size_t br = lineBreakAt(t, i);
    if (br) {
      out += sep;
      i += br;
    } else {
      out += t[i++];
    }
  }
  return out;
}

// A row kept whole: verbatim, every line break normalized to LF and followed by
// a two-space indent (code keeps its relative indentation; a list inside the
// message can never read as a row start, and no line inside a row can open a
// "## " header).
std::string wholeRow(const EpisodicMessage& m) {
  std::string line = std::string("- ") + roleOf(m) + ": " + replaceBreaks(m.text, "\n  ");
  while (!line.empty() && (line.back() == ' ' || line.back() == '\n')) line.pop_back();
  line += '\n';
  return line;
}

// The one-line preview (the digest's row shape): every line break flattened to a
// space, clipped UTF-8-safe to kWindowPreviewBytes with a "..." marker. UTF-8
// safety matters (prism B): a raw byte resize tore multi-byte chars, and the torn
// row then poisoned EVERY turn of that chat with invalid JSON (sticky 400s).
std::string previewRow(const EpisodicMessage& m) {
  std::string t = replaceBreaks(m.text, " ");
  if (t.size() > kWindowPreviewBytes) {
    t.resize((size_t)nimbus::utf8CapLen(t.c_str(), (int)t.size(), (int)kWindowPreviewBytes - 3));
    t += "...";
  }
  return std::string("- ") + roleOf(m) + ": " + t + "\n";
}
}  // namespace

std::string buildConversationWindow(const std::vector<EpisodicMessage>& msgs,
                                    size_t capBytes) {
  std::vector<std::string> rows;   // newest-first; emitted reversed
  size_t total = 0;
  bool clipped = false;
  for (size_t i = 0; i < msgs.size(); ++i) {
    std::string row = wholeRow(msgs[i]);
    if (total + row.size() > capBytes) {
      const size_t room = capBytes - total;
      if (i == 0 && room > kWindowPreviewBytes + 8) {
        // Rule 2: the newest row alone is bigger than the budget - keep its head.
        row.resize((size_t)nimbus::utf8CapLen(row.c_str(), (int)row.size(), (int)room - 4));
        row += "...\n";
      } else {
        row = previewRow(msgs[i]);
        if (total + row.size() > capBytes) {
          clipped = true;
          break;
        }
      }
    }
    total += row.size();
    rows.push_back(std::move(row));
  }
  std::string out;
  out.reserve(total + sizeof(nimbus::orch::kConvOmittedNote));
  if (clipped) out += nimbus::orch::kConvOmittedNote;
  for (auto it = rows.rbegin(); it != rows.rend(); ++it) out += *it;
  return out;
}

std::string buildDreamInputs(const std::string& episodicDigest,
                             const std::string& scratchpadSummary,
                             const MemStats& s) {
  std::string digest = episodicDigest;
  if (digest.size() > kDigestCapBytes) {  // defensive re-cap (callers may bypass the builder)
    digest.resize(kDigestCapBytes);
    digest += "\n...(truncated at byte budget)\n";
  }
  char stats[176];
  snprintf(stats, sizeof stats,
           "vectors=%d (pruned tonight=%d, deduplicated=%d)  scratchpad items=%d  "
           "episodic messages=%d",
           s.vectors, s.pruned, s.deduped, s.scratchItems, s.episodicMsgs);
  std::string out;
  out.reserve(digest.size() + scratchpadSummary.size() + 1024);
  out +=
      "[DREAM]\n"
      "This is your nightly reflection window; the owner is likely asleep and no "
      "message is waiting. Do all of:\n"
      "(a) distill yesterday's conversations (below) into up to 7 DURABLE "
      "mem_write facts worth keeping - preferences, decisions, ongoing "
      "projects, corrections. 0 is the correct number on a quiet day; never "
      "invent facts to fill a quota. Skip trivia, greetings, and anything "
      "already stored;\n"
      "(b) groom your scratchpad: drop stale or completed short-term goals, keep "
      "mid/long-term goals current;\n"
      "(c) refresh your running `memory` summary so it reflects where things "
      "stand;\n"
      "(d) if memories contradict each other or an owner question was left "
      "unresolved, flag it.\n"
      "Reply with an EMPTY string unless something genuinely needs the owner's "
      "attention - never send a routine status report.\n";
  out += "\n[MEMORY STATS]\n";
  out += stats;
  out += "\n";
  if (!scratchpadSummary.empty()) {
    out += "\n";
    out += scratchpadSummary;
    if (out.back() != '\n') out += '\n';
  }
  out += "\n[YESTERDAY]\n";
  out += digest;
  return out;
}

}  // namespace dream
}  // namespace agent
