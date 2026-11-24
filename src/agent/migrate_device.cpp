// The device side of the earlier engine's import (migrate_device.h), lane E2-MIG.
#include "migrate_device.h"

#include <Arduino.h>
#include <LittleFS.h>

#include <memory>
#include <new>
#include <vector>

#include "../sys/agent_log.h"
#include "files_subsystem.h"     // files::saveText - the export of the old summary
#include "fs_atomic.h"           // writeFileAtomic, flashMounted
#include "memory_subsystem.h"    // memory::Lock, memory::episodic
#include "nimbus/harness/turn_memory.h"   // PsramFirstScope
#include "nimbus/orch/compact.h"          // FoldStore
#include "nimbus/orch/rbac.h"
#include "notes_device.h"        // notesdev::notebook
#include "orchestrator.h"        // orchestrator::utcOffsetAt
#include "nimbus/orch/owner_prefs.h"   // kPrefsPath - the earlier engine's rules file
#include "tx_store_device.h"     // txdev::cardTxFiles - is there a transcript yet

namespace agent::migratedev {

namespace tx = nimbus::orch::tx;

namespace {

const char kChatsumPath[] = "/data/chatsum.txt";
const char kRulesMarkerPath[] = "/data/notes/rules_import.txt";
constexpr uint32_t kIndexWaitMs = 10u * 60u * 1000u;   // the longest a first start waits for the index

std::string readFlash(const char* path) {
  if (!flashMounted()) return std::string();
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return std::string();
  String s = f.readString();
  f.close();
  return std::string(s.c_str(), s.length());
}

// A flash file read whole (lane E2-FIX3, findings R5 and R7): false when it cannot be opened or the
// read came back shorter than the file (a refused allocation inside readString). `size` is the
// file's own length, also on false when it was open.
bool readFlashWhole(const char* path, std::string& out, size_t& size) {
  out.clear();
  size = 0;
  if (!flashMounted()) return false;
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return false;
  size = f.size();
  String s = f.readString();
  f.close();
  out.assign(s.c_str(), s.length());
  return out.size() == size;
}

// The fold record, read only: save is never called by a read of it, and does nothing here.
class ReadOnlyFoldIO : public nimbus::orch::FoldStoreIO {
 public:
  std::string load() override { return readFlash(kChatsumPath); }
  void save(const std::string&) override {}
};

std::unique_ptr<tx::EpisodicRowSource> s_rows;   // the last fill's (tg_poll only)

const char kFoldCopyDir[] = "/mem/tx-pre-e2";
const char kFoldCopyPath[] = "/mem/tx-pre-e2/chatsum.txt";

// The earlier engine's fold record as it stood before engine 2 first ran, kept once on the card,
// byte for byte. Never overwritten. Engine 2 no longer writes /data/chatsum.txt (lane E2-FIX2B,
// finding P5); an earlier engine-2 build mirrored its digests into it, and this copy is what
// putBackFoldRecord restores from.
void keepFoldCopy(const std::string& blob) {
  if (blob.empty() || !memory::haveSd()) return;
  memory::Lock lk;
  fs::FS& fs = memory::dataFs();
  if (fs.exists(kFoldCopyPath)) return;
  fs.mkdir("/mem");
  fs.mkdir(kFoldCopyDir);
  File f = fs.open(kFoldCopyPath, FILE_WRITE);
  if (!f) return;
  const size_t w = f.write(reinterpret_cast<const uint8_t*>(blob.data()), blob.size());
  f.close();
  if (w != blob.size()) fs.remove(kFoldCopyPath);   // a short copy is no copy: tried again next boot
}

// The copy above, whole ("" when absent or unreadable).
std::string readFoldCopy() {
  if (!memory::haveSd()) return std::string();
  memory::Lock lk;
  fs::FS& fs = memory::dataFs();
  File f = fs.open(kFoldCopyPath, FILE_READ);
  if (!f) return std::string();
  String s = f.readString();
  const size_t want = f.size();
  f.close();
  return s.length() == want ? std::string(s.c_str(), s.length()) : std::string();
}

// P5: a chat's record that an earlier engine-2 build replaced with its mirror is put back from the
// copy, once (the record then has the earlier engine's shape again). True when it was.
bool putBackFoldRecord(const std::string& chatId, const std::string& blob, size_t fileSize) {
  const std::string copy = readFoldCopy();
  std::string restored;
  if (copy.empty() || !tx::restoreFoldRecordFrom(blob, fileSize, copy, chatId, restored)) return false;
  const bool ok = writeFileAtomic(kChatsumPath, restored, "chatsum");
  alogf("e2: import: the fold record of chat %s was engine 2's mirror: %s from %s", chatId.c_str(),
        ok ? "put back" : "NOT put back", kFoldCopyPath);
  return ok;
}

}  // namespace

bool fill(tx::MigrateDeps& m) {
  s_rows.reset(new (std::nothrow) tx::EpisodicRowSource(
      m.chatKey, [](const nimbus::orch::MsgQuery& q, nimbus::orch::EpiQueryInfo& info) {
        std::vector<nimbus::orch::EpisodicMessage> rows;
        memory::Lock lk;
        rows = memory::episodic().query(q, &info);
        return rows;
      }));
  if (!s_rows) return false;
  {
    ReadOnlyFoldIO io;
    keepFoldCopy(io.load());
    nimbus::orch::FoldStore folds;
    folds.begin(&io);
    m.fold = tx::oldFoldOf(folds.get(m.chatKey));
  }
  m.rows = s_rows.get();
  bool mirrored = false;
  if (tx::dropMirroredPointer(*s_rows, m.fold, mirrored).ok() && mirrored) {
    ReadOnlyFoldIO io;
    std::string blob;
    size_t size = 0;
    // R7: the shared record file is rewritten only from a read of its full length; a short one does nothing.
    if (!readFlashWhole(kChatsumPath, blob, size))
      alogf("e2: import: %s read short (%u of %u B): no put-back", kChatsumPath, (unsigned)blob.size(), (unsigned)size);
    if (putBackFoldRecord(m.chatKey, blob, size)) {   // the earlier engine's own record, read again
      nimbus::orch::FoldStore folds;
      folds.begin(&io);
      m.fold = tx::oldFoldOf(folds.get(m.chatKey));
      mirrored = false;
      (void)tx::dropMirroredPointer(*s_rows, m.fold, mirrored);
    }
    if (mirrored)
      alogf("e2: import: the fold record of chat %s is engine 2's own mirror; its pointer is not used", m.chatKey.c_str());
  }
  m.tzMinutes = (int16_t)orchestrator::utcOffsetAt((uint32_t)time(nullptr));
  m.exportText = [](const std::string& path, const std::string& text) {
    const size_t slash = path.find('/');
    if (slash == std::string::npos) return false;
    std::string err;
    const bool ok = files::saveText(path.substr(0, slash), path.substr(slash + 1), text, err);
    if (!ok) {
      (void)files::noteRefusal(err);   // a full store raises its one owner notice
      alogf("e2: import: the old summary was not exported (%s)", err.c_str());
    }
    return ok;
  };
  return true;
}

bool historyReady(const std::string& chatKey, bool quiet) {
  if (memory::episodicIndexComplete()) return true;
  // An index that stopped (its row cap, read errors) never completes: after kIndexWaitMs the import
  // runs over what the store answers, named in the log.
  if (millis() > kIndexWaitMs) {
    if (!quiet) alogf("e2: import of chat %s runs over an episodic index that is not complete", chatKey.c_str());
    return true;
  }
  bool reentry = false;
  if (!tx::isReentry(txdev::cardTxFiles(), chatKey, reentry).ok()) return true;   // no card: nothing to import
  if (!reentry && !quiet) alogf("e2: import of chat %s waits for the episodic index", chatKey.c_str());
  return reentry;
}

uint32_t lastQueries() { return s_rows ? s_rows->queries() : 0; }

void importRulesOnce() {
  static bool done = false;
  if (done) return;
  done = true;
  if (!flashMounted()) return;
  const uint32_t t0 = millis();
  // R5: the rules from the file itself, read under the store's lock and checked whole; a read that
  // is not whole changes no note (an unopened file is not read at all).
  std::string blob;
  size_t size = 0;
  bool opened = false;
  {
    memory::Lock lk;
    opened = readFlashWhole(nimbus::orch::kPrefsPath, blob, size) || size > 0;
  }
  if (!opened) return;
  const tx::RulesRead read = tx::oldRulesFrom(blob, size);
  if (!read.whole)
    alogf("e2: rules import: %s not read whole (%u of %u B): no rule note changed", nimbus::orch::kPrefsPath,
          (unsigned)blob.size(), (unsigned)size);
  const std::string marker = readFlash(kRulesMarkerPath);
  std::string next;
  tx::RulesImportOutcome out;
  {
    const PsramFirstScope psram;
    const nimbus::orch::Principal owner = nimbus::orch::principalForRole("owner", nimbus::orch::Role::Admin);
    tx::importRules(read, marker, notesdev::notebook(), owner, next, out);
  }
  bool saved = true;
  if (next != marker) {
    LittleFS.mkdir("/data");
    LittleFS.mkdir("/data/notes");
    saved = writeFileAtomic(kRulesMarkerPath, next, "rules_import");
  }
  alogf("e2: rules import: %u rules, added=%u updated=%u kept=%u removed=%u refused=%u marker=%s %u ms%s%s",
        (unsigned)read.rules.size(), (unsigned)out.added, (unsigned)out.updated, (unsigned)out.kept,
        (unsigned)out.removed, (unsigned)out.refused, saved ? "ok" : "NOT saved", (unsigned)(millis() - t0),
        out.firstError.empty() ? "" : " first refusal: ", out.firstError.c_str());
}

#ifdef NIMBUS_TEST
std::string setAside(const std::string& chatIn) {
  std::string chat = chatIn;
  while (!chat.empty() && (chat.back() == ' ' || chat.back() == '\r' || chat.back() == '\n')) chat.pop_back();
  if (!tx::chatKeyOk(chat)) return "TXASIDE ERR bad chat key";
  if (orchestrator::turnInFlight()) return "TXASIDE ERR a turn is running";
  if (!memory::haveSd()) return "TXASIDE ERR no card";
  const std::string from = "/mem/tx/" + chat;
  const std::string to = "/mem/tx-aside/" + chat + "-" + std::to_string((unsigned long)time(nullptr));
  memory::Lock lk;
  fs::FS& fs = memory::dataFs();
  if (!fs.exists(from.c_str())) return "TXASIDE NONE " + from;
  fs.mkdir("/mem/tx-aside");
  const bool ok = fs.rename(from.c_str(), to.c_str());
  return std::string(ok ? "TXASIDE OK " : "TXASIDE ERR rename ") + from + " -> " + to;
}
#endif

}  // namespace agent::migratedev
