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

// A fetched page, before its updates are classified. The count/limit drive the
// drain-to-empty decision (count < limit means the server is drained). It carries NO
// update contents: the driver reads each update lazily through handle(i), so update
// bodies (and their inline side effects) never need to exist all at once.
struct RawPage {
  int  count      = 0;      // whole updates the page holds (count < limit => drained)
  int  limit      = 10;     // the getUpdates limit used for THIS fetch (1 in fallback)
  bool fetchError = false;  // an HTTP/parse error on this page
};

struct DrainResult {
  int32_t ackOffset       = 0;  // commit this AFTER the turns run (per-page crash-safety)
  int     turnsEmitted    = 0;  // == chat buckets left in the batcher (one turn each)
  int     messagesBatched = 0;
  bool    stopped         = false;  // stopped on a cap / busy slot (rest re-served)
  bool    fetchError      = false;  // the fetch failed: nothing accumulated (caller backs off)
};

// Drain ONE getUpdates page into the batcher.
//   fetchPage(offset) -> RawPage : fetch the page at `offset` and make its updates
//     addressable by handle(i) for i in [0, RawPage::count).
//   handle(i) -> ClassifiedUpdate : classify the i-th update of that page and run any
//     inline side effect (queue a voice note, push a pending sender, ...).
//
// ONE page per call is deliberate and load-bearing for crash-safety. getUpdates
// confirms a page server-side only when the NEXT page is fetched, so the caller must
// run this page's turns AND durably commit DrainResult::ackOffset BEFORE the next
// drain fetches the next page. Then each page is at-least-once with NO loss: a reset
// (brownout / WDT / panic / OTA) mid-drain leaves the offset unmoved, so the whole
// current page is re-served; a committed page is never re-run. Draining several pages
// in one call (committing once at the end) would make earlier pages at-most-once - the
// data-loss bug this shape exists to avoid. A rapid burst still fits one page = one
// turn; a multi-page backlog drains one page per cycle.
//
// handle(i) is called LAZILY and IN ORDER, and ONLY for updates the drain reaches: the
// moment a text update trips a cap or a deferred slot is busy, the loop stops and never
// calls handle for the rest, so a side effect never runs for an update that then goes
// un-acked and is re-served (which would run it twice). Side effect and ack advance
// together, update by update.
//
// On return the batcher holds one bucket per chat (the caller emits one turn each,
// reading render(i) one at a time so a single rendered text is alive at once, in PSRAM,
// never a vector of them on internal SRAM). The batcher is cleared on ENTRY and left
// populated on exit (the caller clears it after emitting).
template <class FetchFn, class HandleFn>
DrainResult drainOnePage(MessageBatcher& batch, int32_t startOffset, FetchFn fetchPage,
                         HandleFn handle) {
  batch.clear();
  DrainResult r;
  r.ackOffset = startOffset;
  int32_t offset = startOffset;

  RawPage p = fetchPage(offset);
  if (p.fetchError) { r.fetchError = true; return r; }   // nothing accumulated; back off

  for (int i = 0; i < p.count; ++i) {
    ClassifiedUpdate u = handle(i);   // side effect (if any) runs HERE, in order
    if (u.updateId <= 0) continue;
    if (u.action == ClassifiedUpdate::Action::StopNoAck) { r.stopped = true; break; }
    if (u.action == ClassifiedUpdate::Action::BatchText) {
      if (batch.add(u.chatId, u.from, u.text) == BatchAdd::Deferred) {
        r.stopped = true;   // a cap fired: do NOT ack this update, re-serve it next drain
        break;
      }
      r.messagesBatched++;
    }
    // BatchText(accepted/full) or AckOnly: this update is handled, advance the ack.
    offset = nimbus::core::nextTelegramOffset(offset, u.updateId);
    r.ackOffset = offset;
  }

  r.turnsEmitted = static_cast<int>(batch.chatCount());
  return r;
}

}  // namespace orch
}  // namespace nimbus
