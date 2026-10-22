#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "nimbus/orch/budget.h"
#include "nimbus/orch/compact.h"
#include "nimbus/orch/mem_config.h"
#include "nimbus/orch/wire_limits.h"
#include "nimbus/orch/journal.h"
#include "nimbus/orch/usage_ledger.h"
#include "nimbus/store/moved_store.h"
#include "nimbus/store/nvs_reserve.h"
#include "nimbus/store/persist.h"

#include <cstring>

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// ---- the anchor invariant ----------------------------------------------------
// deriveBudget(200000, {}) MUST reproduce today's shipped constants exactly -
// this is what makes the derive change behavior-identical on the current fleet
// (Anthropic 200K head) until a different window is configured. If this test
// changes, the fleet's effective limits changed: mean it.
static void test_anchor_identity_at_200k() {
  ContextBudget b = deriveBudget(200000, {});
  TEST_ASSERT_EQUAL_INT(32768, b.systemPromptBytes);
  TEST_ASSERT_EQUAL_UINT32(4096, (uint32_t)b.chatSummaryBytes);
  // CUM-485: the recent window is a quarter of the system prompt now (was a
  // 3000 B dead knob - the device used a fixed constant that cut code replies).
  TEST_ASSERT_EQUAL_UINT32(8192, (uint32_t)b.recentConvBytes);
  TEST_ASSERT_EQUAL_UINT32(8192, (uint32_t)b.toolResultBytes);
  TEST_ASSERT_EQUAL_UINT32(65536, (uint32_t)b.toolTotalBytes);
  TEST_ASSERT_EQUAL_UINT32(1200, (uint32_t)b.briefBytes);
  TEST_ASSERT_EQUAL_UINT32(65536, (uint32_t)b.foldSliceBytes);
  TEST_ASSERT_EQUAL_UINT32(200000, b.ctxTokens);
}

// ---- floors bind on a small window -------------------------------------------
static void test_floors_bind_on_8k_window() {
  ContextBudget b = deriveBudget(8000, {});
  // 8K tokens = 32000 B window; every scaled value lands below its floor.
  TEST_ASSERT_EQUAL_INT(8192, b.systemPromptBytes);
  TEST_ASSERT_EQUAL_UINT32(2048, (uint32_t)b.chatSummaryBytes);
  TEST_ASSERT_EQUAL_UINT32(3000, (uint32_t)b.recentConvBytes);   // floor = the old shipped total
  TEST_ASSERT_EQUAL_UINT32(512, (uint32_t)b.toolResultBytes);
  TEST_ASSERT_EQUAL_UINT32(8192, (uint32_t)b.toolTotalBytes);
  TEST_ASSERT_EQUAL_UINT32(600, (uint32_t)b.briefBytes);
  TEST_ASSERT_EQUAL_UINT32(16384, (uint32_t)b.foldSliceBytes);
}

// ---- caps bind on a huge window ----------------------------------------------
static void test_caps_bind_on_1m_window() {
  ContextBudget b = deriveBudget(1000000, {});
  TEST_ASSERT_EQUAL_INT(MemConfig::kContextMax, b.systemPromptBytes);  // 56000, under the string guard
  TEST_ASSERT_EQUAL_UINT32(8192, (uint32_t)b.chatSummaryBytes);
  TEST_ASSERT_EQUAL_UINT32(16384, (uint32_t)b.recentConvBytes);
  TEST_ASSERT_EQUAL_UINT32(40960, (uint32_t)b.toolResultBytes);   // 5x anchor, under cap
  TEST_ASSERT_EQUAL_UINT32(327680, (uint32_t)b.toolTotalBytes);   // 5x anchor, under cap
  TEST_ASSERT_EQUAL_UINT32(4096, (uint32_t)b.briefBytes);
  TEST_ASSERT_EQUAL_UINT32(131072, (uint32_t)b.foldSliceBytes);
}

// ---- overrides win verbatim (then clamp) -------------------------------------
static void test_overrides_win_and_clamp() {
  BudgetOverrides ov;
  ov.maxContextBytes = 20000;
  ov.toolResultCap = 2048;
  ov.toolTotalCap = 4096;
  ContextBudget b = deriveBudget(200000, ov);
  TEST_ASSERT_EQUAL_INT(20000, b.systemPromptBytes);
  TEST_ASSERT_EQUAL_UINT32(2048, (uint32_t)b.toolResultBytes);
  TEST_ASSERT_EQUAL_UINT32(4096, (uint32_t)b.toolTotalBytes);
  // Out-of-range overrides clamp to the SAME ranges the NVS setters enforce.
  ov.maxContextBytes = 1 << 20;
  ov.toolResultCap = 1 << 20;
  ov.toolTotalCap = 1 << 24;
  b = deriveBudget(200000, ov);
  TEST_ASSERT_EQUAL_INT(MemConfig::kContextMax, b.systemPromptBytes);
  // The per-result clamp tops out under the provider string guard (wire_limits.h).
  TEST_ASSERT_EQUAL_UINT32(kToolResultCapMax, (uint32_t)b.toolResultBytes);
  TEST_ASSERT_EQUAL_UINT32(1048576, (uint32_t)b.toolTotalBytes);
}

// ---- zero window falls back to the conservative default ----------------------
static void test_zero_window_uses_default() {
  ContextBudget b = deriveBudget(0, {});
  TEST_ASSERT_EQUAL_UINT32(kCtxDefaultTokens, b.ctxTokens);
  // 100K default = half the anchor: scaled values are half, floors bind where lower.
  TEST_ASSERT_EQUAL_INT(16384, b.systemPromptBytes);
  TEST_ASSERT_EQUAL_UINT32(4096, (uint32_t)b.toolResultBytes);
  TEST_ASSERT_EQUAL_UINT32(32768, (uint32_t)b.toolTotalBytes);
}

// ---- monotonicity: a bigger window never shrinks an allocation ---------------
static void test_monotonic_in_window() {
  uint32_t windows[] = {8000, 32000, 100000, 128000, 200000, 272000, 1000000};
  ContextBudget prev = deriveBudget(windows[0], {});
  for (size_t i = 1; i < sizeof(windows) / sizeof(windows[0]); i++) {
    ContextBudget cur = deriveBudget(windows[i], {});
    TEST_ASSERT_TRUE(cur.systemPromptBytes >= prev.systemPromptBytes);
    TEST_ASSERT_TRUE(cur.chatSummaryBytes >= prev.chatSummaryBytes);
    TEST_ASSERT_TRUE(cur.recentConvBytes >= prev.recentConvBytes);
    TEST_ASSERT_TRUE(cur.toolResultBytes >= prev.toolResultBytes);
    TEST_ASSERT_TRUE(cur.toolTotalBytes >= prev.toolTotalBytes);
    TEST_ASSERT_TRUE(cur.briefBytes >= prev.briefBytes);
    TEST_ASSERT_TRUE(cur.foldSliceBytes >= prev.foldSliceBytes);
    prev = cur;
  }
}

// ---- Mistral 128K sanity: derived values sit between floor and anchor --------
static void test_mistral_128k_derivation() {
  ContextBudget b = deriveBudget(128000, {});
  // 128K/200K = 0.64x the anchor.
  TEST_ASSERT_EQUAL_INT(20971, b.systemPromptBytes);
  TEST_ASSERT_EQUAL_UINT32(5242, (uint32_t)b.toolResultBytes);
  TEST_ASSERT_EQUAL_UINT32(41943, (uint32_t)b.toolTotalBytes);
}

// ---- gate W2 #16: every per-turn size follows the head the engine runs --------
// The window, the sub-agent brief and the fold slice each derived their bytes in
// src/agent/orchestrator.cpp, and only the window used the engine's head
// (headHost, router fallback included); the brief and the fold kept the store's
// guess. Static guard over the device source (code lines, comments stripped):
// exactly ONE deriveBudget call and ONE store::resolvedOrchHost call, both in the
// shared headBudget() helper, so a new size site cannot drift to another head.
static std::string readFile(const char* path) {
  FILE* f = std::fopen(path, "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
  std::string all;
  char buf[4096];
  for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) all.append(buf, n);
  std::fclose(f);
  return all;
}
// Each line cut at its first "//" (comments carry the helper's name too).
static std::string stripLineComments(const std::string& src) {
  std::string out;
  for (size_t s = 0; s < src.size();) {
    size_t e = src.find('\n', s);
    if (e == std::string::npos) e = src.size();
    std::string line = src.substr(s, e - s);
    const size_t c = line.find("//");
    if (c != std::string::npos) line.erase(c);
    out += line + "\n";
    s = e + 1;
  }
  return out;
}
static int countOf(const std::string& hay, const std::string& needle) {
  int n = 0;
  for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) n++;
  return n;
}
static void test_orchestrator_sizes_off_one_head_budget() {
  const std::string src = stripLineComments(readFile("src/agent/orchestrator.cpp"));
  TEST_ASSERT_EQUAL_MESSAGE(1, countOf(src, "deriveBudget("), "a second budget derivation in orchestrator.cpp");
  TEST_ASSERT_EQUAL_MESSAGE(1, countOf(src, "resolvedOrchHost()"), "a size sited off the store's head guess");
  const size_t helper = src.find("ContextBudget headBudget()");
  TEST_ASSERT_TRUE(helper != std::string::npos);
  const size_t body = src.find("\n}\n", helper);
  const std::string fn = src.substr(helper, body - helper);
  TEST_ASSERT_TRUE(fn.find("g_engine->headHost()") != std::string::npos);
  TEST_ASSERT_TRUE(fn.find("resolvedOrchHost()") != std::string::npos);
  TEST_ASSERT_TRUE(fn.find("deriveBudget(") != std::string::npos);
  for (const char* use : {"headBudget().briefBytes", "headBudget().recentConvBytes", "headBudget().foldSliceBytes"})
    TEST_ASSERT_EQUAL_MESSAGE(1, countOf(src, use), use);
}

// ---- the fold input is ONE provider string ------------------------------------
// The fold sends summary + digest as one user string; at the 200K anchor the
// digest budget alone (65536) passed ArduinoJson's 65,535-byte ceiling, which
// stores the string as null (a 400). foldDigestCap bounds the digest so the whole
// input stays under the guard for every summary size the store can hold.
static void test_fold_input_fits_the_string_guard() {
  for (size_t sum : {(size_t)0, (size_t)1, (size_t)2048, kChatSummaryMax}) {
    const std::string summary(sum, 's');
    const ContextBudget b = deriveBudget(1000000, {});   // the largest slice there is
    const size_t cap = std::min(b.foldSliceBytes, foldDigestCap(summary.size()));
    const std::string in = buildCompactInputs(summary, std::string(cap, 'd'));
    TEST_ASSERT_TRUE(in.size() <= kJsonStrGuard);
  }
  // Counter-test: the unbounded anchor slice next to a full summary is over.
  const ContextBudget anchor = deriveBudget(200000, {});
  TEST_ASSERT_TRUE(buildCompactInputs(std::string(kChatSummaryMax, 's'),
                                      std::string(anchor.foldSliceBytes, 'd')).size() > kJsonStringCeiling);
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)foldDigestCap(kJsonStrGuard));
}

// ---- the settings-storage (NVS) budget model (C2) ------------------------------------
// The plan's budget-model test (MASTER-PLAN lane C, critique A8: the v4.5.14 layout
// could not pass it): a provisioned device plus every runtime writer at its worst case
// - six journal records, a maximum-size usage ledger, a maximum-size scratchpad, the
// Telegram names at their cap, the battery model, the OTA guard and notes - still
// leaves the reserve free. The geometry is the real partition: 5 pages of 126 entries,
// one page held back for garbage collection (504 usable entries).
namespace nvsbudget {
using nimbus::store::NvsClass;
using nimbus::store::NvsValKind;

constexpr size_t kUsable = 504;

struct Val { const char* ns; const char* key; NvsValKind kind; size_t bytes; };

// A provisioned desk device (sizes modeled on a real one in October 2026; the IDF's
// own keys are the Wi-Fi radio calibration and station cache and the Bluetooth bonds).
const Val kProvisioned[] = {
    {"phy", "cal_data", NvsValKind::Blob, 1904},   {"nvs.net80211", "sta.apinfo", NvsValKind::Blob, 700},
    {"nvs.net80211", "sta.pswd", NvsValKind::Blob, 65}, {"nvs.net80211", "sta.ssid", NvsValKind::Blob, 36},
    {"nimble_bond", "our_sec_1", NvsValKind::Blob, 88}, {"nimble_bond", "peer_sec_1", NvsValKind::Blob, 88},
    {"nimble_bond", "local_irk_1", NvsValKind::Blob, 23}, {"nimble_bond", "rpa_rec_1", NvsValKind::Blob, 14},
    {"nimble_bond", "cccd_sec_1", NvsValKind::Blob, 16}, {"nimble_bond", "cccd_sec_2", NvsValKind::Blob, 16},
    {"solide", "sysPrompt", NvsValKind::Str, 792},  {"solide", "connectors", NvsValKind::Str, 560},
    {"solide", "mch_cumulo", NvsValKind::Str, 480}, {"solide", "mch_anthropic", NvsValKind::Str, 300},
    {"solide", "mch_openai", NvsValKind::Str, 300}, {"solide", "mch_zai", NvsValKind::Str, 300},
    {"solide", "mch_mistral", NvsValKind::Str, 300}, {"solide", "cloudCred", NvsValKind::Str, 198},
    {"solide", "oaiKey", NvsValKind::Str, 165},     {"solide", "staNets", NvsValKind::Str, 129},
    {"solide", "antKey", NvsValKind::Str, 109},     {"solide", "battCurve", NvsValKind::Str, 86},
    {"solide", "cumuloKey", NvsValKind::Str, 59},   {"solide", "tgToken", NvsValKind::Str, 47},
    {"solide", "cloudDevId", NvsValKind::Str, 37},  {"solide", "mistralKey", NvsValKind::Str, 33},
    {"solide", "zaiKey", NvsValKind::Str, 49},      {"solide", "webTok", NvsValKind::Str, 24},
    {"solide", "apPass", NvsValKind::Str, 10},      {"solide", "provPrio", NvsValKind::Str, 30},
    {"solide", "tgAllow", NvsValKind::Str, 40},     {"solide", "tgOwners", NvsValKind::Str, 20},
    {"solide", "devTz", NvsValKind::Str, 30},       {"solide", "tchCal", NvsValKind::Str, 16},
    {"solide", "otaType", NvsValKind::Str, 10},     {"solide", "theme", NvsValKind::Str, 8},
};
constexpr size_t kProvisionedInts = 60;   // modes, levels, flags, radio settings (one entry each)

// Every runtime writer at its worst case. A key that moved to a flash file (listed in
// nimbus::store::kMovedStores) costs nothing here: that is the point of the moves.
const Val kRuntime[] = {
    {"agjournal", "j0", NvsValKind::Str, nimbus::orch::kJournalSerializeBuf - 1},
    {"agjournal", "j1", NvsValKind::Str, nimbus::orch::kJournalSerializeBuf - 1},
    {"agjournal", "j2", NvsValKind::Str, nimbus::orch::kJournalSerializeBuf - 1},
    {"agjournal", "j3", NvsValKind::Str, nimbus::orch::kJournalSerializeBuf - 1},
    {"agjournal", "j4", NvsValKind::Str, nimbus::orch::kJournalSerializeBuf - 1},
    {"agjournal", "j5", NvsValKind::Str, nimbus::orch::kJournalSerializeBuf - 1},
    {"solide", "usgLedger", NvsValKind::Str, nimbus::orch::UsageLedger::kSerializedMaxBytes},
    {"orchmem", "scratchB", NvsValKind::Blob, 4200},
    {"solide", "antAgents", NvsValKind::Str, 600},
    {"solide", "antEnv", NvsValKind::Str, 40},
    {"solide", "tgNames", NvsValKind::Str, nimbus::store::kTgNamesMaxBytes},
    {"solide", "battModel", NvsValKind::Str, 87},
    {"solide", "orchConv", NvsValKind::Str, 700},
    {"solide", "otaNotes", NvsValKind::Str, 110},
    {"solide", "otaNotif", NvsValKind::Str, 16},
    {"solide", "otaPrev", NvsValKind::Int, 0},     {"solide", "otaPend", NvsValKind::Int, 0},
    {"solide", "otaBoots", NvsValKind::Int, 0},    {"solide", "otaLast", NvsValKind::Int, 0},
    {"solide", "tgOffset", NvsValKind::Int, 0},    {"solide", "lbPingEp", NvsValKind::Int, 0},
    {"solide", "fbSyncTs", NvsValKind::Int, 0},    {"solide", "dreamScrHash", NvsValKind::Str, 20},
    {"solide", "tgBotName", NvsValKind::Str, 32},  {"solide", "zaiBase", NvsValKind::Str, 40},
    {"orchmem", "sdSeen", NvsValKind::Int, 0},
    {"solide", "vfy_openai", NvsValKind::Str, 12}, {"solide", "vfy_anthropic", NvsValKind::Str, 12},
    {"solide", "vfy_mistral", NvsValKind::Str, 12}, {"solide", "vfy_zai", NvsValKind::Str, 12},
    {"solide", "vfy_cumulo", NvsValKind::Str, 12}, {"solide", "vfy_tavily", NvsValKind::Str, 12},
    {"solide", "vrs_openai", NvsValKind::Str, 60},  {"solide", "vrs_anthropic", NvsValKind::Str, 60},
    {"solide", "vrm_openai", NvsValKind::Int, 0},   {"solide", "vrm_anthropic", NvsValKind::Int, 0},
};

bool moved(const char* ns, const char* key) {
  for (size_t i = 0; i < nimbus::store::kMovedStoreCount; ++i) {
    const nimbus::store::MovedKey& m = nimbus::store::kMovedStores[i];
    if (std::strcmp(m.ns, ns) != 0) continue;
    if (m.prefix ? std::strncmp(m.key, key, std::strlen(m.key)) == 0 : std::strcmp(m.key, key) == 0) return true;
  }
  return false;
}

bool rowMatches(const nimbus::store::NvsKeyClass& r, const Val& v) {
  if (std::strcmp(v.ns, r.ns) != 0) return false;
  return r.prefix ? std::strncmp(v.key, r.key, std::strlen(r.key)) == 0 : std::strcmp(v.key, r.key) == 0;
}

// A runtime key is budgeted when it moved to a file, is counted above, or has no writer
// left in the image (the older scratchpad copy, the unused orchestrator agent id).
bool budgeted(const nimbus::store::NvsKeyClass& r) {
  if (moved(r.ns, r.key) || std::strcmp(r.key, "scratch") == 0 || std::strcmp(r.key, "antOrchAg") == 0)
    return true;
  for (const Val& v : kRuntime)
    if (rowMatches(r, v)) return true;
  for (const Val& v : kProvisioned)
    if (r.prefix && rowMatches(r, v)) return true;
  return false;
}

size_t usedEntries(bool movesApplied) {
  size_t used = kProvisionedInts;
  for (const Val& v : kProvisioned) used += nimbus::store::nvsValueEntries(v.kind, v.bytes);
  for (const Val& v : kRuntime) {
    if (movesApplied && moved(v.ns, v.key)) continue;
    used += nimbus::store::nvsValueEntries(v.kind, v.bytes);
  }
  return used;
}
}  // namespace nvsbudget

static void test_nvs_budget_model_keeps_the_reserve() {
  using namespace nvsbudget;
  const size_t used = usedEntries(/*movesApplied=*/true);
  TEST_ASSERT_TRUE(used < kUsable);
  const size_t available = kUsable - used;
  char msg[96];
  std::snprintf(msg, sizeof(msg), "%u entries free with every runtime writer at its worst case",
                (unsigned)available);
  TEST_ASSERT_TRUE_MESSAGE(available >= nimbus::store::kNvsReserveEntries, msg);
  // The counter-model: the v4.5.14 layout (journal, ledger, scratchpad and agent ids
  // in settings storage) cannot keep the reserve on the same device.
  const size_t old = usedEntries(/*movesApplied=*/false);
  TEST_ASSERT_TRUE(old >= kUsable || kUsable - old < nimbus::store::kNvsReserveEntries);
  // Every runtime (rebuildable or telemetry) key the firmware defines is budgeted
  // above or moved: a new runtime writer must be counted here before it ships.
  size_t rows = 0;
  const nimbus::store::NvsKeyClass* table = nimbus::store::nvsKeyTable(&rows);
  for (size_t i = 0; i < rows; ++i) {
    const nimbus::store::NvsKeyClass& r = table[i];
    if (r.cls != NvsClass::Rebuildable && r.cls != NvsClass::Telemetry) continue;
    if (r.evictOrder > 0 && std::strcmp(r.key, "fbRules") != 0) continue;   // evicted under pressure
    TEST_ASSERT_TRUE_MESSAGE(nvsbudget::budgeted(r), r.key);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_anchor_identity_at_200k);
  RUN_TEST(test_floors_bind_on_8k_window);
  RUN_TEST(test_caps_bind_on_1m_window);
  RUN_TEST(test_overrides_win_and_clamp);
  RUN_TEST(test_zero_window_uses_default);
  RUN_TEST(test_monotonic_in_window);
  RUN_TEST(test_mistral_128k_derivation);
  RUN_TEST(test_orchestrator_sizes_off_one_head_budget);
  RUN_TEST(test_fold_input_fits_the_string_guard);
  RUN_TEST(test_nvs_budget_model_keeps_the_reserve);
  return UNITY_END();
}
