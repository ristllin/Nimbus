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
// A short append (the card took part of the bytes) keeps what landed in a file that existed before
// the call (the index follows at the next sync, as on the old engine); a file the call itself
// created is removed again (only its own bytes). Paths are <root>/<project>/<name>.
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
};

struct CardFilesLayout {
  std::string root;        // "/mem/files"
  std::string toolsPart;   // the tools' own part file, never the web upload's
  SwapPaths swap;          // part = toolsPart; bak and note = the old engine's
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
};

class CardFilesIo : public FileToolsIo {
 public:
  CardFilesIo(FileCard& card, FileStore& store, CardFilesLayout layout, CardFilesHost host)
      : card_(card), store_(store), layout_(std::move(layout)), host_(std::move(host)) {}

  bool locked(const std::function<void()>& fn) override;
  FileStore& index() override { return store_; }
  bool readAt(const std::string& p, const std::string& n, uint64_t offset, size_t len, std::string& out) override;
  bool append(const std::string& p, const std::string& n, std::string_view bytes) override;
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

 private:
  // A replace of this path the old engine's swap left unfinished (its note names it).
  bool swapNoted(const std::string& path);

  FileCard& card_;
  FileStore& store_;
  CardFilesLayout layout_;
  CardFilesHost host_;
  bool synced_ = false;   // sync moved an entry inside this hold: the index is written at its end
};

}  // namespace nimbus::orch
