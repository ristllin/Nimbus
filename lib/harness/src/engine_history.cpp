#include "nimbus/harness/engine.h"
#include "engine_internal.h"   // resolveTurnHost / headCtxTokens - shared with engine.cpp

#include <cstdio>
#include <string>
#include <vector>

#include "nimbus/harness/conv_cache.h"     // the chat's rows after thru, kept between turns
#include "nimbus/harness/conversation.h"   // historyTurns / fitHistory / foldSlice / renderHistory
#include "nimbus/harness/jobs.h"           // isJobDelivery
#include "nimbus/harness/log.h"
#include "nimbus/harness/turn_fit.h"       // TurnFit - the context line reads what the turn cut
#include "nimbus/harness/turn_inbox.h"     // what the channel says the sender sent
#include "nimbus/orch/budget.h"            // headContextBudget - the head's derived caps
#include "nimbus/orch/episodic_log.h"      // epiCardReads - the history read's card reads
#include "nimbus/orch/model_window.h"      // legacyCtxTokens - the floor branch's day floor

// engine_history - the TurnEngine's turn-structured history (histV2, conversation.h): the
// rows a turn reads, the window it sends, the owner and trigger rows it captures and the
// fold's slice. Moved out of engine.cpp verbatim (lane P0 of the history fix, 16 Nov
// 2026) so the history's owner edits this file alone; engine.h declares every function here.

namespace agent {

namespace orch = nimbus::orch;
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

bool historyAfterUnserved(const HistoryQuery&) {
  return false;   // every store serves the after cursor (lane H): MsgQuery::after
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
// One forward read of the fold's slice (rows after thru, oldest first), and how many a
// slice may make (the slice is a fold's input: at most foldStep, so a few pages).
constexpr size_t kFoldPageRows = 48;
constexpr int kFoldMaxPages = 24;

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

// The chat's rows after `o.startAfter` (`oldestFirst`, read newest first): an owner row
// identical to the one read just before it folds into that row's count, so a flood of one
// message holds one row of memory; reading stops at the newest /clear, at `o.startAfter` and
// at `maxRows` distinct rows (0 = none). `o`'s zone tells the owner's local days: two
// identical messages either side of LOCAL midnight are two messages. The kept rows (newest
// first) ride PSRAM.
void foldRuns(const std::vector<nimbus::orch::EpisodicMessage>& oldestFirst, HistoryOptions& o, size_t maxRows,
              std::vector<nimbus::orch::EpisodicMessage>& rows, std::vector<uint32_t>& repeats) {
  const OffsetAt zoneAt = localZone(o);
  RepeatStamp chain;   // the row just newer than the one being read: a run is chained row by row
  for (auto it = oldestFirst.rbegin(); it != oldestFirst.rend(); ++it) {
    const nimbus::orch::EpisodicMessage& m = *it;
    if (windowEnds(m, o, zoneAt)) return;
    if (isJobDelivery(m.tags)) continue;   // never conversation: transparent to a run of repeats
    if (joinsRun(rows, o, chain, zoneAt, m)) {
      repeats.back()++;
      chain = repeatStampOf(m);
      continue;
    }
    if (maxRows && rows.size() >= maxRows) return;
    chain = repeatStampOf(m);
    rows.push_back(m);
    repeats.push_back(1);
    spillToPsram(rows.back().text);
    spillToPsram(rows.back().tags);
    spillToPsram(rows.back().blobPath);
  }
}

// Conversation rows of the window (message, media; not a job delivery or a trigger's
// bookkeeping) in (afterId, beforeId) of `rows` (oldest first): counted into `span`.
void countHidden(const std::vector<nimbus::orch::EpisodicMessage>& rows, const std::string& upTo, ConvSpan& span) {
  for (const auto& m : rows) {
    if (!rowIdAfter(upTo, m.id) && m.id != upTo) break;   // past the hidden part
    if (isClearBoundary(m) || isJobDelivery(m.tags)) continue;
    if (span.rows == 0 && span.fromHours == 0) span.fromHours = m.tsHours;
    span.toId = m.id;
    span.toHours = m.tsHours;
    span.rows++;
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
// redelivery after a restart): its newest 48 rows, from the conversation cache.
bool sourceSeen(const std::vector<nimbus::orch::EpisodicMessage>& newestFirst, const std::string& src) {
  size_t n = 0;
  for (const auto& m : newestFirst) {
    if (n++ >= 2 * kHistoryPageRows) break;
    if (tagValue(m.tags, kSrcTag) == src) return true;
  }
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
  if (noted && !in.src.empty() && sourceSeen(recentRows(chatId, 2 * kHistoryPageRows), in.src)) {
    hlog::logf("history: %s%s is already in this chat's history - delivery dropped", kSrcTag, in.src.c_str());
    return false;   // captured once, answered once
  }
  // Stored once: the media rows keep their records, the owner row what was sent.
  curUserRowId_ = d_.captureUserRow(chatId, noted && !in.said.empty() ? in.said : text,
                                    ownerRowTags(fromTag, in, atItem(d_)));
  return true;
}

// ---- the window and the fold read the conversation cache (lane H) -------------------

HistoryBudget TurnEngine::historyBudgetOf(const nimbus::orch::ContextBudget& cb, const std::string& host,
                                          const std::string& model) const {
  HistoryBudget b;
  b.highBytes = cb.recentConvBytes;
  b.messageCap = cb.historyMessageCap;
  b.dayStartUnix = localDayStartUnix(d_.platform.nowEpoch ? d_.platform.nowEpoch() : 0,
                                     d_.utcOffsetMin ? d_.utcOffsetMin() : 0);
  if (cb.floorBranch) {   // v5.1.1's window exactly: its day floor from v5.1.1's window table
    b.dayBytes = historyDayBytes(orch::legacyCtxTokens(host, model));
    return b;
  }
  b.derived = true;
  b.maxBytes = cb.historyMaxBytes;
  b.softBytes = cb.historySoftBytes;
  b.keepBytes = cb.historyKeepBytes;
  b.highRows = cb.historyRows;
  return b;
}

HistoryBudget TurnEngine::headHistoryBudget() const {
  const std::string host = resolveTurnHost(d_);
  const std::string model = detail::headModelOf(d_, host);
  return historyBudgetOf(orch::headContextBudget(host, model, {}), host, model);
}

// Twice the row budget: enough to see past high water and shift once; after the
// shift the whole history after the soft start fits one read. Lane CTX: plus the
// rows a busy day needs, so the day floor sees the whole day. (v5.1.1's read, the
// floor branch's window.)
static size_t historyReadRows(const HistoryBudget& b) {
  return 2 * b.highRows + (b.dayStartUnix ? kHistoryDayRows : 0);
}

// What the cache holds per chat: the window's high water and a quarter (lane H); on the
// floor branch v5.1.1's read (twice its rows, which a flood folds, within 256 KB).
static ConvCache::Bound cacheBound(const HistoryBudget& b) {
  ConvCache::Bound c;
  if (b.derived) {
    c.bytes = b.maxBytes + b.maxBytes / 4;
  } else {
    c.rows = 2 * historyReadRows(b);
    c.bytes = 256 * 1024;
  }
  return c;
}

ConvCache::ReadFn TurnEngine::cacheReader(const std::string& chatId) {
  return [this, chatId](const HistoryQuery& q) {
    const uint32_t t0 = d_.platform.nowMs ? d_.platform.nowMs() : 0;
    const uint32_t reads0 = orch::epiCardReads();
    std::vector<nimbus::orch::EpisodicMessage> rows = d_.historyRows(chatId, q);   // memory::Lock inside
    histRead_.lockMs += (d_.platform.nowMs ? d_.platform.nowMs() : 0) - t0;
    histRead_.cardReads += orch::epiCardReads() - reads0;
    histRead_.pages++;
    return rows;
  };
}

// A page more while the page before read no card row (the store's RAM ring served it):
// at most one page of card reads, however much of the chat RAM already holds.
constexpr int kRamFillPages = 32;

ConvCache::View TurnEngine::cachedRows(const std::string& chatId, const HistoryMarks& marks, const HistoryBudget& b,
                                       int fillPages) {
  uint32_t reads = orch::epiCardReads();
  const auto freePage = [&reads]() {
    const uint32_t now = orch::epiCardReads();
    const bool free = now == reads;
    reads = now;
    return free;
  };
  return convCache_.access(chatId, marks.thru, cacheBound(b), fillPages ? kRamFillPages : 0, cacheReader(chatId),
                           freePage);
}

std::vector<nimbus::orch::EpisodicMessage> TurnEngine::recentRows(const std::string& chatId, size_t rows) {
  const HistoryMarks marks = d_.historyMarks ? d_.historyMarks(chatId) : HistoryMarks();
  cachedRows(chatId, marks, headHistoryBudget(), 1);   // brought up to date (the newest rows first)
  return convCache_.newest(chatId, rows);
}

HistoryWindow TurnEngine::historyWindow(const std::string& chatId, const HistoryBudget& b) {
  const uint32_t t0 = d_.platform.nowMs ? d_.platform.nowMs() : 0;
  const HistoryMarks marks = d_.historyMarks ? d_.historyMarks(chatId) : HistoryMarks();
  // One page of the newest-first fill on a turn at most (the idle pump does the rest).
  const ConvCache::View view = cachedRows(chatId, marks, b, 1);
  HistoryOptions o;
  o.inFlightId = curUserRowId_;
  o.startAfter = rowIdAfter(marks.softStart, marks.thru) ? marks.softStart : marks.thru;
  o.utcOffsetMin = d_.utcOffsetMin ? d_.utcOffsetMin() : 0;
  o.utcOffsetAt = d_.utcOffsetAt;
  o.messageCap = b.messageCap;
  std::vector<nimbus::orch::EpisodicMessage> rows;
  std::vector<uint32_t> repeats;
  foldRuns(view.rows, o, b.derived ? 0 : historyReadRows(b), rows, repeats);
  HistoryWindow w = fitHistory(historyTurns(rows, o, &repeats), marks.softStart, b, o.utcOffsetMin);
  if (w.shifted) {
    hlog::logf("history(%s): overflow shift to %s (%u B kept)", chatId.c_str(), w.softStart.c_str(),
               (unsigned)w.bytes);
    if (d_.noteHistoryShift) d_.noteHistoryShift(chatId, w.softStart);
  }
  // What the request does not carry of the rows after thru: the cache's span (rows it
  // does not hold), then the rows it holds up to the window's first kept turn (a shift).
  ConvSpan span = view.span;
  span.afterId = marks.thru;
  const std::string firstKept = w.kept.empty() ? std::string() : w.kept.front().firstId;
  const std::string hiddenTo = rowIdAfter(w.softStart, marks.thru) ? w.softStart : std::string();
  if (!hiddenTo.empty()) countHidden(view.rows, hiddenTo, span);
  if (!span.empty()) span.beforeId = firstKept;
  w.leftOut = span;
  histRead_.ms += (d_.platform.nowMs ? d_.platform.nowMs() : 0) - t0;
  histRead_.liveRows = view.rows.size() + span.rows;
  return w;
}

// R3 (G1): the same fitted turns as real messages (renderHistory), the summary
// pair first (the caller passes the RAW summary; rawFoldSummary).
std::vector<HistMsg> TurnEngine::historyMessages(const std::string& chatId, const HistoryBudget& b,
                                                 const std::string& summary) {
  lastWindow_ = HistoryWindow();
  if (chatId.empty() || chatId == "system")
    return summary.empty() ? std::vector<HistMsg>() : renderHistory({}, summary, 0);
  const int32_t off = d_.utcOffsetMin ? d_.utcOffsetMin() : 0;
  lastWindow_ = historyWindow(chatId, b);
  return renderHistory(lastWindow_.kept, summary, off);
}

// The first row of the verbatim tail an automatic fold keeps ("" = keep none). False when
// every turn after thru stays and none was hidden: nothing to fold (or the cache has not read
// down to thru yet, so the tail's start is not known and the fold waits for the fill).
bool TurnEngine::foldKeepFrom(const std::string& chatId, const HistoryMarks& marks, const HistoryBudget& b,
                              const HistoryOptions& o, bool hidden, std::string& keepFrom) {
  const ConvCache::View view = cachedRows(chatId, marks, b, 0);
  std::vector<nimbus::orch::EpisodicMessage> rows;
  std::vector<uint32_t> repeats;
  HistoryOptions to = o;
  if (hidden) to.startAfter = marks.softStart;   // the tail is never a hidden row
  foldRuns(view.rows, to, 0, rows, repeats);
  const std::vector<HistTurn> tail = historyTurns(rows, to, &repeats);
  const size_t keep = b.derived ? b.keepBytes : b.lowBytes();
  const HistoryBudget day = b.derived ? HistoryBudget() : b;   // v5.1.1 keeps the owner's day too
  const size_t kept = std::max(foldKeptTail(tail, keep, o.utcOffsetMin), dayTailTurns(tail, day, o.utcOffsetMin));
  if (kept >= tail.size() && !hidden) {
    if (!view.complete || !view.span.empty())
      hlog::logf("fold(%s): the history is still being read - the slice waits", chatId.c_str());
    return false;
  }
  if (kept > 0 && kept <= tail.size()) keepFrom = tail[tail.size() - kept].firstId;
  return true;
}

// The rows after thru, read forward a page at a time, oldest first, until `keepFrom`, about
// one and a half fold steps, or an empty page (the end: a store under its read budget answers
// a short page with more after it). True when the read reached the tail or the end.
bool TurnEngine::readFoldRows(const std::string& chatId, const std::string& thru, const std::string& keepFrom,
                              size_t capBytes, std::vector<nimbus::orch::EpisodicMessage>& fwd) {
  std::string cursor = thru.empty() ? std::string("m00000000") : thru;
  size_t bytes = 0;
  for (int page = 0; page < kFoldMaxPages; page++) {
    std::vector<nimbus::orch::EpisodicMessage> got =
        d_.historyRows(chatId, HistoryQuery(kFoldPageRows, std::string(), cursor));
    if (got.empty()) return true;
    cursor = got.back().id;   // every row seen, kept or not (a /clear included)
    for (auto& m : got) {
      if (!keepFrom.empty() && !rowIdAfter(keepFrom, m.id)) return true;
      if (isClearBoundary(m)) {   // a /clear after thru: the slice before it was cleared
        fwd.clear();
        bytes = 0;
        continue;
      }
      bytes += m.text.size() + 64;
      fwd.push_back(std::move(m));
    }
    if (bytes > capBytes + capBytes / 2) return false;   // enough for one fold: the rest is the next one's
  }
  return false;
}

// The fold's slice: the turns after thru, read FORWARD from it (the after cursor, the cold
// region included), oldest first, up to `capBytes`, never the newest turns the fold keeps
// verbatim (keepBytes; on the floor branch v5.1.1's low water and the owner's day). The
// slice therefore always starts at the row right after thru: a backlog longer than one read
// is folded oldest first, cycle after cycle, with no gap (v5.1.1 read only the newest 400
// rows and could anchor past rows it never read).
FoldSlice TurnEngine::historyFoldSlice(const std::string& chatId, size_t capBytes, bool manual) {
  if (!histV2On() || chatId.empty()) return FoldSlice();
  const HistoryBudget b = headHistoryBudget();
  const HistoryMarks marks = d_.historyMarks ? d_.historyMarks(chatId) : HistoryMarks();
  HistoryOptions o;
  o.startAfter = marks.thru;   // the shift's hidden rows are exactly what a fold must take
  o.utcOffsetMin = d_.utcOffsetMin ? d_.utcOffsetMin() : 0;
  o.utcOffsetAt = d_.utcOffsetAt;
  o.messageCap = b.messageCap;
  // Rows a shift hid (after thru, up to the soft start) are in neither the summary nor the
  // request: a fold always takes them, however small (the 1 KB floor never strands them).
  const bool hidden = rowIdAfter(marks.softStart, marks.thru);
  std::string keepFrom;
  if (!manual && !foldKeepFrom(chatId, marks, b, o, hidden, keepFrom)) return FoldSlice();
  std::vector<nimbus::orch::EpisodicMessage> fwd;
  const bool ended = readFoldRows(chatId, marks.thru, keepFrom, capBytes, fwd);
  std::vector<nimbus::orch::EpisodicMessage> rows;
  std::vector<uint32_t> repeats;
  foldRuns(fwd, o, 0, rows, repeats);
  std::vector<HistTurn> turns = historyTurns(rows, o, &repeats);
  // A read that stopped inside a turn (the cap, the page bound) leaves that turn to the next
  // fold: the slice holds whole turns only.
  if (!ended && turns.size() > 1) turns.pop_back();
  FoldSlice s = foldSlice(turns, 0, capBytes, /*manual=*/true, o.utcOffsetMin);
  if (!manual && !hidden && s.digest.size() < kFoldMinNewBytes) return FoldSlice();
  return s;
}

void TurnEngine::logContextLine(const ComposeInputs& ci, const TurnFit& fit, uint32_t budgetBytes,
                                size_t round0Bytes, const std::string& chatId) const {
  if (!histV2On() || chatId.empty() || chatId == "system") return;
  const int32_t off = d_.utcOffsetMin ? d_.utcOffsetMin() : 0;
  const uint64_t now = d_.platform.nowEpoch ? d_.platform.nowEpoch() : 0;
  const uint32_t dayStart = localDayStartUnix(now, off);
  const int32_t today = dayStart ? (int32_t)(((int64_t)dayStart + off * 60) / 86400) : -1;
  size_t sent = 0, todaySent = 0, todayLive = 0;
  for (const HistMsg& m : ci.history) {
    sent += m.rows;
    if ((int32_t)m.localDay == today) todaySent += m.rows;
  }
  for (const HistTurn& t : lastWindow_.kept)
    if (t.localDay == today) todayLive += t.rows;
  const size_t live = sent + fit.historyRowsLeftOut() + lastWindow_.leftOut.rows;
  const HistoryMarks marks = d_.historyMarks ? d_.historyMarks(chatId) : HistoryMarks();
  char age[16] = "-";
  if (!ci.chatSummary.empty() && marks.foldUnix && now > marks.foldUnix)
    snprintf(age, sizeof(age), "%u.%uh", (unsigned)((now - marks.foldUnix) / 3600u),
             (unsigned)((now - marks.foldUnix) % 3600u / 360u));
  hlog::logf("context: sent %u/%u live rows (today %u/%u), cut %d (%u B), summary age %s, round0 %u B / budget %u B, "
             "hist %u ms %u card reads lock %u ms",
             (unsigned)sent, (unsigned)live, (unsigned)todaySent, (unsigned)todayLive, fit.historyMessagesLeftOut(),
             (unsigned)fit.historyBytesLeftOut(), age, (unsigned)round0Bytes, (unsigned)budgetBytes,
             (unsigned)histRead_.ms, (unsigned)histRead_.cardReads, (unsigned)histRead_.lockMs);
}

bool TurnEngine::historyIdleStep() {
  if (!histV2On() || turnInFlight()) return false;
  // The plain read: the idle pump's pages are not a turn's (histRead_ is the turn's own).
  return convCache_.fillStep([this](const std::string& chat) -> ConvCache::ReadFn {
    return [this, chat](const HistoryQuery& q) { return d_.historyRows(chat, q); };
  });
}

}  // namespace agent
