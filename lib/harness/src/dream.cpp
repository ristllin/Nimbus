#include "nimbus/harness/dream.h"
#include "nimbus/harness/conversation.h"   // isClearBoundary - a /clear marker is not conversation
#include "nimbus/harness/jobs.h"           // isJobDelivery - captured job rows are not conversation
#include "nimbus/mem_cap.h"     // utf8CapLen - UTF-8-safe row clip (prism B)
#include "nimbus/orch/world.h"  // kConvOmittedNote - shared with assembleContext

#include <ArduinoJson.h>

#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>

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
  // quiet window before dreaming. Busy defers are bounded per night (maxDefers);
  // the heap floor is not.
  const bool busyCapped = in.defersTonight >= g.maxDefers;
  if (!busyCapped && (uint32_t)(in.nowMs - in.lastTurnEndMs) < g.minQuietMs)
    return {g.deferSec, "recent-turn"};
  if (!busyCapped && g.requireNoJobs && in.activeJobs > 0) return {g.deferSec, "active-jobs"};
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
  q.limit = kYesterdayRows;
  q.sessionAllow = sessions;
  q.haveKind = true;
  q.kind = MsgKind::Message;
  q.alsoKinds = {MsgKind::Image, MsgKind::File, MsgKind::Audio};
  std::vector<EpisodicMessage> rows;
  for (int page = 0; page < kYesterdayPages && (int)rows.size() < kYesterdayRows; ++page) {
    std::vector<EpisodicMessage> got = store.query(q);
    // A full page may have older rows behind it: continue strictly below its
    // oldest row (the store's id cursor). Read before the rows are moved out.
    const bool more = (int)got.size() == q.limit && !got.back().id.empty();
    if (more) q.before = got.back().id;
    for (EpisodicMessage& m : got) {
      if (agent::isJobDelivery(m.tags) || agent::isClearBoundary(m)) continue;
      if ((int)rows.size() >= kYesterdayRows) break;
      rows.push_back(std::move(m));
    }
    if (!more) break;
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
                             const MemStats& s, bool memoryWritesDown) {
  std::string digest = episodicDigest;
  if (digest.size() > kDigestCapBytes) {  // defensive re-cap (callers may bypass the builder)
    digest.resize((size_t)nimbus::utf8CapLen(digest.c_str(), (int)digest.size(), (int)kDigestCapBytes));
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
  if (memoryWritesDown)
    out +=
        "(long-term memory writes are unavailable tonight - do not attempt "
        "memory.write/search; do (b) and (c) only.)\n";
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

// ---- the dream turn's surface (lane M) ---------------------------------------

bool toolAllowed(const std::string& n) {
  static const char* const kAllow[] = {"memory.write",  "memory.update",     "memory.search",
                                       "memory.pin",    "memory.scratchpad", "memory.episodic"};
  for (const char* a : kAllow)
    if (n == a) return true;
  return false;
}

int restrictTurn(nimbus::orch::Turn& t) {
  const int n = (int)(t.device.size() + t.spawn.size() + t.await_.size() + t.session_ops.size());
  t.device.clear();
  t.spawn.clear();
  t.await_.clear();
  t.session_ops.clear();
  return n;
}

// ---- outcome ------------------------------------------------------------------

const char* outcomeSlug(DreamOutcome o) {
  switch (o) {
    case DreamOutcome::Ran:      return "ran";
    case DreamOutcome::Skipped:  return "skipped";
    case DreamOutcome::Deferred: return "deferred";
    case DreamOutcome::Failed:   return "failed";
    case DreamOutcome::Degraded: return "degraded";
    case DreamOutcome::Starved:  return "starved";
  }
  return "ran";
}

static bool outcomeFromSlug(const std::string& s, DreamOutcome& out) {
  for (uint8_t i = 0; i <= (uint8_t)DreamOutcome::Starved; ++i)
    if (s == outcomeSlug((DreamOutcome)i)) {
      out = (DreamOutcome)i;
      return true;
    }
  return false;
}

namespace {
// First match wins; the order matters (billing before rate limits: OpenAI says
// "no credits" with a 429; a key error before the generic 4xx).
struct CauseRule {
  const char* slug;
  const char* needles[4];
};
const CauseRule kCauseRules[] = {
    {"low-memory", {"low memory", "out of memory", nullptr, nullptr}},
    {"budget", {"budget:", nullptr, nullptr, nullptr}},
    {"parse", {"parse:", "schema parse", nullptr, nullptr}},
    {"billing", {"credit", "billing", "insufficient_quota", "funding_cap"}},
    {"key", {"http 401", "http 403", "key", nullptr}},
    {"rate-limit", {"http 429", "rate limit", "rate_limit", nullptr}},
    {"timeout", {"timeout", "timed out", nullptr, nullptr}},
    {"provider-5xx", {"http 5", nullptr, nullptr, nullptr}},
    {"bad-request", {"http 4", nullptr, nullptr, nullptr}},
    {"network", {"network", "connect", "no response", "noresp"}},
};
}  // namespace

std::string causeOf(const std::string& error, bool deferred) {
  if (deferred) return "low-memory";
  if (error.empty()) return "network";
  std::string e = error;
  for (char& ch : e) ch = (char)std::tolower((unsigned char)ch);
  for (const CauseRule& rule : kCauseRules)
    for (const char* n : rule.needles)
      if (n && e.find(n) != std::string::npos) return rule.slug;
  return "other";
}

bool causeRetryable(const std::string& c) {
  return c == "low-memory" || c == "timeout" || c == "network" || c == "rate-limit" ||
         c == "provider-5xx";
}

// ---- state ----------------------------------------------------------------------

std::string dumpDreamState(const DreamState& s) {
  JsonDocument d;
  d["v"] = 1;
  d["maintDay"] = s.maintDay;
  char hex[17];
  snprintf(hex, sizeof hex, "%016" PRIx64, s.scrHash);
  d["scrHash"] = hex;
  d["attemptDay"] = s.attemptDay;
  d["attempts"] = s.attemptsTonight;
  d["streakDay"] = s.streakDay;
  d["streak"] = s.streak;
  d["last"] = outcomeSlug(s.lastOutcome);
  d["cause"] = s.lastCause;
  if (s.inflightNight) {   // only while set: a finished night's file keeps this format
    d["inNight"] = s.inflightNight;
    d["inAttempt"] = s.inflightAttempt;
    char st[17];
    snprintf(st, sizeof st, "%016" PRIx64, s.inflightStart);
    d["inStart"] = st;
  }
  std::string out;
  serializeJson(d, out);
  return out;
}

bool loadDreamState(const std::string& json, DreamState& out) {
  JsonDocument d;
  if (deserializeJson(d, json) != DeserializationError::Ok || !d.is<JsonObject>()) return false;
  DreamState s;
  s.maintDay = d["maintDay"] | 0u;
  s.scrHash = strtoull(d["scrHash"] | "0", nullptr, 16);
  s.attemptDay = d["attemptDay"] | 0u;
  const unsigned att = d["attempts"] | 0u;
  s.attemptsTonight = (uint8_t)(att > 255 ? 255 : att);
  s.streakDay = d["streakDay"] | 0u;
  const unsigned st = d["streak"] | 0u;
  s.streak = (uint8_t)(st > 255 ? 255 : st);
  if (!outcomeFromSlug(d["last"] | "ran", s.lastOutcome)) s.lastOutcome = DreamOutcome::Ran;
  s.lastCause = d["cause"] | "";
  if (s.lastCause.size() > 32) s.lastCause.resize(32);
  s.inflightNight = d["inNight"] | 0u;
  const unsigned ia = d["inAttempt"] | 0u;
  s.inflightAttempt = (uint8_t)(ia > 255 ? 255 : ia);
  s.inflightStart = strtoull(d["inStart"] | "0", nullptr, 16);
  out = s;
  return true;
}

DreamState stateFromLegacyHash(const std::string& hex) {
  DreamState s;
  s.scrHash = hex.empty() ? 0 : strtoull(hex.c_str(), nullptr, 16);
  return s;
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

namespace {

bool alertDue(uint8_t streak) {
  if (streak < kAlertStreak) return false;
  return streak == kAlertStreak || (streak - kAlertStreak) % kAlertEvery == 0;
}

// A night's outcome from what the turn returned and what its writes did.
void classify(const nimbus::orch::FireOutcome& o, NightReport& r) {
  if (o.deferred) {
    r.outcome = DreamOutcome::Deferred;
    r.cause = "low-memory";
  } else if (!o.ok) {
    r.outcome = DreamOutcome::Failed;
    r.cause = causeOf(o.error, false);
  } else if (r.ops.failed > 0) {
    r.outcome = DreamOutcome::Degraded;   // ok only when every write landed
    r.cause = "memory-writes-failed";
  } else if (r.memoryWritesDown) {
    r.outcome = DreamOutcome::Degraded;
    r.cause = "memory-unavailable";
  } else {
    r.outcome = DreamOutcome::Ran;
  }
}

// Retry tonight when the cause is transient, attempts remain and the window is open;
// a defer that cannot retry any more is a starved night.
void scheduleRetry(const NightClock& c, const DreamState& st, NightReport& r) {
  const bool retryable = r.outcome == DreamOutcome::Deferred ||
                         (r.outcome == DreamOutcome::Failed && causeRetryable(r.cause));
  const uint32_t nextMin = (uint32_t)c.minuteOfDay + kDeferSec / 60u;
  if (retryable && st.attemptsTonight < kMaxAttempts && nextMin < windowEndFor(c.slotMinute)) {
    r.retryAfterSec = kDeferSec;
    r.retryAtMinute = (uint16_t)nextMin;
  } else if (r.outcome == DreamOutcome::Deferred) {
    r.outcome = DreamOutcome::Starved;
  }
}

// The streak moves once per night, when the night's outcome is final.
void settleStreak(const NightClock& c, DreamState& st, NightReport& r) {
  if (r.retryAfterSec == 0) {
    if (r.outcome == DreamOutcome::Ran) {
      st.streak = 0;
    } else if (st.streakDay != c.localDay) {
      st.streakDay = c.localDay;
      if (st.streak < 255) st.streak++;
      r.alert = alertDue(st.streak);
    }
  }
  r.streak = st.streak;
}

void fingerprint(const NightClock& c, const NightReport& r, nimbus::orch::FireOutcome& o) {
  if (!o.detail.empty()) return;
  // Varies by day so the semantic-repeat guard never pauses dreaming for doing its
  // job quietly (the guard still catches a stuck non-empty reply).
  char fp[96];
  snprintf(fp, sizeof fp, "(dream d%lu: %s a%u pruned=%d deduped=%d)",
           (unsigned long)(c.epoch / 86400), outcomeSlug(r.outcome), (unsigned)r.attempt,
           r.maint.pruned, r.maint.deduped);
  o.detail = fp;
}

}  // namespace

namespace {

// Load the state, roll the per-night attempt counter, and run maintenance at most
// once per local day (it decays importance, so it is not idempotent - which is what
// makes a same-night retry of the reflection safe). A boot-relative clock would
// corrupt every age, so maintenance waits for a synced one.
DreamState openNight(const NightDeps& d, const NightClock& c, NightReport& r) {
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
  if (st.attemptDay != c.localDay) {
    st.attemptDay = c.localDay;
    st.attemptsTonight = 0;
  }
  if (c.synced && st.maintDay != c.localDay) {
    if (d.maintain) r.maint = d.maintain();
    st.maintDay = c.localDay;
  } else {
    r.maintDoneToday = c.synced;
  }
  return st;
}

// The reflection turn and what its writes did.
nimbus::orch::FireOutcome reflect(const NightDeps& d, const std::string& inputs, NightReport& r) {
  const nimbus::orch::MemOpCounters before = d.memOps ? d.memOps() : nimbus::orch::MemOpCounters{};
  nimbus::orch::FireOutcome o;
  if (d.injectTurn) {
    o = d.injectTurn(inputs);
  } else {
    o.error = "no turn executor";
  }
  const nimbus::orch::MemOpCounters after = d.memOps ? d.memOps() : before;
  r.ops = after - before;
  r.tokens = o.tokens.total();
  r.error = o.ok ? std::string() : o.error;
  return o;
}

}  // namespace

NightResult runNight(const NightDeps& d, const NightClock& c, bool force) {
  NightResult res;
  NightReport& r = res.report;
  nimbus::orch::FireOutcome& o = res.fire;
  // RAM moves first, so a store that is down never rewinds the night. A store that is
  // not mounted is not written at all (fail fast, and no file-system error per fire).
  auto save = [&](const DreamState& st) {
    if (d.memo) d.memo->st = st;
    if (d.storeReady && !d.storeReady()) {
      r.stateWhy = "storage not mounted";
      return false;
    }
    if (!d.saveState || !d.saveState(st)) {
      r.stateWhy = "storage write failed";
      return false;
    }
    r.stateWhy.clear();
    return true;
  };

  DreamState st = openNight(d, c, r);
  const Yesterday y = d.yesterday ? d.yesterday() : Yesterday{};
  const uint64_t hashBefore = d.scratchHash ? d.scratchHash() : 0;
  if (skipReflection(y.empty, hashBefore, st.scrHash, force)) {
    r.outcome = DreamOutcome::Skipped;
    st.lastOutcome = r.outcome;
    st.lastCause.clear();
    st.inflightNight = 0;   // an outcome clears any marker, even one left by an
    st.inflightAttempt = 0; // earlier night a failing store kept on disk
    st.inflightStart = 0;
    r.streak = st.streak;
    r.stateSaved = save(st);
    o.ok = true;
    fingerprint(c, r, o);
    return res;
  }

  st.attemptsTonight++;
  r.attempt = st.attemptsTonight;
  st.inflightNight = c.localDay;         // the marker: the turn is running, its
  st.inflightAttempt = st.attemptsTonight; // outcome write is what clears it
  st.inflightStart = c.epoch;
  save(st);   // a reboot mid-turn still counts the attempt, and now names it

  MemStats stats = d.stats ? d.stats() : MemStats{};
  stats.pruned = r.maint.pruned;
  stats.deduped = r.maint.deduped;
  r.memoryWritesDown = d.embeddingsUp && !d.embeddingsUp();
  const std::string scratch = d.scratchBlock ? d.scratchBlock() : std::string();
  o = reflect(d, buildDreamInputs(y.digest, scratch, stats, r.memoryWritesDown), r);

  classify(o, r);
  scheduleRetry(c, st, r);
  if (r.outcome == DreamOutcome::Ran || r.outcome == DreamOutcome::Degraded) {
    const uint64_t post = d.scratchHash ? d.scratchHash() : hashBefore;
    r.scratchChanged = post != hashBefore;
    st.scrHash = post;   // tomorrow's quiet-skip baseline
  }
  settleStreak(c, st, r);
  st.lastOutcome = r.outcome;
  st.lastCause = r.cause;
  st.inflightNight = 0;   // the outcome write clears the marker
  st.inflightAttempt = 0;
  st.inflightStart = 0;
  r.stateSaved = save(st);
  o.retryAfterSec = r.retryAfterSec;
  fingerprint(c, r, o);
  return res;
}

// ---- a night the restart cut short (L19-31) --------------------------------------

InterruptedNight reconcileBoot(DreamState& st, const NightClock& c) {
  InterruptedNight out;
  if (st.inflightNight == 0) return out;
  out.interrupted = true;
  NightReport& r = out.report;
  // The marker is the truth about the attempt (its pre-turn save landed), so the
  // state's counters are restored to it before any retry re-opens the night.
  st.attemptDay = st.inflightNight;
  st.attemptsTonight = st.inflightAttempt;
  r.outcome = DreamOutcome::Failed;
  r.cause = "restarted";
  r.error = "the device restarted during this run";
  r.attempt = st.inflightAttempt;
  r.stateSaved = true;   // the caller persists before applying anything
  // Retry while the marker's night is still the current local night and its
  // window is open; the interrupted attempt is one of the bounded retries. After
  // the window (or a later day) the loop keeps the next slot the boot computed.
  const uint32_t nextMin = (uint32_t)c.minuteOfDay + kDeferSec / 60u;
  if (c.localDay == st.inflightNight && st.attemptsTonight < kMaxAttempts &&
      nextMin < windowEndFor(c.slotMinute)) {
    r.retryAfterSec = kDeferSec;
    r.retryAtMinute = (uint16_t)nextMin;
  }
  // The night failed: the streak moves exactly once for it, so a second boot of
  // the same interruption (a save that failed) moves nothing.
  if (st.streakDay != st.inflightNight) {
    st.streakDay = st.inflightNight;
    if (st.streak < 255) st.streak++;
    r.alert = alertDue(st.streak);
  }
  r.streak = st.streak;
  // The interrupted verdict replaces whatever older outcome the file still
  // carried, and the marker clears: this save IS the outcome write.
  st.lastOutcome = r.outcome;
  st.lastCause = r.cause;
  st.inflightNight = 0;
  st.inflightAttempt = 0;
  st.inflightStart = 0;
  return out;
}

void applyInterruptedToLoop(nimbus::orch::LoopRecord& l, uint64_t nowEpoch,
                            uint32_t retryAfterSec) {
  l.lastResult = nimbus::orch::LastResult::Fail;
  if (l.consecFails < 255) l.consecFails++;   // the post-fire outcome write never ran
  if (retryAfterSec) l.nextRun = nowEpoch + retryAfterSec;
}

// ---- the row ----------------------------------------------------------------------

namespace {

// One clean line of at most `cap` bytes: control bytes and line breaks become spaces.
std::string oneLine(const std::string& t, size_t cap) {
  std::string out = replaceBreaks(t, " ");
  for (char& ch : out)
    if ((unsigned char)ch < 0x20 || ch == 0x7f) ch = ' ';
  if (out.size() > cap) {
    out.resize((size_t)nimbus::utf8CapLen(out.c_str(), (int)out.size(), (int)cap - 3));
    out += "...";
  }
  return out;
}

std::string hhmm(uint16_t minuteOfDay) {
  char b[8];
  snprintf(b, sizeof b, "%02u:%02u", (unsigned)((minuteOfDay / 60) % 24), (unsigned)(minuteOfDay % 60));
  return b;
}

std::string maintText(const NightReport& r) {
  char b[96];
  if (r.maint.ran)
    snprintf(b, sizeof b, "maintenance pruned=%d deduped=%d%s", r.maint.pruned, r.maint.deduped,
             r.maint.persisted ? "" : " (not saved)");
  else if (r.maintDoneToday)
    snprintf(b, sizeof b, "maintenance done earlier today");
  else if (r.outcome == DreamOutcome::Starved)
    snprintf(b, sizeof b, "maintenance not run tonight");   // the night never fired
  else
    snprintf(b, sizeof b, "maintenance skipped (clock not set)");
  return b;
}

std::string writesText(const nimbus::orch::MemOpCounters& w) {
  const uint32_t stored = w.added + w.updated;
  const uint32_t tried = stored + w.dupSkipped + w.queued + w.failed;
  // A queued write is NOT stored (it waits for its embedding); it is named so a
  // billing outage during the night can never read as "memories stored".
  char b[112];
  snprintf(b, sizeof b, "%u of %u memories stored (%u new, %u updated, %u queued, %u failed)",
           (unsigned)stored, (unsigned)tried, (unsigned)w.added, (unsigned)w.updated,
           (unsigned)w.queued, (unsigned)w.failed);
  return b;
}

std::string retryText(const NightReport& r) {
  if (!r.retryAfterSec) return ", no retry tonight";
  char b[48];
  snprintf(b, sizeof b, ", retry %u of %u at %s", (unsigned)(r.attempt + 1), (unsigned)kMaxAttempts,
           hhmm(r.retryAtMinute).c_str());
  return b;
}

// The failed night's row arm (lane M). An interrupted night (L19-31: the device
// restarted mid-reflection, reconciled at boot) says what happened and when it
// tries again, nothing else - the turn died in flight, so its write counts and
// its spend are unknowable and are never guessed.
std::string failedText(const NightReport& r, const std::string& spent) {
  if (r.cause == "restarted")
    return "interrupted (the device restarted during the reflection)" + retryText(r);
  return "FAILED (" + r.cause + (r.error.empty() ? "" : ": " + oneLine(r.error, 72)) + ")" +
         retryText(r) + "; " + writesText(r.ops) + spent + "; " + maintText(r);
}

}  // namespace

std::string renderDreamRow(const NightReport& r, size_t cap) {
  std::string row = "Nightly dream ";
  const std::string spent = "; spent " + std::to_string(r.tokens) + " tokens";
  switch (r.outcome) {
    case DreamOutcome::Ran:
      row += "ok: " + writesText(r.ops) + (r.scratchChanged ? ", scratchpad groomed" : "") + "; " +
             maintText(r) + spent;
      break;
    case DreamOutcome::Skipped:
      row += "skipped (quiet day); " + maintText(r);
      break;
    case DreamOutcome::Deferred:
      row += "deferred (low memory)" + retryText(r) + "; " + maintText(r);
      break;
    case DreamOutcome::Starved:
      row += std::string("skipped tonight (") +
             (r.cause == "low-memory" || r.cause == "low-heap" ? "low memory" : "device busy") +
             " after " + std::to_string(r.attempt) + " tries); " + maintText(r);
      break;
    case DreamOutcome::Failed:
      row += failedText(r, spent);
      break;
    case DreamOutcome::Degraded:
      row += std::string("degraded (") +
             (r.cause == "memory-unavailable" ? "memory unavailable" : "memory writes failed") +
             "): " + writesText(r.ops) + "; " + maintText(r) + spent;
      break;
  }
  if (!r.stateSaved) row += "; state not saved" + (r.stateWhy.empty() ? "" : " (" + r.stateWhy + ")");
  if (row.size() > cap) {
    row.resize((size_t)nimbus::utf8CapLen(row.c_str(), (int)row.size(), (int)cap - 3));
    row += "...";
  }
  return row;
}

std::string streakAlertText(const NightReport& r) {
  const char* why = "it could not finish";
  if (r.cause == "low-memory") why = "the device was low on memory";
  else if (r.cause == "timeout" || r.cause == "network" || r.cause == "provider-5xx")
    why = "the provider could not be reached";
  else if (r.cause == "rate-limit") why = "the provider was rate limiting";
  else if (r.cause == "billing") why = "the provider account is out of credit";
  else if (r.cause == "key") why = "the provider key was rejected";
  else if (r.cause == "budget") why = "the monthly token budget is used up";
  else if (r.cause == "memory-writes-failed" || r.cause == "memory-unavailable")
    why = "memories could not be saved";
  return "Nightly memory upkeep has not worked for " + std::to_string(r.streak) +
         " nights in a row (" + why + "). It keeps trying each night. To pause it, send "
         "/loop off dream.";
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
