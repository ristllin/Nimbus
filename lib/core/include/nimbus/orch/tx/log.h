#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/item.h"
#include "nimbus/orch/tx/status.h"

// tx/log.h - the transcript on storage (PRD section 2):
//   /mem/tx/<chatKey>/seg-000001.jsonl   items, one line each, rolled at 1 MB
//   /mem/tx/<chatKey>/head.json          a cache of HeadState, temp then rename
//   /mem/tx/<chatKey>/b/<turn>.txb       bulk payloads, one append per round, 30 days
//   /mem/tx/<chatKey>/digests-YYYY-MM.md archived digests
//   /mem/tx/<chatKey>/migrated.json      the migration marker
// None of it is in the file index; older firmware ignores the folder. Nothing goes to NVS.
//
// The device and the hosts reach storage through TxFiles, so the same log runs over the
// card (through the existing gated card append), nimbusd's volume and a host fake.
//
// Seam declared by lane RB-B0a; the body (orch_tx_log.cpp) is lane RB-T2's.
namespace nimbus {
namespace orch {
namespace tx {

inline constexpr const char* kTxRoot = "/mem/tx";
inline constexpr size_t kSegmentRollBytes = 1024 * 1024;
inline constexpr size_t kFillSliceBytes = 8192;       // the restart fill reads in these slices
inline constexpr uint32_t kBulkRetentionDays = 30;
inline constexpr uint32_t kRestartFillBudgetMs = 3000; // restart to full transcript, card time

// The storage the log runs over. Every call is one bounded operation; false is a
// refusal (no card, lock not taken, write failed) and never throws.
class TxFiles {
 public:
  virtual ~TxFiles() = default;
  virtual bool append(const std::string& path, const char* data, size_t len) = 0;
  virtual bool readAt(const std::string& path, size_t offset, char* buf, size_t want, size_t& got) = 0;
  virtual bool size(const std::string& path, size_t& out) = 0;
  virtual bool writeWhole(const std::string& path, const std::string& data) = 0;
  virtual bool rename(const std::string& from, const std::string& to) = 0;
  virtual bool remove(const std::string& path) = 0;
  virtual bool list(const std::string& dir, std::vector<std::string>& names) = 0;
};

// head.json: a cache, rebuilt by scanning back to the newest s and x items when it is
// missing or stale.
struct HeadState {
  uint8_t v = 1;
  uint64_t nextQ = 1;
  uint64_t thru = 0;            // the newest q a digest covers
  uint64_t stub = 0;            // the stub pointer
  uint32_t seg = 1;             // the open segment number
  uint64_t thruOffset = 0;      // byte offset of thru in its segment (restart fill start)
  uint64_t hw = 0;              // highest Telegram update id seen on this chat
  uint64_t mHi = 0;             // newest day-stream row mirrored (re-entry rule)
  std::string profile;          // tx::profileSlug
  std::vector<std::string> pinned;
  uint64_t lastReqAt = 0;       // epoch seconds of the last head request on this chat
  uint32_t openTurn = 0;        // 0 = no turn open
  uint8_t attempts = 0;         // attempts of the open turn
};

// Where an item went: a failed append leaves the item in RAM, not stored, and the
// model sees one device line about it (PRD section 2, "no card").
enum class StoreState : uint8_t { Stored = 0, NotStored = 1 };

class TxLog {
 public:
  TxLog(TxFiles& files, const std::string& chatKey) : files_(files), chatKey_(chatKey) {}

  // Read head.json, or rebuild it by scanning when it is missing or does not match.
  Status open(HeadState& head);
  // One append of one encoded item to the open segment (rolling at kSegmentRollBytes).
  // NotStored when the storage refused: the caller keeps the item in RAM.
  Status append(const Item& item, StoreState& state);
  // Append one round's bulk payloads to b/<turn>.txb; `handle` names where it landed.
  Status appendBulk(uint32_t turn, const std::string& payload, std::string& handle);
  // Read part of a bulk payload by handle (view and exact paging).
  Status readBulk(const std::string& handle, size_t offset, size_t limit, std::string& out);
  // Restart fill: one sequential read from thruOffset into the arena.
  Status fill(const HeadState& head, TxArena& arena);
  // Write head.json by temp and rename (turn close, after a compaction).
  Status writeHead(const HeadState& head);
  // Rebuild HeadState by scanning the segments back to the newest s and x items.
  Status rebuildHead(HeadState& out);

  const std::string& chatKey() const { return chatKey_; }

 private:
  TxFiles& files_;
  std::string chatKey_;
};

// The paths above, for one chat. InvalidArgument for an empty or unsafe chat key.
Status txDir(const std::string& chatKey, std::string& out);
Status segmentPath(const std::string& chatKey, uint32_t seg, std::string& out);
Status headPath(const std::string& chatKey, std::string& out);
Status bulkPath(const std::string& chatKey, uint32_t turn, std::string& out);
Status digestArchivePath(const std::string& chatKey, const std::string& yyyymm, std::string& out);
Status migratedPath(const std::string& chatKey, std::string& out);

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
