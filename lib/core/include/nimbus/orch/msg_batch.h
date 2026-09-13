#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/caps.h"
#include "nimbus/orch/psram_alloc.h"
#include "nimbus/telegram_offset.h"

// msg_batch - portable, host-tested accumulator for CUM-398 rapid-message
// batching. When several messages arrive in quick succession, the device drains
// them into ONE orchestrator turn per chat instead of one turn per message. This
// class owns the pure logic: bucket by chatId (arrival order preserved), enforce
// the hard bounds (per-chat bytes/count, chat count, total bytes) so it can never
// overflow, and render each chat's messages into a single model-facing turn text
// with preserved per-message boundaries.
//
// It adds NO concurrency: the caller (the tg_poll task) fills it serially and
// emits one turn per bucket, still single-task, single-TLS (AGENTS.md 4).
//
// Memory: every accumulated payload byte lives behind the WorkingAllocator, which
// routes to PSRAM on the device (installed at boot) and to malloc on the host, so
// the whole working set stays off the scarce internal SRAM. The batcher object
// itself is a handful of pointers; reuse one instance across drains and clear()
// between them.
namespace nimbus {
namespace orch {

// A PSRAM-routed string: SSO content rides inside its owning bucket (itself in
// PSRAM) and any overflow allocation lands in PSRAM too. Used for every stored
// payload and for the rendered turn text, so a batch never touches internal SRAM.
using PsString = std::basic_string<char, std::char_traits<char>, WorkingAllocator<char>>;

// Outcome of add(): whether the message was buffered, and whether a cap fired.
enum class BatchAdd : uint8_t {
  Accepted,    // buffered into its chat bucket; more of this chat still fits
  ChatFull,    // buffered, but this chat has now reached a cap; the next add for it
               // will be Deferred. (The caller may keep draining OTHER chats.)
  Deferred,    // NOT buffered: a cap prevented it (this chat is full, the chat-count
               // cap is hit for a new chat, or the total-bytes cap is hit). The
               // caller must stop advancing its ack point and re-serve this message.
};

class MessageBatcher {
 public:
  // Add one message to chatId's bucket, in arrival order. `from` may be empty.
  // Byte accounting counts the payload (text) only, not render scaffolding.
  BatchAdd add(const std::string& chatId, const std::string& from, const std::string& text);

  size_t chatCount() const { return chats_.size(); }
  bool   empty()     const { return chats_.empty(); }

  // Chats are addressed by first-seen (insertion) index, so the caller emits turns
  // in the order the chats first appeared in the drain. The accessors return
  // NUL-terminated C strings (the device wraps them in an Arduino String).
  const char* chatIdAt(size_t i)   const { return chats_[i].chatId.c_str(); }
  // The sender to attribute the turn to (the FIRST message's `from` for this chat).
  const char* turnFromAt(size_t i) const { return chats_[i].turnFrom.c_str(); }
  size_t messageCountAt(size_t i)  const { return chats_[i].msgs.size(); }
  bool   cappedAt(size_t i)        const { return chats_[i].capped; }

  // Render chat i's buffered messages into ONE model-facing turn text:
  //   - exactly one message and not capped -> its raw text, unchanged (byte-for-byte
  //     identical to the pre-batching one-turn-per-message behavior);
  //   - two or more -> each message as a delimited block in arrival order, so the
  //     model reads them as distinct messages, not one run-on, with per-message from;
  //   - if this chat was capped (messages were shed because a cap fired) an honest
  //     trailing note says more are waiting.
  // The note and delimiters follow device copy rules (no em dash, no " - ").
  PsString render(size_t i) const;

  void clear();

 private:
  struct Msg {
    PsString chatFrom;   // per-message sender (may differ within a group chat)
    PsString text;
  };
  struct Bucket {
    PsString chatId;
    PsString turnFrom;
    std::vector<Msg, WorkingAllocator<Msg>> msgs;
    size_t   bytes  = 0;      // running payload bytes (text only)
    bool     capped = false;  // a message for this chat was shed (more waiting)
  };

  Bucket* find(const std::string& chatId);

  std::vector<Bucket, WorkingAllocator<Bucket>> chats_;
  size_t totalBytes_ = 0;
};

// ---- Drain driver -----------------------------------------------------------
// The pagination + offset/ack loop, factored out of the device so it is host-
// tested (pagination past a full page, offset re-serve on a busy slot or a cap,
// and drain-to-empty are the acceptance criteria that most need proving). The
// device provides fetchPage (real getUpdates + per-update classification with its
// inline side effects) and then emits the returned turns and commits ackOffset.

// One update after the device has classified it (and performed any inline side
// effect: queueing a voice note, calling the files sink, pushing a pending sender).
struct ClassifiedUpdate {
  enum class Action : uint8_t {
    BatchText,   // an allowed text message: add it to the batcher
    AckOnly,     // handled inline (pending-approval, disallowed, a deferred file
                 // queued): advance the ack past it, do not batch
    StopNoAck,   // a deferred slot (voice/attachment) is busy: stop the drain WITHOUT
                 // acking this update, so it and the rest are re-served next cycle
  };
  int32_t     updateId = 0;
  Action      action   = Action::AckOnly;
  std::string chatId;   // BatchText only
  std::string from;     // BatchText only
  std::string text;     // BatchText only
};

// One page returned by the device's fetchPage.
struct DrainPage {
  std::vector<ClassifiedUpdate> updates;
  int  rawCount   = 0;      // updates the parser returned (rawCount < limit => drained)
  int  limit      = 10;     // the getUpdates limit used for THIS fetch (1 in fallback)
  bool fetchError = false;  // an HTTP/parse error on this page
};

// A turn to run: one per chat, in first-seen order.
struct DrainTurn {
  std::string from;
  std::string chatId;
  std::string text;
};

struct DrainResult {
  int32_t ackOffset       = 0;  // commit this AFTER the turns run (crash-safety)
  int     turnsEmitted    = 0;
  int     messagesBatched = 0;
  int     pagesFetched    = 0;
  bool    stopped         = false;  // stopped on a cap / busy slot (rest re-served)
  bool    fetchError      = false;  // page 0 failed: nothing accumulated (caller backs off)
};

// Drive the drain. fetchPage(offset, firstPage) fetches one getUpdates page at the
// given offset. Text is batched by chat (bounded); the loop advances the offset and
// paginates while a page is full, and stops on a cap, a busy slot, server-drained,
// or the page ceiling. Turns to emit are appended to `turns`; the offset to commit
// is DrainResult::ackOffset. The batcher is cleared on entry.
template <class FetchFn>
DrainResult drainPages(MessageBatcher& batch, int32_t startOffset, FetchFn fetchPage,
                       std::vector<DrainTurn>& turns) {
  batch.clear();
  DrainResult r;
  r.ackOffset = startOffset;
  int32_t offset = startOffset;
  bool stop = false;

  for (int page = 0; page < kBatchMaxPages && !stop; ++page) {
    DrainPage p = fetchPage(offset, page == 0);
    r.pagesFetched++;
    if (p.fetchError) {
      // Page 0 error: nothing accumulated, tell the caller to back off. A later page
      // error keeps what earlier pages accumulated (already server-confirmed by
      // pagination) and just stops draining.
      if (page == 0) r.fetchError = true;
      break;
    }
    for (const auto& u : p.updates) {
      if (u.updateId <= 0) continue;
      if (u.action == ClassifiedUpdate::Action::StopNoAck) { stop = true; break; }
      if (u.action == ClassifiedUpdate::Action::BatchText) {
        if (batch.add(u.chatId, u.from, u.text) == BatchAdd::Deferred) {
          stop = true;   // a cap fired: do NOT ack this update, re-serve it next cycle
          break;
        }
        r.messagesBatched++;
      }
      // BatchText(accepted/full) or AckOnly: this update is handled, advance the ack.
      offset = nimbus::core::nextTelegramOffset(offset, u.updateId);
      r.ackOffset = offset;
    }
    if (stop) break;
    if (p.rawCount < p.limit) break;   // fewer than the limit: the server is drained
    // A full page: loop. The next fetch at the advanced offset confirms this page
    // server-side (the documented at-least-once window for a >1-page backlog).
  }

  for (size_t i = 0; i < batch.chatCount(); ++i) {
    DrainTurn t;
    t.from   = batch.turnFromAt(i);
    t.chatId = batch.chatIdAt(i);
    t.text   = batch.render(i).c_str();
    turns.push_back(std::move(t));
  }
  r.turnsEmitted = static_cast<int>(turns.size());
  r.stopped = stop;
  return r;
}

}  // namespace orch
}  // namespace nimbus
