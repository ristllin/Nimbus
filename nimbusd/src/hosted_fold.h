#pragma once
#include <string>

#include "nimbus/orch/compact.h"
#include "posix_fs.h"

// hosted_fold - where a hosted instance keeps a conversation's fold state: the
// shared FoldStore (nimbus/orch/compact.h) over one file on the instance volume, the
// role /data/chatsum.txt plays on the device. Every decision (when a fold is due, its
// wait, the anchor) is the shared store's and the shared FoldDriver's; the fold call is
// the shared engine's.
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

// The device-wide fold control (lane F of the history fix): the daily paid count, the
// alert day, the queued manual requests and the firmware at the last failure, in a
// sibling file of the fold store (the device's /data/fold_ctl.txt).
class FileFoldCtlIO {
 public:
  void setPath(const std::string& path) { path_ = path; }
  nimbus::orch::FoldCtlIO io() const {
    nimbus::orch::FoldCtlIO io;
    const std::string path = path_;
    io.load = [path] {
      std::string text;
      return fsutil::readFile(path, text) ? text : std::string();
    };
    io.save = [path](const std::string& text) { return fsutil::writeFileAtomic(path, text); };
    return io;
  }

 private:
  std::string path_;
};

// The size of a chat's new rows that makes a fold due (the device's compactAtKB
// default). A hosted instance does not offer the knob; the page shows this value. It
// means "derived" once lane B's history budget lands (foldTriggerBytes).
constexpr unsigned kHostedCompactKb = 48;

}  // namespace nimbusd
