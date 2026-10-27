#pragma once
// tg_archive - the Telegram archive on the SD card.
//
// Owner ask: everything exchanged with the device over Telegram is kept on the
// device, at full fidelity. Every inbound update is written as it arrived (the raw
// update JSON) before classification, batching or replay handling; every media
// file is fetched at the largest size Telegram offers, streamed to the card in
// slices; every outbound message is written with the same shape, before any
// splitting. Nothing here is read by the model path: the turn keeps its own
// bounded copy (kPhotoBudget), this is the record.
//
// Layout (all under kRoot, on the SD card only):
//   /mem/tg/<chat>/<yyyy-mm-dd>/<update_id>.json     raw inbound update
//   /mem/tg/<chat>/<yyyy-mm-dd>/<update_id>.<ext>    its media file
//   /mem/tg/<chat>/<yyyy-mm-dd>/<update_id>.turn.<ext>  the assistant's own copy
//                                                    (a smaller rendition), kept
//                                                    until the full size lands
//   /mem/tg/<chat>/<yyyy-mm-dd>/o<seq>.json          outbound message
//   /mem/tg/<chat>/<yyyy-mm-dd>/o<seq>.<ext>         outbound media file
//   /mem/tg/<chat>/<yyyy-mm-dd>/index.jsonl          one line per item, plus
//                                                    media status events
// The day of an inbound item is Telegram's own message date (UTC), so names never
// depend on uptime or on the device clock, and a replay of the same update maps to
// the same path: dedupe is a file-exists check, with no RAM state to lose. The
// outbound sequence is derived from the names already in the day directory, so a
// reboot or a clock jump can never reuse a name.
//
// Portable and Arduino-free: the engine talks to a small file-system seam (Fs) and
// a download seam (Source), so host tests drive it with an in-memory card that can
// fail a write at any byte, and the device backs both with the SD card and the
// Telegram file API. The episodic retention pass and the blob GC walk only
// /mem/episodic, /mem/blobs, /mem/battery and /mem/trace, so they never reach the
// archive; isArchivePath() is the predicate for any pass that walks wider. The
// archive is removed only by the storage-pressure policy below or by the factory
// sweep (it lives inside the /mem tree that the sweep deletes).

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <ArduinoJson.h>

#include "nimbus/tg_updates.h"

namespace nimbus {
namespace tgarc {

constexpr const char* kRoot = "/mem/tg";
// Telegram's Bot API serves files up to 20 MB through getFile; anything larger is
// recorded with an explicit "not archived: too large" row instead of a failed fetch.
constexpr uint64_t kMaxFileBytes = 20ull * 1024 * 1024;
// Pending downloads held in RAM. Overflow is not a loss: the index row already says
// "pending", and the resume pass re-reads the newest 14 days once the queue drains.
constexpr size_t kMaxJobs = 64;
// Attempts per download before it ends "failed" (a network error) or stays
// "incomplete" (a card write or rename error). Every attempt resumes from the bytes
// already on the card; an incomplete attempt writes its own status line, and the
// count of those lines bounds the retries across restarts too.
constexpr uint8_t kMaxTries = 4;
// Day directories (newest first, per chat) the resume pass re-reads after a restart:
// an item there that is still waiting for its file is fetched again.
constexpr int kResumeDays = 14;
// Chats that are not on the allowlist (strangers who found the bot) are archived
// as text only, within fixed budgets, and their media is never fetched, so nobody
// outside the allowlist can spend the card or the radio, or push the owner's own
// history out through the storage-pressure policy:
//   - at most kStrangerDailyCap updates per stranger chat per day;
//   - at most kStrangerDayUpdates updates and kStrangerDayBytes bytes per day across
//     all stranger chats together;
//   - at most kStrangerChatsMax stranger chat directories on the card; a further
//     stranger's updates become "not archived: stranger limit" rows in the "0"
//     directory (still charged to the budgets above);
//   - at most kStrangerTableMax stranger chats counted per day; past that a new
//     stranger is only counted in Stats::strangerSkipped.
// A budget that is spent denies; nothing is ever evicted to make room.
constexpr uint32_t kStrangerDailyCap = 50;
constexpr uint32_t kStrangerDayUpdates = 200;
constexpr uint64_t kStrangerDayBytes = 2ull * 1024 * 1024;
constexpr size_t kStrangerChatsMax = 32;
constexpr size_t kStrangerTableMax = 2 * kStrangerChatsMax;
// The chat directory the restore tool uses for backed-up files no row names.
constexpr const char* kRestoredChat = "restored";
// TGINJECT (test builds): a file id "LOCAL_/mem/..." names a file already on the card,
// read instead of fetched from Telegram. Real Telegram file ids never take this form.
constexpr const char* kLocalPrefix = "LOCAL_";
// The card path of a "LOCAL_" file id (under /mem/, plain characters, no ".."), or
// false when `fileId` is not one.
bool localInjectPath(const std::string& fileId, std::string& path);

// True for the archive root and everything under it.
bool isArchivePath(const std::string& path);

// The allocator for every JSON document the archive builds or parses (default: the
// heap). The device passes its PSRAM allocator once at start.
void setJsonAllocator(ArduinoJson::Allocator* alloc);
ArduinoJson::Allocator* jsonAllocator();

// ---- names ---------------------------------------------------------------------
bool validChatDir(const std::string& s);    // "-?[0-9]{1,20}" or kRestoredChat
bool isTelegramChat(const std::string& s);  // a real chat id (not kRestoredChat)
bool validDay(const std::string& s);        // "YYYY-MM-DD" or "undated"
bool validItemName(const std::string& s);   // [A-Za-z0-9._-]{1,80}, no leading '.', no ".."
// "YYYY-MM-DD" (UTC) for an epoch, or "undated" when the epoch is not plausible
// (before 2015 or after 2200).
std::string dayFromEpoch(int64_t epochSec);
std::string chatDirPath(const std::string& chat);
std::string dayDirPath(const std::string& chat, const std::string& day);
std::string itemPath(const std::string& chat, const std::string& day, const std::string& name);
std::string indexPathOf(const std::string& chat, const std::string& day);
std::string inBase(int64_t updateId);   // "123"
std::string outBase(uint32_t seq);      // "o7"
// One past the largest "o<seq>." name in `names` (1 when there is none).
uint32_t nextOutSeq(const std::vector<std::string>& names);
// The name of the assistant's own copy of an inbound file: "<base>.turn.<ext>".
std::string turnCopyName(const std::string& base, const std::string& ext);

// ---- content types and the archive policy ----------------------------------------
// Every message content type the archive recognizes. Download types are fetched in
// full; RawOnly types are complete in the raw JSON; NotArchived types have content
// a bot cannot fetch, so their row says "not archived: <type>" (the raw JSON is
// still archived).
enum class Content : uint8_t {
  Text, Photo, Document, Voice, Audio, Video, VideoNote, Sticker, Animation,
  Location, Contact, Venue, Poll, Dice, Service,
  Story, PaidMedia, Game, Invoice,
  Unknown,
  Count   // sentinel: the class test walks every value below it
};
enum class Policy : uint8_t { Unknown = 0, Download, RawOnly, NotArchived };
struct ContentInfo {
  const char* name;     // the message key ("photo", "video_note", ...)
  Policy      policy;
  const char* defExt;   // file extension for Download types ("" otherwise)
};
// Total over Content (a static_assert in the table keeps it so); Count and anything
// past it read Policy::Unknown.
ContentInfo contentInfo(Content c);
// A tripwire for the parser: its attachment kinds mapped onto the archive's types,
// so a kind added to tg_updates without an archive decision fails the build and the
// class test. (Classification itself reads the raw JSON keys in tg_archive.cpp, so a
// new Telegram content key also needs a row there.)
Content contentForKind(tg::Attachment::Kind k);

// ---- raw page ------------------------------------------------------------------
struct Span { size_t off = 0; size_t len = 0; };
// Byte ranges of each element of a getUpdates body's top-level "result" array, in
// wire order. Returns false (keeping the whole elements found so far) when the body
// is not a JSON object with a well-formed result array.
bool splitResult(const char* body, size_t len, std::vector<Span>& out);

// ---- per-update metadata ----------------------------------------------------------
struct Media {
  Content     type = Content::Unknown;
  std::string fileId;
  std::string fileName;
  std::string mime;
  std::string ext;
  uint64_t    size = 0;     // Telegram's file_size (0 = not reported)
};
struct InMeta {
  int64_t     updateId = 0;
  std::string updateType;   // "message", "edited_message", ... ("" if none)
  std::string chat;         // chat.id as decimal text ("" if the update has no chat)
  std::string from;         // first_name, else username
  std::string fromId;
  int64_t     messageId = 0;
  int64_t     date = 0;     // message.date (edit_date for edits), Telegram server time
  std::string mediaGroupId;
  bool        edited = false;
  Content     content = Content::Unknown;
  bool        hasMedia = false;
  Media       media;        // valid when hasMedia
};
// Parse one raw update. Photos pick the LARGEST PhotoSize (by pixels, then bytes),
// never the model path's budget-bound rendition. Returns false when the span is not
// a JSON object with a positive update_id.
bool parseUpdateMeta(const char* raw, size_t len, InMeta& out);
// File extension for a media item: from the document's own name when it has a safe
// one, else from the MIME type, else the type's default.
std::string mediaExt(const Media& m);

// ---- storage pressure ---------------------------------------------------------
struct DayUsage {
  std::string chat;
  std::string day;
  uint64_t    bytes = 0;
  bool        stranger = false;   // the chat is not on the allowlist
};
struct PressurePlan {
  bool                triggered = false;
  std::vector<size_t> remove;      // indexes into the input, in removal order
  uint64_t            freed = 0;
};
// Free space below lowWater starts a pass. It removes whole day directories until
// free space reaches targetFree: every stranger day first (oldest first), and only
// then the allowlisted chats' days, oldest first ("undated" counts as oldest) and
// never any allowlisted chat's own newest day. So an allowlisted chat's history is
// never deleted to make room for stranger data, and each chat keeps its newest items.
uint64_t lowWater(uint64_t totalBytes);
uint64_t targetFree(uint64_t totalBytes);
PressurePlan planPressure(const std::vector<DayUsage>& days, uint64_t freeBytes,
                          uint64_t totalBytes);

// ---- file-system seam -------------------------------------------------------------
class Writer {
 public:
  virtual ~Writer() = default;
  virtual bool write(const uint8_t* data, size_t len) = 0;   // all or false
  virtual bool close() = 0;
};
class Reader {
 public:
  virtual ~Reader() = default;
  virtual int read(uint8_t* data, size_t cap) = 0;   // >0 bytes, 0 end, <0 error
};
struct Entry {
  std::string name;   // basename
  bool        dir = false;
  uint64_t    size = 0;
};
class Fs {
 public:
  virtual ~Fs() = default;
  virtual bool exists(const std::string& path) = 0;
  virtual bool mkdirs(const std::string& dir) = 0;
  virtual std::unique_ptr<Writer> openWrite(const std::string& path, bool append) = 0;
  virtual std::unique_ptr<Reader> openRead(const std::string& path) = 0;
  virtual bool rename(const std::string& from, const std::string& to) = 0;
  virtual bool remove(const std::string& path) = 0;
  virtual bool removeTree(const std::string& dir) = 0;
  virtual int64_t size(const std::string& path) = 0;   // -1 when absent
  virtual std::vector<Entry> list(const std::string& dir) = 0;
};

// ---- download seam ------------------------------------------------------------------
struct OpenResult {
  bool        ok = false;
  bool        resumed = false;   // the server honored the resume offset
  bool        permanent = false; // retrying cannot help (unknown file, too big)
  int64_t     total = -1;        // bytes this transfer will carry, when known
  std::string err;
};
class Source {
 public:
  virtual ~Source() = default;
  // Start fetching `fileId`, asking to resume at `resumeAt` bytes (0 = from the start).
  virtual OpenResult open(const std::string& fileId, uint64_t resumeAt) = 0;
  virtual int read(uint8_t* buf, size_t cap) = 0;   // >0 bytes, 0 end, <0 error
  virtual void close() = 0;
};

// ---- the engine -------------------------------------------------------------------
struct Now {
  int64_t  epoch = 0;   // device clock (0 or implausible = not set)
  uint32_t ms = 0;      // uptime, only for the server-time fallback below
};
struct Stats {
  uint32_t inItems = 0, outItems = 0, dupes = 0, repaired = 0;
  uint32_t mediaOk = 0, mediaIncomplete = 0, mediaFailed = 0, notArchived = 0;
  uint32_t turnCopies = 0;   // the turn's smaller copy kept while the full size is due
  uint32_t writeErrors = 0, pressureDays = 0, strangerSkipped = 0;
};
struct Job {
  std::string chat, day, base, ext, fileId;
  uint8_t     tries = 0;
  bool        noResume = false;   // the server ignored a resume once: fetch whole
};
enum class Step : uint8_t { Idle, Done, Partial, Retry, Incomplete, Failed };
// A file the restore tool placed into the archive (a backup of the earlier media
// store, which had no update ids): it is recorded with dir "restored".
struct Restored {
  std::string chat, day, file;
  std::string kind;   // "photo", "voice", "tool_output", ... from the backed-up row
  std::string role;   // "user" | "assistant" | "system" ("" if unknown)
  std::string note;   // where it came from, e.g. "backup 2026-10-18"
  uint64_t    bytes = 0;
  int64_t     date = 0;
};
// Restore names: "r<16 lowercase hex>.<ext>", ext [a-z0-9]{1,5}.
bool validRestoredName(const std::string& name);
struct OutMedia {
  std::string chat;
  std::string kind;      // "photo" | "document" | "voice" | "audio"
  std::string srcPath;   // the file being sent, on `src`
  std::string caption;
  std::string origin;
};
struct PressureOutcome {
  bool        triggered = false;
  bool        tellOwner = false;   // first pass of this low-space episode
  uint32_t    removedDays = 0;
  uint64_t    freed = 0;
  std::string keptFrom;            // oldest day still on the card ("" if none)
};
enum class Trust : uint8_t { Allowed, Stranger };
// What Archive::adoptMedia kept of the turn's download: nothing, the file itself (the
// same rendition: the job is done), or the turn's copy (the full size is still due).
enum class Adopted : uint8_t { No, Whole, TurnCopy };

class Archive {
 public:
  explicit Archive(Fs& fs) : fs_(fs) {}

  // Who may have media fetched (unset: every chat). Called per inbound update and
  // per day directory in the pressure pass.
  void setTrust(std::function<Trust(const std::string& chat)> fn) { trust_ = std::move(fn); }
  // Exact strings never written to the card in any record (the device's own keys
  // and bot token); shorter than 8 characters is ignored.
  void setRedactions(std::vector<std::string> secrets);

  // Inbound: write every update of one getUpdates body. A replay of an update
  // already archived adds nothing (counted in Stats::dupes). Returns the number of
  // new items written.
  int captureInboundPage(const char* body, size_t len, const Now& now);
  // Outbound text, archived whole (before any splitting into Telegram-size parts).
  bool captureOutText(const std::string& chat, const std::string& text,
                      const std::string& origin, const Now& now);
  // Outbound media: the file is copied from `src` into the archive.
  bool captureOutMedia(const OutMedia& om, Fs& src, const Now& now);
  // Record a restored file that is already on the card.
  bool recordRestored(const Restored& r);

  // The turn path's download, kept the moment it exists. The turn path fetches an
  // inbound photo, document or voice note once, into its own staging file:
  //   - the archive is waiting for the very same file (same update, same file id, so
  //     the same rendition): the staged file is copied in instead of downloading it
  //     again, and the job ends "ok";
  //   - the archive wants another rendition (the turn reads a smaller photo, the
  //     archive keeps the largest): the staged file is kept as turnCopyName() with
  //     its own "ok" status line (`copy`: "turn"), so the item is on the card at turn
  //     time, and the full-size job stays queued; its own line follows when it lands.
  // Returns which (Adopted::No when there is no such job or the copy failed: the job
  // stays queued either way).
  Adopted adoptMedia(const std::string& chat, int64_t updateId, int64_t date, const std::string& fileId,
                     Fs& src, const std::string& srcPath);
  // The other direction: the archive's finished media file for that update when it is
  // the file `fileId` ("" otherwise), so the turn path copies it instead of fetching.
  std::string mediaFor(const std::string& chat, int64_t updateId, int64_t date,
                       const std::string& fileId);

  // Queued jobs, plus one while an overflow waits for the resume pass to re-read it.
  size_t pending() const { return jobs_.size() + (overflowed_ ? 1 : 0); }
  // Downloads. One step fetches the next job, writing the card in `cap`-byte slices,
  // until it ends, `budgetMs` passes (Partial: resumed next step) or `yield` says
  // stop. A write failure leaves the .part file and an "incomplete" status line, and
  // the job is tried again (from the .part) until it has used kMaxTries. A job whose
  // file is already on the card ends "ok" with its status line.
  Step downloadStep(Source& src, uint8_t* buf, size_t cap, uint32_t budgetMs,
                    const std::function<uint32_t()>& nowMs,
                    const std::function<bool()>& yield);
  // Re-queue items still waiting for their file (after a reboot, or once an
  // overflowed queue has drained), from the newest `days` day directories of every
  // chat: a "pending" row with no final status line (the turn's copy is not final:
  // the full size is still due), or one whose last line is "incomplete" with tries
  // left. Returns the jobs added.
  int resumePending(int days = kResumeDays);

  // Storage pressure, given the card's free and total bytes.
  PressureOutcome pressurePass(uint64_t freeBytes, uint64_t totalBytes);

  const Stats& stats() const { return stats_; }
  // A transfer's buffer and limits, and how it ended (defined in the engine; public
  // so its file-local helper can name them).
  struct Pump;
  enum class PumpEnd : uint8_t;
  // The best UTC epoch for an outbound item: the device clock when set, else the
  // last Telegram server date seen plus the uptime since, else 0 (undated).
  int64_t bestEpoch(const Now& now) const;

 private:
  struct MediaRow { std::string file, st; };
  struct OutSlot { int64_t epoch; std::string day, dir; uint32_t seq; };
  enum class Admit : uint8_t { Yes, Overflow, No };

  bool writeSmallAtomic(const std::string& path, const std::string& bytes);
  bool appendLine(const std::string& path, const std::string& line);
  std::string scrub(const std::string& s) const;
  bool archiveOneUpdate(const char* raw, size_t len, const Now& now);
  void replayed(const InMeta& m, const std::string& chat, const std::string& day, const std::string& base);
  bool strangerAdmitted(const InMeta& m, const std::string& chat, const std::string& day, size_t len);
  void writeInRow(const InMeta& m, const std::string& chat, const std::string& day,
                  const std::string& base);
  MediaRow recordMedia(const InMeta& m, const std::string& chat, const std::string& day,
                       const std::string& base);
  Admit admitStranger(const std::string& chat, const std::string& day, size_t bytes);
  bool knownStrangerDir(const std::string& chat);
  void loadStrangerDay(const std::string& day);
  bool isStrangerChat(const std::string& chat) const;
  uint32_t strangerItemsOn(const std::string& chat, const std::string& day);
  bool strangerOverflowSeen(const std::string& chat, const std::string& day, int64_t updateId);
  void writeOverflowRow(const InMeta& m, const std::string& day);
  bool indexHas(const std::string& indexPath, const std::string& needle);
  bool claimOut(const std::string& chat, const Now& now, OutSlot& slot);
  uint32_t claimOutSeq(const std::string& chat, const std::string& day);
  bool copyIn(Fs& src, const std::string& srcPath, const std::string& dstPath,
              uint64_t& bytes);
  bool keepTurnCopy(const Job& j, Fs& src, const std::string& srcPath);
  Step incompleteOrRetry(uint64_t got, const char* err);
  void finishJob(const Job& j, const char* st, uint32_t& counter, uint64_t bytes,
                 const std::string& err);
  bool enqueue(const Job& j);
  std::vector<Job> pendingIn(const std::string& chat, const std::string& day);
  Step retryOrFail(const std::string& err, bool permanent);
  Step endJob(PumpEnd end, bool closed, uint64_t got, int64_t expect);
  Step runFront(Source& src, const Pump& p);
  PumpEnd transfer(Source& src, const Pump& p, const Job& j, bool append, uint64_t& got,
                   bool& closed);
  std::vector<DayUsage> usage();
  void applyPlan(const std::vector<DayUsage>& days, const PressurePlan& plan,
                 PressureOutcome& out);

  // Updates written or verified this boot, by (chat, update id): a re-served update in
  // it costs no card read at all (an album held over several polls, a page re-served
  // after a busy slot or a restart). A fixed ring; an id that fell out of it is checked
  // on the card as before.
  struct SeenId {
    uint32_t chat;
    int64_t  id;
  };
  bool seenThisBoot(uint32_t chat, int64_t id) const;
  void noteSeen(uint32_t chat, int64_t id);
  std::vector<SeenId> seen_;
  size_t              seenHead_ = 0;

  Fs&                 fs_;
  std::vector<std::string> redact_;
  std::deque<Job>     jobs_;
  bool                overflowed_ = false;
  Stats               stats_;
  int64_t             lastServerEpoch_ = 0;
  uint32_t            lastServerMs_ = 0;
  // Next outbound seq per (chat/day), learned from the directory on first use.
  std::vector<std::pair<std::string, uint32_t>> seqCache_;
  std::function<Trust(const std::string&)> trust_;
  Trust curTrust_ = Trust::Allowed;
  // The stranger budgets for the newest day seen (re-read from the card when a later
  // day starts, so a restart does not hand out a fresh budget): per-chat counts (at
  // most kStrangerTableMax entries; a full table denies), and the totals across all
  // stranger chats.
  std::string strangerDay_;
  std::vector<std::pair<std::string, uint32_t>> strangerCount_;
  uint32_t strangerUpdates_ = 0;
  uint64_t strangerBytes_ = 0;
  // Stranger chat directories already on the card (loaded on first need).
  std::vector<std::string> strangerDirs_;
  bool strangerDirsLoaded_ = false;
};

// ---- reading the archive (the web routes and the Memory & Files listing) ---------
// Chat directories, then each chat's day directories newest first.
std::vector<std::string> listChats(Fs& fs);
std::vector<std::string> listDays(Fs& fs, const std::string& chat);
// One page of a day's index lines (each a JSON object), from line `from`.
struct RowPage {
  std::vector<std::string> rows;
  size_t                   next = 0;
  bool                     more = false;
};
RowPage readRows(Fs& fs, const std::string& chat, const std::string& day, size_t from,
                 size_t limit);

// An item's media status as the owner should read it. The index is append-only: an
// item row says "pending" once, when its download is queued, and each outcome is a
// later status line ({"ev":"media","ref":...}). The fold below reads both, plus what
// the card really holds, so a status never says "pending" over a file that is there,
// or "ok" over one that is not.
struct MediaFacts {
  std::string rowSt;            // the item row's own st ("pending", "ok", "not archived: ...")
  std::string lastEv;           // the latest full-size status line's st ("" = none yet)
  int         incompleteEvs = 0;   // "incomplete" lines for it (attempts used)
  bool        fullOnCard = false;  // the full-size file is on the card
  bool        turnOnCard = false;  // the turn's copy (turnCopyName) is on the card
  bool        inResumeWindow = true;   // its day is among the chat's newest kResumeDays
};
struct ItemStatus {
  // "ok" (a copy is on the card), "pending" (not on the card yet, still to be
  // fetched), "failed", "incomplete" (the card refused it, no tries left), "missing"
  // (not on the card and no longer fetched), or the row's "not archived: <why>".
  std::string st;
  bool        turnCopy = false;   // the copy on the card is the turn's smaller one
  std::string full;               // with turnCopy: the full-size file's own status
};
ItemStatus foldMediaStatus(const MediaFacts& f);
// One page of a day's ITEM rows (the status lines are folded into their item, never
// returned), each media row's "st" made truthful by foldMediaStatus; a row whose
// best copy is the turn's carries "file" = that copy, "copy":"turn" and "full" = the
// full-size status. `from`/`next` count item rows. Unreadable lines are skipped.
RowPage readItemRows(Fs& fs, const std::string& chat, const std::string& day, size_t from,
                     size_t limit);
// Media files (not the JSON records), newest day first, across chats or one chat.
struct MediaRef {
  std::string chat, day, name;
  uint64_t    bytes = 0;
};
bool isMediaName(const std::string& name);
std::vector<MediaRef> newestMedia(Fs& fs, const std::string& chat, size_t limit);
// Stream every line of a file to `fn` (lines over 16 KB are skipped). Returns false
// when the file cannot be opened.
bool forEachLine(Fs& fs, const std::string& path, const std::function<void(const std::string&)>& fn);
// The same, stopping at the first line for which `fn` returns true (the rest of the
// file is not read). Returns whether some line stopped it.
bool forEachLineUntil(Fs& fs, const std::string& path, const std::function<bool(const std::string&)>& fn);
// `s` with every raw control byte (0x00-0x1F) written as \u00XX: safe to paste into
// JSON (a serialized document has no raw control bytes outside strings).
std::string escapeControls(const std::string& s);

}  // namespace tgarc
}  // namespace nimbus
