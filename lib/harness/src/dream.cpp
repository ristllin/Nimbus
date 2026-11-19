#include "nimbus/harness/dream.h"
#include "nimbus/harness/conversation.h"   // tagValue, splitRefs - a row's skills: suffix
#include "nimbus/mem_cap.h"     // utf8CapLen - UTF-8-safe row clip (prism B)
#include "nimbus/orch/world.h"  // kConvOmittedNote - shared with assembleContext

#include <ArduinoJson.h>

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
      "Nightly memory upkeep: decay, prune and dedup of long-term memories. No model "
      "call. Pause with /loop off dream.";
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
  // quiet window before dreaming. Busy defers are bounded per night (maxDefers);
  // the heap floor is not.
  const bool busyCapped = in.defersTonight >= g.maxDefers;
  if (!busyCapped && (uint32_t)(in.nowMs - in.lastTurnEndMs) < g.minQuietMs)
    return {g.deferSec, "recent-turn"};
  if (!busyCapped && g.requireNoJobs && in.activeJobs > 0) return {g.deferSec, "active-jobs"};
  if (in.freeHeap < g.minHeap) return {g.deferSec, "low-heap"};
  return {};
}


// ---- the conversation digest and window (not the dream's: kept here until RB-Z0 moves them)

namespace {
std::string previewRow(const EpisodicMessage& m);   // defined with the window helpers below
}  // namespace

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
  // The omission note rides INSIDE the cap: drop more of the oldest lines for
  // it. (It used to ride on top, so a full 8 KB digest overran kDigestCapBytes and
  // buildDreamInputs' re-cap cut the NEWEST row mid-character - invalid UTF-8.)
  const size_t noteBytes = sizeof(nimbus::orch::kConvOmittedNote) - 1;
  while (clipped && !lines.empty() && total + noteBytes > capBytes) {
    total -= lines.back().size();
    lines.pop_back();
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

// THE one break normalizer every conversation row rendered here goes through
// (whole rows, the window's previews, the digest - so the brief, the fold and the
// nightly digest too): each lineBreakAt() break becomes `sep`. A row-renderer that flattened
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
  // F1 (lane I): an assistant row that used or had skills attached carries the
  // skills: tag; render it as the suffix the model can answer "did you use the
  // skill for that?" from. Unknown tags on other roles are ignored. The ids
  // join with '.' inside the tag (a comma would read as the next tag item).
  const std::string sk = tagValue(m.tags, kSkillsTag);
  if (!sk.empty()) {
    std::string joined;
    for (const auto& id : splitRefs(sk)) joined += (joined.empty() ? "" : ", ") + id;
    if (!joined.empty()) line += " (skills: " + joined + ")";
  }
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

// ---- outcome and state ----------------------------------------------------------

const char* outcomeSlug(DreamOutcome o) {
  switch (o) {
    case DreamOutcome::Ran:      return "ran";
    case DreamOutcome::Skipped:  return "skipped";
    case DreamOutcome::Starved:  return "starved";
  }
  return "ran";
}

std::string dumpDreamState(const DreamState& s) {
  JsonDocument d;
  d["v"] = 1;
  d["maintDay"] = s.maintDay;
  std::string out;
  serializeJson(d, out);
  return out;
}

bool loadDreamState(const std::string& json, DreamState& out) {
  if (json.size() > kStateFileMax) return false;
  JsonDocument d;
  if (deserializeJson(d, json) != DeserializationError::Ok || !d.is<JsonObject>()) return false;
  DreamState s;
  s.maintDay = d["maintDay"] | 0u;   // the fields earlier firmware wrote beside it are ignored
  out = s;
  return true;
}

// ---- the night ------------------------------------------------------------------

MaintResult maintainVectors(nimbus::orch::VectorMemory& v, float decayFactor, uint32_t nowHours,
                            int dedupWindow, const std::function<bool()>& persist) {
  MaintResult m;
  m.ran = true;
  v.decayImportance(decayFactor);
  m.pruned = v.pruneExpired(nowHours);
  m.deduped = v.deduplicate(dedupWindow);
  m.vectors = v.size();
  m.persisted = persist ? persist() : false;
  return m;
}

agent::DreamEndEv dreamEndEvent(const NightReport& rep) {
  agent::DreamEndEv ev;
  ev.prunedVectors = rep.maint.pruned;
  ev.dedupedVectors = rep.maint.deduped;
  ev.report = &rep;
  return ev;
}

NightResult runNight(const NightDeps& d, const NightClock& c) {
  NightResult res;
  NightReport& r = res.report;
  DreamState st;
  if (d.memo && d.memo->loaded) {
    st = d.memo->st;   // this boot's copy: the file may lag behind it
  } else {
    if (!d.loadState || !d.loadState(st)) st = DreamState{};
    if (d.memo) {
      d.memo->loaded = true;
      d.memo->st = st;
    }
  }
  // Maintenance decays importance, so it is not idempotent: at most once per local day,
  // and only on a synced clock (a boot-relative clock would corrupt every age).
  if (c.synced && st.maintDay != c.localDay) {
    if (d.maintain) r.maint = d.maintain();
    st.maintDay = c.localDay;
    r.outcome = DreamOutcome::Ran;
    // RAM moves first, so a store that is down never runs the decay twice in a day. A
    // store that is not mounted is not written at all.
    if (d.memo) d.memo->st = st;
    if (d.storeReady && !d.storeReady()) {
      r.stateSaved = false;
      r.stateWhy = "storage not mounted";
    } else if (!d.saveState || !d.saveState(st)) {
      r.stateSaved = false;
      r.stateWhy = "storage write failed";
    }
  } else {
    r.outcome = DreamOutcome::Skipped;
    r.maintDoneToday = c.synced;
  }
  nimbus::orch::FireOutcome& o = res.fire;
  o.ok = true;
  // Varies by day so the semantic-repeat guard never pauses the upkeep for doing its job.
  char fp[96];
  snprintf(fp, sizeof fp, "(dream d%lu: %s pruned=%d deduped=%d)", (unsigned long)(c.epoch / 86400),
           outcomeSlug(r.outcome), r.maint.pruned, r.maint.deduped);
  o.detail = fp;
  if (d.emitRow) d.emitRow(dreamEndEvent(r));
  return res;
}

// ---- the row ----------------------------------------------------------------------

std::string renderDreamRow(const NightReport& r, size_t cap) {
  char b[160];
  if (r.outcome == DreamOutcome::Starved) {
    snprintf(b, sizeof b, "Nightly dream skipped tonight (%s after %u tries); maintenance not run tonight",
             r.cause == "low-heap" ? "low memory" : "device busy", (unsigned)r.defers);
  } else if (r.maint.ran) {
    snprintf(b, sizeof b, "Nightly dream ok: maintenance pruned=%d deduped=%d (%d memories)%s",
             r.maint.pruned, r.maint.deduped, r.maint.vectors, r.maint.persisted ? "" : ", not saved");
  } else {
    snprintf(b, sizeof b, "Nightly dream skipped (%s)",
             r.maintDoneToday ? "maintenance done earlier today" : "clock not set");
  }
  std::string row = b;
  if (!r.stateSaved) row += "; state not saved (" + r.stateWhy + ")";
  if (row.size() > cap) {
    row.resize((size_t)nimbus::utf8CapLen(row.c_str(), (int)row.size(), (int)cap - 3));
    row += "...";
  }
  return row;
}

uint16_t windowEndFor(uint16_t slotMinute) {
  const uint32_t end = (uint32_t)slotMinute + (kWindowEndMinute - kDefaultMinuteOfDay);
  return (uint16_t)(end > 1439u ? 1439u : end);
}

uint32_t secondsToNextSlot(const NightClock& c, uint16_t slot) {
  uint32_t mins = (1440u + slot - (c.minuteOfDay % 1440u)) % 1440u;
  if (mins == 0) mins = 1440u;
  const uint32_t secs = mins * 60u;
  const uint32_t intoMinute = (uint32_t)(c.epoch % 60u);
  return secs > intoMinute ? secs - intoMinute : secs;
}

}  // namespace dream
}  // namespace agent
