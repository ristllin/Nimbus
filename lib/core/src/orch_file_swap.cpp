#include "nimbus/orch/file_swap.h"

namespace nimbus::orch {

namespace {
// The new copy could not be renamed into place: put the old one back.
bool restoreOld(SwapFs& fs, const SwapPaths& p, const std::string& dst, std::string& err) {
  fs.remove(p.part);
  if (fs.rename(p.bak, dst)) {
    fs.remove(p.note);
    err = "SD rename failed - the previous copy is kept";
    return false;
  }
  // The note stays: the next boot puts the backup back.
  err = "SD rename failed - the previous copy is restored at the next restart";
  return false;
}
}  // namespace

bool swapIn(SwapFs& fs, const SwapPaths& p, const std::string& dst, std::string& err) {
  // An earlier swap that could not put its old copy back still owns the backup:
  // finish it first, and never start a new swap over it.
  if (!fs.readText(p.note).empty()) {
    recoverSwap(fs, p);
    if (!fs.readText(p.note).empty()) {
      fs.remove(p.part);
      err = "an earlier file replace is not finished - restart the device, then try again";
      return false;
    }
  }
  fs.remove(p.bak);   // a stale backup (a finished swap whose remove failed): never undo into it
  const bool had = fs.exists(dst);
  if (had) {
    if (!fs.writeText(p.note, dst)) {
      fs.remove(p.part);
      err = "SD write failed - the previous copy is kept";
      return false;
    }
    if (!fs.rename(dst, p.bak)) {
      fs.remove(p.note);
      fs.remove(p.part);
      err = "SD rename failed - the previous copy is kept";
      return false;
    }
  }
  if (fs.rename(p.part, dst)) return true;
  if (had) return restoreOld(fs, p, dst, err);
  fs.remove(p.part);
  err = "SD rename failed";
  return false;
}

void swapDone(SwapFs& fs, const SwapPaths& p) {
  fs.remove(p.bak);
  fs.remove(p.note);
}

bool swapUndo(SwapFs& fs, const SwapPaths& p, const std::string& dst) {
  fs.remove(dst);
  const bool ok = !fs.exists(p.bak) || fs.rename(p.bak, dst);
  if (ok) fs.remove(p.note);
  return ok;
}

std::string recoverSwap(SwapFs& fs, const SwapPaths& p) {
  const std::string dst = fs.readText(p.note);
  if (dst.empty()) {
    if (fs.exists(p.bak)) fs.remove(p.bak);
    return std::string();
  }
  std::string did;
  if (fs.exists(p.bak)) {
    if (!fs.exists(dst)) {
      if (!fs.rename(p.bak, dst)) return "could not restore " + dst + " from its backup";   // the note stays
      did = "restored " + dst + " from its backup";
    } else {
      fs.remove(p.bak);
      did = "kept the new " + dst + ", dropped its backup";
    }
  }
  fs.remove(p.note);
  return did;
}

}  // namespace nimbus::orch
