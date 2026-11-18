// The digest block, archive, recent-files list and chatsum mirror (tx/digest.h). Stub
// bodies from lane RB-B0a: each answers Err::NotImplemented until lane RB-C1 writes them.
#include "nimbus/orch/tx/digest.h"

namespace nimbus {
namespace orch {
namespace tx {

Status digestBlock(const std::vector<DigestEntry>&, Profile, std::string&, std::vector<DigestEntry>&) {
  return Status::pending("tx::digestBlock");
}
Status archiveDigest(TxFiles&, const std::string&, const DigestEntry&) { return Status::pending("tx::archiveDigest"); }
Status recentFilesList(const std::vector<RecentFile>&, std::string&) { return Status::pending("tx::recentFilesList"); }
Status chatsumMirror(const std::vector<DigestEntry>&, uint64_t, std::string&) {
  return Status::pending("tx::chatsumMirror");
}

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
