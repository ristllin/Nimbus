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
  // L19-9 (lane C): the reserve block sits next to the nvs object, so a starved
  // device explains itself the same way a full one does.
  JsonObject nvsReserve = d["nvsReserve"].to<JsonObject>();
  nvsReserve["starved"] = s.nvsReserveStarved;
  nvsReserve["refused"] = s.nvsReserveRefused;
  d["tgOffsetSaved"] = s.tgOffsetSaved;
  d["embedParked"] = s.embedParked;
  d["embedParkedWhy"] = s.embedParkedWhy.empty() ? nullptr : JsonString(s.embedParkedWhy.c_str());
  // L19-9 (lane E): the photo reader's last failure, typed, never a response body.
  if (s.visionErrClass.empty()) {
    d["visionLastErr"] = nullptr;
  } else {
    JsonObject v = d["visionLastErr"].to<JsonObject>();
    v["atS"] = s.visionErrAtS;
    v["provider"] = s.visionErrProvider;
    v["class"] = s.visionErrClass;
    v["status"] = s.visionErrStatus;
  }
  std::string out;
  serializeJson(d, out);
  return out;
}

}  // namespace orch
}  // namespace nimbus
