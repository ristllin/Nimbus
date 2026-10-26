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

// Both the backup and the file exist after a cut: the file is the new copy. A
// backup LARGER than it is never deleted (the new copy may be a stray write over
// a missing file): it is kept beside it as <name>.recovered (or .recovered2, ...).
std::string keepOrDropBackup(SwapFs& fs, const SwapPaths& p, const std::string& dst, std::string* keptAside) {
  if (fs.size(p.bak) <= fs.size(dst)) {
    fs.remove(p.bak);
    return "kept the new " + dst + ", dropped its backup";
  }
  for (int i = 1; i <= 9; i++) {
    const std::string aside = dst + ".recovered" + (i == 1 ? std::string() : std::to_string(i));
    if (fs.exists(aside)) continue;
    if (!fs.rename(p.bak, aside)) break;
    if (keptAside) *keptAside = aside;
    return "kept the new " + dst + "; its larger backup is kept as " + aside;
  }
  return std::string();   // could not keep it aside: the note stays, nothing is lost
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
  if (fs.exists(p.bak)) {
    fs.remove(p.part);
    err = "SD remove failed - nothing was changed";
    return false;
  }
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
  // Only this swap's own backup (its note names dst) is ever put back.
  const bool mine = fs.readText(p.note) == dst;
  const bool ok = !mine || !fs.exists(p.bak) || fs.rename(p.bak, dst);
  if (ok && mine) fs.remove(p.note);
  return ok;
}

bool swapPendingFor(SwapFs& fs, const SwapPaths& p, const std::string& dst) {
  if (fs.readText(p.note).empty()) return false;
  recoverSwap(fs, p);
  return fs.readText(p.note) == dst;
}

std::string recoverSwap(SwapFs& fs, const SwapPaths& p, std::string* keptAside) {
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
      did = keepOrDropBackup(fs, p, dst, keptAside);
      if (did.empty()) return "could not keep the backup of " + dst + " aside";   // the note stays
    }
  }
  fs.remove(p.note);
  return did;
}

}  // namespace nimbus::orch
