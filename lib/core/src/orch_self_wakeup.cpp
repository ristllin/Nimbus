#include "nimbus/orch/self_wakeup.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>

// Self-wakeups (owner design, 9 Nov + 11 Nov rulings) - the pure core. See
// nimbus/orch/self_wakeup.h for the design; this file owns every decision:
// quiet-hours math, the one-pending-per-kind arming bound, the due-within-24 h
// bound, the note envelope, and the wake-time re-check.

namespace nimbus {
namespace orch {

bool inQuietHours(uint16_t minuteOfDay, uint16_t quietEndMinute) {
  const uint16_t m = minuteOfDay % 1440u;
  const uint16_t end = quietEndMinute > 1439u ? 1439u : quietEndMinute;
  // Quiet hours run from 22:00 to the quiet-end minute. An end at or past 22:00
  // is the SAME evening (no wrap): [22:00, end), empty when end == 22:00. An end
  // before 22:00 is the following morning: the overnight window [22:00, end)
  // wrapping midnight.
  if (end >= kQuietHoursStart) return m >= kQuietHoursStart && m < end;
  return m >= kQuietHoursStart || m < end;
}

const char* wakeKindSlug(WakeKind k) {
  switch (k) {
    case WakeKind::Upkeep:   return "upkeep";
    case WakeKind::Routine:  return "routine";
    case WakeKind::Subagent: return "subagent";
  }
  return "upkeep";
}

bool isSelfWakeupId(const std::string& id) {
  return id.compare(0, 2, kSelfWakeupIdPrefix) == 0 && id.size() > 2;
}

std::string dumpWakeNote(const WakeNote& n) {
  ArduinoJson::JsonDocument d;
  d["selfWakeup"] = true;
  d["kind"] = wakeKindSlug(n.kind);
  d["text"] = n.text;
  d["noticedEpoch"] = (uint64_t)n.noticedEpoch;
  d["noticedMinute"] = (uint16_t)n.noticedMinute;
  d["notAfterEpoch"] = (uint64_t)n.notAfterEpoch;
  d["dedupKey"] = (uint64_t)n.dedupKey;
  std::string out;
  ArduinoJson::serializeJson(d, out);
  return out;
}

bool parseWakeNote(const std::string& prompt, WakeNote& out) {
  ArduinoJson::JsonDocument d;
  if (ArduinoJson::deserializeJson(d, prompt)) return false;
  if (!d["selfWakeup"].is<bool>() || !d["selfWakeup"].as<bool>()) return false;
  const char* kind = d["kind"] | "";
  if (!strcmp(kind, "upkeep")) out.kind = WakeKind::Upkeep;
  else if (!strcmp(kind, "routine")) out.kind = WakeKind::Routine;
  else if (!strcmp(kind, "subagent")) out.kind = WakeKind::Subagent;
  else return false;
  out.text = std::string(d["text"] | "");
  out.noticedEpoch = (uint64_t)(d["noticedEpoch"] | 0ULL);
  out.noticedMinute = (uint16_t)(d["noticedMinute"] | 0);
  out.notAfterEpoch = (uint64_t)(d["notAfterEpoch"] | 0ULL);
  out.dedupKey = (uint64_t)(d["dedupKey"] | 0ULL);
  return true;
}

uint32_t selfWakeupDelaySec(uint64_t nowEpoch, uint16_t nowMinute, uint16_t quietEndMinute) {
  const uint16_t end = quietEndMinute > 1439u ? 1439u : quietEndMinute;
  const uint16_t now = nowMinute % 1440u;
  // Minutes until the next occurrence of `end` strictly after now (1..1440).
  uint32_t mins = (uint32_t)((1440u + end - now) % 1440u);
  if (mins == 0) mins = 1440u;
  uint32_t secs = mins * 60u;
  const uint32_t intoMinute = (uint32_t)(nowEpoch % 60u);
  if (secs > intoMinute) secs -= intoMinute;
  if (secs < kWakeupMinSec) secs = kWakeupMinSec;          // the wakeup floor (2 min)
  if (secs > kSelfWakeupMaxSec) secs = kSelfWakeupMaxSec;  // due within 24 h, hard bound
  return secs;
}

bool hasPendingSelfWakeup(const std::vector<LoopRecord>& loops, WakeKind kind) {
  for (const LoopRecord& l : loops) {
    if (!isSelfWakeupId(l.id) || !l.enabled || l.sched.kind != SchedKind::Once) continue;
    WakeNote n;
    if (parseWakeNote(l.prompt, n) && n.kind == kind) return true;
  }
  return false;
}

std::string armSelfWakeup(std::vector<LoopRecord>& loops, const WakeNote& note,
                          uint64_t nowEpoch, uint16_t nowMinute, uint16_t quietEndMinute) {
  if (note.text.empty()) return "";
  if (!inQuietHours(nowMinute, quietEndMinute)) return "";   // not quiet hours: no wakeup, no hold
  if (hasPendingSelfWakeup(loops, note.kind)) return "";     // at most one pending per kind
  LoopRecord l;
  // "sw" + 6 hex, unique against the table (routine ids are "lp"+hex).
  uint64_t seed = fnv64(wakeKindSlug(note.kind) + note.text) ^ (nowEpoch * 2654435761ULL);
  for (int tries = 0; tries < 8; ++tries) {
    char b[10];
    snprintf(b, sizeof b, "sw%06x", (unsigned)(seed & 0xFFFFFF));
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    bool clash = false;
    for (const LoopRecord& e : loops) if (e.id == b) { clash = true; break; }
    if (clash) continue;
    l.id = b;
    break;
  }
  if (l.id.empty()) return "";
  l.name = std::string("Self-wakeup: ") + wakeKindSlug(note.kind);
  if (l.name.size() > kLoopNameMax) l.name.resize(kLoopNameMax);
  l.prompt = dumpWakeNote(note);
  if (l.prompt.size() > kLoopPromptMax) l.prompt.resize(kLoopPromptMax);
  l.chatId = "";   // the owner default channel
  l.sched.kind = SchedKind::Once;
  l.sched.intervalSec = selfWakeupDelaySec(nowEpoch, nowMinute, quietEndMinute);
  l.createdBy = CreatedBy::Agent;
  l.enabled = true;
  l.approved = true;   // 11 Nov ruling: a single self-wakeup needs NO owner approval
  l.nextRun = nowEpoch + l.sched.intervalSec;
  loops.push_back(l);
  return l.id;
}

bool wakeNoteStale(const WakeNote& n, uint64_t nowEpoch) {
  return n.notAfterEpoch != 0 && nowEpoch > n.notAfterEpoch;
}

bool wakeNoteAlreadyDelivered(const WakeNote& n, const std::vector<uint64_t>& deliveredKeys) {
  if (n.dedupKey == 0) return false;
  for (uint64_t k : deliveredKeys) if (k == n.dedupKey) return true;
  return false;
}

std::string wakeupDeliveryText(const WakeNote& n) {
  char at[8];
  snprintf(at, sizeof at, "%02u:%02u", (unsigned)((n.noticedMinute / 60) % 24),
           (unsigned)(n.noticedMinute % 60));
  return std::string("Noticed at ") + at + ":\n" + n.text;
}

std::string wakeupRecheck(const WakeNote& n, uint64_t nowEpoch,
                          const std::vector<uint64_t>& deliveredKeys) {
  if (n.text.empty()) return "";
  if (wakeNoteStale(n, nowEpoch)) return "";
  if (wakeNoteAlreadyDelivered(n, deliveredKeys)) return "";
  return wakeupDeliveryText(n);
}

std::vector<uint64_t> deliveredNoteKeys(const std::vector<LoopRecord>& loops,
                                        const std::string& exceptId) {
  std::vector<uint64_t> keys;
  for (const LoopRecord& l : loops) {
    if (l.id == exceptId || isSelfWakeupId(l.id)) continue;
    if (l.lastReplyHash != 0) keys.push_back(l.lastReplyHash);
  }
  return keys;
}

int restrictSelfWakeupTurn(Turn& t) {
  const int n = (int)(t.device.size() + t.spawn.size() + t.await_.size() +
                      t.session_ops.size() + t.mem_write.size() + t.mem_query.size()) +
                (t.scratchpad.present ? 1 : 0);
  t.device.clear();
  t.spawn.clear();
  t.await_.clear();
  t.session_ops.clear();
  t.mem_write.clear();
  t.mem_query.clear();
  t.scratchpad = ScratchUpdate{};
  return n;
}

}  // namespace orch
}  // namespace nimbus
