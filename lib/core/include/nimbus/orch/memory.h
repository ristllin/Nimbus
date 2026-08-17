#pragma once
#include <string>
#include <utility>
#include <vector>

#include "nimbus/orch/caps.h"

// Ported and adapted from Nuage-Solide src/agent/orch_memory.{h,cpp}
// (Head Orchestrator v2). Made portable: std::string instead of Arduino String,
// and LittleFS file I/O + the store::sysPrompt() directive source are hoisted out
// behind a MemoryStore seam so the cap logic + prompt assembly are host-testable.
// The DEVICE implements MemoryStore against LittleFS (/data/orchmem.txt); host
// tests use an in-memory store.
//
// Two-part Head Orchestrator memory (orchestrator-v2-spec §3):
//  - directive : user-owned, IMMUTABLE by the model (device passes store::sysPrompt()).
//                Byte-capped to kMemDirectiveMax; always injected first in the prompt.
//  - model mem : maintained by the orchestrator via its JSON `memory` field,
//                persisted by the device (survives reboot), byte-capped to
//                kMemModelMax with the cap ENFORCED HERE (never trusted to the
//                model). It is the provider-agnostic failover/reboot seed - the
//                ONLY state that survives across turns and provider failovers, so
//                the cap must never split a UTF-8 sequence (utf8CapLen).
namespace nimbus {
namespace orch {

// Device implements this against LittleFS; host tests use an in-memory struct.
struct MemoryStore {
  virtual ~MemoryStore() = default;
  virtual std::string loadModel() = 0;             // "" if none persisted
  virtual void        saveModel(const std::string& v) = 0;
  virtual void        clearModel() = 0;            // remove the persisted blob
};

class OrchMemory {
 public:
  // Load + cap the persisted model memory; cap + store the user directive.
  // `store` is borrowed (device owns its lifetime); pass nullptr in pure tests
  // that never persist (setModel/model still work in RAM).
  void begin(MemoryStore* store, const std::string& directive);

  std::string directive() const;  // re-capped to kMemDirectiveMax on every read

  // Per-chat running memory (Release B3 - the zero-extra-turns fold): the model
  // refreshes its `memory` field every turn anyway; scoping it per chat turns it
  // into a rolling per-conversation summary. LRU-8 chats, kMemModelMax each,
  // serialized into the ONE MemoryStore string ("chat\x1F mem\x1E..."); legacy
  // single-blob values are discarded at load (working notes, benign to reset).
  std::string model(const std::string& chatId = "") const;  // "" = shared default slot
  std::string modelAny() const;   // newest chat's memory (web echo/diagnostics)

  // Update a chat's running memory from its turn's `memory` field. Caps to
  // kMemModelMax, persists the whole map, returns TRUE when it truncated -
  // the device logs it and the model is thereby told to compress next turn.
  bool setModel(const std::string& chatId, const std::string& v);
  bool setModel(const std::string& v) { return setModel(std::string(), v); }

  // Append the two labelled blocks to the system prompt being assembled:
  //   [USER DIRECTIVE - always honor; you cannot change this]
  //   [YOUR MEMORY - … return an updated "memory" field (<=1200 bytes) …]
  // with "(empty)" when the model memory is blank.
  void appendPromptBlock(std::string& sys, const std::string& chatId = "") const;

  void clear();  // wipe model memory (RAM + persisted); directive is untouched

 private:
  MemoryStore* store_ = nullptr;
  std::string  directive_;
  // Per-chat map, age-ordered (newest LAST); bounded LRU-8 in setModel.
  std::vector<std::pair<std::string, std::string>> chats_;
  void save();   // serialize chats_ -> store_
};

}  // namespace orch
}  // namespace nimbus
