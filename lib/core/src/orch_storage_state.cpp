#include "nimbus/orch/storage_state.h"

#include <ArduinoJson.h>

namespace nimbus {
namespace orch {

std::string storageStateJson(const StorageState& s) {
  JsonDocument d;
  JsonObject nvs = d["nvs"].to<JsonObject>();
  if (s.nvsStats) nvs["free"] = s.nvsFree;
  else nvs["free"] = nullptr;
  nvs["fails"] = s.saveFails;
  nvs["lastFail"] = s.lastSaveFail.empty() ? nullptr : JsonString(s.lastSaveFail.c_str());
  d["tgOffsetSaved"] = s.tgOffsetSaved;
  d["embedParked"] = s.embedParked;
  d["embedParkedWhy"] = s.embedParkedWhy.empty() ? nullptr : JsonString(s.embedParkedWhy.c_str());
  std::string out;
  serializeJson(d, out);
  return out;
}

}  // namespace orch
}  // namespace nimbus
