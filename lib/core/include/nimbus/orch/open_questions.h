#pragma once
// open_questions - the owner's open items: follow-ups the owner left open in
// conversation ("photograph the skyr pot label next time"). The 2 Nov owner
// signals (owner-signals-2026-11-02.md): one open item was re-raised in at least
// nine replies across a day, because nothing on the device held it - every turn
// re-derived it from vector memory. This store is that device-side list: small,
// bounded, rendered into every owner-facing prompt, and each item is raised to
// the owner at most once unless they return to it.
//
// File format (line-oriented, tab-separated, tolerant: a bad line is skipped):
//   #nimbus-openq v1 next=<n>
//   q<id> <R|-> <createdHours> <text>
// R = raised (the owner was already asked about it once; the prompt says not to
// raise it again unless the owner returns to it). Ids are never reused while the
// file exists.
//
// The raise-once bookkeeping is deterministic and device-side (observeOpenQ):
// an item is marked raised when a reply mentions its subject, re-opened when
// the owner's own message mentions it, and removed when the owner's message
// pairs a drop cue ("forget ...", "stop asking about ...") with the subject.
// The model never has to remember what it already asked.
//
// Portable, Arduino-free, host-tested (test_open_questions). The device glue
// (src/agent/prefs_subsystem.cpp) and nimbusd (rig.h) supply the file IO.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/owner_prefs.h"   // PrefsStoreIO - the same file seam

namespace nimbus {
namespace orch {

inline constexpr const char* kOpenQPath = "/data/openq.txt";
inline constexpr size_t kOpenQMaxItems = 5;      // the list stays small on purpose
inline constexpr size_t kOpenQItemMax = 160;     // bytes per item text
// The file at its fullest (5 items at 160 bytes with their fields).
inline constexpr size_t kOpenQFileMax = 2048;

struct OpenQEntry {
  uint32_t    id = 0;             // shown as "q<id>"
  bool        raised = false;    // the owner was asked about it once already
  uint32_t    createdHours = 0;  // device epoch hours (0 = clock not set)
  std::string text;              // one line, valid UTF-8, <= kOpenQItemMax
  std::string idStr() const { return "q" + std::to_string(id); }
};

enum class OpenQErr : uint8_t {
  Ok = 0,
  Empty,          // nothing left after cleaning the text
  TooLong,        // over kOpenQItemMax bytes
  Full,           // kOpenQMaxItems items already
  NotFound,       // no item with that id
  PersistFailed,  // the file could not be written: nothing changed
};
const char* openQErrWords(OpenQErr e);

class OpenQuestions {
 public:
  void begin(PrefsStoreIO* io);
  bool fileExisted() const { return existed_; }

  // Every mutation persists first and changes the list only when the save
  // landed. Re-adding an existing item returns its id with Ok.
  OpenQErr add(const std::string& text, uint32_t nowHours, std::string* idOut);
  OpenQErr remove(uint32_t id);
  OpenQErr clear();
  // Replace the whole list in one persisted write (ids and raised flags as
  // given). observeOpenQ uses it; nothing else should.
  OpenQErr replaceAll(const std::vector<OpenQEntry>& next);

  const std::vector<OpenQEntry>& entries() const { return items_; }
  const OpenQEntry* find(uint32_t id) const;
  size_t count() const { return items_.size(); }

  // The prompt block body: one "- (q<id>) <text>" line per item, a raised one
  // tagged so the model does not raise it again. "" when there are none.
  std::string renderBlock() const;

  std::string serialize() const;
  size_t parse(const std::string& blob);

 private:
  OpenQErr commit(const std::vector<OpenQEntry>& next, uint32_t nextId);
  int indexOf(uint32_t id) const;
  int indexOfKey(const std::string& key) const;

  PrefsStoreIO* io_ = nullptr;
  std::vector<OpenQEntry> items_;
  bool existed_ = false;
  uint32_t nextId_ = 1;
};

// ---- the deterministic raise-once observation ---------------------------------

// The subject words of an item: the shared subject-word helper (word_match.h),
// lowercase ASCII tokens of 3+ bytes with the stop words (about, next, time,
// need, ...) left out. Host-tested.
std::vector<std::string> openQKeywords(const std::string& text);

// Does `other` (a reply, or the owner's message) mention this item's subject?
// True when at least half the subject words (rounded up, at least one) appear.
bool openQMentions(const std::string& itemText, const std::string& other);

// The owner's message pairs a drop cue with the item's subject: the item is
// resolved or withdrawn ("Forget the skyr label", "stop asking about Duolingo").
bool openQOwnerDrops(const std::string& itemText, const std::string& ownerText);

// One completed owner-facing turn: first the owner's message (a mention re-opens
// a raised item, a drop cue removes it), then the reply (a mention marks it
// raised). Returns a plain log line of what changed, "" when nothing did.
std::string observeOpenQ(OpenQuestions& q, const std::string& ownerText,
                         const std::string& reply);

}  // namespace orch
}  // namespace nimbus
