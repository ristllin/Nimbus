#include "nimbus/harness/dream.h"
#include "nimbus/mem_cap.h"   // utf8CapLen - UTF-8-safe row clip (prism B)

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

std::string buildEpisodicDigest(const std::vector<EpisodicMessage>& msgs,
                                size_t capBytes) {
  if (msgs.empty()) return "(no episodic messages captured in the last day)\n";
  // Collect newest-first (the query order) so the budget clips the OLDEST
  // lines, then emit oldest-first for narrative order.
  std::vector<std::string> lines;
  size_t total = 0;
  bool clipped = false;
  for (const EpisodicMessage& m : msgs) {
    std::string line = "- ";
    line += m.role.empty() ? "?" : m.role.c_str();
    line += ": ";
    std::string t = m.text;
    for (char& c : t)
      if (c == '\n' || c == '\r') c = ' ';
    if (t.size() > 300) {
      // UTF-8-safe (prism B): a raw byte resize tore multi-byte chars; the torn
      // row then poisoned EVERY turn of that chat with invalid JSON (sticky
      // 400s) once B1/B4 promoted this digest to the hot path.
      t.resize((size_t)nimbus::utf8CapLen(t.c_str(), (int)t.size(), 297));
      t += "...";
    }
    line += t;
    line += '\n';
    if (total + line.size() > capBytes) {
      clipped = true;
      break;
    }
    total += line.size();
    lines.push_back(std::move(line));
  }
  std::string out;
  out.reserve(total + 48);
  if (clipped) out += "(earlier messages omitted - byte budget)\n";
  for (auto it = lines.rbegin(); it != lines.rend(); ++it) out += *it;
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
