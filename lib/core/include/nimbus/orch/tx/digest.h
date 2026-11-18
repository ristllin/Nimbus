#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/orch/tx/budget.h"
#include "nimbus/orch/tx/item.h"
#include "nimbus/orch/tx/log.h"
#include "nimbus/orch/tx/status.h"

// tx/digest.h - the digest block, its archive file, the device-written recent-files list
// and the chatsum mirror (PRD section 3, "Shape" and "Swap"). Bodies: lane RB-C1
// (lib/core/src/orch_tx_digest.cpp). The compaction driver (RB-C2) and the loop (RB-E1a)
// call them; nothing here calls a model, reads a clock or holds state between calls.
//
// Digests
// - One digest per local day of a compacted slice, appended as an s item {day, x, thru}
//   and never rewritten. Its text is one line, at most kDigestMaxBytes, starting with the
//   device-written day header: "Tue 17 Nov 2026: <summary>" (digestText builds it).
// - The block the request shows is the newest digests that fit the profile's cap
//   (blockCapFor), oldest first, one per line. It is a pure function of the digests, so
//   it is byte-identical from turn to turn until a compaction adds a digest; a digest
//   added at the end extends it, and only a move to the archive changes its start.
// - "Live" digests are the ones not yet archived, oldest first, thru strictly rising.
//   When the block passes its cap, settleDigests moves the oldest live digests that no
//   longer fit to /mem/tx/<chatKey>/digests-YYYY-MM.md (the month of the digest's day),
//   one line each, oldest first, and stops at the first refusal: what did not move stays
//   live and moves at the next settle. The archive line is appended only when the month
//   file does not hold it yet, so a cut at any step leaves every digest either live (in
//   the segments, found again by loadDigests) or archived (a whole line), never neither
//   and never twice.
// - loadDigests rebuilds the live list after a restart from the segments (digests older
//   than thru are not in the arena): newest segment first, until the newest archived
//   digest (the last whole line of the newest month file) is reached. A chat with no
//   digest (head.thru 0) reads nothing.
//
// Recent files: the device's own list of the live file store (path and last write),
// newest first, whole lines, at most kRecentFilesMaxBytes. RecentFilesBlock rebuilds it
// at a cold turn open and at the close of a turn that moved or deleted a file, and keeps
// it byte-identical otherwise (a cache change point only there, PRD section 2).
//
// The chatsum mirror: /data/chatsum.txt in the v5.1.1 FoldStore format, so a rollback to
// v5.1.1 still has a summary. chatsumMirror is the summary text (the newest digests
// through thru that fit kChatsumMirrorMaxBytes); mirrorChatsum puts it into an existing
// blob as this chat's record: other chats' records kept byte for byte, counters 0, the
// failure count and the pause cleared, the row pointer forward only.
namespace nimbus {
namespace orch {
namespace tx {

inline constexpr uint32_t kDigestMaxBytes = 768;
inline constexpr uint32_t kDigestBlockStandard = 6144;   // standard and wide
inline constexpr uint32_t kDigestBlockLean = 4096;
inline constexpr uint32_t kDigestBlockTight = 2048;
inline constexpr uint32_t kRecentFilesMaxBytes = 600;
inline constexpr uint32_t kChatsumMirrorMaxBytes = 4096;  // /data/chatsum.txt, v5.1.1 format
// v5.1.1's FoldStore limits (its decoder reads at most 16 records; its blob cap was 48 KB).
inline constexpr uint32_t kMirrorMaxRecords = 16;
inline constexpr uint32_t kMirrorBlobMaxBytes = 48 * 1024;
// A recent-files path longer than this is left out of the list.
inline constexpr uint32_t kRecentPathMaxBytes = 160;

// The device-written pieces of digest and recent-files text (model-facing: catalogued
// by tools/release_gate/gen_model_text_catalog.py, E2_EXTRA_SOURCES, class core). The
// dates themselves are tx::photoWhen's (rules.h, Ruling 11: with the year).
namespace digest_text {
inline constexpr char kDaySep[] = ": ";     // "Tue 17 Nov 2026: <summary>"
inline constexpr char kWhenOpen[] = " (";   // "notes/boat.md (Tue 17 Nov 2026 13:02)"
inline constexpr char kWhenClose[] = ")";
}  // namespace digest_text

struct DigestEntry {
  std::string day;      // "YYYY-MM-DD"
  std::string text;     // one line, at most kDigestMaxBytes, day header included
  uint64_t q = 0;       // the s item
  uint64_t thru = 0;    // the last q it covers
};

struct RecentFile {
  std::string path;     // "project/name"
  uint64_t lastWrite = 0;   // epoch seconds; 0 = not known
};

// The block cap of a profile: tight 2,048, lean 4,096, standard and wide 6,144 B.
uint32_t blockCapFor(Profile profile);

// "Tue 17 Nov 2026" for a valid "YYYY-MM-DD" (InvalidArgument otherwise).
Status dayHeader(const std::string& day, std::string& out);
// One digest's text: the day header, kDaySep, then `body` on one line (line ends, tabs,
// control characters, U+2028/2029 and bidi controls become spaces, invalid UTF-8 '?',
// runs of spaces one). Over kDigestMaxBytes it is cut at the last sentence end that fits,
// else at the last space, else at a whole character. InvalidArgument for a bad day or a
// body with no visible character.
Status digestText(const std::string& day, const std::string& body, std::string& out);
// An s item (q, turn and t are the caller's) and back. digestOf answers InvalidArgument
// for an item that is not a digest, is marked ext, or carries an invalid text or day.
Status digestItem(const DigestEntry& d, uint32_t turn, uint64_t t, Item& out);
Status digestOf(const Item& s, DigestEntry& out);

// The block the request shows for `profile`: the newest digests that fit its cap, oldest
// first, joined by '\n' (no trailing line end). `archive` gets the older digests that no
// longer fit (oldest first), for archiveDigest. InvalidArgument (block and archive empty)
// when a digest is not valid (see digestOf) or thru does not rise strictly.
Status digestBlock(const std::vector<DigestEntry>& digests, Profile profile, std::string& block,
                   std::vector<DigestEntry>& archive);
// Append one digest to its month's archive file, unless the file already holds it as a
// whole line. One append of at most kDigestMaxBytes + 2 B. Io when the storage refused.
Status archiveDigest(TxFiles& files, const std::string& chatKey, const DigestEntry& digest);

// What a boot load read.
struct DigestScan {
  size_t segments = 0;   // segments read
  size_t reads = 0;      // TxFiles::readAt calls
  uint64_t bytes = 0;    // bytes read
  size_t torn = 0;       // lines dropped for framing or crc
  bool reachedArchive = false;   // stopped at the newest archived digest
};
// The live digests of a chat after a restart (see above), oldest first. `head` is the one
// TxLog::open returned. Io when the storage refused a read (live is left empty).
Status loadDigests(TxFiles& files, const std::string& chatKey, const HeadState& head,
                   std::vector<DigestEntry>& live, DigestScan* scan = nullptr);

struct SettleReport {
  size_t moved = 0;     // digests archived by this call
  size_t pending = 0;   // digests over the cap still live (a refusal stopped the move)
};
// Archive what no longer fits (oldest first, stopping at the first refusal), drop the
// archived ones from `live`, and set `block` to digestBlock's text. Ok even when a move
// was refused (report.pending says so; the block never holds a digest over the cap).
Status settleDigests(TxFiles& files, const std::string& chatKey, Profile profile,
                     std::vector<DigestEntry>& live, std::string& block, SettleReport& report);

// The recent-files list: one line per file, "<path> (<photoWhen of lastWrite>)" or the path
// alone when the write time is not known, newest write first (ties by path), one line per
// path, whole lines up to kRecentFilesMaxBytes. Paths are flattened to one line; a path
// over kRecentPathMaxBytes is left out.
Status recentFilesList(const std::vector<RecentFile>& files, int16_t tzMinutes, std::string& out);
// Does a successful call of `tool` change which files exist (so the list is rebuilt at the
// close of its turn)? delete_file, move_file and rename_file do; writes do not (a cold open
// picks them up). The caller passes the resolved tool name (the one `more` routed to).
bool changesFileSet(const std::string& tool);

// The list as the request shows it, rebuilt only at the allowed change points.
class RecentFilesBlock {
 public:
  // Lists the live file store; false when it cannot be read (no card).
  using Source = std::function<bool(std::vector<RecentFile>&)>;
  // At turn open: a cold open rebuilds, a warm one keeps the text byte for byte. Io when
  // the store could not be read (the list is then empty).
  Status atOpen(bool cold, const Source& source, int16_t tzMinutes);
  // At turn close: rebuilt when the turn moved or deleted a file, otherwise kept.
  Status atClose(bool movedOrDeleted, const Source& source, int16_t tzMinutes);
  const std::string& text() const { return text_; }
  uint32_t rebuilds() const { return rebuilds_; }

 private:
  Status rebuild(const Source& source, int16_t tzMinutes);
  std::string text_;
  uint32_t rebuilds_ = 0;
};

// The v5.1.1 summary text: the newest digests with thru at or below `thru` that fit
// kChatsumMirrorMaxBytes, oldest first, one per line. Empty (and ok) when none qualifies.
// InvalidArgument when a digest is not valid or thru does not rise strictly.
Status chatsumMirror(const std::vector<DigestEntry>& digests, uint64_t thru, std::string& out);

struct MirrorFacts {
  std::string chatId;   // the FoldStore chat id (the Telegram chat id as engine 1 keys it)
  uint64_t thru = 0;    // the newest q the digests cover (head.thru)
  uint64_t thruRow = 0; // the day-stream row id of the newest owner message at or below thru
  uint32_t epoch = 0;   // when the digests were made (lastFoldEpoch); 0 = clock not set
};
// /data/chatsum.txt after a swap: `blob` (the file as it is, "" when missing) with this
// chat's record set to chatsumMirror's text and pointer "m%08x" of thruRow, moved to the
// newest place; counters 0, failures and pause 0, no shift pointer. Unchanged (out = blob)
// when the text is empty or the pointer would move back. Other chats' records are kept byte
// for byte; the oldest are dropped to keep v5.1.1's 16 records and 48 KB. A trailing
// partial record is dropped. InvalidArgument for an empty chat id or one holding a
// separator or control byte.
Status mirrorChatsum(const std::string& blob, const MirrorFacts& facts, const std::vector<DigestEntry>& digests,
                     std::string& out);
// The row pointer for MirrorFacts: the highest u.m among owner messages with q <= thru
// (0 when none).
uint64_t rowThru(const std::vector<Item>& items, uint64_t thru);

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
