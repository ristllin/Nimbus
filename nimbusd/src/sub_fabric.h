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
//   FileJournalStore  the JournalStore seam over files under the instance volume
//                     (<mem>/journal/j<slot>.json), the hosted twin of the
//                     device's NVS "agjournal" namespace: an unfinished job
//                     re-attaches after a restart instead of being re-dispatched.
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

// JournalStore over one small file per slot. Writes are atomic (tmp + rename), so a
// crash mid-write leaves the previous record, never a torn one.
class FileJournalStore : public nimbus::orch::JournalStore {
 public:
  explicit FileJournalStore(std::string dir) : dir_(std::move(dir)) {}
  std::string get(int slot) override {
    std::string v;
    return fsutil::readFile(path(slot), v) ? v : std::string();
  }
  void put(int slot, const std::string& v) override {
    fsutil::mkdirs(dir_);
    fsutil::writeFileAtomic(path(slot), v);
  }
  void remove(int slot) override { std::remove(path(slot).c_str()); }
  void clearNs() override {
    for (int i = 0; i < nimbus::orch::kAgentMaxJobs; i++) remove(i);
  }

 private:
  std::string path(int slot) const { return dir_ + "/j" + std::to_string(slot) + ".json"; }
  std::string dir_;
};

}  // namespace nimbusd
