#pragma once
// ResultStore - bounded registry of FULL recent results (clamped tool outputs +
// overflowed sub-agent results), so a clip is a VIEW, never a loss (owner ask
// 2026-08-05: "we should never stupidly trim"). The head fetches any stored
// result on demand via the results.get / results.list registry tools; misses
// point at memory.episodic (the durable store).
//
// PSRAM-resident on device: entry text is std::string (≥128 B bodies spill via
// heap_caps_malloc_extmem_enable) and the entries vector rides WorkingAllocator.
// Ring semantics: kSlots entries max AND kTotalMax summed bytes - oldest evicts
// first past either bound; a single entry clips at kEntryMax (UTF-8-safe).
//
// Arduino-free, host-tested (test_orch_result_store).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/orch/psram_alloc.h"
#include "nimbus/orch/tool_registry.h"

namespace nimbus {
namespace orch {

class ResultStore {
 public:
  static constexpr int    kSlots    = 16;
  static constexpr size_t kEntryMax = 65536;        // clip on put (UTF-8-safe)
  static constexpr size_t kTotalMax = 512u * 1024;  // summed bytes; ring evicts oldest

  struct Entry {
    std::string tag;    // "r<seq>" (tool spill) | "sub:<jobtag>" (sub-agent result)
    std::string kind;   // "tool" | "sub"
    std::string name;   // tool name / job model label
    std::string text;   // full (entry-clipped) result text
    std::string ns;     // OWNING namespace - the data boundary (see get/list)
    uint32_t    atMs = 0;
  };

  // Returns the assigned tag. kind "sub" uses "sub:"+name-as-jobtag when
  // jobTag is non-empty; everything else gets "r<seq>".
  // ⚠ `ns` is the owning tenant. A spill with an EMPTY ns is device-internal and
  // readable only by a principal with readAll (admin) - never by a guest.
  std::string put(const char* kind, const std::string& name, const std::string& fullText,
                  uint32_t nowMs, const std::string& jobTag = std::string(),
                  const std::string& ns = std::string());
  // Bounded view: out gets bytes [offset, offset+maxBytes) of the stored text;
  // total returns the full stored size. False when the tag is unknown OR the
  // caller may not read it - an unreadable tag is indistinguishable from a
  // missing one, so the ring cannot be probed for other tenants' tags.
  bool get(const std::string& tag, size_t offset, size_t maxBytes, std::string& out,
           size_t& total, const Principal& who) const;
  // One line per VISIBLE slot (same boundary as get), newest last.
  std::string list(const Principal& who) const;
  size_t count() const { return e_.size(); }
  void clear() { e_.clear(); bytes_ = 0; }

 private:
  std::vector<Entry, WorkingAllocator<Entry>> e_;
  size_t bytes_ = 0;
  uint32_t seq_ = 0;
};

// Device wiring (SessionHandlers pattern): the tools route through these so the
// device can take the memory Lock around the shared instance.
struct ResultHandlers {
  std::function<bool(const std::string& tag, size_t offset, size_t maxBytes,
                     std::string& out, size_t& total, const Principal& who)> get;
  std::function<std::string(const Principal& who)> list;
  // The effective per-tool-result clamp for THIS turn (device: the derived
  // budget). The view must fit INSIDE it once the header is added, or the loop
  // clips the tail off the page while the header still claims the full window.
  // Null => the conservative default below.
  std::function<size_t()> viewCap;
};

// results.get / results.list - registered next to the session tools.
void registerResultTools(ToolRegistry& reg, const ResultHandlers& h);

}  // namespace orch
}  // namespace nimbus
