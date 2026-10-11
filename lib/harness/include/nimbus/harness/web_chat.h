#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/orch/psram_alloc.h"   // WorkingAllocator - reply text rides PSRAM on the device

// web_chat - per-turn reply pairing for the device web chat (CUM-482).
//
// The bug this replaces: GET /api/chat served ONE global slot that every
// delivery to chat "web" overwrote, so an async delivery (a sub-agent synthesis,
// a spawn ack, a raw fallback) that landed between two turns was handed to the
// NEXT turn's first poll as its reply - off by one from then on.
//
// The book pairs strictly by turn:
//   * POST /api/chat books the message under a fresh turn id (open) BEFORE it is
//     injected, and returns that id to the client;
//   * when tg_poll starts the web turn, begin() claims the booked turn(s) the turn
//     answers (a CUM-398 batch folds several POSTs into one turn);
//   * every delivery to chat "web" between begin() and end() is that turn's
//     reply; a delivery outside a claimed turn is ASYNC - it bumps asyncSeq() for
//     the history watcher and is never attached to any turn;
//   * GET /api/chat?turn=<id> reads view(id): queued / running / answered.
//
// Why a turn id and not the episodic user row id: the user row is captured on
// tg_poll when the turn starts, after POST has answered, and a batch folds
// several POSTs into one row - so POST cannot know a row id. The episodic rows
// keep their turn:<user row id> tags exactly as before (the chat history view
// pairs by those); this book only answers "what was THIS POST's reply".
//
// Portable and single-threaded by contract: the device calls every method under
// one mutex (POST/GET on the AsyncTCP task, begin/deliver/end on tg_poll).
// Bounded: at most kMaxTurns entries; answered turns are evicted oldest-first.
// The message probes and reply texts live behind the WorkingAllocator (PSRAM on
// the device), so retained answers never sit on the scarce internal heap.
namespace agent {

class WebChatBook {
 public:
  enum class State : uint8_t { Unknown = 0, Queued = 1, Running = 2, Answered = 3 };
  struct View {
    State state = State::Unknown;
    std::string reply;   // Answered only: every delivery the turn made, in order
  };

  static constexpr size_t kMaxTurns = 8;   // > the inbound queue a client can fill
  // A booked turn that never started (its message was lost) stops reading as
  // queued after this long - the same 12 min backstop the legacy slot used.
  static constexpr uint32_t kQueuedMaxMs = 12UL * 60UL * 1000UL;
  // Bytes of a message used to recognize it in the turn text. The inbound queue
  // carries at most 4 KB of a message, so only a prefix is ever comparable.
  static constexpr size_t kProbeBytes = 256;

  // `seed` makes turn ids unique across restarts (the device seeds from its RNG)
  // so a client still polling an old id can never read a new turn. reseed() is
  // for a seed that only becomes trustworthy later; call it before the first open.
  explicit WebChatBook(uint32_t seed = 0) : seq_(seed) {}
  void reseed(uint32_t seed) { seq_ = seed; }

  // POST: book `text` as a new turn. Returns its id, or "" when the book is full
  // of unanswered turns (the route answers busy instead of pretending success).
  // The caller books and injects under one lock, so bookings are in queue order:
  // turns run FIFO and each claims the oldest matching booking - its own.
  std::string open(const std::string& text, uint32_t nowMs);
  // POST failed to inject the booked message: forget it.
  void cancel(const std::string& id);

  // tg_poll: a web turn starts with this text (one message verbatim, or the
  // CUM-398 "Message 1 of N" batch render). Claims the booked turns it answers
  // and returns how many (0 = an unbooked turn, e.g. a test inject; its
  // deliveries count as async). Booked turns queued AHEAD of the claimed ones
  // never ran (lost): they are answered empty so their clients stop waiting.
  int begin(const std::string& turnText);
  // A delivery to chat "web" (tg_poll, inside or outside a turn).
  void deliver(const std::string& text);
  // A delivery that is never a turn's reply, whatever is running: it only bumps
  // asyncSeq() so an open page re-renders its history (a routine's output).
  void deliverAsync(const std::string& text);
  // tg_poll: the web turn ended. Its claimed turns become Answered.
  void end();

  View view(const std::string& id, uint32_t nowMs) const;
  uint32_t asyncSeq() const { return asyncSeq_; }

 private:
  using PsText = std::basic_string<char, std::char_traits<char>,
                                   nimbus::orch::WorkingAllocator<char>>;
  struct Entry {
    std::string id;      // "w%08x" - inside the small-string buffer, no allocation
    PsText probe;        // first kProbeBytes of the message
    bool whole = true;   // the probe IS the whole message (it fit)
    State state = State::Queued;
    uint32_t openedMs = 0;
    PsText reply;
  };
  bool claimable(const Entry& e, const std::string& turnText, bool batch) const;
  size_t firstClaimable(const std::string& turnText, bool batch) const;   // size() = none
  void evictForOpen(uint32_t nowMs);

  std::vector<Entry> turns_;     // oldest first
  PsText current_;               // deliveries of the running turn so far
  bool inTurn_ = false;          // begin() claimed at least one booked turn
  uint32_t seq_;
  uint32_t asyncSeq_ = 0;
};

// One delivery to chat "web" (the device's orchSendSink), so the host suite drives
// the real routing. Every delivery reaches the book: inside a claimed turn it is
// that turn's reply, otherwise it is async (asyncSeq moves and an open page
// re-renders its history, where captureAssistant already put the text). A
// routine's delivery (`routine`: a loop's scheduled turn is running) is ALWAYS
// async (CUM-505): nobody is waiting on it, so it is never a turn's reply. Returns
// true when the caller must also fill the legacy single slot (clients that poll
// without a turn id) - never for a routine, whose text the next legacy poll would
// otherwise show as the answer to the owner's next message.
bool deliverToWebChat(WebChatBook& book, const std::string& text, bool routine);

// ---- the /api/chat route bodies (portable: test_harness_webchat drives the
// real decision; the device handlers only adapt the HTTP request) -------------

// POST /api/chat accepted the message: {"ok":true,"turn":"<id>"}.
std::string chatPostJson(const std::string& turn);

struct ChatGetRequest {
  bool hasTurn = false;   // ?turn=<id> - this turn's reply (idempotent)
  std::string turn;
  bool watch = false;     // ?watch=1 - activity only, consumes nothing
};
struct ChatGetDeps {
  std::function<WebChatBook::View(const std::string& turn)> view;   // null => legacy only
  std::function<std::string()> legacyPoll;   // returns + CLEARS the legacy slot
  std::function<bool()> legacyPending;       // null => "pending while empty" (old inference)
  std::function<void(uint32_t& asyncSeq, int& busy)> activity;   // nullable
};
// GET /api/chat. With a turn id: {turn, reply, pending, done, gone} for THAT turn
// only (a delivery between turns is never in it). With watch: activity only.
// Neither: the legacy single slot {reply, pending}. Every body adds {async, busy}
// when activity is wired, so the page can pick up async results by itself.
std::string chatGetJson(const ChatGetRequest& rq, const ChatGetDeps& d);

}  // namespace agent
