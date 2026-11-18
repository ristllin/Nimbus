#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/tx/budget.h"
#include "nimbus/orch/tx/log.h"
#include "nimbus/orch/tx/status.h"

// tx/digest.h - the digest block, its archive file, the device-written recent-files list
// and the chatsum mirror (PRD section 3, "Shape" and "Swap"). One digest per local day,
// at most kDigestMaxBytes, appended as s items and never rewritten. When the block is
// full the oldest digest moves to /mem/tx/<chatKey>/digests-YYYY-MM.md.
//
// Seam declared by lane RB-B0a; the bodies (orch_tx_digest.cpp) are lane RB-C1's.
namespace nimbus {
namespace orch {
namespace tx {

inline constexpr uint32_t kDigestMaxBytes = 768;
inline constexpr uint32_t kDigestBlockStandard = 6144;   // standard and wide
inline constexpr uint32_t kDigestBlockLean = 4096;
inline constexpr uint32_t kDigestBlockTight = 2048;
inline constexpr uint32_t kRecentFilesMaxBytes = 600;
inline constexpr uint32_t kChatsumMirrorMaxBytes = 4096;  // /data/chatsum.txt, v5.1.1 format

struct DigestEntry {
  std::string day;      // "YYYY-MM-DD"
  std::string text;     // at most kDigestMaxBytes, device-written day header included
  uint64_t q = 0;       // the s item
  uint64_t thru = 0;    // the last q it covers
};

struct RecentFile {
  std::string path;     // "project/name"
  uint64_t lastWrite = 0;
};

// The block the request shows for `profile`, newest digests that fit; `archive` gets
// the digests that no longer fit (oldest first), for archiveDigest.
Status digestBlock(const std::vector<DigestEntry>& digests, Profile profile, std::string& block,
                   std::vector<DigestEntry>& archive);
// Append one digest to its month's archive file.
Status archiveDigest(TxFiles& files, const std::string& chatKey, const DigestEntry& digest);
// The recent-files list (path and last write), at most kRecentFilesMaxBytes.
Status recentFilesList(const std::vector<RecentFile>& files, std::string& out);
// /data/chatsum.txt in its v5.1.1 format: newest digests that fit 4,096 B, pointer
// forward only, pause cleared.
Status chatsumMirror(const std::vector<DigestEntry>& digests, uint64_t thru, std::string& out);

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
