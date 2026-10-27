#include "food_ledger_device.h"

#include <ArduinoJson.h>
#include <FS.h>

#include <ctime>

#include "files_subsystem.h"    // available / listJson / absPath - the store and its boundary
#include "memory_subsystem.h"   // Lock + dataFs - the card
#include "store.h"              // deviceTz - the owner's local day

namespace agent::foodledger {

namespace {

// Is project/name this person's own file? The store's listing as that namespace
// alone (owner = false, no shares): the owner's turn must not read a ledger someone
// else wrote, whatever its name (the store refuses those names to everyone else too).
bool ownLedger(const std::string& name, const nimbus::orch::Principal& who) {
  const String listed = files::listJson(nimbus::orch::kFoodLedgerProject, who.ns, /*owner=*/false,
                                        /*mayReadShared=*/false);
  JsonDocument d;
  if (deserializeJson(d, listed.c_str()) != DeserializationError::Ok) return false;
  for (JsonObjectConst e : d.as<JsonArrayConst>())
    if (name == (e["name"] | "")) return true;
  return false;
}

}  // namespace

nimbus::orch::FoodLedger today(const std::string& chatId, const nimbus::orch::Principal& who) {
  using namespace nimbus::orch;
  const LogDay day = logDayOf(time(nullptr), store::deviceTz().length() > 0);   // settings: before the lock
  FoodLedger l;
  l.today = day.date;
  l.zone = day.zone;
  l.project = kFoodLedgerProject;
  l.name = foodLedgerName(who.owner, chatId);
  if (l.today.empty() || l.name.empty() || !who.valid() || !files::available() || !ownLedger(l.name, who))
    return l;
  const std::string path = files::absPath(l.project, l.name);
  if (path.empty()) return l;
  l.exists = true;
  FoodLedgerScan scan(l.today);
  RowSplitter rows([&scan](const std::string& row, bool complete) {
    scan.feed(row, complete);
    return true;
  });
  memory::Lock g;
  File f = memory::dataFs().open(path.c_str(), FILE_READ);
  if (!f) {
    l.error = "the card could not open it";
    return l;
  }
  const size_t size = f.size();
  if (size > kKeyedMaxBytes) {
    f.close();
    l.error = "the file is over 1 MB";
    return l;
  }
  const StreamEnd end =
      streamRows(size, [&f](char* d, size_t n) { return (int)f.read(reinterpret_cast<uint8_t*>(d), n); }, rows);
  f.close();
  if (end == StreamEnd::ReadFailed) l.error = "the card read failed";
  else if (end == StreamEnd::Stopped) l.error = "a row is longer than 16 KB";
  else scan.finish(l);
  return l;
}

}  // namespace agent::foodledger
