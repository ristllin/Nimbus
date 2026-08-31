#pragma once
// Provider-config read cache (CUM-238).
//
// The provider credentials + routing keys (openai/anthropic/mistral/... keys,
// orchHost, the priority lists) are read on hot paths: the screen render
// resolves the active host provider every frame, and the engine re-checks
// provider readiness on every turn / head resolution. Each read went straight to
// NVS via getString(), so a device with UNSET keys - the brand-new / just-reset
// state - spammed hundreds of `nvs_get_str ... NOT_FOUND` reads per second (wear
// + CPU + a console drowned in log noise during exactly the first-run diagnosis
// that matters most).
//
// The fix: snapshot the values once and serve reads from RAM. A provider-config
// WRITE marks the snapshot dirty; the next read refreshes. Read cost becomes
// O(writes), not O(calls) - and the NEGATIVE "still unset" result is cached too,
// so a fresh device stops re-asking NVS whether it is still unconfigured.
//
// Portable + host-tested (no Arduino / no NVS here): the device seam
// (src/agent/store.cpp) supplies the `Fill` that actually reads NVS and wraps
// this in its store mutex; the host test drives it single-threaded and counts
// refreshes to prove reads stay bounded across N loop ticks.

#include <functional>
#include <string>

namespace nimbus {
namespace store {

// The provider-config values hot paths read every tick.
struct ProviderSnapshot {
  std::string openaiKey, anthropicKey, mistralKey, tavilyKey;
  std::string zaiKey, zaiBase, cumuloKey, cumuloBase;
  std::string customBase, customKey, customConv, customModel;
  std::string orchHost, providerPriority, subPriority, fallbackRules;
};

// True iff at least one of the three built-in cloud providers (openai/anthropic/
// mistral) has a key - the "any provider configured" predicate the readiness
// gate polls. Computed from the cached snapshot, so its negative (unset) answer
// is served from RAM without touching NVS.
inline bool anyBuiltinKeyed(const ProviderSnapshot& s) {
  return !s.openaiKey.empty() || !s.anthropicKey.empty() || !s.mistralKey.empty();
}

// A dirty-gated snapshot cache. snapshot() serves from RAM, refreshing via
// `fill` only after markDirty(). Not internally locked: the device wraps it in
// the store mutex; the host test is single-threaded.
class ProviderCache {
 public:
  using Fill = std::function<void(ProviderSnapshot&)>;

  // Serve the current snapshot, refreshing (exactly once) if a write has marked
  // it dirty since the last refresh. `fill` is the only path that touches NVS.
  const ProviderSnapshot& snapshot(const Fill& fill) {
    if (dirty_) {
      fill(snap_);
      dirty_ = false;
    }
    return snap_;
  }

  // A provider-config write happened: the next snapshot() re-reads.
  void markDirty() { dirty_ = true; }

  // True while a refresh is pending (before the first read, and after a write).
  bool dirty() const { return dirty_; }

 private:
  ProviderSnapshot snap_{};
  bool dirty_ = true;  // first read always populates
};

}  // namespace store
}  // namespace nimbus
