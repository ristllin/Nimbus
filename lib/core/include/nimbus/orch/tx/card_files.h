#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "nimbus/orch/tx/log.h"

// tx/card_files.h - the transcript's storage on a memory card (lane E2-DEV, the RB-I3 minimum):
// tx::TxFiles over a small card interface, so the device binding (src/agent/tx_store_device.cpp)
// and a host fake of the card run the same rules. Copied from the effect ring's CardFxFiles
// (src/agent/effect_ring_device.cpp) with three changes: the root is /mem/tx (every chat's folder
// and its b/ folder below it), a write creates every missing folder on its path, and a write is
// refused while the largest free internal block is under the card's memory floor (the card's
// bounce buffer and the TLS handshake share that memory).
//
// Each call is one card step under the card lock. A refusal answers false and never throws, so
// the log runs from RAM with no card (TxLog::open answers Io, appends NotStored). list() answers
// "no files" only on positive evidence that the folder is absent; a failure is never "empty". Nothing here
// cuts, drops or rewrites stored bytes: append only adds at the end, writeWhole replaces one whole
// file the log names (head.json.tmp), rename replaces its target (FAT cannot rename over a file:
// the target is removed first, as the TxFiles contract allows; a cut between the two leaves only
// head.json.tmp, which TxLog::open reads first).
namespace nimbus {
namespace orch {
namespace tx {

// The card, as the store needs it. Every path is absolute ("/mem/tx/...").
class CardOps {
 public:
  virtual ~CardOps() = default;
  virtual bool present() = 0;             // a card is mounted
  virtual bool mayWrite() = 0;            // this task may touch a file, no injected card fault
  virtual uint32_t largestInternal() = 0; // the largest free internal block, bytes
  virtual uint32_t nowMs() = 0;           // monotonic, for the hold times
  virtual void lock() = 0;                // the card lock (recursive)
  virtual void unlock() = 0;
  virtual bool exists(const std::string& path) = 0;
  virtual bool isDir(const std::string& path) = 0;
  virtual bool mkdir(const std::string& path) = 0;   // one level
  // Adds `len` bytes at the end (creates the file); the bytes written.
  virtual size_t append(const std::string& path, const char* data, size_t len) = 0;
  // false: no such file or a read error. Past the end: true with got = 0.
  virtual bool readAt(const std::string& path, size_t offset, char* buf, size_t want, size_t& got) = 0;
  virtual bool fileSize(const std::string& path, size_t& out) = 0;   // false: missing or a folder
  virtual size_t writeWhole(const std::string& path, const char* data, size_t len) = 0;   // bytes written
  virtual bool rename(const std::string& from, const std::string& to) = 0;   // `to` must not exist
  virtual bool remove(const std::string& path) = 0;
  // The bare names of the files (not folders) in an existing folder. false on any failure,
  // never a short listing.
  virtual bool listFiles(const std::string& dir, std::vector<std::string>& names) = 0;
  // The bare names of every entry (files and folders) of an existing folder. false on any
  // failure (a missing folder included), never a short listing: a name missing from a true
  // answer is positive evidence that the folder does not hold it (lane E2-STORE).
  virtual bool listNames(const std::string& dir, std::vector<std::string>& names) = 0;
};

// The largest free internal block a card write needs left (DECISION, lane E2-DEV): the SD
// driver's bounce buffer is one sector, the card lock's holder may log; 8 KB keeps every write
// clear of the TLS handshake's own floor, which the turn admission checks separately.
inline constexpr uint32_t kCardWriteInternalFloor = 8192;

// The longest card-lock hold of each kind of call, and how many calls were refused.
struct CardHolds {
  uint32_t appendMs = 0, wholeMs = 0, renameMs = 0, readMs = 0, listMs = 0;
  uint32_t appends = 0, wholes = 0, renames = 0, reads = 0;
  uint32_t refusedNoCard = 0, refusedFloor = 0, refusedTask = 0;
};

class CardTxFiles : public TxFiles {
 public:
  // `root`: the one folder this store reaches (lane E2-SCHED: /mem/routines for the routine state files
  // and outboxes, beside the transcripts' /mem/tx).
  explicit CardTxFiles(CardOps& card, uint32_t floor = kCardWriteInternalFloor, std::string root = kTxRoot)
      : card_(card), floor_(floor), prefix_(std::move(root) + "/") {}

  bool append(const std::string& path, const char* data, size_t len) override {
    if (!writable(path)) return false;
    Hold h(card_, holds_.appendMs);
    ++holds_.appends;
    if (!makeParents(path)) return false;
    return card_.append(path, data, len) == len;
  }
  bool readAt(const std::string& path, size_t offset, char* buf, size_t want, size_t& got) override {
    got = 0;
    if (!inside(path) || !card_.present()) return false;
    Hold h(card_, holds_.readMs);
    ++holds_.reads;
    return card_.readAt(path, offset, buf, want, got);
  }
  bool size(const std::string& path, size_t& out) override {
    if (!inside(path) || !card_.present()) return false;
    Hold h(card_, holds_.readMs);
    return card_.fileSize(path, out);
  }
  bool writeWhole(const std::string& path, const std::string& data) override {
    if (!writable(path)) return false;
    Hold h(card_, holds_.wholeMs);
    ++holds_.wholes;
    if (!makeParents(path)) return false;
    return card_.writeWhole(path, data.data(), data.size()) == data.size();
  }
  bool rename(const std::string& from, const std::string& to) override {
    if (!writable(from) || !inside(to)) return false;
    Hold h(card_, holds_.renameMs);
    ++holds_.renames;
    if (!card_.exists(from)) return false;
    if (card_.exists(to) && !card_.remove(to)) return false;
    return card_.rename(from, to);
  }
  bool remove(const std::string& path) override {
    if (!writable(path)) return false;
    Hold h(card_, holds_.wholeMs);
    return card_.remove(path);
  }
  // Three answers (lane E2-STORE, finding S1): the folder's files; true with no names only
  // when the folder is proven absent (its parent listed and does not hold the name, or the
  // parent is proven absent the same way); false when it cannot tell. An exists() that
  // answers false is not evidence: a bad read or a refused allocation answers false too.
  bool list(const std::string& dir, std::vector<std::string>& names) override {
    names.clear();
    if (!(inside(dir + "/") || dir + "/" == prefix_) || !card_.present()) return false;   // the root itself lists too
    Hold h(card_, holds_.listMs);
    if (card_.exists(dir)) {
      if (!card_.isDir(dir)) return false;
      if (card_.listFiles(dir, names)) return true;
      names.clear();
      return false;
    }
    return provenAbsent(dir);
  }

  const CardHolds& holds() const { return holds_; }
  void resetHolds() { holds_ = CardHolds{}; }

  // Inside the root (/mem/tx/ by default), no "..", no empty segment.
  bool inside(const std::string& path) const {
    if (path.rfind(prefix_, 0) != 0 || path.size() == prefix_.size()) return false;
    return path.find("/..") == std::string::npos && path.find("//") == std::string::npos;
  }

 private:
  // The lock for one call, and its hold time into the kind's maximum.
  struct Hold {
    Hold(CardOps& c, uint32_t& maxMs) : c_(c), max_(maxMs), t0_(c.nowMs()) { c_.lock(); }
    ~Hold() {
      c_.unlock();
      const uint32_t ms = c_.nowMs() - t0_;
      if (ms > max_) max_ = ms;
    }
    CardOps& c_;
    uint32_t& max_;
    uint32_t t0_;
  };

  // A write may run: inside the root, a card, a task that may touch a file, no injected card
  // fault, and the internal memory floor kept.
  bool writable(const std::string& path) {
    if (!inside(path)) return false;
    if (!card_.present()) return ++holds_.refusedNoCard, false;
    if (!card_.mayWrite()) return ++holds_.refusedTask, false;
    if (card_.largestInternal() < floor_) return ++holds_.refusedFloor, false;
    return true;
  }
  // `path` is not on the card: its parent listed whole and holds no such name, or the parent
  // is itself proven absent. Any failure on the way (the root's listing included) answers
  // false: cannot tell. The card lock held; at most one listing per level.
  bool provenAbsent(const std::string& path) {
    std::string at = path;
    std::vector<std::string> entries;
    for (;;) {   // a loop, not a recursion: one level up per pass (the tg_poll stack gate)
      const size_t slash = at.rfind('/');
      if (slash == std::string::npos || slash + 1 >= at.size()) return false;
      const std::string parent = slash == 0 ? std::string("/") : at.substr(0, slash);
      if (card_.listNames(parent, entries)) {
        const std::string name = at.substr(slash + 1);
        for (const std::string& e : entries)
          if (e == name) return false;   // there, yet exists() said no: cannot tell
        return true;
      }
      if (parent == "/" || card_.exists(parent)) return false;   // the parent is there: cannot tell
      at = parent;
    }
  }
  // Every folder from /mem down to the file's own (the card lock held).
  bool makeParents(const std::string& path) {
    const size_t last = path.rfind('/');
    if (last == std::string::npos || last == 0) return false;
    for (size_t at = path.find('/', 1); at != std::string::npos && at <= last; at = path.find('/', at + 1)) {
      const std::string dir = path.substr(0, at);
      if (card_.exists(dir)) {
        if (!card_.isDir(dir)) return false;
        continue;
      }
      if (!card_.mkdir(dir) && !card_.isDir(dir)) return false;
    }
    return true;
  }

  CardOps& card_;
  uint32_t floor_;
  std::string prefix_;
  CardHolds holds_;
};

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
