#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/caps.h"
#include "nimbus/orch/command.h"
#include "nimbus/orch/psram_alloc.h"
#include "nimbus/telegram_offset.h"

// msg_batch - portable, host-tested accumulator for CUM-398 rapid-message
// batching. When several messages arrive in quick succession, the device drains
// them into ONE orchestrator turn per chat instead of one turn per message. This
// class owns the pure logic: bucket by chatId (arrival order preserved), enforce
// the hard bounds (per-chat bytes/count, chat count, total bytes, media count) so it
// can never overflow, and render each chat's messages into a single model-facing
// turn text with preserved per-message boundaries.
//
// Albums and bursts as one turn (R2): a photo or document is a media segment of its
// chat's bucket, in arrival order, interleaved with the text messages; its caption
// rides with it at full length. The device fills each media segment's text (the one
// label, nimbus/orch/media_label.h, plus the description) after it has downloaded and
// described the item, then renders and runs the turn. At most kBatchMaxMediaPerChat
// media share a turn (the Telegram album maximum) and kBatchMaxMediaPerDrain occupy
// the device's slots in one drain; the next one is Deferred and served next drain.
//
// A slash command never shares a turn (CommandBoundary): inside a "Message 1 of N"
// block it would reach the model as conversation instead of running as a command.
// So a command gets a bucket of its own, and a chat's messages after it start
// another. A voice note ends the batch the same way (VoiceBoundary): it is a turn of
// its own (its transcript), so the reply order follows the order things were sent.
// A chat can therefore own several buckets in one drain; each bucket is one turn,
// emitted in creation order, so the chat's order is kept. The bucket count is bounded
// by kBatchMaxChats (turns per drain).
//
// Contiguous mode (setContiguous, set by the drain for a page that holds a command):
// a message joins only the newest bucket of the whole drain, and only when that bucket
// is its chat's and still open. Each bucket is then one contiguous run of update ids,
// so the drain can store the offset after every bucket and a restart re-runs at most
// the bucket in flight; a command whose turn finished is never run again.
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
  Accepted,         // buffered into its chat bucket; more of this chat still fits
  TurnFull,         // buffered, but this chat's bucket has now reached a cap; the next
                    // add for it will be Deferred. (The caller may keep draining OTHER
                    // chats.)
  CommandBoundary,  // buffered as a turn of its own: a slash command never shares a
                    // turn. The chat's earlier messages stay one turn, the command is
                    // the next, and the chat's next message starts another.
  VoiceBoundary,    // buffered as a turn of its own: a voice note (its transcript),
                    // with the same boundaries as a command.
  Deferred,         // NOT buffered: a cap prevented it (this chat's bucket is full, the
                    // bucket-count cap is hit for a new turn, the total-bytes cap is hit,
                    // or the media caps). The caller must stop advancing its ack point
                    // and re-serve this message.
};

// What one buffered message is.
enum class SegKind : uint8_t {
  Text,    // a text message (or a caption with no media the device could take)
  Media,   // a photo or document: its segment text comes from the device (setMediaText)
  Voice,   // a voice note: a turn of its own, its text is the transcript (setVoiceText)
};

class MessageBatcher {
 public:
  // Add one text message to chatId's newest bucket, in arrival order (a command, or
  // the first message after one, opens a new bucket). `from` may be empty. Byte
  // accounting counts the payload (text) only, not render scaffolding.
  // `updateId` (0 when there is none, as for web/serial/voice injects) is kept as the
  // bucket's first id when the message opens a bucket. `sentAt` ("" when on time)
  // marks a late or redelivered message with its original send time.
  BatchAdd add(const std::string& chatId, const std::string& from, const std::string& text,
               int32_t updateId = 0, const std::string& sentAt = std::string());

  // Add a photo or document as a media segment of chatId's open bucket (or a new one).
  // `caption` is its caption at full length (counted like text); `slot` is the
  // device's index for the item's download state. Deferred when this chat's turn
  // already holds kBatchMaxMediaPerChat media, the drain holds kBatchMaxMediaPerDrain,
  // or a text cap fires.
  BatchAdd addMedia(const std::string& chatId, const std::string& from, const std::string& caption,
                    int slot, int32_t updateId = 0);

  // Add a voice note as a turn of its own (VoiceBoundary); it takes a media slot.
  BatchAdd addVoice(const std::string& chatId, const std::string& from, int slot,
                    int32_t updateId = 0);

  // Contiguous mode (see the header comment). Reset to off by clear().
  void setContiguous(bool on) { contiguous_ = on; }

  // Buckets (one turn each) in this drain. A chat owns more than one when a command
  // or a voice note split it.
  size_t turnCount() const { return buckets_.size(); }
  bool   empty()     const { return buckets_.empty(); }

  // Buckets are addressed by creation index, so the caller emits turns in arrival
  // order per chat. The accessors return NUL-terminated C strings (the device wraps
  // them in an Arduino String).
  const char* chatIdAt(size_t i)   const { return buckets_[i].chatId.c_str(); }
  // The sender to attribute the turn to (the FIRST message's `from` for this chat).
  const char* turnFromAt(size_t i) const { return buckets_[i].turnFrom.c_str(); }
  size_t messageCountAt(size_t i)  const { return buckets_[i].msgs.size(); }
  bool   cappedAt(size_t i)        const { return buckets_[i].capped; }
  // The bucket holds one slash command (its render is the command, byte for byte).
  bool   commandAt(size_t i)       const { return buckets_[i].command; }
  // The update id of the bucket's first message (its lowest: buckets open in order).
  int32_t firstIdAt(size_t i)      const { return buckets_[i].firstId; }
  // The update id of message m of bucket i (0 for an inject).
  int32_t updateIdAt(size_t i, size_t m) const { return buckets_[i].msgs[m].updateId; }
  SegKind kindAt(size_t i, size_t m) const { return buckets_[i].msgs[m].kind; }

  // Media of bucket i, in arrival order: k in [0, mediaCountAt(i)).
  size_t  mediaCountAt(size_t i) const { return buckets_[i].media; }
  int     mediaSlotAt(size_t i, size_t k) const { return mediaSeg(i, k).slot; }
  int32_t mediaUpdateIdAt(size_t i, size_t k) const { return mediaSeg(i, k).updateId; }
  const char* mediaCaptionAt(size_t i, size_t k) const { return mediaSeg(i, k).text.c_str(); }
  // The segment text of media k (the label plus the description, or why it has none).
  void setMediaText(size_t i, size_t k, const std::string& text);
  const char* mediaTextAt(size_t i, size_t k) const { return mediaSeg(i, k).segText.c_str(); }
  // Media items held by this drain (every bucket), for the device's slot budget.
  size_t  mediaInDrain() const { return mediaTotal_; }

  // The bucket is one voice note; its slot, and its transcript once the device has it.
  bool voiceAt(size_t i) const { return buckets_[i].voice; }
  int  voiceSlotAt(size_t i) const { return buckets_[i].msgs[0].slot; }
  void setVoiceText(size_t i, const std::string& transcript);

  // Render bucket i's buffered messages into ONE model-facing turn text:
  //   - exactly one text message and not capped -> its raw text, unchanged (byte-for-
  //     byte identical to the pre-batching one-turn-per-message behavior); a late one
  //     is prefixed with its send time;
  //   - exactly one media item -> its segment text, then its caption ("They wrote:");
  //   - a voice note -> its transcript;
  //   - two or more -> each message as a delimited block in arrival order, so the
  //     model reads them as distinct messages, not one run-on, with per-message from;
  //     a photo's block holds its segment text and caption;
  //   - if this chat was capped (messages were shed because a cap fired) an honest
  //     trailing note says more are waiting.
  // The note and delimiters follow device copy rules (no em dash, no " - ").
  PsString render(size_t i) const;

  void clear();

 private:
  struct Msg {
    SegKind  kind = SegKind::Text;
    PsString chatFrom;   // per-message sender (may differ within a group chat)
    PsString text;       // the text, a media item's caption, or a voice transcript
    PsString segText;    // Media: the label and description (setMediaText)
    PsString sentAt;     // a late message's send time ("" when on time)
    int      slot = -1;  // Media / Voice: the device's slot
    int32_t  updateId = 0;
  };
  struct Bucket {
    PsString chatId;
    PsString turnFrom;
    std::vector<Msg, WorkingAllocator<Msg>> msgs;
    size_t   bytes   = 0;      // running payload bytes (text and captions)
    size_t   media   = 0;      // media segments in this bucket
    bool     capped  = false;  // a message for this chat was shed (more waiting)
    bool     command = false;  // sealed: one slash command, a turn of its own
    bool     voice   = false;  // sealed: one voice note, a turn of its own
    int32_t  firstId = 0;      // update id of the first message (0: none)
  };
  struct NewMsg {
    SegKind kind;
    const std::string& from;
    const std::string& text;
    const std::string& sentAt;
    int slot;
    int32_t updateId;
  };

  // The bucket a new message of `chatId` may join or close: the chat's newest bucket,
  // or in contiguous mode the drain's newest bucket if it is this chat's (else none).
  Bucket* lastBucket(const std::string& chatId);
  static bool full(const Bucket& b);
  static bool sealed(const Bucket& b) { return b.command || b.voice; }
  const Msg& mediaSeg(size_t i, size_t k) const;
  Msg*    mediaSegMut(size_t i, size_t k);
  void    append(Bucket& b, const NewMsg& m);
  BatchAdd addToOpen(Bucket& b, const NewMsg& m);
  BatchAdd addNewBucket(Bucket* last, const std::string& chatId, const NewMsg& m, bool command);
  BatchAdd addAny(const std::string& chatId, const NewMsg& m, bool command);
  static void renderBody(PsString& out, const Msg& m);

  std::vector<Bucket, WorkingAllocator<Bucket>> buckets_;
  size_t totalBytes_ = 0;
  size_t mediaTotal_ = 0;
  bool   contiguous_ = false;
};

// ---- Drain driver -----------------------------------------------------------
// The pagination + offset/ack loop, factored out of the device so it is host-
// tested (pagination past a full page, offset re-serve on a busy slot or a cap,
// and drain-to-empty are the acceptance criteria that most need proving). The
// device provides fetchPage (real getUpdates) and handle (per-update classification
// with its inline side effects); drainCycle emits the turns and advances the offset
// cursor in RAM (the stored copy is written by the cursor's persist/checkpoint).

// One update after the device has classified it (and performed any inline side
// effect: queueing a voice note, calling the files sink, pushing a pending sender).
struct ClassifiedUpdate {
  enum class Action : uint8_t {
    BatchText,   // an allowed text message: add it to the batcher
    BatchMedia,  // an allowed photo or document: a media segment of its chat's turn
    BatchVoice,  // an allowed voice note: a turn of its own (its transcript)
    AckOnly,     // handled inline (pending-approval, disallowed, a duplicate): advance
                 // the ack past it, do not batch
    StopNoAck,   // no room for it this drain (the device's media slots are full): stop
                 // the drain WITHOUT acking this update, so it and the rest are
                 // re-served next cycle
  };
  int32_t     updateId = 0;
  Action      action   = Action::AckOnly;
  std::string chatId;   // Batch* only
  std::string from;     // Batch* only
  std::string text;     // BatchText: the text; BatchMedia: the caption (full length)
  std::string sentAt;   // BatchText: a late message's send time ("" when on time)
  int         slot = -1;   // BatchMedia / BatchVoice: the device's slot for the item
};

// A fetched page, before its updates are classified. The count/limit drive the
// drain-to-empty decision (count < limit means the server is drained). It carries NO
// update contents: the driver reads each update lazily through handle(i), so update
// bodies (and their inline side effects) never need to exist all at once.
struct RawPage {
  int  count       = 0;      // whole updates the page holds (count < limit => drained)
  int  limit       = 10;     // the getUpdates limit used for THIS fetch (1 in fallback)
  bool fetchError  = false;  // an HTTP/parse error on this page
  int  retryAfterS = -1;     // with fetchError: Telegram's retry_after (-1 when none)
  bool hasCommand  = false;  // some update's text is a (non-forwarded) slash command:
                             // the drain batches contiguously and commits per bucket
};

struct DrainResult {
  int32_t ackOffset       = 0;  // advance the offset cursor to this AFTER the turns run
  int     turnsEmitted    = 0;  // == buckets left in the batcher (one turn each)
  int     messagesBatched = 0;
  bool    stopped         = false;  // stopped on a cap / busy slot (rest re-served)
  bool    fetchError      = false;  // the fetch failed: nothing accumulated (caller backs off)
  int     retryAfterS     = -1;     // with fetchError: Telegram's retry_after (-1 when none)
  bool    hasCommand      = false;  // the page held a command (contiguous, per-bucket commit)
};

// Drain ONE getUpdates page into the batcher.
//   fetchPage(offset) -> RawPage : fetch the page at `offset` and make its updates
//     addressable by handle(i) for i in [0, RawPage::count).
//   handle(i) -> ClassifiedUpdate : classify the i-th update of that page and run any
//     inline side effect (queue a voice note, push a pending sender, ...).
//
// ONE page per call is deliberate and load-bearing for crash-safety. getUpdates
// confirms a page server-side only when the NEXT poll carries a higher offset, so the
// caller runs this page's turns and only then advances its offset cursor to
// DrainResult::ackOffset (drainCycle below), before the next drain fetches the next
// page. Then each page is at-least-once with NO loss: a reset (brownout / WDT / panic /
// OTA) mid-drain leaves the page unconfirmed, so the whole current page is re-served,
// and a page the next poll confirmed is never re-run. The cursor lives in RAM; its
// stored copy only seeds the cursor after a restart, so a failed write costs at most a
// replay of the one page drained just before the restart, never a loop (see
// TgOffsetCursor). Draining several pages in one call would make earlier pages
// at-most-once - the data-loss bug this shape exists to avoid. A rapid burst still fits
// one page; a multi-page backlog drains one page per cycle.
//
// handle(i) is called LAZILY and IN ORDER, and ONLY for updates the drain reaches: the
// moment a text update trips a cap or a deferred slot is busy, the loop stops and never
// calls handle for the rest, so a side effect never runs for an update that then goes
// un-acked and is re-served (which would run it twice). Side effect and ack advance
// together, update by update.
//
// On return the batcher holds one bucket per turn (the caller emits one turn each,
// reading render(i) one at a time so a single rendered text is alive at once, in PSRAM,
// never a vector of them on internal SRAM). The batcher is cleared on ENTRY and left
// populated on exit (the caller clears it after emitting).
// Route one classified update into the batcher by its action (never AckOnly or
// StopNoAck: the drain handles those).
inline BatchAdd addClassified(MessageBatcher& batch, const ClassifiedUpdate& u) {
  switch (u.action) {
    case ClassifiedUpdate::Action::BatchMedia:
      return batch.addMedia(u.chatId, u.from, u.text, u.slot, u.updateId);
    case ClassifiedUpdate::Action::BatchVoice:
      return batch.addVoice(u.chatId, u.from, u.slot, u.updateId);
    default:
      return batch.add(u.chatId, u.from, u.text, u.updateId, u.sentAt);
  }
}

template <class FetchFn, class HandleFn>
DrainResult drainOnePage(MessageBatcher& batch, int32_t startOffset, FetchFn fetchPage,
                         HandleFn handle) {
  batch.clear();
  DrainResult r;
  r.ackOffset = startOffset;
  int32_t offset = startOffset;

  RawPage p = fetchPage(offset);
  if (p.fetchError) {   // nothing accumulated; back off
    r.fetchError = true;
    r.retryAfterS = p.retryAfterS;
    return r;
  }
  r.hasCommand = p.hasCommand;
  batch.setContiguous(p.hasCommand);

  for (int i = 0; i < p.count; ++i) {
    ClassifiedUpdate u = handle(i);   // side effect (if any) runs HERE, in order
    if (u.updateId <= 0) continue;
    if (u.action == ClassifiedUpdate::Action::StopNoAck) { r.stopped = true; break; }
    if (u.action != ClassifiedUpdate::Action::AckOnly) {
      if (addClassified(batch, u) == BatchAdd::Deferred) {
        r.stopped = true;   // a cap fired: do NOT ack this update, re-serve it next drain
        break;
      }
      r.messagesBatched++;
    }
    // Batched (accepted/full/boundary) or AckOnly: this update is handled, advance the ack.
    offset = nimbus::core::nextTelegramOffset(offset, u.updateId);
    r.ackOffset = offset;
  }

  r.turnsEmitted = static_cast<int>(batch.turnCount());
  return r;
}

// One poll cycle's drain over the RAM offset cursor (TgOffsetCursor): fetch the page at
// cursor.offset(), run one turn per bucket through emit(i) (i in [0, turnCount())),
// then advance the cursor to the page's ack offset. The next getUpdates carries that
// offset and so confirms this page server-side, whether or not the stored copy could
// be written: the caller persists the cursor (cursor.persist), and a failed write no
// longer replays anything while the device runs.
//
// A page that holds a command is committed bucket by bucket: its buckets are
// contiguous id runs (setContiguous), and after each bucket but the last the cursor
// advances to the next bucket's first id and checkpoint() stores it (the device calls
// cursor.checkpoint). A restart then re-runs at most the bucket in flight, so a command
// whose turn finished (a reminder, an approval, an install) is not run twice. On a fetch error
// nothing runs and the cursor stays. The batcher is cleared on return.
template <class FetchFn, class HandleFn, class EmitFn, class CheckpointFn>
DrainResult drainCycle(MessageBatcher& batch, nimbus::core::TgOffsetCursor& cursor,
                       FetchFn fetchPage, HandleFn handle, EmitFn emit,
                       CheckpointFn checkpoint) {
  DrainResult r = drainOnePage(batch, cursor.offset(), fetchPage, handle);
  if (!r.fetchError) {
    cursor.pageStart();
    const size_t n = batch.turnCount();
    for (size_t i = 0; i < n; ++i) {
      emit(i);
      if (!r.hasCommand || i + 1 == n) continue;
      const int32_t next = batch.firstIdAt(i + 1);
      cursor.advance(next < r.ackOffset ? next : r.ackOffset);
      checkpoint();
    }
    cursor.advance(r.ackOffset);
  }
  batch.clear();
  return r;
}

}  // namespace orch
}  // namespace nimbus
