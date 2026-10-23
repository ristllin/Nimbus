#pragma once
#include <cstdint>
#include <string>

// storage_state - the settings-storage and Telegram-offset block of device.status
// (L19-9). On 19 Oct the head had no storage state to cite and blamed the owner's
// Telegram app for repeats; the getters exist (lane A) but reached only /api/state.
//
// The getters live on the device (config_nvs / telegram); this portable builder
// turns their values into the device.status block so host tests can pin the JSON
// shape on a fresh device and on a full settings partition (the NVSFILL-0 shape).
// Every key is always present; `free` and `lastFail` are null when unknown/none,
// and `lastFail` is a key name only, never a value.
namespace nimbus {
namespace orch {

struct StorageState {
  bool        nvsStats = false;      // the partition usage query answered
  uint32_t    nvsFree = 0;           // free bytes in the settings partition
  uint32_t    saveFails = 0;         // writes that did not land since boot
  std::string lastSaveFail;          // the most recent failing key ("" = none)
  bool        tgOffsetSaved = true;  // the stored Telegram offset cursor is current
  // L19-9 (lane P): embeddings are parked - new memories wait in the queue on
  // the device instead of being stored (the short reason /api/mem/stats shows;
  // "" = not parked). The head cites it instead of claiming memory is working.
  bool        embedParked = false;
  std::string embedParkedWhy;        // "" = not parked; a secret-safe cause slug
  // L19-9 (lane C): the NVS reserve. `starved` means the partition is so full
  // that background saves are refused to keep owner settings landing (the same
  // flag /api/state reports as nvsReserve.starved); `refused` counts the writes
  // the reserve turned away since boot. The head cites it to explain a device
  // that is out of settings-storage room instead of guessing.
  bool        nvsReserveStarved = false;
  uint32_t    nvsReserveRefused = 0;
  // L19-9 (lane E): the photo reader's last failure since boot - when (uptime
  // seconds), which provider, the typed class ("billing", "network", ...) and the
  // HTTP status; never the response body. visionErrClass "" = none (null).
  uint32_t    visionErrAtS = 0;
  std::string visionErrProvider;
  std::string visionErrClass;
  int         visionErrStatus = 0;
};

// "nvs":{"free":..,"fails":..,"lastFail":..},"nvsReserve":{"starved":..,"refused":..},
// "tgOffsetSaved":..,"embedParked":..,"embedParkedWhy":..(null when not parked),
// "visionLastErr":{"atS":..,"provider":..,"class":..,"status":..} (null when none)
std::string storageStateJson(const StorageState& s);

}  // namespace orch
}  // namespace nimbus
