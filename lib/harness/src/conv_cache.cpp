#include "nimbus/harness/conv_cache.h"

#include <algorithm>

#include "nimbus/harness/conversation.h"   // isClearBoundary, rowIdAfter, spillToPsram
#include "nimbus/harness/engine.h"         // HistoryQuery
#include "nimbus/orch/row_overlay.h"       // the patch journal

// The conversation cache (conv_cache.h).
namespace agent {

using nimbus::orch::EpisodicMessage;

size_t ConvCache::rowBytes(const EpisodicMessage& m) {
  return m.id.size() + m.text.size() + m.tags.size() + m.blobPath.size() + m.role.size() + 32;
}

ConvCache::Entry* ConvCache::find(const std::string& chat) {
  for (auto& e : entries_)
    if (e.chat == chat) return &e;
  return nullptr;
}

const ConvCache::Entry* ConvCache::find(const std::string& chat) const {
  for (const auto& e : entries_)
    if (e.chat == chat) return &e;
  return nullptr;
}

void ConvCache::reset(Entry& e, const std::string& thru) {
  e.thru = thru;
  e.rows.clear();
  e.rows.shrink_to_fit();
  e.newestId.clear();
  e.fillBefore.clear();
  e.complete = false;
  e.started = false;
  e.bytes = 0;
  e.span = ConvSpan();
  e.patchSeq = nimbus::orch::rowPatchSeq();   // rows read from here on carry every earlier record
}

ConvCache::Entry& ConvCache::open(const std::string& chat, const std::string& thru) {
  if (Entry* e = find(chat)) return *e;
  if (entries_.size() >= kChats) {   // the least recently used chat goes
    auto lru = std::min_element(entries_.begin(), entries_.end(),
                                [](const Entry& a, const Entry& b) { return a.lastUse < b.lastUse; });
    entries_.erase(lru);
  }
  entries_.emplace_back();
  Entry& e = entries_.back();
  e.chat = chat;
  reset(e, thru);
  return e;
}

// A fold moved thru forward: its rows leave (the summary carries them). An anchor that
// lands inside the span (rows the cache does not hold) cannot be re-counted without a
// read, so the entry starts again; so does an anchor that moved back.
void ConvCache::syncThru(Entry& e, const std::string& thru) {
  if (e.thru == thru) return;
  const bool forward = !thru.empty() && (e.thru.empty() || rowIdAfter(thru, e.thru));
  const bool coversSpan = e.span.empty() || thru == e.span.toId ||
                          (!e.rows.empty() && !rowIdAfter(e.rows.front().id, thru));
  if (!forward || !coversSpan) {
    reset(e, thru);
    return;
  }
  size_t drop = 0, freed = 0;
  while (drop < e.rows.size() && !rowIdAfter(e.rows[drop].id, thru)) freed += rowBytes(e.rows[drop++]);
  e.rows.erase(e.rows.begin(), e.rows.begin() + (long)drop);
  e.bytes -= freed;
  e.thru = thru;
  e.span = ConvSpan();   // the whole span is at or before the new anchor
  if (drop) e.complete = true;   // the rows reached the new anchor
}

void ConvCache::applyPatches(Entry& e) {
  const bool whole = nimbus::orch::rowPatchesSince(e.patchSeq, [&e](const std::string& id, const std::string& text) {
    for (auto& m : e.rows)
      if (m.id == id) {
        m.text = text;
        spillToPsram(m.text);
      }
  });
  if (!whole) {   // records left the journal unseen: read the rows again
    reset(e, e.thru);
    return;
  }
  e.patchSeq = nimbus::orch::rowPatchSeq();
}

// The rows newer than the newest held: one read, oldest first. A full page means the
// entry is more than a page behind: it is filled again newest first instead (an after
// read that pages forward would carry the oldest of them first).
int ConvCache::readNewer(Entry& e, const ReadFn& read) {
  if (e.newestId.empty()) return 0;
  std::vector<EpisodicMessage> got = read(HistoryQuery(kPageRows, std::string(), e.newestId));
  if (got.size() >= kPageRows) {
    reset(e, e.thru);
    return 1;
  }
  for (auto& m : got) {
    if (!rowIdAfter(m.id, e.newestId)) continue;
    e.newestId = m.id;
    if (isClearBoundary(m)) {   // the conversation starts over: older rows are not conversation
      e.rows.clear();
      e.bytes = 0;
      e.span = ConvSpan();
      e.complete = true;
    }
    spillToPsram(m.text);
    spillToPsram(m.tags);
    spillToPsram(m.blobPath);
    e.bytes += rowBytes(m);
    e.rows.push_back(std::move(m));
  }
  return 1;
}

// One page of the fill, newest first, below the oldest row held. It stops at thru, at a
// /clear boundary (kept: the window ends there) and at the chat's first row.
bool ConvCache::fillPage(Entry& e, const Bound& bound, const ReadFn& read) {
  if (e.complete) return false;
  std::vector<EpisodicMessage> got = read(HistoryQuery(kPageRows, e.fillBefore));
  if (!got.empty()) {   // the cursors, before the rows move
    if (!e.started) e.newestId = got.front().id;   // the delta reads after it
    e.fillBefore = got.back().id;
  }
  e.started = true;
  std::vector<EpisodicMessage> older;   // newest first
  bool reached = got.size() < kPageRows;
  for (auto& m : got) {
    if (!e.thru.empty() && !rowIdAfter(m.id, e.thru)) {
      reached = true;
      break;
    }
    const bool clear = isClearBoundary(m);
    older.push_back(std::move(m));
    if (clear) {
      reached = true;
      break;
    }
  }
  for (auto& m : older) {
    spillToPsram(m.text);
    spillToPsram(m.tags);
    spillToPsram(m.blobPath);
    e.bytes += rowBytes(m);
  }
  e.rows.insert(e.rows.begin(), std::make_move_iterator(older.rbegin()), std::make_move_iterator(older.rend()));
  if (reached) e.complete = true;
  enforce(e, bound);
  return true;
}

// The bound: the oldest rows go, counted in the span (the rows after thru the cache does
// not hold). A fill that hit the bound stops there: what is below stays unread.
void ConvCache::enforce(Entry& e, const Bound& bound) {
  size_t drop = 0;
  const auto over = [&]() {
    return (bound.bytes && e.bytes > bound.bytes) || (bound.rows && e.rows.size() - drop > bound.rows);
  };
  while (drop + 1 < e.rows.size() && over()) {
    const EpisodicMessage& m = e.rows[drop];
    if (e.span.empty()) {
      e.span.afterId = e.thru;
      e.span.fromHours = m.tsHours;
    }
    e.span.toId = m.id;
    e.span.toHours = m.tsHours;
    e.span.rows++;
    e.bytes -= rowBytes(m);
    drop++;
  }
  if (!drop) return;
  e.rows.erase(e.rows.begin(), e.rows.begin() + (long)drop);
  e.span.beforeId = e.rows.front().id;
  if (!e.complete) {   // the rest below is left unread
    e.span.uncounted = true;
    e.complete = true;
  }
}

ConvCache::View ConvCache::viewOf(const Entry& e) const {
  View v;
  v.rows = e.rows;
  v.complete = e.complete;
  v.span = e.span;
  if (!e.complete) {   // not filled down to thru yet: the rows below the oldest held are unread
    v.span.afterId = e.thru;
    v.span.beforeId = e.rows.empty() ? e.fillBefore : e.rows.front().id;
    v.span.uncounted = true;
  }
  return v;
}

ConvCache::View ConvCache::access(const std::string& chat, const std::string& thru, const Bound& bound,
                                  int fillPages, const ReadFn& read, const std::function<bool()>& freePage) {
  std::lock_guard<std::mutex> lk(mu_);
  lastBound_ = bound;
  Entry& e = open(chat, thru);
  e.lastUse = ++tick_;
  syncThru(e, thru);
  applyPatches(e);
  if (e.complete && e.newestId.empty()) reset(e, thru);   // a chat that had no row yet: read from the newest
  int pages = readNewer(e, read);
  for (int i = 0; i < fillPages && !e.complete; i++) {
    if (i > 0 && (!freePage || !freePage())) break;
    pages += fillPage(e, bound, read) ? 1 : 0;
  }
  enforce(e, bound);
  View v = viewOf(e);
  v.pages = pages;
  return v;
}

bool ConvCache::fillStep(const std::function<ReadFn(const std::string& chat)>& readerFor) {
  std::lock_guard<std::mutex> lk(mu_);
  Entry* best = nullptr;
  for (auto& e : entries_)
    if (!e.complete && (!best || e.lastUse > best->lastUse)) best = &e;
  if (!best) return false;
  return fillPage(*best, lastBound_, readerFor(best->chat));
}

void ConvCache::drop(const std::string& chat) {
  std::lock_guard<std::mutex> lk(mu_);
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.chat == chat; }),
                 entries_.end());
}

void ConvCache::clear() {
  std::lock_guard<std::mutex> lk(mu_);
  entries_.clear();
}

size_t ConvCache::chats() const {
  std::lock_guard<std::mutex> lk(mu_);
  return entries_.size();
}

size_t ConvCache::bytesOf(const std::string& chat) const {
  std::lock_guard<std::mutex> lk(mu_);
  const Entry* e = find(chat);
  return e ? e->bytes : 0;
}

bool ConvCache::holds(const std::string& chat) const {
  std::lock_guard<std::mutex> lk(mu_);
  return find(chat) != nullptr;
}

std::string ConvCache::filling() const {
  std::lock_guard<std::mutex> lk(mu_);
  const Entry* best = nullptr;
  for (const auto& e : entries_)
    if (!e.complete && (!best || e.lastUse > best->lastUse)) best = &e;
  return best ? best->chat : std::string();
}

}  // namespace agent
