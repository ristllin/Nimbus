#include "nimbus/harness/engine.h"
#include "engine_internal.h"   // resolveTurnHost / headCtxTokens - shared with engine.cpp

#include <string>
#include <vector>

#include "nimbus/harness/conversation.h"   // historyTurns / fitHistory / foldSlice / renderHistory
#include "nimbus/harness/log.h"
#include "nimbus/harness/turn_inbox.h"     // what the channel says the sender sent
#include "nimbus/orch/budget.h"            // headContextBudget - the head's derived caps

// engine_history - the TurnEngine's turn-structured history (histV2, conversation.h): the
// rows a turn reads, the window it sends, the owner and trigger rows it captures and the
// fold's slice. Moved out of engine.cpp verbatim (lane P0 of the history fix, 16 Nov
// 2026) so the history's owner edits this file alone; engine.h declares every function here.

namespace agent {

namespace orch = nimbus::orch;
using detail::headCtxTokens;
using detail::resolveTurnHost;

// ---- histV2: the turn-structured history (conversation.h) --------------------

namespace {
// A routine or loop id inside a tag: id characters only, bounded.
std::string tagSafe(const std::string& v) {
  std::string o;
  for (char c : v) {
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    c == '_' || c == '-' || c == ':';
    if (ok && o.size() < 40) o += c;
  }
  return o;
}

// "at:<unix>" - when the device stores an owner or trigger row, to the second, so the
// history tells its local day to the minute and a repeat apart from a habit. "" until the
// clock is set (a boot-relative stamp has no date).
std::string atItem(const TurnEngine::Deps& d) {
  const uint64_t now = d.platform.nowEpoch ? d.platform.nowEpoch() : 0;
  if (now < (uint64_t)nimbus::orch::kClockValidHours * 3600u || now > UINT32_MAX) return std::string();
  return std::string(kAtTag) + std::to_string((unsigned long)now);
}
}  // namespace

bool historyAfterUnserved(const HistoryQuery& q) {
  if (q.after.empty()) return false;
  static bool said = false;   // once a boot: nothing sets the cursor until lane H serves it
  if (!said) {
    said = true;
    hlog::logf("history: a read after %s is not served yet - no rows", q.after.c_str());
  }
  return true;
}

bool TurnEngine::histV2On() const {
  return d_.histV2 && d_.histV2() && d_.historyRows && d_.captureUserRow;
}

std::string TurnEngine::assistantRowTags() const {
  std::string tags = claims_.effects().fxTag();
  if (!outKind_.empty()) tags += std::string(",") + kOutTag + outKind_;
  // F1 (lane I): the ids this turn read (skill.get) or had attached, so a
  // later turn answers "did you use the skill for that?" from the row - the
  // renderer turns the tag into " (skills: a, b)". Ids join with '.' (the att:
  // precedent): the tag list is comma-separated, so a comma would read as the
  // next tag's key.
  std::string ids;
  for (const auto& id : skillsUsed_) {
    const std::string s = tagSafe(id);
    if (s.empty() || ids.size() + s.size() + 1 > 60) break;
    ids += (ids.empty() ? "" : ".") + s;
  }
  if (!ids.empty()) tags += std::string(",") + kSkillsTag + ids;
  const std::string at = atItem(d_);   // when the reply was stored: the history's day floor reads it
  if (!at.empty()) tags += "," + at;
  return tags;
}

void TurnEngine::noteSkillUsed(const std::string& id) {
  for (const auto& s : skillsUsed_)
    if (s == id) return;
  if (skillsUsed_.size() >= 4) return;   // the attach block carries at most 4 anyway
  skillsUsed_.push_back(id);
}

namespace {

constexpr size_t kHistoryPageRows = 24;   // one read of the chat's rows (transient)
constexpr int kHistoryMaxPages = 32;      // a flood of one repeated line folds as it is read

// True when `m` ends the window (a /clear, or the row the window starts after). The row the
// window starts after is context only: its local day becomes `o.dayFloor`.
bool windowEnds(const nimbus::orch::EpisodicMessage& m, HistoryOptions& o, const OffsetAt& zoneAt) {
  if (isClearBoundary(m)) return true;
  if (o.startAfter.empty() || rowIdAfter(m.id, o.startAfter)) return false;
  if (m.id == o.startAfter) o.dayFloor = rowLocalDay(m, zoneAt);
  return true;
}

// True when `m` is one more of the run `rows.back()` stands for. The in-flight row never starts
// or absorbs a run: it is dropped from the history, and its count with it.
bool joinsRun(const std::vector<nimbus::orch::EpisodicMessage>& rows, const HistoryOptions& o,
              const RepeatStamp& chain, const OffsetAt& zoneAt, const nimbus::orch::EpisodicMessage& m) {
  if (rows.empty() || rows.back().id == o.inFlightId) return false;
  return isRepeatOf(rows.back(), chain, m, zoneAt);
}

// The chat's rows after `startAfter`, newest first, a page at a time. An owner
// row identical to the one read just before it folds into that row's count as
// it is read, so a flood of one message holds one row of memory; reading stops
// at the newest /clear, at `o.startAfter` and at `maxRows` distinct rows. `o`'s zone tells the
// owner's local days: two identical messages either side of LOCAL midnight are two messages.
void readHistory(const TurnEngine::Deps& d, const std::string& chatId, HistoryOptions& o, size_t maxRows,
                 std::vector<nimbus::orch::EpisodicMessage>& rows, std::vector<uint32_t>& repeats) {
  const OffsetAt zoneAt = localZone(o);
  RepeatStamp chain;   // the row just newer than the one being read: a run is chained row by row
  std::string before;
  for (int page = 0; page < kHistoryMaxPages; page++) {
    std::vector<nimbus::orch::EpisodicMessage> got = d.historyRows(chatId, HistoryQuery(kHistoryPageRows, before));
    const bool full = got.size() >= kHistoryPageRows;
    if (full) before = got.back().id;
    for (auto& m : got) {
      if (windowEnds(m, o, zoneAt)) return;
      if (isJobDelivery(m.tags)) continue;   // never conversation: transparent to a run of repeats
      if (joinsRun(rows, o, chain, zoneAt, m)) {
        repeats.back()++;
        chain = repeatStampOf(m);
        continue;
      }
      if (rows.size() >= maxRows) return;
      chain = repeatStampOf(m);
      rows.push_back(std::move(m));
      repeats.push_back(1);
      spillToPsram(rows.back().text);   // the kept rows ride PSRAM, the page is transient
      spillToPsram(rows.back().tags);
      spillToPsram(rows.back().blobPath);
    }
    if (!full) return;
  }
}
}  // namespace

void TurnEngine::setTrigger(const std::string& text, const std::string& src) {
  curUserRowId_.clear();   // a seedless turn has no owner row in flight
  triggerText_ = text;
  const std::string at = atItem(d_);
  triggerTags_ = std::string(kTriggerTag) + "," + kSrcTag + tagSafe(src) + (at.empty() ? "" : "," + at);
  triggerPending_ = histV2On();
}

namespace {
// The owner row's tags: the sender, then what the channel noted (H1), then when it was stored.
std::string ownerRowTags(const std::string& fromTag, const history::Inbound& in, const std::string& at) {
  std::string tags = fromTag;
  const auto add = [&tags](const std::string& item) { tags += (tags.empty() ? "" : ",") + item; };
  std::string refs;
  for (const auto& r : in.refs) refs += (refs.empty() ? "" : ".") + tagSafe(r);
  if (!refs.empty()) add(kAttTag + refs);
  if (!in.src.empty()) add(kSrcTag + tagSafe(in.src));
  if (in.sentUnix) add(kSentTag + std::to_string(in.sentUnix));
  if (!at.empty()) add(at);
  return tags;
}

// H7: a source already in the chat's recent history (a re-served update, a
// redelivery after a restart).
bool sourceSeen(const TurnEngine::Deps& d, const std::string& chatId, const std::string& src) {
  for (const auto& m : d.historyRows(chatId, HistoryQuery(2 * kHistoryPageRows)))   // 48 rows
    if (tagValue(m.tags, kSrcTag) == src) return true;
  return false;
}
}  // namespace

bool TurnEngine::captureOwnerRow(const std::string& chatId, const std::string& text,
                                 const std::string& fromTag) {
  curUserRowId_.clear();
  triggerPending_ = false;
  if (!histV2On()) {
    if (d_.episodicCaptureUser) d_.episodicCaptureUser(chatId, text, fromTag);
    return true;
  }
  history::Inbound in;
  const bool noted = history::takeInbound(chatId, text, in);
  if (noted && !in.src.empty() && sourceSeen(d_, chatId, in.src)) {
    hlog::logf("history: %s%s is already in this chat's history - delivery dropped", kSrcTag, in.src.c_str());
    return false;   // captured once, answered once
  }
  // Stored once: the media rows keep their records, the owner row what was sent.
  curUserRowId_ = d_.captureUserRow(chatId, noted && !in.said.empty() ? in.said : text,
                                    ownerRowTags(fromTag, in, atItem(d_)));
  return true;
}

std::vector<HistTurn> TurnEngine::loadTurns(const std::string& chatId, const HistoryOptions& o,
                                            size_t maxRows) const {
  // The device's own zone, in force at each row's time, tells the days (day markers, repeats).
  HistoryOptions zoned = o;
  if (!zoned.utcOffsetAt) zoned.utcOffsetAt = d_.utcOffsetAt;
  std::vector<nimbus::orch::EpisodicMessage> rows;
  std::vector<uint32_t> repeats;
  readHistory(d_, chatId, zoned, maxRows, rows, repeats);
  return historyTurns(rows, zoned, &repeats);
}

HistoryBudget TurnEngine::historyBudgetFor(size_t highBytes) const {
  HistoryBudget b;
  b.highBytes = highBytes;
  b.dayBytes = historyDayBytes(headCtxTokens(d_));
  b.dayStartUnix = localDayStartUnix(d_.platform.nowEpoch ? d_.platform.nowEpoch() : 0,
                                     d_.utcOffsetMin ? d_.utcOffsetMin() : 0);
  return b;
}

// Twice the row budget: enough to see past high water and shift once; after the
// shift the whole history after the soft start fits one read. Lane CTX: plus the
// rows a busy day needs, so the day floor sees the whole day.
static size_t historyReadRows(const HistoryBudget& b) {
  return 2 * b.highRows + (b.dayStartUnix ? kHistoryDayRows : 0);
}

HistoryWindow TurnEngine::historyWindow(const std::string& chatId, size_t highBytes) {
  const HistoryMarks marks = d_.historyMarks ? d_.historyMarks(chatId) : HistoryMarks();
  const HistoryBudget b = historyBudgetFor(highBytes);
  HistoryOptions o;
  o.inFlightId = curUserRowId_;
  o.startAfter = rowIdAfter(marks.softStart, marks.thru) ? marks.softStart : marks.thru;
  o.utcOffsetMin = d_.utcOffsetMin ? d_.utcOffsetMin() : 0;
  HistoryWindow w = fitHistory(loadTurns(chatId, o, historyReadRows(b)), marks.softStart, b, o.utcOffsetMin);
  if (w.shifted) {
    hlog::logf("history(%s): overflow shift to %s (%u B kept)", chatId.c_str(), w.softStart.c_str(),
               (unsigned)w.bytes);
    if (d_.noteHistoryShift) d_.noteHistoryShift(chatId, w.softStart);
  }
  return w;
}

std::string TurnEngine::historyBlock(const std::string& chatId, size_t highBytes) {
  if (chatId.empty() || chatId == "system") return std::string();
  return historyWindow(chatId, highBytes).block;
}

// R3 (G1): the same fitted turns as real messages (renderHistory), the summary
// pair first (the caller passes the RAW summary; rawFoldSummary).
std::vector<HistMsg> TurnEngine::historyMessages(const std::string& chatId, size_t highBytes,
                                                 const std::string& summary) {
  if (chatId.empty() || chatId == "system")
    return summary.empty() ? std::vector<HistMsg>() : renderHistory({}, summary, 0);
  const int32_t off = d_.utcOffsetMin ? d_.utcOffsetMin() : 0;
  return renderHistory(historyWindow(chatId, highBytes).kept, summary, off);
}

FoldSlice TurnEngine::historyFoldSlice(const std::string& chatId, size_t capBytes, bool manual) {
  if (!histV2On() || chatId.empty()) return FoldSlice();
  const std::string host = resolveTurnHost(d_);
  const HistoryBudget b = historyBudgetFor(orch::headContextBudget(host, detail::headModelOf(d_, host), {}).recentConvBytes);
  const HistoryMarks marks = d_.historyMarks ? d_.historyMarks(chatId) : HistoryMarks();
  HistoryOptions o;
  o.startAfter = marks.thru;   // the shift's hidden rows are exactly what a fold must take
  o.utcOffsetMin = d_.utcOffsetMin ? d_.utcOffsetMin() : 0;
  return foldSlice(loadTurns(chatId, o, 400), b.lowBytes(), capBytes, manual, o.utcOffsetMin, b);
}

}  // namespace agent
