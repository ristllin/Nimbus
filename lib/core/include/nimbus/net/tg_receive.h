#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "nimbus/net/tg_dup_guard.h"
#include "nimbus/net/tg_settle.h"
#include "nimbus/orch/command.h"
#include "nimbus/orch/msg_batch.h"
#include "nimbus/telegram_offset.h"
#include "nimbus/tg_updates.h"

// tg_receive - one Telegram receive cycle on the poll task, as portable code: fetch a
// page at the offset cursor, settle it (hold for the rest of an album or burst, or for
// the photo sent right after a text, or answer now: tg_settle.h), break duplicates,
// classify every update, process each turn's media in order, run one turn per bucket,
// and only then move the cursor past the page. A bucket sent before its chat's previous
// answer went out (it missed that turn) is told the answer is already in the chat. The
// device (src/agent/telegram.cpp) supplies the I/O; the host tests (test_tg_burst)
// drive the very same code with fakes, so the properties they prove are the device's.
//
// The order inside a dispatched page is load-bearing (rows 35 and 73 of the Lumi fix
// plan): a photo used to be acked in the poll that saw it and described in a later
// cycle, after the text that followed it had already been answered ("I didn't receive
// the third image"). Now every photo, document and voice note of a page is downloaded
// and described inside its turn, the turn runs with every item of its bucket, and the
// cursor moves only after the page's turns returned. A reset anywhere re-serves the
// page (at-least-once); the duplicate guard then skips what was already answered, and
// the poison guard ends a page that keeps crashing the device.
//
// The Io type provides (all called on the poll task, in this order, one TLS session at
// a time; see the device seam for each):
//   Fetch      fetch(int32_t offset, int timeoutS, std::vector<tg::Update>& out)
//   uint32_t   nowMs();  int64_t nowEpoch();  uint32_t bestEpoch();
//   bool       allowed(const std::string& chat);
//   bool       isOwnerChat(const std::string& chat);   the chat the owner's reserve is for
//   orch::ClassifiedUpdate classify(const tg::Update& u);   side effects, slot reserve
//                                               (StopNoAck when no slot is free)
//   bool       processMedia(size_t turn, size_t k, size_t n, int slot, std::string& seg);
//                                               (false: it could not be fetched; its label says why)
//   void       skipMedia(size_t turn, size_t k, size_t n, int slot, std::string& seg);
//                                               the label of an item the poison guard skips
//   bool       processVoice(int slot, std::string& transcript);
//                                               (false: the sender was told why)
//   void       mediaFailed(const std::string& chat, size_t turn);   a turn of failed media only:
//                                               one plain reply instead of a paid turn
//   void       runTurn(const std::string& from, const std::string& chat,
//                      const orch::PsString& text, bool command);
//   void       notice(const Notice& n);         one plain owner notice
//   void       typing(const std::string& chat); Telegram's typing indicator while that
//                                               chat's text is held (best effort)
//   void       checkpoint();                    a command page's per-bucket store
//   void       endPage();                       free this page's media slots
namespace nimbus::net {

// What fetch() reports besides the updates.
struct TgFetch {
  orch::RawPage rp;
  bool          yielded = false;   // the long-poll was given up for a local turn
};

// An owner notice the cycle raises (the device words and sends it).
struct TgNotice {
  enum class Kind : uint8_t { Duplicate, Ceiling, Poison } kind = Kind::Duplicate;
  DupVerdict verdict = DupVerdict::Replay;   // Duplicate
  uint32_t   resumeAt = 0;                   // Ceiling: device epoch when replies resume
  bool       owner = true;                   // Ceiling: the owner's own replies paused
                                             // (false: only the other chats')
  int        items = 0;                      // Poison: updates acked unprocessed
};

struct TgCycle {
  enum class Kind : uint8_t { Idle, Held, Yielded, FetchError, Ran, Poisoned } kind = Kind::Idle;
  int retryAfterS = -1;   // FetchError
  int messages = 0;       // Ran: messages taken into turns
  int turns = 0;          // Ran: turns run
  int duplicates = 0;     // dropped by the duplicate breaker this cycle
  int ceilingSkips = 0;   // turns skipped at the hourly ceiling this cycle
  int poisonSkips = 0;    // buckets skipped by the poison guard this cycle
};

// The duplicate key of an update: its chat, message id, date and content.
inline DupKey dupKeyOf(const tg::Update& u) {
  using Kind = tg::Attachment::Kind;
  DupKey k;
  k.chat = dupHash(u.chatId);
  k.messageId = u.messageId;
  k.date = u.date;
  const Kind a = u.attachment.kind;
  if (a == Kind::Photo || a == Kind::Document || a == Kind::Voice) {
    const char tag = a == Kind::Photo ? 'p' : (a == Kind::Document ? 'd' : 'v');
    k.content = contentKeyMedia(tag, u.attachment.fileUniqueId, u.attachment.fileId, u.text);
  } else {
    k.content = contentKeyText(u.text);
    k.command = a == Kind::None && !u.forwarded && orch::isCommandText(u.text);
  }
  return k;
}

// An update that can become (part of) a turn: a message with text, a photo, a
// document or a voice note. Edits and other kinds are handled, never run.
inline bool turnCandidate(const tg::Update& u) {
  using Kind = tg::Attachment::Kind;
  if (u.edited || u.updateId <= 0) return false;
  const Kind a = u.attachment.kind;
  return a == Kind::Photo || a == Kind::Document || a == Kind::Voice || !u.text.empty();
}

// An update that becomes a command turn: plain text (not a caption), not a forward,
// starting with '/'.
inline bool commandUpdate(const tg::Update& u) {
  return u.attachment.kind == tg::Attachment::Kind::None && !u.forwarded && orch::isCommandText(u.text);
}

// What the settle rule needs from a page: its size and newest Telegram date, and over the
// turn candidates of allowed chats (`allowed(chatId)`) whether one is a slash command
// (commands never wait) and the newest one: its update id (growth: a stranger's message
// or an edit never restarts a hold) and its kind (what may follow it: the text window
// after a plain text, the media window after a photo, file or voice note).
template <class Allowed>
inline SettlePage settlePageOf(const std::vector<tg::Update>& page, const orch::RawPage& rp, Allowed allowed) {
  using Kind = tg::Attachment::Kind;
  SettlePage sp;
  sp.count = rp.count;
  sp.limit = rp.limit;
  for (const tg::Update& u : page) {
    if (u.date > sp.newestDate) sp.newestDate = u.date;
    if (!turnCandidate(u) || !allowed(u.chatId)) continue;
    sp.command = sp.command || commandUpdate(u);
    if (u.updateId <= sp.newestId) continue;
    sp.newestId = u.updateId;
    sp.tail = u.attachment.kind == Kind::None ? SettleTail::Text : SettleTail::Media;
  }
  return sp;
}

// Model-facing (copy rules exempt; no em dash): a voice note of a turn with other
// messages that could not be fetched or transcribed (the sender was already told).
inline constexpr char kVoiceNotHeard[] = "[this voice note could not be heard; the sender was told]";

// The chat of the page's newest plain-text turn candidate from an allowed chat (the held
// text the typing indicator is for); "" when there is none.
template <class Allowed>
inline std::string newestTextChatOf(const std::vector<tg::Update>& page, Allowed allowed) {
  for (size_t k = page.size(); k-- > 0;) {
    const tg::Update& u = page[k];
    if (turnCandidate(u) && u.attachment.kind == tg::Attachment::Kind::None && allowed(u.chatId)) return u.chatId;
  }
  return std::string();
}

class TgReceiver {
 public:
  TgReceiver(core::TgOffsetCursor& cursor, orch::MessageBatcher& batch, TgGuard& guard)
      : cursor_(cursor), batch_(batch), guard_(guard) {}

  // A page is being held: the next poll is an immediate re-poll (timeout 0) after the
  // settle pause (TgSettle::repollInMs, at most kSettleRepollMs), at the same offset.
  bool holding() const { return settle_.holding(); }
  const TgSettle& settle() const { return settle_; }
  // Inside Io::runTurn: the batch bucket that turn renders (its update ids, media
  // segments and voice note), for the history's source and stored-media note.
  size_t currentTurn() const { return current_; }
  // An update of the page being run, by id (nullptr: not in it). Valid inside the Io
  // calls of a dispatched page (processMedia, runTurn): the words a photo's sender wrote
  // just before it are its context when it has no caption.
  const tg::Update* updateOf(int32_t id) const {
    for (const tg::Update& u : page_)
      if (u.updateId == id) return &u;
    return nullptr;
  }

  // One polled cycle at the cursor. `longPollS` is the wait for a fresh poll; a held
  // page is re-polled with timeout 0.
  template <class Io>
  TgCycle cycle(Io& io, int longPollS) {
    TgCycle out;
    page_.clear();
    const TgFetch f = io.fetch(cursor_.offset(), settle_.holding() ? 0 : longPollS, page_);
    if (f.yielded) return kind(out, TgCycle::Kind::Yielded);
    if (f.rp.fetchError) {
      settle_.reset();
      out.retryAfterS = f.rp.retryAfterS;
      return kind(out, TgCycle::Kind::FetchError);
    }
    auto allowed = [&](const std::string& chat) { return io.allowed(chat); };
    const SettlePage sp = settlePageOf(page_, f.rp, allowed);
    const uint32_t now = io.nowMs();
    const Settle s = settle_.observe(cursor_.offset(), sp, now, io.nowEpoch());
    if (typing_.due(s == Settle::Hold && sp.tail == SettleTail::Text, now)) io.typing(newestTextChatOf(page_, allowed));
    if (s == Settle::Hold) return kind(out, TgCycle::Kind::Held);
    if (page_.empty()) return kind(out, TgCycle::Kind::Idle);
    if (pagePoisoned()) return poisoned(io, out);
    polled_ = true;
    orch::RawPage rp = f.rp;
    rp.hasCommand = pageHasCommand(io);
    auto fetch = [&](int32_t) { return rp; };
    auto handle = [&](int i) { return classify(io, page_[(size_t)i], out); };
    auto emit = [&](size_t i) { runBucket(io, i, out); };
    auto checkpoint = [&]() { io.checkpoint(); };
    const orch::DrainResult r = orch::drainCycle(batch_, cursor_, fetch, handle, emit, checkpoint);
    finishPage(io);
    out.messages = r.messagesBatched;
    return kind(out, TgCycle::Kind::Ran);
  }

  // A page that does not come from getUpdates (the test build's injected page): no
  // settle, no cursor, no poison mark (its ids are not Telegram's). Runs from update
  // `first`; returns the index of the update that was not taken (it runs next cycle),
  // or page.size() when the page is done.
  template <class Io>
  size_t injected(Io& io, const std::vector<tg::Update>& page, size_t first, TgCycle& out) {
    polled_ = false;   // not Telegram's ids: no poison marks
    page_.assign(page.begin() + (std::ptrdiff_t)first, page.end());
    size_t last = 0;
    orch::RawPage rp;
    rp.count = (int)page_.size();
    rp.limit = rp.count + 1;   // one page, nothing more to fetch
    rp.hasCommand = pageHasCommand(io);
    auto fetch = [&](int32_t) { return rp; };
    auto handle = [&](int i) {
      last = (size_t)i;
      return classify(io, page_[(size_t)i], out);
    };
    const orch::DrainResult r = orch::drainOnePage(batch_, 0, fetch, handle);
    for (size_t i = 0; i < batch_.turnCount(); ++i) runBucket(io, i, out);
    batch_.clear();
    finishPage(io);
    out.messages = r.messagesBatched;
    out.kind = TgCycle::Kind::Ran;
    return r.stopped ? first + last : page.size();
  }

 private:
  static TgCycle kind(TgCycle& out, TgCycle::Kind k) {
    out.kind = k;
    return out;
  }

  // A page holds a command when some update can become a command turn: plain text
  // (not a caption), an allowed chat, not a forward, starting with '/'.
  template <class Io>
  bool pageHasCommand(Io& io) const {
    for (const tg::Update& u : page_)
      if (commandUpdate(u) && io.allowed(u.chatId)) return true;
    return false;
  }

  // The poison guard fired: ack the whole page unprocessed, tell the owner once.
  template <class Io>
  TgCycle poisoned(Io& io, TgCycle& out) {
    int32_t next = cursor_.offset();
    for (const tg::Update& u : page_) next = core::nextTelegramOffset(next, u.updateId);
    cursor_.pageStart();
    cursor_.advance(next);
    TgNotice n;
    n.kind = TgNotice::Kind::Poison;
    n.items = (int)page_.size();
    io.notice(n);
    io.endPage();
    return kind(out, TgCycle::Kind::Poisoned);
  }

  // Classify one update: an allowed turn candidate that duplicates an answered or
  // already-taken message is dropped here (one notice per episode); anything else goes
  // to the device, and a message it took is remembered as pending.
  template <class Io>
  orch::ClassifiedUpdate classify(Io& io, const tg::Update& u, TgCycle& out) {
    if (!turnCandidate(u) || !io.allowed(u.chatId)) return io.classify(u);
    const DupKey key = dupKeyOf(u);
    const DupVerdict v = guard_.check(key);
    if (v == DupVerdict::New) {
      orch::ClassifiedUpdate cu = io.classify(u);
      if (cu.action != orch::ClassifiedUpdate::Action::AckOnly &&
          cu.action != orch::ClassifiedUpdate::Action::StopNoAck) {
        guard_.notePending(key);
        keys_.emplace_back(u.updateId, key);
      }
      return cu;
    }
    ++out.duplicates;
    if (guard_.noteDuplicate(key)) {
      TgNotice n;
      n.kind = TgNotice::Kind::Duplicate;
      n.verdict = v;
      io.notice(n);
    }
    orch::ClassifiedUpdate cu;
    cu.updateId = u.updateId;
    cu.action = orch::ClassifiedUpdate::Action::AckOnly;
    return cu;
  }

  const DupKey* keyFor(int32_t updateId) const {
    for (const auto& kv : keys_)
      if (kv.first == updateId) return &kv.second;
    return nullptr;
  }

  // Every message of bucket i was answered (or handled without a turn).
  void rememberBucket(size_t i, uint32_t nowS) {
    for (size_t m = 0; m < batch_.messageCountAt(i); ++m)
      if (const DupKey* k = keyFor(batch_.updateIdAt(i, m))) guard_.noteTurned(*k, nowS);
  }

  // The hourly ceiling: false (the turn is skipped and acked, one notice per trip)
  // when the bucket would be one inbound head turn too many for its chat's limit (the
  // owner's chat may use the whole ceiling, the others leave the owner's reserve).
  // Commands always run.
  template <class Io>
  bool ceilingAllows(Io& io, size_t i, TgCycle& out) {
    if (batch_.commandAt(i)) return true;
    const uint32_t now = io.bestEpoch();
    const bool owner = io.isOwnerChat(batch_.chatIdAt(i));
    if (guard_.ceilingAllows(now, owner)) return true;
    ++out.ceilingSkips;
    if (guard_.noteCeilingSkip(now, owner)) {
      TgNotice n;
      n.kind = TgNotice::Kind::Ceiling;
      n.resumeAt = guard_.ceilingResumeAt(now, owner);
      n.owner = owner;
      io.notice(n);
    }
    return false;
  }

  // ---- the poison guard's marks (see tg_dup_guard.h) ----
  // The page is about to be classified: true when its classification already crashed
  // the device kPoisonMaxTries times. A mark left inside this page (an item or a bucket
  // of it crashed) is kept for that work; otherwise the page is marked.
  bool pagePoisoned() {
    const int32_t first = page_.front().updateId;
    if (guard_.poisoned(first, kPhasePage)) return true;
    const int32_t id = guard_.markedId();
    const bool inside = id >= first && id <= page_.back().updateId;
    if (!(inside && guard_.markedPhase() != kPhasePage)) guard_.mark(first, kPhasePage);
    return false;
  }

  // The bucket carries words of its own: a text message, a caption or a transcript.
  bool hasWords(size_t i) const {
    for (size_t m = 0; m < batch_.messageCountAt(i); ++m)
      if (batch_.kindAt(i, m) != orch::SegKind::Media || batch_.mediaCaptionAt(i, mediaIndexOf(i, m))[0])
        return true;
    return false;
  }
  size_t mediaIndexOf(size_t i, size_t m) const {
    size_t k = 0;
    for (size_t j = 0; j < m; ++j) k += batch_.kindAt(i, j) == orch::SegKind::Media;
    return k;
  }

  bool bucketHolds(size_t i, int32_t id) const {
    for (size_t m = 0; m < batch_.messageCountAt(i); ++m)
      if (batch_.updateIdAt(i, m) == id) return true;
    return false;
  }

  // A bucket whose turn already crashed the device kPoisonMaxTries times: skipped
  // without its turn, its messages remembered as handled, the owner told once. A mark
  // on one of its items is kept for that item.
  template <class Io>
  bool poisonedBucket(Io& io, size_t i) {
    if (!polled_) return false;
    const int32_t first = batch_.firstIdAt(i);
    if (!guard_.poisoned(first, kPhaseBucket)) {
      if (!(guard_.markedPhase() == kPhaseItem && bucketHolds(i, guard_.markedId())))
        guard_.mark(first, kPhaseBucket);
      return false;
    }
    rememberBucket(i, io.bestEpoch());
    poisonNotice(io, (int)batch_.messageCountAt(i));
    return true;
  }

  // Media item k of bucket i: false (and the item is marked) when it may be processed;
  // true when it already crashed the device kPoisonMaxTries times (it is skipped). A
  // mark is never moved back onto earlier work of a bucket served again: while the
  // bucket's own turn holds a counted crash, its items keep that mark; items before a
  // crashed item leave the item's mark alone (ids rise within a bucket).
  template <class Io>
  bool itemPoisoned(Io& io, size_t i, size_t k) {
    if (!polled_) return false;
    const int32_t id = batch_.mediaUpdateIdAt(i, k);
    const int32_t marked = guard_.markedId();
    const uint8_t phase = guard_.markedPhase();
    if (phase == kPhaseBucket && marked == batch_.firstIdAt(i) && guard_.tries() > 0) return false;
    if (phase == kPhaseItem && bucketHolds(i, marked) && id < marked) return false;
    if (guard_.poisoned(id, kPhaseItem)) {
      poisonNotice(io, 1);
      return true;
    }
    guard_.mark(id, kPhaseItem);
    return false;
  }

  template <class Io>
  void poisonNotice(Io& io, int items) {
    TgNotice n;
    n.kind = TgNotice::Kind::Poison;
    n.items = items;
    io.notice(n);
  }

  // One bucket: its media in arrival order (or its voice note), then its one turn.
  template <class Io>
  void runBucket(Io& io, size_t i, TgCycle& out) {
    if (poisonedBucket(io, i)) {
      ++out.poisonSkips;
      return;
    }
    if (!ceilingAllows(io, i, out)) return;
    const std::string chat = batch_.chatIdAt(i), from = batch_.turnFromAt(i);
    if (batch_.voiceAt(i)) {
      std::string transcript;
      if (!io.processVoice(batch_.voiceSlotAt(i), transcript)) {
        if (batch_.messageCountAt(i) == 1) {   // the voice note alone: the sender was told why
          rememberBucket(i, io.bestEpoch());
          return;
        }
        transcript = kVoiceNotHeard;   // the rest of the turn is still answered
      }
      batch_.setVoiceText(i, transcript);
    }
    const size_t n = batch_.mediaCountAt(i);
    bool fetched = false;
    for (size_t k = 0; k < n; ++k) {
      std::string seg;
      if (itemPoisoned(io, i, k)) io.skipMedia(i, k, n, batch_.mediaSlotAt(i, k), seg);
      else fetched = io.processMedia(i, k, n, batch_.mediaSlotAt(i, k), seg) || fetched;
      batch_.setMediaText(i, k, seg);
    }
    // Nothing to answer but failures (no text, no caption, no item fetched): one plain
    // reply says why, instead of a paid turn that could only say the same.
    if (n && !fetched && !hasWords(i)) {
      io.mediaFailed(chat, i);
      rememberBucket(i, io.bestEpoch());
      return;
    }
    if (polled_) guard_.mark(batch_.firstIdAt(i), kPhaseBucket);   // the turn is the risk now
    runTurnOf(io, i, chat, from);
    ++out.turns;
  }

  // Bucket i's turn. A bucket sent before its chat's previous answer went out (it
  // missed that turn: a photo seconds too late for its text) carries kCrossedNote, so
  // the turn knows that answer is already in the chat. Commands render byte for byte.
  template <class Io>
  void runTurnOf(Io& io, size_t i, const std::string& chat, const std::string& from) {
    const bool command = batch_.commandAt(i);
    if (!command) guard_.noteTurn(io.bestEpoch());
    current_ = i;   // currentTurn(): the bucket the turn below routes (histV2's source)
    orch::PsString text = batch_.render(i);
    if (!command && crossedAnswer(earliestSentAt(i), answeredAt(chat))) text += kCrossedNote;
    io.runTurn(from, chat, text, command);
    const uint32_t now = io.bestEpoch();
    rememberBucket(i, now);
    noteAnswered(chat, now);
  }

  // The earliest Telegram date among bucket i's messages (0: none known).
  int64_t earliestSentAt(size_t i) const {
    int64_t at = 0;
    for (size_t m = 0; m < batch_.messageCountAt(i); ++m) {
      const tg::Update* u = updateOf(batch_.updateIdAt(i, m));
      if (u && u->date > 0 && (at == 0 || u->date < at)) at = u->date;
    }
    return at;
  }

  // When `chat`'s last turn returned (device epoch; 0: none this boot).
  uint32_t answeredAt(const std::string& chat) const {
    const uint32_t h = dupHash(chat);
    for (const Answered& a : answered_)
      if (a.at && a.chat == h) return a.at;
    return 0;
  }
  void noteAnswered(const std::string& chat, uint32_t at) {
    const uint32_t h = dupHash(chat);
    Answered* slot = &answered_[0];
    for (Answered& a : answered_) {
      if (a.at && a.chat == h) {
        slot = &a;
        break;
      }
      if (a.at < slot->at) slot = &a;   // else the oldest (or an empty) entry
    }
    slot->chat = h;
    slot->at = at;
  }

  template <class Io>
  void finishPage(Io& io) {
    guard_.commitPage();
    guard_.clearPending();
    keys_.clear();
    io.endPage();
  }

  core::TgOffsetCursor& cursor_;
  orch::MessageBatcher& batch_;
  TgGuard& guard_;
  TgSettle settle_;
  HoldTyping typing_;
  std::vector<tg::Update> page_;
  std::vector<std::pair<int32_t, DupKey>> keys_;
  bool polled_ = true;   // the page came from getUpdates (poison marks apply)
  size_t current_ = 0;   // the bucket of the turn being routed (valid inside runTurn)
  // When the last few chats' turns returned, for the crossed-answer note (chat by its
  // dupHash; the oldest entry is replaced). RAM only: a restart forgets it.
  struct Answered {
    uint32_t chat = 0;
    uint32_t at = 0;
  };
  static constexpr size_t kAnsweredChats = 4;
  Answered answered_[kAnsweredChats];
};

}  // namespace nimbus::net
