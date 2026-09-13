#pragma once

// Storage-tier decision (CUM-405). Pure logic, NO Arduino / FS deps, so it is
// unit-tested on the host (test/test_storage_tier) while memory_subsystem.cpp
// feeds it the real device inputs. The problem it exists for: a flaky SD (cold
// joint, cardType=0) that fails to mount on a boot makes begin() read the EMPTY
// LittleFS /data tier while the owner's real memories sit safe on the card. The
// old code showed that empty store with no hint, which reads as data loss. This
// decides when to raise the loud "SD not detected, memories are on the card"
// banner and report sd=absent instead of a silent empty store.

namespace agent {
namespace memory {

struct TierInputs {
  bool mountedSd = false;       // g_haveSd: a card was mounted into the data FS this boot
  bool prevSdSeen = false;      // persisted flag: a card was present the previous boot
  bool cardHoldsData = false;   // best-effort: a non-empty /mem/vectors.bin is readable off the card
};

struct TierDecision {
  bool haveSd = false;            // effective SD tier this boot (== mountedSd)
  bool sdMissingWithData = false; // LOUD banner: no card mounted, but evidence a card holds memories
};

// A card is "missing with data" when it is NOT mounted this boot yet either the
// previous boot saw one OR the card is physically readable with a non-empty
// vector blob. Never fires while the card is mounted (haveSd true) - the store is
// live then, so there is nothing to warn about.
inline TierDecision decideStorageTier(const TierInputs& in) {
  TierDecision d;
  d.haveSd = in.mountedSd;
  d.sdMissingWithData = !in.mountedSd && (in.prevSdSeen || in.cardHoldsData);
  return d;
}

}  // namespace memory
}  // namespace agent
