#pragma once
#include <atomic>
#include <string>

// store_full - a full file store is said once (storm leg g, 22 Oct 2026).
//
// With the file store at its entry limit, five owner photos each got a reply that
// explained the full store again and asked which files to delete, while the device
// itself never said a word. Now the device tells the owner once per EPISODE, in plain
// words with the one next step, and the head reads the store state as a fact the owner
// already has, so it answers the photo instead of explaining the store.
//
// An episode starts with the first save the owner did not watch fail (an inbound
// Telegram photo or file, an assistant save) that the store refused because it is
// full (FileStore::fullRefusal), and ends when files are removed and the store has
// room again (the owner made room). A save that lands in between does not end it: a
// store a few bytes under its quota is still full for the next photo, and the owner
// would be told again for every small note. A web upload the store refuses is not
// one: the web page shows that refusal to the owner itself. RAM only (no settings
// key): a restart mid-episode tells the owner once more.
//
// The rule holds for any limit the store enforces (its entry count, its byte quota or
// a later flash tier's space): it keys on the store's own full refusal, never on a
// number. Portable and host-tested (test_store_full).
namespace nimbus {
namespace orch {

class StoreFullEpisode {
 public:
  // A save was refused and `full` says the store is full (FileStore::fullRefusal).
  // Returns `full`; the episode's first full refusal raises its one owner notice.
  bool refused(bool full);
  // Files were removed and the store has room again: the episode ends, and a notice
  // not yet taken is dropped (the owner already made room).
  void room();
  // The episode's one owner notice: true once, then false until the next episode.
  bool takeNotice();
  // True from the episode's first full refusal until the store has room again.
  bool active() const { return active_.load(); }

 private:
  std::atomic<bool> active_{false};
  std::atomic<bool> notice_{false};
};

// The owner notice (device copy, after "<device name>: "): what happened, then the
// one next step.
extern const char kStoreFullNotice[];

// What the head reads after an inbound item the full store did not keep: the store
// state as a fact the owner already has. `photo` names the item ("photo" or "file");
// `inHistory` adds that the item itself is still in the chat's history: the caller
// passes it only when the item's media row reached the card, not just its blob
// (captureMediaFile's result).
std::string storeFullFact(bool photo, bool inHistory);

// Did an inbound item's media row reach the card, so it stays in the chat's history?
// (captureMediaFile's answer, the `inHistory` above.) `captured`: a row was minted;
// `sdLog`: the store is the SD append-log; `lastAppendOk` and `heldRows`: that log's
// state right after the row's write. A refused write holds the row in RAM until the
// card answers (a restart before that loses it), and a held row queues every newer
// one behind it, so a row is on the card only when the write was taken AND nothing is
// held: the last write can be taken while it only moved older held rows. The no-log
// stores keep a captured row as their only copy.
bool mediaRowLanded(bool captured, bool sdLog, bool lastAppendOk, int heldRows);

// Appended to an assistant save the full store refused (artifact.save, files.append,
// files.pdf, image.generate, a helper's saved report, a provider sandbox file), so the
// head says the save did not land and does not explain the store.
extern const char kStoreFullToolNote[];

}  // namespace orch
}  // namespace nimbus
