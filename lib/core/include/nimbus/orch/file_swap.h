#pragma once
#include <string>

// file_swap - replacing a whole file on a card whose rename cannot overwrite (FAT)
// without a moment where the file is gone (host-tested; the device supplies the
// card calls).
//
// The old way removed the stored file and then renamed the new copy over it: a
// failed rename, or a power cut between the two steps, lost the file (a food log
// rewritten by a keyed update was gone). Now the stored copy is first renamed to a
// backup, the new copy is renamed into place, and the backup is removed only once
// the new copy is accepted; any failure on the way puts the old copy back. A note
// file names the file being replaced, so a power cut mid-way is repaired at the
// next boot by recoverSwap().
namespace nimbus::orch {

// The card calls a swap needs. Paths are absolute on the card.
class SwapFs {
 public:
  virtual ~SwapFs() = default;
  virtual bool exists(const std::string& path) = 0;
  virtual bool rename(const std::string& from, const std::string& to) = 0;
  virtual bool remove(const std::string& path) = 0;
  virtual bool writeText(const std::string& path, const std::string& text) = 0;
  virtual std::string readText(const std::string& path) = 0;   // "" = missing
};

// The swap's fixed paths: the finished new copy, the backup, and the note.
struct SwapPaths {
  std::string part, bak, note;
};

// Put `paths.part` in place of `dst`. True: the new copy is in place and the old
// one (if any) waits in the backup until swapDone() or swapUndo(). False: `err`
// says why, the part is removed, and the old copy is where it was (or, if even
// the restore failed, in the backup, which recoverSwap() puts back at boot). An
// earlier swap still pending (its note left by a failed restore) is recovered
// first; while it cannot be, no new swap starts, so its backup is never lost.
bool swapIn(SwapFs& fs, const SwapPaths& paths, const std::string& dst, std::string& err);

// The new copy was accepted: drop the backup and the note.
void swapDone(SwapFs& fs, const SwapPaths& paths);

// The new copy was refused after it landed (the file index said no): remove it
// and put the old copy back (a new file simply goes away).
bool swapUndo(SwapFs& fs, const SwapPaths& paths, const std::string& dst);

// Boot: finish or roll back a swap a power cut interrupted. Returns what it did,
// for the log ("" = nothing to do).
std::string recoverSwap(SwapFs& fs, const SwapPaths& paths);

}  // namespace nimbus::orch
