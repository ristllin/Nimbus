#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "nimbus/orch/file_store.h"   // FileStore - the one index the old engine and the web panel share
#include "nimbus/orch/file_swap.h"    // SwapFs, SwapPaths - the old engine's whole-file replace
#include "nimbus/orch/file_tools.h"   // FileToolsIo

// file_card_io - engine 2's file tools over the OLD engine's file store on a card (lane E2-TOOLS):
// the FileToolsIo the device binds (src/agent/files_subsystem.cpp's engine-2 block supplies the SD
// card and its hooks), portable so a host test runs it on a fake card (test_file_card_io).
//
// It adds no crash repair of its own (ruling 112, ruling 114 item 4):
//   - a replace or an edit is the old engine's swap (file_swap.h swapIn then swapDone in one hold)
//     with the old engine's backup and note paths, so a power cut mid-replace is finished or rolled
//     back at boot by the old engine's recoverInterruptedSwap, unchanged;
//   - an append lands at the size ON THE CARD (open for append), as the old engine's appendRows does;
//   - no code here truncates, drops or re-assigns stored bytes because the index is behind. sync()
//     brings a lagging index entry to the size on the card (its hash unknown), as the old engine
//     adopts a longer file, so a read, an edit or a replace sees every stored byte;
//   - a file on the card with no index entry is never written to, appended to or replaced (the call
//     is refused, nothing changed): whose it is cannot be derived from the path on this store.
// A short append (the card took part of the bytes) is undone in the same hold: the file is cut back
// to the size measured in that hold before the call wrote (only this call's own bytes), or removed
// when the call created it; when the card refuses that too, the bytes that stayed are counted for
// the result (lane E2-FSAFE, finding S2). While a web upload is in progress no change starts
// (CardFilesHost::busy, the old engine's gate; S4).
//
// The journal (ruling 112; lane E2-FSAFE, findings S6 and S8): every change notes itself in
// layout.toolsNote (the op, the person, project, name, the size before, an append's length or a
// write's size) in its commit hold before its first byte; the note leaves in the index-rewrite hold
// once the index has landed. recoverNote() at boot, after the old engine's swap recovery and the
// index load, only brings the INDEX to what the card holds for the noted file (ruling 115 item 1,
// lane E2-FIX2A): it never cuts a file back, removes one or re-assigns one. An entry whose file is
// gone is dropped; an entry whose size or hash differs takes the card's size and an unknown hash; a
// file the note says was new, in place with no entry, is adopted under the person the note names
// (never the device owner by default; when the note names nobody readable, or the size does not fit
// the note, the file stays unindexed and untouched); an append the power cut mid-row gets one line
// end so the next row starts cleanly. A partly written row stays on the card. A note whose index
// already agrees is simply cleared. Clearing is remove, else overwrite with kToolsNoteCleared, else
// cut to empty; the reader ignores both, so a note the card would not remove causes no later action.
// Paths are <root>/<project>/<name>.
namespace nimbus::orch {

// The card calls the tools need beyond the swap's. Paths are absolute on the card.
class FileCard : public SwapFs {
 public:
  // At most len bytes from offset into out (replacing it). False on a read fault or a missing file.
  virtual bool readAt(const std::string& path, uint64_t offset, size_t len, std::string& out) = 0;
  // Open for append (create when missing) and write; landed = the bytes the card took.
  virtual bool append(const std::string& path, std::string_view bytes, size_t& landed) = 0;
  // Write bytes to path: first starts it empty, else appends. True when all landed.
  virtual bool write(const std::string& path, std::string_view bytes, bool first) = 0;
  // Make the directory (one level under the root); true when it exists after.
  virtual bool mkdir(const std::string& dir) = 0;
  // Cut the file to `size` bytes (only ever a short append's own bytes, back to the size this hold
  // measured before its write, or the tools' own note to empty); true when it is that size after.
  virtual bool truncate(const std::string& path, uint64_t size) = 0;
};

// What a cleared journal note holds when the card would not remove it: never read as a change.
inline constexpr char kToolsNoteCleared[] = "done\n";

struct CardFilesLayout {
  std::string root;        // "/mem/files"
  std::string toolsPart;   // the tools' own part file, never the web upload's
  SwapPaths swap;          // part = toolsPart; bak and note = the old engine's
  std::string toolsNote;   // the tools' journal note ("" = none)
};

struct CardFilesHost {
  // Run fn under the card lock; false (fn not run) when there is no card.
  std::function<bool(const std::function<void()>&)> locked;
  std::function<bool()> persistIndex;          // the index written; true when it landed
  std::function<uint32_t()> now;               // epoch seconds, 0 unknown
  std::function<int32_t(uint32_t)> utcOffsetMin;   // minutes east of UTC at an instant; null: UTC
  // Room for addBytes at project/name (err says why not); null: the index's own rule.
  std::function<bool(const std::string&, const std::string&, uint64_t, std::string&)> roomFor;
  // The approved body of skill `id` (read_file on skills/<id>.md); null: no skill reads (lane E2-CHAT).
  std::function<bool(const std::string& id, std::string& out)> skillBody;
  std::function<bool()> busy;                  // a web upload holds the store (inside the lock); null: never
};

class CardFilesIo : public FileToolsIo {
 public:
  CardFilesIo(FileCard& card, FileStore& store, CardFilesLayout layout, CardFilesHost host)
      : card_(card), store_(store), layout_(std::move(layout)), host_(std::move(host)) {}

  bool locked(const std::function<void()>& fn) override;
  FileStore& index() override { return store_; }
  bool readAt(const std::string& p, const std::string& n, uint64_t offset, size_t len, std::string& out) override;
  bool append(const std::string& p, const std::string& n, std::string_view bytes) override;
  bool appendCounted(const std::string& p, const std::string& n, std::string_view bytes, uint64_t& landed) override;
  bool writeBusy() override { return host_.busy && host_.busy(); }
  bool journal(const Change& c) override;
  bool partWrite(const std::string& p, const std::string& n, std::string_view bytes, bool first) override;
  bool partSwap(const std::string& p, const std::string& n) override;
  void partDiscard(const std::string& p, const std::string& n) override;
  bool persistIndex() override;
  bool roomFor(const std::string& p, const std::string& n, uint64_t addBytes, std::string& err) override;
  bool remove(const std::string& p, const std::string& n) override;
  void sync(const std::string& p, const std::string& n) override;
  uint32_t now() override { return host_.now ? host_.now() : 0; }
  bool skillBody(const std::string& id, std::string& out) override { return host_.skillBody && host_.skillBody(id, out); }
  std::string timeText(uint32_t epoch) override;

  std::string pathOf(const std::string& p, const std::string& n) const;

  // Boot, before any writer runs and after the swap recovery and the index load: resolve a
  // journal note a power cut left (see above). What it did, for the log ("" = nothing).
  std::string recoverNote();

 private:
  // A replace of this path the old engine's swap left unfinished (its note names it).
  bool swapNoted(const std::string& path);
  // The journal: drop the note (false: the card kept it; the next change settles it first), a
  // note of a change that did not happen, an earlier note settled by an index rewrite that lands.
  bool dropNote();
  bool clearNote();   // removed, else the marker, else empty; false when the card refused all three
  void unnote();
  bool settle();
  // A short append's own bytes undone in its hold; what stayed when the card refused (0: undone).
  uint64_t undoShort(const std::string& path, long long before, size_t got);
  // recoverNote's steps (ruling 115 item 1): a torn append's line end (what it did, "" = nothing),
  // then the index entry brought to the card (true: the index changed). Nothing is cut or removed.
  std::string endTornAppend(const Change& c, const std::string& path);
  bool indexAsCard(const Change& c, const std::string& path, std::string& did);
  bool adoptNoted(const Change& c, long long size, const std::string& path, std::string& did);
  bool hashOnCard(const std::string& path, uint64_t size, uint64_t& hash);

  FileCard& card_;
  FileStore& store_;
  CardFilesLayout layout_;
  CardFilesHost host_;
  bool synced_ = false;   // sync moved an entry inside this hold: the index is written at its end
  bool noted_ = false;    // this change's note is on the card
  bool notePending_ = false;   // an earlier change's note is still on the card (its index rewrite failed)
  char noteOp_ = 0;
};

}  // namespace nimbus::orch
