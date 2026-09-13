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
  // PRIMARY evidence: a non-empty /mem/vectors.bin is readable off the card (via the
  // read-only solide::storage probe). Needs NO NVS write, so the banner fires even on
  // a full-NVS device where prevSdSeen could not be written.
  bool cardHoldsData = false;
  // SECONDARY best-effort hint: a card was present the previous boot (persisted NVS
  // flag). May be false on a full NVS; the decision must not depend on it alone.
  bool prevSdSeen = false;
};

struct TierDecision {
  bool haveSd = false;            // effective SD tier this boot (== mountedSd)
  bool sdMissingWithData = false; // LOUD banner: no card mounted, but evidence a card holds memories
};

// A card is "missing with data" when it is NOT mounted this boot yet the card is
// physically readable with a non-empty vector blob (primary, NVS-independent) OR the
// previous boot saw a card (secondary hint). Either evidence alone raises the banner,
// so it fires from the probe even when the NVS flag write dropped. Never fires while
// the card is mounted (haveSd true) - the store is live then, nothing to warn about.
inline TierDecision decideStorageTier(const TierInputs& in) {
  TierDecision d;
  d.haveSd = in.mountedSd;
  d.sdMissingWithData = !in.mountedSd && (in.cardHoldsData || in.prevSdSeen);
  return d;
}

}  // namespace memory
}  // namespace agent
