#include "nimbus/harness/web_chat.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace agent {

namespace {

// The CUM-398 batch render (lib/core msg_batch.cpp) opens with this header when
// it folds two or more messages of one chat into a single turn.
const char kBatchHead[] = "Message 1 of ";

// N from a "Message 1 of N ..." batch render, or 0 when the text is one message
// rendered verbatim. ("Message 1 of 1" is real: one message with more waiting.)
size_t batchSize(const std::string& turnText) {
  const size_t headLen = sizeof(kBatchHead) - 1;
  if (turnText.compare(0, headLen, kBatchHead) != 0) return 0;
  const char* p = turnText.c_str() + headLen;
  char* endp = nullptr;
  const unsigned long n = std::strtoul(p, &endp, 10);
  return endp != p ? (size_t)n : 0;
}

// Erase the oldest entry matching `pred`; false when none does.
template <class Entries, class Pred>
bool eraseFirst(Entries& v, Pred pred) {
  const auto it = std::find_if(v.begin(), v.end(), pred);
  if (it == v.end()) return false;
  v.erase(it);
  return true;
}

}  // namespace

std::string WebChatBook::open(const std::string& text, uint32_t nowMs) {
  if (turns_.size() >= kMaxTurns) evictForOpen(nowMs);
  if (turns_.size() >= kMaxTurns) return std::string();   // all unanswered: refuse
  char id[12];
  std::snprintf(id, sizeof(id), "w%08lx", (unsigned long)(++seq_));
  // The inbound queue carries C strings: a message reaches the turn cut at its
  // first NUL, so book exactly that much of it.
  const size_t len = std::min(text.find('\0'), text.size());
  Entry e;
  e.id = id;
  e.whole = len <= kProbeBytes;
  e.probe.assign(text.data(), e.whole ? len : kProbeBytes);
  e.openedMs = nowMs;
  turns_.push_back(std::move(e));
  return turns_.back().id;
}

// Make room: the oldest answered turn goes first, then a queued one whose
// client has long given up. A book full of live turns refuses instead.
void WebChatBook::evictForOpen(uint32_t nowMs) {
  if (eraseFirst(turns_, [](const Entry& e) { return e.state == State::Answered; })) return;
  eraseFirst(turns_, [nowMs](const Entry& e) {
    return e.state == State::Queued && nowMs - e.openedMs > kQueuedMaxMs;
  });
}

void WebChatBook::cancel(const std::string& id) {
  eraseFirst(turns_, [&id](const Entry& e) { return e.id == id && e.state == State::Queued; });
}

bool WebChatBook::claimable(const Entry& e, const std::string& turnText, bool batch) const {
  if (e.state != State::Queued || e.probe.empty()) return false;
  const std::string_view probe(e.probe.data(), e.probe.size());
  // One message renders verbatim: the whole text (or, for a long one, its
  // prefix) IS the turn text - so "hi" never claims a booking of "hi there".
  if (!batch)
    return e.whole ? std::string_view(turnText) == probe
                   : turnText.compare(0, probe.size(), probe) == 0;
  // A batch carries each message right after its "Message k of N ...:\n" header,
  // followed by the "\n\n" separator (or the end of the text).
  for (size_t pos = turnText.find(probe); pos != std::string::npos;
       pos = turnText.find(probe, pos + 1)) {
    const bool headed = pos >= 2 && turnText.compare(pos - 2, 2, ":\n") == 0;
    const size_t after = pos + probe.size();
    const bool closed =
        !e.whole || after == turnText.size() || turnText.compare(after, 2, "\n\n") == 0;
    if (headed && closed) return true;
  }
  return false;
}

size_t WebChatBook::firstClaimable(const std::string& turnText, bool batch) const {
  for (size_t i = 0; i < turns_.size(); i++)
    if (claimable(turns_[i], turnText, batch)) return i;
  return turns_.size();
}

int WebChatBook::begin(const std::string& turnText) {
  if (inTurn_) end();   // defensive: a turn that never reported its end
  size_t n = batchSize(turnText);
  size_t first = firstClaimable(turnText, n != 0);
  // A single message that merely STARTS like a batch header ("Message 1 of 3 ...")
  // is rendered verbatim, not as blocks: match it as the one message it is.
  if (n && first == turns_.size()) {
    n = 0;
    first = firstClaimable(turnText, false);
  }
  if (first == turns_.size()) return 0;   // an unbooked turn: its deliveries are async
  const size_t want = n ? n : 1;
  // Booked turns queued ahead of this one never ran (their message was lost):
  // answer them empty so their clients stop waiting instead of polling 13 min.
  for (size_t i = 0; i < first; i++)
    if (turns_[i].state == State::Queued) turns_[i].state = State::Answered;
  int claimed = 0;
  for (size_t i = first; i < turns_.size() && (size_t)claimed < want; i++) {
    if (!claimable(turns_[i], turnText, n != 0)) break;
    turns_[i].state = State::Running;
    claimed++;
  }
  inTurn_ = true;
  current_.clear();
  return claimed;
}

void WebChatBook::deliver(const std::string& text) {
  if (text.empty()) return;
  if (!inTurn_) { asyncSeq_++; return; }   // between turns: never a turn's reply
  if (!current_.empty()) current_ += "\n\n";
  current_.append(text.data(), text.size());
}

void WebChatBook::deliverAsync(const std::string& text) {
  if (!text.empty()) asyncSeq_++;
}

void WebChatBook::end() {
  if (!inTurn_) return;
  for (auto& e : turns_)
    if (e.state == State::Running) { e.state = State::Answered; e.reply = current_; }
  current_.clear();
  inTurn_ = false;
}

WebChatBook::View WebChatBook::view(const std::string& id, uint32_t nowMs) const {
  View v;
  for (const auto& e : turns_) {
    if (e.id != id) continue;
    v.state = e.state;
    // A booked turn that never started is gone, not "queued forever".
    if (e.state == State::Queued && nowMs - e.openedMs > kQueuedMaxMs) v.state = State::Unknown;
    if (e.state == State::Answered) v.reply.assign(e.reply.data(), e.reply.size());
    return v;
  }
  return v;
}

bool deliverToWebChat(WebChatBook& book, const std::string& text, bool routine) {
  if (routine) {
    book.deliverAsync(text);
    return false;
  }
  book.deliver(text);
  return true;
}

// ---- route bodies -----------------------------------------------------------

std::string chatPostJson(const std::string& turn) {
  JsonDocument d;
  d["ok"] = true;
  d["turn"] = turn;
  std::string out;
  serializeJson(d, out);
  return out;
}

namespace {

void fillTurn(JsonDocument& d, const std::string& turn, const WebChatBook::View& v) {
  using State = WebChatBook::State;
  d["turn"] = turn;
  d["reply"] = v.reply;
  d["pending"] = v.state == State::Queued || v.state == State::Running;
  d["done"] = v.state == State::Answered;
  d["gone"] = v.state == State::Unknown;
}

void fillLegacy(JsonDocument& d, const ChatGetDeps& deps) {
  const std::string reply = deps.legacyPoll ? deps.legacyPoll() : std::string();
  // `pending` = a turn is RUNNING, not merely "no reply queued" (the resting
  // state). Falls back to the old inference when the hook is absent.
  d["reply"] = reply;
  d["pending"] = reply.empty() && (deps.legacyPending ? deps.legacyPending() : true);
}

}  // namespace

std::string chatGetJson(const ChatGetRequest& rq, const ChatGetDeps& d) {
  JsonDocument doc;
  doc.to<JsonObject>();   // a bare watch with nothing wired is still "{}", never "null"
  if (rq.hasTurn && d.view) fillTurn(doc, rq.turn, d.view(rq.turn));
  else if (!rq.watch) fillLegacy(doc, d);
  if (d.activity) {
    uint32_t asyncSeq = 0;
    int busy = 0;
    d.activity(asyncSeq, busy);
    doc["async"] = asyncSeq;
    doc["busy"] = busy;
  }
  std::string out;
  serializeJson(doc, out);
  return out;
}

}  // namespace agent
