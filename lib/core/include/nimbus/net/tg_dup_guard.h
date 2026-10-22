#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// tg_dup_guard - the inbound storm breakers for Telegram turns.
//
// On 19 Oct 2026 one short owner message was served again on every poll and ran 735
// paid turns (728 on the most expensive model) before anything stopped it: no guard
// knew it had already been answered, and nothing bounded inbound turns per hour
// (routines have kLoopMaxRepeats; inbound turns had nothing). The offset fix keeps the
// page from being re-served while the device runs, but a re-served page, a re-sent
// message or a future regression must still cost at most one paid turn. Three
// breakers, one record:
//
// 1. Duplicate breaker, per chat. A message is a duplicate when
//    - Replay: the same message again (same chat, message_id and date), however
//      late; or
//    - Repeat: the same content in the same chat (the text; for a photo, file or
//      voice note its Telegram file_unique_id plus the caption) sent within
//      kRepeatWindowS of the answered one AND before that answer went out. A repeat
//      sent after the answer is a genuine message ("yes" to a second question) and
//      runs; so does any repeat after the window. With the device clock unset, the
//      window alone decides. Commands are never a Repeat (they cost no model turn
//      and repeating one is deliberate); a re-served command is still a Replay.
//    A duplicate is acked and dropped, counted (dupDrops), and the first duplicate of
//    each answered message raises one plain owner notice (one per episode).
//    A message is recorded only after its turn returned (noteTurned), so a crash
//    before that leaves the message free to run again: at-least-once, never lost.
// 2. Hourly ceiling, device-wide: at most kInboundTurnsPerHour inbound head turns in
//    any rolling hour. The owner's chat keeps kOwnerReserveTurns of them: the other
//    allowed chats (a family member, a group) stop at the ceiling minus the reserve, so
//    a busy shared chat can never use up the owner's own replies. A turn past its limit
//    is acked and not run, and the first skip of each trip (one for the owner's limit,
//    one for the others') raises one owner notice naming when replies resume. Commands
//    do not count and always run (the owner's controls must keep working in a storm).
// 3. Poison guard: media are acked only after processing, so a page whose processing
//    crashes the device would be served again on every boot. The work in flight is
//    marked by an update id and a phase: the page while it is classified, each media
//    item while it is fetched and described, each bucket (one turn) while its turn runs.
//    A boot after a panic, watchdog or brownout reset counts a try against the mark;
//    once kPoisonMaxTries crash resets hit the same work it is skipped and the owner is
//    told once to send it again: a page is acked unprocessed, a media item becomes "could
//    not be processed" in a turn that still runs with everything else, a bucket is acked
//    without its turn. A page served again after a crash keeps a mark that lies inside
//    it (the receive cycle checks), whether or not an earlier bucket's offset was
//    stored, so the count survives a command page's per-bucket checkpoints. A commit
//    clears the mark. Other resets (a restart, an update, USB) count nothing.
//
// The record is a plain struct the device keeps in RTC no-init memory (TgGuardStore):
// it survives a restart, a panic, the watchdogs, a brownout, a deep-sleep wake and a
// USB reset, but not a power cycle, and costs no settings (NVS) write. A magic value,
// the firmware build id and a CRC-32 reject the garbage a power-on leaves and a layout
// from another build; a power-on clears it. It is double-buffered: every write fills
// the other copy and seals it (sequence number, then CRC) last, so a restart in the
// middle of a write leaves the previous copy intact and the next boot reads the state
// from before or after that write, never an emptied record. Portable and host-tested
// (test_tg_dup_guard, test_tg_burst).
namespace nimbus::net {

constexpr uint32_t kRepeatWindowS = 120;
constexpr uint32_t kInboundTurnsPerHour = 60;
constexpr uint32_t kOwnerReserveTurns = 12;
constexpr uint8_t  kPoisonMaxTries = 2;
constexpr size_t   kDupRing = 24;          // answered messages remembered (over two pages)
constexpr uint32_t kTgGuardMagic = 0x54474405u;   // "TGD" + layout version

// One message, as the breakers see it (the device fills it at classification).
struct DupKey {
  uint32_t chat = 0;       // dupHash of the chat id text
  int64_t  messageId = 0;  // Telegram message_id (0: unknown, the Replay rule is skipped)
  int64_t  date = 0;       // Telegram date, seconds
  uint32_t content = 0;    // dupHash of the content (see contentKey*)
  bool     command = false;
};

// FNV-1a 32 over a byte string (stable across builds). A collision can only make
// two different messages look alike inside the repeat window or for one replayed id.
uint32_t dupHash(const std::string& s);
uint32_t dupHash(const char* s, size_t n);
// Content keys: text messages hash their text; media hash a kind letter, the
// file_unique_id (the file_id when absent) and the caption.
uint32_t contentKeyText(const std::string& text);
uint32_t contentKeyMedia(char kind, const std::string& fileUniqueId, const std::string& fileId,
                         const std::string& caption);

enum class DupVerdict : uint8_t { New, Replay, Repeat };
const char* dupVerdictName(DupVerdict v);
// The rule for one pair: is `k` a duplicate of `seen` (answered at `doneAt`, 0 when
// unknown or not answered yet)?
DupVerdict compareKeys(const DupKey& seen, uint32_t doneAt, const DupKey& k);

// How the previous run ended, for the poison count.
enum class BootKind : uint8_t {
  PowerOn,   // the record is garbage: clear it
  Crash,     // panic, a watchdog or a brownout: counts a try against the page in flight
  Other,     // a restart, an update, USB, deep sleep: keeps the record, counts nothing
};
// The BootKind of an esp_reset_reason_t value (nimbus/sys/reset_reason.h numbering).
BootKind bootKindOf(int resetReason);

// Telegram message ids and dates fit 32 bits; the ring keeps them that way.
struct TgGuardEntry {
  uint32_t chat;
  uint32_t content;
  uint32_t messageId;
  uint32_t date;
  uint32_t doneAt;   // device epoch when its turn returned (0: clock unset)
  uint32_t flags;    // kEntryNoticed: its duplicate notice went out
};
constexpr uint32_t kEntryNoticed = 1u;
constexpr uint8_t kPhasePage = 1;     // the page is being classified (id: its first update)
constexpr uint8_t kPhaseItem = 2;     // a media item is being fetched or described (its update)
constexpr uint8_t kPhaseBucket = 3;   // a bucket's turn is running (its first update)

// One copy of the record.
struct TgGuardState {
  uint32_t magic;
  uint32_t build;
  uint32_t seq;               // write sequence: the valid copy with the newer one is current
  int32_t  inflightFirstId;   // first update id of the work in flight (0: none)
  uint8_t  tries;             // crash resets counted against it
  uint8_t  head;              // next ring slot
  uint8_t  count;             // ring fill
  uint8_t  turnHead;          // next turns[] slot
  uint8_t  inflightPhase;     // kPhasePage, kPhaseItem or kPhaseBucket (0: none)
  uint8_t  pad[3];
  uint32_t dupDrops;          // duplicates dropped since the record was cleared
  uint32_t ceilingDrops;      // turns skipped at the ceiling
  uint32_t ceilingNoticeUntil;   // a notice went out for the owner's trip that ends here
  uint32_t othersNoticeUntil;    // ... and for the other chats' trip that ends here
  uint32_t poisonAcks;        // pages acked unprocessed by the poison guard
  TgGuardEntry ring[kDupRing];
  uint32_t turns[kInboundTurnsPerHour];   // start epochs of recent inbound head turns
  uint32_t crc;
};

// What the device keeps in RTC memory: two copies, one current.
struct TgGuardStore {
  TgGuardState slot[2];
};

class TgGuard {
 public:
  explicit TgGuard(TgGuardStore& st) : st_(st) {}

  // Once at boot, before the first poll. Clears the record on a power-on, on a bad
  // magic or CRC and on another build's record; counts a crash reset against a page
  // in flight. Returns true when the record was kept (it survived the reset).
  bool boot(BootKind kind, uint32_t build);

  // ---- duplicate breaker
  // The verdict for a message about to be classified, against the answered messages
  // remembered and the messages already taken from the page being drained.
  DupVerdict check(const DupKey& k) const;
  // A duplicate was dropped: counts it. True when the owner notice for that episode
  // is due (the first duplicate of that message).
  bool noteDuplicate(const DupKey& k);
  // A new message was taken into this drain's turns (not answered yet). RAM only.
  void notePending(const DupKey& k);
  // The message's turn returned (or it was handled without one): remember it, with
  // the device epoch `nowS` (0 when the clock is unset) as the answer's time.
  void noteTurned(const DupKey& k, uint32_t nowS);
  // The drain ended: forget what was taken but not answered (a message whose turn
  // never ran must be free to run when it is served again).
  void clearPending() { pending_.clear(); }
  uint32_t dupDrops() const { return cur().dupDrops; }

  // ---- hourly ceiling (`nowS`: the device's best epoch; see the device seam).
  // `owner`: the turn is the owner's chat (its limit is the whole ceiling; the other
  // chats' limit leaves the owner's reserve).
  bool ceilingAllows(uint32_t nowS, bool owner) const;
  void noteTurn(uint32_t nowS);
  // A turn was skipped at its limit: counts it. True when that trip's notice is due.
  bool noteCeilingSkip(uint32_t nowS, bool owner);
  // When such a turn may run again: when enough counted turns have aged out of the hour
  // (0 when it may run now).
  uint32_t ceilingResumeAt(uint32_t nowS, bool owner) const;
  uint32_t ceilingDrops() const { return cur().ceilingDrops; }

  // ---- poison guard
  // True when the work (`id`, `phase`) already cost kPoisonMaxTries crash resets: skip
  // it (the skip is counted and the mark cleared).
  bool poisoned(int32_t id, uint8_t phase);
  // The work (`id`, `phase`) is about to start: mark it in flight (its tries are kept
  // when it is the work already marked, reset otherwise).
  void mark(int32_t id, uint8_t phase);
  int32_t markedId() const { return cur().inflightFirstId; }
  uint8_t markedPhase() const { return cur().inflightPhase; }
  // The page in flight was committed: clear the mark.
  void commitPage();
  uint8_t tries() const { return cur().tries; }
  uint32_t poisonAcks() const { return cur().poisonAcks; }

  // The current copy is intact (magic, build, CRC). For tests and the boot log.
  bool valid(uint32_t build) const;
  // A snapshot of the counters, for the device to publish.
  struct Counts {
    uint32_t dupDrops, ceilingDrops, poisonAcks;
    uint8_t  tries;
  };
  Counts counts() const { return {cur().dupDrops, cur().ceilingDrops, cur().poisonAcks, cur().tries}; }

 private:
  struct Pending {
    DupKey key;
    bool   noticed;
  };
  void clear(uint32_t build);
  const TgGuardState& cur() const { return st_.slot[active_]; }
  // A write: the other copy, filled from the current one, to change and then seal.
  TgGuardState& edit();
  // Seal the copy edit() returned (sequence number, then CRC, last) and make it current.
  void seal(TgGuardState& w);
  // The remembered (ring) or pending message `k` duplicates, and how (-1: none).
  int answeredIndex(const DupKey& k, DupVerdict& v) const;
  int pendingIndex(const DupKey& k, DupVerdict& v) const;
  TgGuardStore& st_;
  uint8_t active_ = 0;
  std::vector<Pending> pending_;
};

// The copy of `st` that is intact for `build` and newest (-1: neither is).
int currentSlot(const TgGuardStore& st, uint32_t build);

}  // namespace nimbus::net
