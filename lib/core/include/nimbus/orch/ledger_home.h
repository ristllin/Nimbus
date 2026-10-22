#pragma once
// ledger_home - where the device serves its usage ledger from (prism row 21).
//
// The ledger lives in a flash file (/data/usage_ledger.txt, the moved-store rule in
// nimbus/store/moved_store.h). While the file store is not mounted (early boot, or a
// mount that failed) the device serves the legacy NVS copy read-only. It used to
// re-read that copy on EVERY call, throwing away the spend recorded since, so a monthly
// cap could never trip while the file store was down. Now the copy is read once and
// the RAM ledger keeps counting; when the file store comes up, the file is loaded and
// the spend counted meanwhile is added to it (UsageLedger::mergeSpendSince), never lost
// and never counted twice. Host-tested in test/test_orch_usage_ledger.

#include <cstdint>
#include <string>

#include "nimbus/orch/usage_ledger.h"
#include "nimbus/store/moved_store.h"

namespace nimbus {
namespace orch {

class LedgerHome {
 public:
  enum class Step : uint8_t {
    None = 0,   // nothing changed (already on the file, or still counting on the fallback)
    Fallback,   // served the legacy NVS copy, read once (the file store is not up)
    File,       // loaded from the file store (file, else the legacy key)
    Merged,     // loaded from the file store, plus the spend counted on the fallback
  };

  // Call before every use of `L`. `fileUp`: the file store is mounted. `io` is the
  // moved-store Io (readFile, readNvs, ...).
  template <typename Io>
  Step ensure(UsageLedger& L, bool fileUp, Io& io) {
    if (state_ == State::File) return Step::None;
    if (!fileUp) {
      if (state_ == State::Fallback) return Step::None;   // keep counting in RAM
      std::string v;
      io.readNvs(v);
      L.deserialize(v);
      // Still on the NVS fallback: no new (provider, tag) audit row may grow it.
      L.setAllowNewTags(false);
      base_ = L;
      state_ = State::Fallback;
      return Step::Fallback;
    }
    const store::MovedLoad m = store::loadMoved(io);
    src_ = m.src;
    const bool merge = state_ == State::Fallback;
    state_ = State::File;
    if (!merge) {
      L.deserialize(m.value);
      return Step::File;
    }
    UsageLedger file;
    file.deserialize(m.value);
    file.mergeSpendSince(base_, L);
    L = file;
    base_ = UsageLedger();
    return Step::Merged;
  }

  bool onFile() const { return state_ == State::File; }
  // Where the file-store load read the ledger from (None before it ran).
  store::MovedSource source() const { return src_; }

 private:
  enum class State : uint8_t { Unloaded = 0, Fallback, File };
  State              state_ = State::Unloaded;
  store::MovedSource src_ = store::MovedSource::None;
  UsageLedger        base_;   // the NVS copy as read, while on the fallback
};

}  // namespace orch
}  // namespace nimbus
