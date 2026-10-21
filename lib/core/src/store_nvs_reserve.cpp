#include "nimbus/store/nvs_reserve.h"

#include <cstring>

namespace nimbus {
namespace store {

namespace {
// Namespaces. "solide" is solide::memory's (every agent::store key, the mode, profile,
// BLE and name ints, Wi-Fi); "orchmem" the memory subsystem's; "agjournal" the
// sub-agent journal's.
constexpr char kNsSolide[]  = "solide";
constexpr char kNsOrchmem[] = "orchmem";
constexpr char kNsJournal[] = "agjournal";

constexpr NvsClass C = NvsClass::Critical;
constexpr NvsClass O = NvsClass::Owner;
constexpr NvsClass R = NvsClass::Rebuildable;
constexpr NvsClass T = NvsClass::Telemetry;

// One row per key the firmware defines (test_nvs_full fails on a defined key with no
// row). Eviction orders: the verify side channels first (diagnostic text and a number,
// rewritten by the next verify), then a legacy catalog cache, then the dream hash
// (the next dream recomputes it). fbRules is evictable only while it is a cloud copy;
// the device pass checks fbSyncTs before erasing it (an owner's local rule edit is
// owner config and never evicted).
const NvsKeyClass kTable[] = {
    // ---- critical: the device's own safety ----------------------------------
    {kNsSolide, "tgOffset", false, C, 0},     // Telegram ack offset (the replay loop)
    {kNsSolide, "otaPend", false, C, 0},      // OTA rollback guard
    {kNsSolide, "otaBoots", false, C, 0},
    {kNsSolide, "otaPrev", false, C, 0},
    {kNsSolide, "otaLast", false, C, 0},
    {kNsSolide, "otaType", false, C, 0},      // typed-OTA device identity
    {kNsSolide, "otaSimCrash", false, C, 0},  // test-build rollback drill (guard state)
    {kNsSolide, "webTok", false, C, 0},       // the web and MCP sign-in token
    {kNsSolide, "apPass", false, C, 0},       // the setup network passphrase
    {kNsSolide, "cloudDevId", false, C, 0},   // the cloud credential
    {kNsSolide, "cloudCred", false, C, 0},
    {kNsSolide, "cloudHost", false, C, 0},
    {kNsSolide, "embLocked", false, C, 0},    // guards the stored vectors' space
    // ---- owner config: what a person set ------------------------------------
    {kNsSolide, "oaiKey", false, O, 0},
    {kNsSolide, "antKey", false, O, 0},
    {kNsSolide, "mistralKey", false, O, 0},
    {kNsSolide, "tavilyKey", false, O, 0},
    {kNsSolide, "devTz", false, O, 0},
    {kNsSolide, "cloudOptIn", false, O, 0},
    {kNsSolide, "cloudName", false, O, 0},
    {kNsSolide, "custBase", false, O, 0},
    {kNsSolide, "custKey", false, O, 0},
    {kNsSolide, "custConv", false, O, 0},
    {kNsSolide, "custModel", false, O, 0},
    {kNsSolide, "zaiKey", false, O, 0},
    {kNsSolide, "cumuloKey", false, O, 0},
    {kNsSolide, "cumuloBase", false, O, 0},
    {kNsSolide, "fbRules", false, O, 4},      // owner edit, or a cloud copy (evictable then)
    {kNsSolide, "fbSyncTs", false, O, 0},
    {kNsSolide, "orchHost", false, O, 0},
    {kNsSolide, "provPrio", false, O, 0},
    {kNsSolide, "subPrio", false, O, 0},
    {kNsSolide, "sysPrompt", false, O, 0},
    {kNsSolide, "tgToken", false, O, 0},
    {kNsSolide, "tgAllow", false, O, 0},
    {kNsSolide, "tgOwners", false, O, 0},
    {kNsSolide, "tgNames", false, O, 0},
    {kNsSolide, "tgPublic", false, O, 0},
    {kNsSolide, "ttsEnabled", false, O, 0},
    {kNsSolide, "orchLoop", false, O, 0},
    {kNsSolide, "codeSbx", false, O, 0},
    {kNsSolide, "midFail", false, O, 0},
    {kNsSolide, "orchPromptV2", false, O, 0},
    {kNsSolide, "orchLoopRnds", false, O, 0},
    {kNsSolide, "orchLoopDlS", false, O, 0},
    {kNsSolide, "orchLoopRCap", false, O, 0},
    {kNsSolide, "orchLoopTCap", false, O, 0},
    {kNsSolide, "loopMaxCnt", false, O, 0},
    {kNsSolide, "loopMinIvl", false, O, 0},
    {kNsSolide, "loopFires", false, O, 0},
    {kNsSolide, "loopTokens", false, O, 0},
    {kNsSolide, "loopDevTok", false, O, 0},
    {kNsSolide, "loopDevFir", false, O, 0},
    {kNsSolide, "allowHwTest", false, O, 0},
    {kNsSolide, "autoUpd", false, O, 0},
    {kNsSolide, "usbCfm", false, O, 0},
    {kNsSolide, "otrace", false, O, 0},
    {kNsSolide, "onbrded", false, O, 0},
    {kNsSolide, "tlsSlots", false, O, 0},
    {kNsSolide, "tlsVerify", false, O, 0},
    {kNsSolide, "capProbe", false, O, 0},
    {kNsSolide, "fetchPol", false, O, 0},
    {kNsSolide, "modInbound", false, O, 0},
    {kNsSolide, "modOutbound", false, O, 0},
    {kNsSolide, "modInject", false, O, 0},
    {kNsSolide, "capProbeH", false, O, 0},
    {kNsSolide, "sttProv", false, O, 0},
    {kNsSolide, "ttsProv", false, O, 0},
    {kNsSolide, "ttsVoice", false, O, 0},
    {kNsSolide, "theme", false, O, 0},
    {kNsSolide, "scrModel", false, O, 0},
    {kNsSolide, "tchCal", false, O, 0},
    {kNsSolide, "sfxLvlN", false, O, 0},
    {kNsSolide, "sfxLvlO", false, O, 0},
    {kNsSolide, "sfxTheme", false, O, 0},
    {kNsSolide, "sfxVol", false, O, 0},
    {kNsSolide, "saverMin", false, O, 0},
    {kNsSolide, "saverMinSet", false, O, 0},
    {kNsSolide, "sfxLvlNSet", false, O, 0},
    {kNsSolide, "sfxLvlOSet", false, O, 0},
    {kNsSolide, "profSeedInit", false, O, 0},
    {kNsSolide, "compactKB", false, O, 0},
    {kNsSolide, "sleepMv", false, O, 0},
    {kNsSolide, "battRtop", false, O, 0},
    {kNsSolide, "battRbot", false, O, 0},
    {kNsSolide, "battCapMah", false, O, 0},
    {kNsSolide, "battChem", false, O, 0},
    {kNsSolide, "battCells", false, O, 0},
    {kNsSolide, "battCurve", false, O, 0},
    {kNsSolide, "wakeMv", false, O, 0},
    {kNsSolide, "sleepOvr", false, O, 0},
    {kNsSolide, "brightOvr", false, O, 0},
    {kNsSolide, "lbRing", false, O, 0},
    {kNsSolide, "tftFlip", false, O, 0},
    {kNsSolide, "lbSaver", false, O, 0},
    {kNsSolide, "battMon", false, O, 0},
    {kNsSolide, "connectors", false, O, 0},
    {kNsSolide, "agFabric", false, O, 0},
    {kNsSolide, "orchM_", true, O, 0},
    {kNsSolide, "subM_", true, O, 0},
    {kNsSolide, "embProv", false, O, 0},
    {kNsSolide, "embModel", false, O, 0},
    {kNsSolide, "embDims", false, O, 0},
    {kNsSolide, "ledBright", false, O, 0},
    {kNsSolide, "nimbus_mode", false, O, 0},  // src/sys/config_nvs.cpp
    {kNsSolide, "nimbus_prof", false, O, 0},
    {kNsSolide, "nimbus_ble", false, O, 0},
    {kNsSolide, "nimbus_name", false, O, 0},
    {kNsSolide, "staSsid", false, O, 0},      // Wi-Fi (include/nimbus_config.h)
    {kNsSolide, "staPass", false, O, 0},
    {kNsSolide, "staNets", false, O, 0},
    {kNsOrchmem, "memcfg", false, O, 0},      // memory retrieval knobs
    // ---- rebuildable: the device can fetch or derive these again --------------
    {kNsSolide, "vrs_", true, R, 1},          // verify reason (diagnostic text)
    {kNsSolide, "vrm_", true, R, 1},          // verify low-memory number
    {kNsSolide, "mcat_", true, R, 2},         // legacy catalog cache (moved to flash)
    {kNsSolide, "dreamScrHash", false, R, 3},
    {kNsSolide, "vfy_", true, R, 0},          // verify verdicts (the next verify)
    {kNsSolide, "mch_", true, R, 0},          // model choices: no file store yet, kept
    {kNsSolide, "antEnv", false, R, 0},       // managed-agent ids: evicting orphans the
    {kNsSolide, "antAgents", false, R, 0},    //   remote agents (they keep costing), kept
    {kNsSolide, "antOrchAg", false, R, 0},
    {kNsSolide, "zaiBase", false, R, 0},      // the probed Z.ai host
    {kNsSolide, "tgBotName", false, R, 0},    // the bot's @name, from getMe
    // ---- telemetry: runtime state and counters --------------------------------
    {kNsSolide, "orchConv", false, T, 0},
    {kNsSolide, "otaNotif", false, T, 0},
    {kNsSolide, "otaNotes", false, T, 0},     // release notes across the install restart
    {kNsSolide, "battModel", false, T, 0},
    {kNsSolide, "lbPingEp", false, T, 0},
    {kNsSolide, "usgLedger", false, T, 0},    // legacy copy: the ledger lives in a file now
    {kNsOrchmem, "scratch", false, T, 0},     // legacy copies: the scratchpad lives in a
    {kNsOrchmem, "scratchB", false, T, 0},    //   file now
    {kNsOrchmem, "sdSeen", false, T, 0},
    {kNsJournal, "j", true, T, 0},            // sub-agent journal records
};
constexpr size_t kTableCount = sizeof(kTable) / sizeof(kTable[0]);

bool rowMatches(const NvsKeyClass& row, const char* ns, const char* key) {
  if (std::strcmp(row.ns, ns) != 0) return false;
  if (!row.prefix) return std::strcmp(row.key, key) == 0;
  return std::strncmp(row.key, key, std::strlen(row.key)) == 0;
}
}  // namespace

const char* nvsClassName(NvsClass c) {
  switch (c) {
    case NvsClass::Critical:    return "critical";
    case NvsClass::Owner:       return "owner";
    case NvsClass::Rebuildable: return "rebuildable";
    case NvsClass::Telemetry:   return "telemetry";
  }
  return "owner";
}

const NvsKeyClass* nvsKeyTable(size_t* count) {
  if (count) *count = kTableCount;
  return kTable;
}

const NvsKeyClass* findNvsKeyClass(const char* ns, const char* key) {
  if (!ns || !key) return nullptr;
  // An exact row wins over a prefix row, so a fixed key never hides behind a family.
  const NvsKeyClass* pre = nullptr;
  for (const NvsKeyClass& row : kTable) {
    if (!rowMatches(row, ns, key)) continue;
    if (!row.prefix) return &row;
    if (!pre) pre = &row;
  }
  return pre;
}

NvsClass classifyNvsKey(const char* ns, const char* key) {
  const NvsKeyClass* row = findNvsKeyClass(ns, key);
  return row ? row->cls : NvsClass::Owner;
}

size_t nvsValueEntries(NvsValKind kind, size_t bytes) {
  constexpr size_t kEntry = 32;
  switch (kind) {
    case NvsValKind::Int:  return 1;
    case NvsValKind::Str:  return 1 + (bytes + 1 + kEntry - 1) / kEntry;   // + the NUL
    case NvsValKind::Blob: return 2 + (bytes + kEntry - 1) / kEntry;       // index + header
  }
  return 1;
}

size_t nvsBlobWriteSpan(size_t bytes) {
  constexpr size_t kEntry = 32;
  constexpr size_t kPageEntries = 126;                       // entries per NVS page
  constexpr size_t kChunkMax = kEntry * (kPageEntries - 1);  // 4,000 B: one chunk per page
  // A page tail under kChunkMax / 10 that cannot take the value is skipped: it holds
  // (tail / 32) + 1 entries (the IDF keeps one back), so at most 13.
  constexpr size_t kSkippedTail = (kChunkMax / 10 - 1) / kEntry + 1;
  const size_t data = bytes ? (bytes + kEntry - 1) / kEntry : 1;   // an empty blob: one chunk
  const size_t headers = (data + kPageEntries - 2) / (kPageEntries - 1);
  const size_t skipped = data < kSkippedTail ? data : kSkippedTail;
  return data + headers + 1 + skipped;   // + the index entry
}

NvsAdmit nvsAdmit(const NvsAdmitIn& in) {
  if (in.erase) return NvsAdmit::Write;
  // A blob, new or rewrite, must fit whole before the IDF starts writing it.
  if (in.blobSpan && in.available < in.blobSpan) return NvsAdmit::RefuseNoRoom;
  if (nvsWriteShrinks(false, in.newEntries, in.oldEntries)) return NvsAdmit::Write;
  // The IDF writes the new value before it erases the old one: with fewer available
  // entries than the value takes, nothing can hold it, whatever its class.
  if (in.available < in.newEntries) return NvsAdmit::RefuseNoRoom;
  if (in.cls == NvsClass::Critical) return NvsAdmit::Write;
  if (in.cls == NvsClass::Owner && in.starved) return NvsAdmit::Write;
  // NVS writes the new value before it erases the old one, but the net cost of a
  // growing rewrite is only the growth: that is what must leave the reserve intact.
  const size_t growth = in.newEntries - in.oldEntries;   // > 0 here (not a shrink)
  if (in.available >= growth && in.available - growth >= kNvsReserveEntries)
    return NvsAdmit::Write;
  return NvsAdmit::RefuseReserve;
}

size_t nvsEvictionPlan(const char* const* keys, size_t n, bool fbRulesIsCloudCopy,
                       size_t* out, size_t outCap) {
  size_t m = 0;
  // A stable selection by order: at most a handful of distinct orders, so one pass
  // per order keeps this allocation-free (it runs on the main loop of a full device).
  for (uint8_t order = 1; order != 0 && m < outCap; ++order) {
    bool any = false;
    for (size_t i = 0; i < n && m < outCap; ++i) {
      const NvsKeyClass* row = findNvsKeyClass(kNsSolide, keys[i]);
      if (!row || row->evictOrder == 0) continue;
      if (row->evictOrder > order) any = true;
      if (row->evictOrder != order) continue;
      if (std::strcmp(keys[i], "fbRules") == 0 && !fbRulesIsCloudCopy) continue;
      out[m++] = i;
    }
    if (!any) break;   // no higher order left to visit
  }
  return m;
}

}  // namespace store
}  // namespace nimbus
