#include "nimbus/harness/engine.h"
#include "engine_internal.h"   // resolveTurnHost / headCtxTokens - shared with engine.cpp

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "nimbus/harness/conv_cache.h"     // the chat's rows after thru, kept between turns
#include "nimbus/harness/conversation.h"   // historyTurns / fitHistory / foldSlice / renderHistory
#include "nimbus/harness/jobs.h"           // isJobDelivery
#include "nimbus/harness/log.h"
#include "nimbus/harness/turn_fit.h"       // TurnFit - the context line reads what the turn cut
#include "nimbus/harness/turn_inbox.h"     // what the channel says the sender sent
#include "nimbus/harness/turn_memory.h"    // PsramFirstScope - the cache's rows go to PSRAM
#include "nimbus/orch/budget.h"            // headContextBudget - the head's derived caps
#include "nimbus/orch/episodic_log.h"      // epiCardReads - the history read's card reads
#include "nimbus/orch/model_window.h"      // legacyCtxTokens - the floor branch's day floor
#include "nimbus/orch/request_budget.h"    // requestBudgetFor - the budget line

// engine_history - the TurnEngine's turn-structured history (histV2, conversation.h): the
// rows a turn reads (through the conversation cache, conv_cache.h), the window it sends,
// the owner and trigger rows it captures and the fold's slice. Split out of engine.cpp (lane
// P0 of the history fix) so the history's owner edits this file alone; engine.h declares
// every function here.

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

bool TurnEngine::histV2On() const {
  return d_.histV2 && d_.histV2() && d_.historyRows && d_.captureUserRow;
}

std::string TurnEngine::assistantRowTags() const {
  std::string tags = turnFx_.fxTag();
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
  if (noted && !in.src.empty() && sourceInHistory(chatId, in.src)) {
    hlog::logf("history: %s%s is already in this chat's history - delivery dropped", kSrcTag, in.src.c_str());
    return false;   // captured once, answered once
  }
  // Stored once: the media rows keep their records, the owner row what was sent.
  curUserRowId_ = d_.captureUserRow(chatId, noted && !in.said.empty() ? in.said : text,
                                    ownerRowTags(fromTag, in, atItem(d_)));
  return true;
}

// ---- the window and the fold read the conversation cache (lane H) -------------------

// Lane RB-BGb (ruling 83 item 9 (a)): a turn that sent a round at the send floor defers the
// chat's automatic fold; a turn whose rounds all went above it ends the deferral. A turn that
// sent no round (a command, a refused turn) changes nothing.
void TurnEngine::noteFloorSend(const std::string& chatId) {
  const orch::SendFloorCounts now = orch::sendFloorCounts();
  const auto at = std::find(floorChats_.begin(), floorChats_.end(), chatId);
  if (now.floorRounds == floorAtTurn_.floorRounds) {
    if (now.rounds != floorAtTurn_.rounds && at != floorChats_.end()) {
      floorChats_.erase(at);
      hlog::logf("history(%s): a request went above the send floor - the automatic fold may run", chatId.c_str());
    }
    return;
  }
  if (at != floorChats_.end()) return;
  // One line when the deferral starts (never per round or per turn).
  hlog::logf("history(%s): a request went at the send floor - the automatic fold waits for one above it",
             chatId.c_str());
  if (floorChats_.size() >= 4) floorChats_.erase(floorChats_.begin());
  floorChats_.push_back(chatId);
}

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
  // RB-BG (ruling 81): a measured floor head keeps v5.1.1's day floor too: today's turns up to
  // its cap stay in the window and the fold's tail, as on the floor branch.
  if (cb.floorMeasured) b.dayBytes = historyDayBytes(orch::legacyCtxTokens(host, model));
  return b;
}

HistoryBudget TurnEngine::headHistoryBudget() const {
  const std::string host = resolveTurnHost(d_);
  const std::string model = detail::headModelOf(d_, host);
  return historyBudgetOf(orch::headContextBudget(host, model, detail::toolLoopSetting(d_), {}), host, model);
}

// Twice the row budget: enough to see past high water and shift once; after the
// shift the whole history after the soft start fits one read. Lane CTX: plus the
// rows a busy day needs, so the day floor sees the whole day. (v5.1.1's read, the
// floor branch's window.)
static size_t historyReadRows(const HistoryBudget& b) {
  return 2 * b.highRows + (b.dayStartUnix ? kHistoryDayRows : 0);
}

// What the cache holds per chat: the window's high water and a quarter (lane H), and never
// more than kCacheMaxRows rows (a flood of one-word messages costs a row's fixed size each,
// which the byte count does not see); on the floor branch v5.1.1's read (twice its rows,
// which a flood folds, within 256 KB).
constexpr size_t kCacheMaxRows = 4096;
static ConvCache::Bound cacheBound(const HistoryBudget& b) {
  ConvCache::Bound c;
  if (b.derived) {
    c.bytes = std::max<size_t>(b.maxBytes + b.maxBytes / 4, 256 * 1024);   // RB-BG: a small window still reads past one long input
    c.rows = kCacheMaxRows;
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
    return rows;
  };
}

// A page more while the page before read no card row (the store's RAM ring served it):
// at most one page of card reads on a turn, however much of the chat RAM already holds.
constexpr int kRamFillPages = 32;

ConvCache::View TurnEngine::cachedRows(const std::string& chatId, const HistoryMarks& marks, const HistoryBudget& b,
                                       bool fill) {
  // The cache's rows outlive the turn: their small blocks go to PSRAM, never the scarce
  // internal heap (turn_memory.h; the idle pump and the fold run outside a turn's scope).
  const PsramFirstScope psramFirst;
  uint32_t reads = orch::epiCardReads();
  bool first = true;
  // The turn's one card page is spent once any of its history reads went to the card (the
  // duplicate check's read included); past it, a page only while the page before was free.
  const auto pageAllowed = [this, &reads, &first]() {
    const uint32_t now = orch::epiCardReads();
    const bool ok = first ? histRead_.cardReads == 0 : now == reads;
    first = false;
    reads = now;
    return ok;
  };
  return convCache_.access(chatId, marks.thru, cacheBound(b), fill ? kRamFillPages : 0, cacheReader(chatId),
                           pageAllowed);
}

bool TurnEngine::sourceInHistory(const std::string& chatId, const std::string& src) {
  const size_t rows = 2 * kHistoryPageRows;
  const HistoryMarks marks = d_.historyMarks ? d_.historyMarks(chatId) : HistoryMarks();
  cachedRows(chatId, marks, headHistoryBudget(), true);   // brought up to date (the newest rows first)
  const std::vector<nimbus::orch::EpisodicMessage> held = convCache_.newest(chatId, rows);
  if (sourceSeen(held, src) || held.size() >= rows) return sourceSeen(held, src);
  // The cache holds fewer than the chat's newest rows: only rows after thru and the newest
  // /clear (after a manual fold or a /clear it holds few or none), or, while it fills after a
  // restart (exactly when Telegram re-serves updates), only what its first page read. The
  // check reads the chat's newest rows from the store, whatever thru or a /clear says (v5.1.1's
  // check; lane CTXR1). The store's RAM ring serves it in steady state; it is counted as the
  // turn's history read.
  const PsramFirstScope psramFirst;
  return sourceSeen(cacheReader(chatId)(HistoryQuery(rows)), src);
}

HistoryWindow TurnEngine::historyWindow(const std::string& chatId, const HistoryBudget& b) {
  const uint32_t t0 = d_.platform.nowMs ? d_.platform.nowMs() : 0;
  const HistoryMarks marks = d_.historyMarks ? d_.historyMarks(chatId) : HistoryMarks();
  // One page of card reads on a turn at most (the idle pump does the rest).
  const ConvCache::View view = cachedRows(chatId, marks, b, true);
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
  if (b.derived && d_.noteHistoryBytes) d_.noteHistoryBytes(chatId, w.bytes);
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

// The verbatim tail an automatic fold keeps: the head's keep mark (v5.1.1's low water on the
// floor branch). An owner's own trigger below the head's soft mark (Compact at 24 KB on a head
// that keeps 111 KB) makes the fold due at that trigger, so the tail shrinks in proportion, or
// every due fold would find nothing beyond the kept history. The send floor never shortens it
// (RB-BGb): a chat at the floor defers its fold instead (deferAtFloor).
size_t TurnEngine::foldKeepBytes(const HistoryBudget& b) const {
  if (!b.derived) return b.lowBytes();
  const uint32_t trigger = d_.foldTriggerBytes ? d_.foldTriggerBytes() : 0;
  if (trigger && trigger < b.softBytes) return (size_t)((uint64_t)b.keepBytes * trigger / b.softBytes);
  return b.keepBytes;
}

// Where an automatic fold's forward read stops: before the first row of the verbatim tail it
// keeps, from the conversation cache. Nothing when every turn after thru stays and none was
// hidden. While the cache does not hold enough to know the tail's start (a restart, an evicted
// or reset entry) a fold with hidden rows takes those rows alone (the read stops after the soft
// start: a shift never hides a kept turn), and any other fold waits for the fill. Never Nothing
// on a guess: only a cache that holds every row after thru may say every turn stays.
void TurnEngine::noteHeld(const ConvCache::View& view, const HistoryBudget& b, FoldBounds& out) {
  if (!b.derived) return;   // the floor branch's trigger restarts at 0 (v5.1.1)
  for (const auto& m : view.rows)
    if (!isClearBoundary(m) && !isJobDelivery(m.tags)) out.held.push_back({orch::epiIdSuffix(m.id), (uint32_t)m.text.size()});
}

SliceState TurnEngine::foldBounds(const std::string& chatId, const HistoryMarks& marks, const HistoryBudget& b,
                                  const HistoryOptions& o, bool hidden, FoldBounds& out) {
  const ConvCache::View view = cachedRows(chatId, marks, b, false);
  noteHeld(view, b, out);
  std::vector<nimbus::orch::EpisodicMessage> rows;
  std::vector<uint32_t> repeats;
  HistoryOptions to = o;
  if (hidden) to.startAfter = marks.softStart;   // the tail is never a hidden row
  foldRuns(view.rows, to, 0, rows, repeats);
  const std::vector<HistTurn> tail = historyTurns(rows, to, &repeats);
  const size_t keep = foldKeepBytes(b);
  const HistoryBudget day = b;   // the owner's day, where the budget has a day floor (dayBytes)
  const size_t kept = std::max(foldKeptTail(tail, keep, o.utcOffsetMin), dayTailTurns(tail, day, o.utcOffsetMin));
  if (kept > 0 && kept < tail.size()) {
    out.keepFrom = tail[tail.size() - kept].firstId;
    return SliceState::Ready;
  }
  // Every turn the cache holds stays. Rows after thru it does not hold (evicted for its bound,
  // or not read yet) lie below them.
  const bool evicted = view.complete && !view.span.empty();
  if (!tail.empty() && (evicted || hidden)) {
    out.keepFrom = tail.front().firstId;
    if (view.complete) return SliceState::Ready;
  }
  if (hidden) {
    out.stopAt = marks.softStart;
    return SliceState::Ready;
  }
  if (evicted && !out.keepFrom.empty()) return SliceState::Ready;
  if (view.complete) return SliceState::Nothing;
  hlog::logf("fold(%s): the history is still being read - the slice waits", chatId.c_str());
  return SliceState::Wait;
}

TurnEngine::FoldCursor& TurnEngine::foldCursor(const std::string& chatId, const std::string& anchor) {
  FoldCursor* fc = nullptr;
  for (FoldCursor& c : foldCursors_)
    if (c.chat == chatId) fc = &c;
  if (!fc) {   // a few chats at a time, the oldest goes (only on a new chat: no cursor is held then)
    if (foldCursors_.size() >= 8) foldCursors_.erase(foldCursors_.begin());
    foldCursors_.emplace_back();
    fc = &foldCursors_.back();
    fc->chat = chatId;
  }
  if (fc->anchor != anchor) {   // a fold or a /clear moved the anchor: start over
    *fc = FoldCursor();
    fc->chat = chatId;
    fc->anchor = anchor;
  }
  return *fc;
}

// The read's start: thru; for a chat with no anchor (never folded, or cleared before a /clear
// set one) after its newest /clear, else its first row. `known` false: the start is not known
// yet, and only a read that reaches its bound may be used (one that stopped short could miss a
// later /clear and fold the conversation the owner cleared). The cache knows the start when its
// fill reached a /clear or the chat's start; a scan that stopped short goes on, a bounded read a
// pass with nothing kept. Wait while it goes on.
SliceState TurnEngine::foldStart(const std::string& chatId, FoldPlan& p) {
  p.from = p.marks.thru;
  if (!p.from.empty()) return SliceState::Ready;
  const ConvCache::View view = cachedRows(chatId, p.marks, p.b, false);
  if (!view.rows.empty() && isClearBoundary(view.rows.front())) {
    p.from = view.rows.front().id;
    return SliceState::Ready;
  }
  if (view.complete && view.span.empty()) return SliceState::Ready;   // the fill reached the chat's start
  FoldCursor& fc = foldCursor(chatId, p.marks.thru);
  if (!fc.scanned && fc.scanAt.empty()) {   // no scan yet: the read itself may reach its bound
    p.known = false;
    return SliceState::Ready;
  }
  if (!fc.scanned) {
    const FoldRead scan = readFoldRows(chatId, std::string(), p.bounds, 0, nullptr);
    p.s.progressed = scan.progressed;
    if (scan.end == FoldRead::End::Unread) return SliceState::Unread;
    if (scan.end != FoldRead::End::Tail && scan.end != FoldRead::End::Last) {
      hlog::logf("fold(%s): looking for the newest /clear (read to %s) - the slice waits", chatId.c_str(),
                 fc.scanAt.c_str());
      return SliceState::Wait;
    }
    fc.scanned = true;
  }
  p.from = fc.clearAt;
  return SliceState::Ready;
}

// One forward read of the fold's rows (readFoldRows): its cursor, the bytes kept, and how it
// ended. With `fwd` null it is the /clear scan, which keeps nothing.
struct TurnEngine::FoldReadPass {
  TurnEngine& eng;
  const std::string& chat;
  const FoldBounds& bounds;
  size_t capBytes;
  std::vector<nimbus::orch::EpisodicMessage>* fwd;
  FoldCursor& fc;
  FoldRead r;
  std::string cursor;
  std::string start;   // the read's anchor cursor (thru, a /clear row, or before the first row)
  size_t bytes = 0;

  bool scan() const { return fwd == nullptr; }
  bool holdsNone() const { return !fwd || fwd->empty(); }
  // The read moved past rows a later attempt need not read again.
  void note() {
    if (scan()) fc.scanAt = cursor;
    else if (holdsNone()) fc.resume = cursor;   // nothing of the chat before it to read again
    r.progressed = true;
  }
  bool stop(FoldRead::End e) {
    r.end = e;
    return false;
  }
  // A cursor the store's state moved under (lane CTXR1, v5.1.2 fix review): a byte cursor
  // ("day:off") from an earlier pass that the store, its index now complete, names stale
  // (EpiQueryInfo::cursorStale), or that brings back rows at or before the anchor (the index
  // read from the start). Never the end: the stored cursor is dropped and the next pass reads
  // again from the anchor. An empty page the store does not name stale is the real end.
  bool staleCursor() {
    hlog::logf("fold(%s): the read cursor %s no longer fits the store - the read starts again from %s",
               chat.c_str(), cursor.c_str(), start.c_str());
    if (scan()) {
      fc.scanAt.clear();
      fc.clearAt.clear();
    } else {
      fc.resume.clear();
    }
    if (fwd) fwd->clear();
    r.progressed = true;
    return stop(FoldRead::End::Wait);
  }
  bool beforeAnchor(const nimbus::orch::EpisodicMessage& m) const {
    return start != ConvCache::kBeforeFirstRow && !rowIdAfter(m.id, start);
  }
  // An empty page: the end, a failing card, or a read budget to go on past.
  bool emptyPage(const nimbus::orch::EpiQueryInfo& info) {
    if (info.olderExists && !info.newerExists) {   // a failing card: the RAM ring does not reach here
      hlog::logf("fold(%s): rows after %s are not readable now (the card) - the slice waits", chat.c_str(),
                 cursor.c_str());
      return stop(FoldRead::End::Wait);
    }
    if (info.cursorStale) return staleCursor();   // the store says so: never inferred from the shape
    if (!info.newerExists || info.nextAfter.empty()) return stop(FoldRead::End::Last);
    cursor = info.nextAfter;   // the store's read budget stopped first: go on from where it did
    note();
    return true;
  }
  bool pastBound(const nimbus::orch::EpisodicMessage& m) const {
    if (!bounds.keepFrom.empty() && !rowIdAfter(bounds.keepFrom, m.id)) return true;
    return !bounds.stopAt.empty() && rowIdAfter(m.id, bounds.stopAt);
  }
  void keep(nimbus::orch::EpisodicMessage& m) {
    bytes += m.text.size() + 64;
    spillToPsram(m.text);
    spillToPsram(m.tags);
    spillToPsram(m.blobPath);
    fwd->push_back(std::move(m));
  }
  // The page's rows, oldest first: false once a bound ends the read.
  bool takeRows(std::vector<nimbus::orch::EpisodicMessage>& got) {
    for (auto& m : got) {
      if (beforeAnchor(m)) return staleCursor();
      if (pastBound(m)) return stop(FoldRead::End::Tail);
      cursor = m.id;   // every row seen, kept or not (a /clear included)
      r.lastSeen = m.id;
      if (isClearBoundary(m)) {   // a /clear after the start: what was read before it was cleared
        r.clearAt = m.id;
        if (scan()) fc.clearAt = m.id;
        if (fwd) fwd->clear();
        bytes = 0;
      } else if (!scan()) {
        keep(m);
      }
    }
    if (holdsNone()) note();
    if (!scan() && bytes > capBytes + capBytes / 2) return stop(FoldRead::End::Full);   // the next fold's
    return true;
  }
  // One page: false when the read is over (r.end says how).
  bool page() {
    nimbus::orch::EpiQueryInfo info;
    HistoryQuery q(kFoldPageRows, std::string(), cursor);
    q.info = &info;
    std::vector<nimbus::orch::EpisodicMessage> got = eng.d_.historyRows(chat, q);
    if (!info.unreadable.empty()) {
      hlog::logf("fold(%s): rows after %s are not readable yet (%s) - the slice stops before them", chat.c_str(),
                 cursor.c_str(), info.unreadable.c_str());
      return stop(FoldRead::End::Unread);
    }
    return got.empty() ? emptyPage(info) : takeRows(got);
  }
};

// The rows after `from`, read forward a page at a time, oldest first, until the tail's first
// row, past the stop row, about one and a half fold steps (`fwd`), or the end (an empty page the
// store did not cut short: a store under its read budget answers a short or even empty page with
// more after it, and says so). Each kept row rides PSRAM as it is read (turn_memory.h: the fold
// runs outside a turn's scope). A /clear drops what was read before it. A span the card would not
// give back stops the read before it, and a failing card's empty page is not the end: the fold
// never anchors past rows it did not read. With `fwd` null the read is the /clear scan: nothing
// is kept, and it goes on next pass from where it stopped.
TurnEngine::FoldRead TurnEngine::readFoldRows(const std::string& chatId, const std::string& from,
                                              const FoldBounds& bounds, size_t capBytes,
                                              std::vector<nimbus::orch::EpisodicMessage>* fwd) {
  FoldCursor& fc = foldCursor(chatId, d_.historyMarks ? d_.historyMarks(chatId).thru : std::string());
  FoldReadPass pass{*this, chatId, bounds, capBytes, fwd, fc, FoldRead(), std::string(), std::string()};
  pass.start = from.empty() ? std::string(ConvCache::kBeforeFirstRow) : from;
  const std::string& resume = fwd ? fc.resume : fc.scanAt;
  pass.cursor = resume.empty() ? pass.start : resume;
  for (int page = 0; page < kFoldMaxPages; page++)
    if (!pass.page()) return pass.r;
  pass.r.end = pass.holdsNone() ? FoldRead::End::Wait : FoldRead::End::Full;   // the page bound
  return pass.r;
}

namespace {
// The conversation bytes the cache holds after `thru` (a derived head's trigger restarts there).
uint32_t bytesAfter(const std::vector<std::pair<uint32_t, uint32_t>>& held, const std::string& thru) {
  const uint32_t from = thru.empty() ? 0 : nimbus::orch::epiIdSuffix(thru);
  uint64_t sum = 0;
  for (const auto& h : held)
    if (thru.empty() || h.first > from) sum += h.second;
  return (uint32_t)std::min<uint64_t>(sum, UINT32_MAX);
}
}  // namespace

// Lane RB-BGb (ruling 83 item 9 (a)): an automatic fold while the chat's requests go at the send
// floor. Rows a shift hid are in neither the request nor the summary, so they fold, and only
// they (the read stops at the soft start, every visible row stays). Otherwise the fold waits for
// a request above the floor: the rows the floor leaves out are named by each request's LEFT OUT
// note, and the fold that follows keeps the head's full tail.
SliceState TurnEngine::deferAtFloor(const std::string& chatId, FoldPlan& p) {
  noteHeld(cachedRows(chatId, p.marks, p.b, false), p.b, p.bounds);
  if (p.hidden) {
    p.bounds.stopAt = p.marks.softStart;
    return SliceState::Ready;
  }
  p.s.floorDeferred = true;
  return SliceState::Wait;
}

// What the slice knows before it reads: the budget, the marks, the kept tail's bounds and the
// read's start. p.s.state says whether to read at all.
void TurnEngine::planFold(const std::string& chatId, bool manual, FoldPlan& p) {
  p.b = headHistoryBudget();
  p.marks = d_.historyMarks ? d_.historyMarks(chatId) : HistoryMarks();
  p.o.startAfter = p.marks.thru;   // the shift's hidden rows are exactly what a fold must take
  p.o.utcOffsetMin = d_.utcOffsetMin ? d_.utcOffsetMin() : 0;
  p.o.utcOffsetAt = d_.utcOffsetAt;
  p.o.messageCap = p.b.messageCap;
  // Rows a shift hid (after thru, up to the soft start) are in neither the summary nor the
  // request: a fold always takes them, however small (the 1 KB floor never strands them).
  p.hidden = rowIdAfter(p.marks.softStart, p.marks.thru);
  p.s.state = SliceState::Ready;
  if (manual) noteHeld(cachedRows(chatId, p.marks, p.b, false), p.b, p.bounds);   // a manual fold keeps no tail
  else if (foldDeferredAtFloor(chatId)) p.s.state = deferAtFloor(chatId, p);
  else p.s.state = foldBounds(chatId, p.marks, p.b, p.o, p.hidden, p.bounds);
  p.s.keptBytes = bytesAfter(p.bounds.held, p.marks.thru);
  if (p.s.state != SliceState::Ready) return;
  p.s.state = foldStart(chatId, p);
  p.o.startAfter = p.from;
}

// A read that held no row, or found nothing worth a summary, or (no anchor) stopped short.
FoldSlice TurnEngine::sliceWithoutDigest(const std::string& chatId, FoldPlan& p, const FoldRead& r,
                                         bool nothingToSummarize) {
  FoldSlice s = p.s;
  const bool ended = r.end == FoldRead::End::Tail || r.end == FoldRead::End::Last;
  if (!p.known && !ended) {
    // No anchor and the read stopped short of its bound: a later /clear may follow. The scan
    // goes on from here on the next pass; nothing read is folded.
    FoldCursor& fc = foldCursor(chatId, p.marks.thru);
    fc.scanAt = r.lastSeen.empty() ? std::string(ConvCache::kBeforeFirstRow) : r.lastSeen;
    fc.clearAt = r.clearAt;
    hlog::logf("fold(%s): no anchor - looking for the newest /clear first (read to %s)", chatId.c_str(),
               fc.scanAt.c_str());
    s.state = r.end == FoldRead::End::Unread ? SliceState::Unread : SliceState::Wait;
    // Progress only when the read got somewhere (a failing card read nothing: it backs off).
    s.progressed = r.end != FoldRead::End::Unread && (r.progressed || !r.lastSeen.empty());
    return s;
  }
  s.progressed = r.progressed;
  if (ended) {
    s.state = SliceState::Nothing;   // only a read that reached its bound
  } else {
    s.state = r.end == FoldRead::End::Unread ? SliceState::Unread : SliceState::Wait;
    if (nothingToSummarize && !r.lastSeen.empty()) {
      // The rows it held were none of the conversation (job deliveries, a /clear's): the next
      // read goes on after them instead of reading them again (fix review B).
      foldCursor(chatId, p.marks.thru).resume = r.lastSeen;
      s.progressed = s.state == SliceState::Wait;
    }
  }
  s.readTo = ended && p.hidden ? p.marks.softStart : r.lastSeen;   // a read to its bound saw every hidden row
  return s;
}

// The fold's slice: the turns after thru, read FORWARD from it (the after cursor, the cold
// region included), oldest first, up to `capBytes`, never the newest turns the fold keeps
// verbatim (keepBytes; on the floor branch v5.1.1's low water and the owner's day). The
// slice therefore always starts at the row right after thru: a backlog longer than one read
// is folded oldest first, cycle after cycle, with no gap (v5.1.1 read only the newest 400
// rows and could anchor past rows it never read). With no digest it says why (SliceState):
// only Nothing lets the driver restart the chat's counters or give up hidden rows.
FoldSlice TurnEngine::historyFoldSlice(const std::string& chatId, size_t capBytes, bool manual) {
  if (!histV2On() || chatId.empty()) return FoldSlice();
  // The slice's rows are read outside a turn: their small blocks go to PSRAM (turn_memory.h).
  const PsramFirstScope psramFirst;
  FoldPlan p;
  planFold(chatId, manual, p);
  if (p.s.state != SliceState::Ready) return p.s;
  std::vector<nimbus::orch::EpisodicMessage> fwd;
  const FoldRead r = readFoldRows(chatId, p.from, p.bounds, capBytes, &fwd);
  const bool ended = r.end == FoldRead::End::Tail || r.end == FoldRead::End::Last;
  if (fwd.empty() || (!p.known && !ended)) return sliceWithoutDigest(chatId, p, r, false);
  const std::string lastRead = fwd.back().id;
  std::vector<nimbus::orch::EpisodicMessage> rows;
  std::vector<uint32_t> repeats;
  foldRuns(fwd, p.o, 0, rows, repeats);
  fwd.clear();
  fwd.shrink_to_fit();
  std::vector<HistTurn> turns = historyTurns(rows, p.o, &repeats);
  // A read that stopped inside a turn (the cap, the page bound, a span it could not read)
  // leaves that turn to the next fold: the slice holds whole turns only.
  if (!ended && turns.size() > 1) turns.pop_back();
  FoldSlice built = foldSlice(turns, 0, capBytes, /*manual=*/true, p.o.utcOffsetMin);
  if (built.digest.empty() || (!manual && !p.hidden && built.digest.size() < kFoldMinNewBytes))
    return sliceWithoutDigest(chatId, p, r, built.digest.empty());
  built.state = SliceState::Ready;
  built.firstId = turns.front().firstId;
  built.more = !ended || rowIdAfter(lastRead, built.thru);   // read short, or the digest cap left rows
  built.keptBytes = bytesAfter(p.bounds.held, built.thru);
  return built;
}

__attribute__((noinline)) uint32_t TurnEngine::logBudgetLine(const std::string& host, const std::string& model,
                                                              const nimbus::orch::ContextBudget& b, bool owner) const {
  const orch::RequestBudget rb = orch::requestBudgetFor(host, model, false);
  hlog::logf("budget: %s/%s window %uK target %uK tok (%u KB) hist %u/%u/%u KB (%s)", host.c_str(),
             model.empty() ? "-" : model.c_str(), (unsigned)(b.ctxTokens / 1000), (unsigned)(rb.targetTokens / 1000),
             (unsigned)(rb.wireBodyBytes / 1000), (unsigned)(b.historyMaxBytes / 1024),
             (unsigned)(b.historySoftBytes / 1024), (unsigned)(b.historyKeepBytes / 1024), owner ? "owner" : "auto");
  return rb.wireBodyBytes;
}

__attribute__((noinline)) std::string TurnEngine::coverageSeed(const std::string& chatId, bool hasSummary,
                                                               const std::string& inputs) const {
  if (!histV2On() || chatId.empty() || chatId == "system") return inputs;
  const HistoryMarks marks = d_.historyMarks ? d_.historyMarks(chatId) : HistoryMarks();
  return historyCoverageLine(lastWindow_.leftOut, hasSummary ? marks.foldUnix : 0u,
                             d_.platform.nowEpoch ? d_.platform.nowEpoch() : 0, d_.utcOffsetMin ? d_.utcOffsetMin() : 0) +
         inputs;
}

__attribute__((noinline)) std::vector<HistMsg> TurnEngine::turnHistory(const std::string& chatId,
                                                                        const nimbus::orch::ContextBudget& cb,
                                                                        const std::string& host,
                                                                        const std::string& model,
                                                                        const std::string& summary) {
  return historyMessages(chatId, historyBudgetOf(cb, host, model), summary);
}

__attribute__((noinline)) TurnFit* TurnEngine::newTurnFit(ComposeInputs& ci, const std::string& seed, HeadTools* tools) const {
  return new TurnFit(ci, seed, tools, fitContextNow());
}

FitContext TurnEngine::fitContextNow() const {
  FitContext c;
  c.utcOffsetMin = d_.utcOffsetMin ? d_.utcOffsetMin() : 0;
  const uint32_t dayStart = localDayStartUnix(d_.platform.nowEpoch ? d_.platform.nowEpoch() : 0, c.utcOffsetMin);
  if (dayStart) c.todayLocalDay = (int32_t)(((int64_t)dayStart + c.utcOffsetMin * 60) / 86400);
  return c;
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
  const PsramFirstScope psramFirst;   // the cache keeps the rows this page reads (cachedRows)
  // The plain read: the idle pump's pages are not a turn's (histRead_ is the turn's own).
  return convCache_.fillStep([this](const std::string& chat) -> ConvCache::ReadFn {
    return [this, chat](const HistoryQuery& q) { return d_.historyRows(chat, q); };
  });
}

}  // namespace agent
