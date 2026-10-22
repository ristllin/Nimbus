#pragma once
// sub_fabric - the hosted instance's sub-agent fabric (device parity).
//
// A spawned sub-agent is how the device runs a provider's connectors off its own
// turn: the head (tool loop on) spawns a Mistral sub, the sub's Conversations call
// carries the Studio connectors (gcal/notion/slack) server-side, and the result
// comes back through the JobEngine's synthesis turn. Without a fabric the
// JobEngine drops every spawn SILENTLY (dispatchSpawn returns when fabric is
// null), so a hosted instance could never run a Mistral Studio connector under
// its default tool loop, and the owner got an "on it" with nothing after.
//
// Everything that decides anything is already portable and host-tested in
// lib/harness: the per-provider wire (providers::*Dispatch/Poll/Cancel), the job
// machinery (JobEngine: queue, one dispatch per pump, round-robin poll, synthesis
// clock) and the journal logic (nimbus::orch::Journal). This header only binds
// them to the daemon, exactly as the device's src/agent/adapters/*_adapter.cpp
// bind them to the board:
//   SubAdapter        one ManagedAgentAdapter per backend; every call resolves
//                     the rig's CURRENT deps/key/model, so an in-app key or model
//                     change applies to the next dispatch with no re-register.
//   FileJournalStore  the device's journal file (nimbus::orch::FileJournalStore)
//                     under the instance volume (<mem>/journal/agjournal.json), the
//                     same shape as the device's /data/agjournal.json: an unfinished
//                     job re-attaches after a restart instead of being re-dispatched,
//                     and the finished-job ring tells the head what already finished.
#include <cerrno>
#include <cstdio>
#include <functional>
#include <string>

#include "nimbus/harness/fabric.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/journal.h"
#include "posix_fs.h"

namespace nimbusd {

class SubAdapter : public agent::ManagedAgentAdapter {
 public:
  struct Ops {
    std::function<agent::FabricErr(const agent::Directive& d, char outJobId[72])> dispatch;
    std::function<agent::FabricErr(const char* jobId, agent::ResultEnvelope& env)> poll;
    std::function<agent::FabricErr(const char* jobId)> cancel;
    uint16_t typicalLatencySec = 60;
  };
  SubAdapter(std::string backend, Ops ops) : backend_(std::move(backend)), ops_(std::move(ops)) {}

  const char* backendId() const override { return backend_.c_str(); }
  agent::Capabilities capabilities() const override {
    agent::Capabilities c;
    c.typicalLatencySec = ops_.typicalLatencySec;
    return c;
  }
  agent::FabricErr dispatch(const agent::Directive& d, char outJobId[72]) override {
    return ops_.dispatch ? ops_.dispatch(d, outJobId) : agent::FabricErr::Unsupported;
  }
  agent::FabricErr poll(const char* jobId, agent::ResultEnvelope& env) override {
    return ops_.poll ? ops_.poll(jobId, env) : agent::FabricErr::Unsupported;
  }
  agent::FabricErr cancel(const char* jobId) override {
    return ops_.cancel ? ops_.cancel(jobId) : agent::FabricErr::Unsupported;
  }

 private:
  std::string backend_;
  Ops ops_;
};

// The journal file on the instance volume, the hosted twin of the device's flash
// file. Writes are atomic (tmp + rename), so a crash mid-write leaves the previous
// image, never a torn one; put and remove report whether they landed. The per-slot
// files of earlier hosted releases (j<slot>.json) are read once to migrate and removed
// once the new file reads back (a hosted instance has no trial boot to roll back from).
class PosixJournalIo : public nimbus::orch::JournalFileIo {
 public:
  explicit PosixJournalIo(std::string dir) : dir_(std::move(dir)) {}
  bool readFile(std::string& out) override { return fsutil::readFile(file(), out); }
  bool writeFileAtomic(const std::string& v) override {
    return fsutil::mkdirs(dir_) && fsutil::writeFileAtomic(file(), v);
  }
  std::string readLegacySlot(int slot) override {
    std::string v;
    return fsutil::readFile(legacy(slot), v) ? v : std::string();
  }
  // Gone means gone: a slot file that never existed is as removed as a deleted one.
  bool eraseLegacy() override {
    bool ok = true;
    for (int i = 0; i < nimbus::orch::kAgentMaxJobs; i++)
      ok = (std::remove(legacy(i).c_str()) == 0 || errno == ENOENT) && ok;
    return ok;
  }

 private:
  std::string file() const { return dir_ + "/agjournal.json"; }
  std::string legacy(int slot) const { return dir_ + "/j" + std::to_string(slot) + ".json"; }
  std::string dir_;
};

// The I/O is a private base listed first, so it exists before the store that keeps a
// reference to it.
class FileJournalStore : private PosixJournalIo, public nimbus::orch::FileJournalStore {
 public:
  // Loads at once: the journal's begin() reads the slots right after construction.
  explicit FileJournalStore(std::string dir)
      : PosixJournalIo(std::move(dir)),
        nimbus::orch::FileJournalStore(static_cast<PosixJournalIo&>(*this)) {
    load();
    retireLegacy(/*imageValidAtBoot=*/true);
  }
};

}  // namespace nimbusd
