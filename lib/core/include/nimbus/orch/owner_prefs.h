#pragma once
// owner_prefs - the owner's standing rules: short "always / never" rules and
// standing defaults the owner gave once and wants applied in EVERY later reply,
// on every channel, without a reminder ("never use em dashes", "meat weights I
// give are raw", "my usual drink is a flat white unless I say otherwise").
//
// Why a store of its own: vector memory is recalled by topic, so a rule that
// applies to every topic is almost never recalled (no message is ABOUT it); the
// scratchpad and the conversation window are short-lived. These rules live in one
// small file on the internal flash (/data/prefs.txt, zero NVS keys) and are
// rendered into the never-dropped part of every owner-facing prompt
// ([STANDING RULES], compose.cpp).
//
// File format (line-oriented, tab-separated, tolerant: a bad line is skipped):
//   #nimbus-prefs v1 seeded=<0|1> listed=<0|1> next=<n>
//   p<id> <A|P> <createdHours> <origin> <filters|-> <text>
// A = active (in the prompt), P = pending (saved from a turn that read outside
// content; the owner approves it). Ids are never reused while the file exists.
//
// Portable, Arduino-free, host-tested (test_owner_prefs). The device glue
// (src/agent/prefs_subsystem.cpp) supplies the file IO and the lock.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/rbac.h"   // Role

namespace nimbus {
namespace orch {

inline constexpr const char* kPrefsPath = "/data/prefs.txt";
inline constexpr size_t kPrefsMaxItems = 12;     // active + pending
inline constexpr size_t kPrefItemMax = 240;      // bytes per rule text
inline constexpr size_t kPrefsMaxBytes = 1200;   // all active rule texts together
inline constexpr size_t kPrefsPendingMax = 4;

// Device-enforced output filters a rule can switch on (a closed set: no pattern
// from a model ever becomes a rewrite rule).
enum PrefFilter : uint32_t {
  kPrefFilterNoDash = 1u << 0,   // em dashes removed from delivered replies (no_dash.h)
};

// Where a rule came from. Stored as a slug; an unknown slug reads as "chat".
enum class PrefOrigin : uint8_t { Chat = 0, Web, Command, Seed, Import };
const char* prefOriginSlug(PrefOrigin o);

enum class PrefErr : uint8_t {
  Ok = 0,
  Empty,          // nothing left after cleaning the text
  TooLong,        // over kPrefItemMax bytes
  Full,           // kPrefsMaxItems rules, or the active texts would pass kPrefsMaxBytes
  PendingFull,    // kPrefsPendingMax pending rules already
  NotFound,       // no rule with that id
  NotPending,     // approve on a rule that is already active
  PersistFailed,  // the file could not be written: nothing changed
};
// Plain words for an error, for the tool result, /prefs and the web route.
const char* prefErrWords(PrefErr e);

struct PrefEntry {
  uint32_t    id = 0;             // shown as "p<id>"
  bool        pending = false;
  uint32_t    createdHours = 0;   // device epoch hours (0 = clock not set)
  PrefOrigin  origin = PrefOrigin::Chat;
  uint32_t    filters = 0;        // PrefFilter bits
  std::string text;               // one line, valid UTF-8, <= kPrefItemMax
  std::string idStr() const { return "p" + std::to_string(id); }
};

// The file seam. load() returns false when the file does not exist (a fresh
// device or the first boot after the update: the seed may run); saveChecked()
// returns true only when the whole blob landed.
struct PrefsStoreIO {
  virtual ~PrefsStoreIO() = default;
  virtual bool load(std::string& out) = 0;
  virtual bool saveChecked(const std::string& blob) = 0;
};

// One line, no control characters, collapsed spaces, valid UTF-8. CR, LF, TAB,
// U+2028 and U+2029 become a space, so a rule can never open a new prompt line
// (and so never forge a section header or a block frame). Not length-capped.
std::string sanitizePrefText(const std::string& in);
// Case-folded (ASCII), space-collapsed form: two rules that differ only in case
// or spacing are the same rule (an add of either returns the existing id).
std::string prefKey(const std::string& text);
// The filters a rule's own words ask for: a rule that forbids em dashes switches
// on kPrefFilterNoDash; a rule that allows them does not.
uint32_t filtersForRule(const std::string& text);
// "p3" / "P3" / "3" -> 3; 0 when it is not an id.
uint32_t parsePrefId(const std::string& s);

class OwnerPrefs {
 public:
  // Load and parse the file. A missing or unreadable file is an empty list;
  // fileExisted() says which (the first-boot seed runs only when it did not).
  void begin(PrefsStoreIO* io);
  bool fileExisted() const { return existed_; }

  // Every mutation persists first and changes the list only when the save landed
  // (PersistFailed leaves everything as it was). Re-adding an existing rule
  // returns its id with Ok; a human add of a pending rule approves it.
  PrefErr add(const std::string& text, PrefOrigin origin, uint32_t nowHours, bool pending,
              std::string* idOut);
  PrefErr replace(uint32_t id, const std::string& text, PrefOrigin origin, uint32_t nowHours);
  PrefErr remove(uint32_t id);
  PrefErr approve(uint32_t id);
  PrefErr clear();

  // First-boot seed: rules recovered from memory, added active with origin Seed
  // (up to the caps; texts longer than kPrefItemMax are cut at a word). Marks the
  // file seeded (and owes one listing when anything was added) even when nothing
  // qualified, so the seed never runs again. Returns the ids it added.
  PrefErr seed(const std::vector<std::string>& texts, uint32_t nowHours,
               std::vector<std::string>* idsOut);
  bool seeded() const { return seeded_; }
  // The seed's one listing to the owner is still owed (cleared by markListed()).
  bool listingOwed() const { return seeded_ && !listed_; }
  PrefErr markListed();

  const std::vector<PrefEntry>& entries() const { return items_; }
  const PrefEntry* find(uint32_t id) const;
  size_t activeCount() const;
  size_t pendingCount() const;
  size_t activeBytes() const;

  // The prompt block body: one "- (p<id>) <text>" line per ACTIVE rule, "" when
  // there are none. Whole lines only, never more than kPrefsMaxBytes of rule text
  // (a hand-edited or restored file cannot grow the prompt past the cap).
  std::string renderBlock() const;
  // OR of the active rules' filters.
  uint32_t activeFilters() const;

  std::string serialize() const;
  // Parse a blob into this object (tolerant; caps enforced). Returns the number
  // of rules kept.
  size_t parse(const std::string& blob);

 private:
  PrefErr commit(const std::vector<PrefEntry>& next, bool seeded, bool listed, uint32_t nextId);
  PrefErr checkRoom(const std::vector<PrefEntry>& next) const;
  int indexOf(uint32_t id) const;
  int indexOfKey(const std::string& key) const;

  PrefsStoreIO* io_ = nullptr;
  std::vector<PrefEntry> items_;
  bool existed_ = false;
  bool seeded_ = false;
  bool listed_ = false;
  uint32_t nextId_ = 1;
};

// ---- who sees the rules, and what reads as one -------------------------------

// The rules are the owner's: they ride every turn whose chat resolves to the
// owner (Telegram owner chat, web, voice, serial) whatever started the turn (a
// message, a sub-agent synthesis, a routine, a wakeup, the nightly dream), and no
// member or guest turn (owner decision D5).
bool standingRulesApply(Role role);

// Does a memory read as a standing rule or default the owner gave? Lexical: a
// standing marker ("always", "never", "from now on", "by default", "unless ...
// otherwise", "assume", ...) AND a behavior or logging marker ("reply", "use",
// "write", "call me", "log", "treat", "weights", the device's name, ...).
// "Roy never eats quinoa" is a fact, not a rule; "Roy never wants em dashes in
// replies" is a rule.
bool looksLikeStandingRule(const std::string& text, const std::string& devName = "");

// A memory the first-boot seed may lift into the rules.
struct SeedCandidate {
  std::string text;
  bool        pinned = false;      // a permanent memory (or a long-tier scratchpad goal)
  float       importance = 0.5f;
  uint32_t    createdHours = 0;
};
// The candidates that read as standing rules, best first (pinned, then
// importance, then newest), duplicates removed. The caller seeds them in order.
std::vector<std::string> pickSeedRules(const std::vector<SeedCandidate>& in,
                                       const std::string& devName = "");

// The owner's one listing after the seed: every active rule the seed added
// (plain words, device copy). "" when there is none.
std::string seedListingText(const OwnerPrefs& p);

}  // namespace orch
}  // namespace nimbus
