#include "nimbus/orch/store_full.h"

// A full file store, said once (see store_full.h). Pure state and strings; no I/O.
namespace nimbus {
namespace orch {

bool StoreFullEpisode::refused(bool full) {
  if (full && !active_.exchange(true)) notice_.store(true);
  return full;
}

void StoreFullEpisode::room() {
  active_.store(false);
  notice_.store(false);
}

bool StoreFullEpisode::takeNotice() { return notice_.exchange(false); }

const char kStoreFullNotice[] =
    "the file store is full, so new photos and files are not saved to it. To make room, "
    "open the device web page, delete files you don't need in the Memory tab's Files "
    "section, then send them again.";

std::string storeFullFact(bool photo, bool inHistory) {
  std::string out =
      "\n[Not saved to the file store: it is full. The device has already sent the owner a "
      "message saying so, with the steps to make room and send it again, so do not explain "
      "it, say how to make room or offer to save it later: at most say in a few words that it "
      "was not saved. Do not ask which files to delete (only the owner can, on the device web "
      "page).";
  if (inHistory) out += std::string(" The ") + (photo ? "photo" : "file") + " itself stays in this chat's history.";
  return out + "]";
}

bool mediaRowLanded(bool captured, bool sdLog, bool lastAppendOk, int heldRows) {
  if (!captured) return false;
  if (!sdLog) return true;
  return lastAppendOk && heldRows == 0;
}

const char kStoreFullToolNote[] =
    " The file store is full. The device tells the owner once, with how to make room; say "
    "in a few words that this was not saved, and do not explain the store or ask which "
    "files to delete.";

}  // namespace orch
}  // namespace nimbus
