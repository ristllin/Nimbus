#pragma once
#include <string>

#include "nimbus/orch/compact.h"
#include "posix_fs.h"

// hosted_fold - where a hosted instance keeps a conversation's fold state: the
// shared FoldStore (nimbus/orch/compact.h) over one file on the instance volume, the
// role /data/chatsum.txt plays on the device. Every decision (when a fold is due, the
// breaker, the anchor) is the shared store's; the fold call is the shared engine's.
namespace nimbusd {

class FileFoldIO : public nimbus::orch::FoldStoreIO {
 public:
  void setPath(const std::string& path) { path_ = path; }
  std::string load() override {
    std::string blob;
    return fsutil::readFile(path_, blob) ? blob : std::string();
  }
  void save(const std::string& blob) override { fsutil::writeFileAtomic(path_, blob); }

 private:
  std::string path_;
};

// The size of a chat's new rows that makes a fold due (the device's compactAtKB
// default). A hosted instance does not offer the knob; the page shows this value.
constexpr unsigned kHostedCompactKb = 48;

}  // namespace nimbusd
