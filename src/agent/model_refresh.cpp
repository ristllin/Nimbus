#include "model_refresh.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <time.h>

#include <cstring>
#include <mutex>
#include <vector>

#include "../sys/agent_log.h"
#include "../sys/heap_watch.h"          // restingLargest - the handshake floor gate
#include "../sys/ps_json.h"             // PsramJsonAllocator - the catalog parse off internal heap
#include "fs_atomic.h"                  // writeFileAtomic, flashMounted
#include "memory_subsystem.h"           // registry() - where models.list lives
#include "nimbus/orch/model_catalog.h"  // ModelInfo, modelsFromJson, classifyCatalogEntry
#include "nimbus/orch/model_choices.h"  // effectiveModelChoices - the list orch_model checks
#include "nimbus/orch/model_refresh.h"
#include "nimbus/orch/provider_slots.h"
#include "nimbus/orch/usage_ledger.h"   // rateSrcName
#include "orchestrator.h"               // turnInFlight
#include "provider_verify.h"
#include "router_catalog.h"
#include "store.h"
#include "version.h"                    // NIMBUS_FW_BUILD - the firmware a list was fetched by

namespace agent {
namespace model_refresh {

namespace {

using nimbus::orch::RefreshAsk;

std::mutex                       g_mx;   // the queue: turn task, web task, main loop
nimbus::orch::ModelRefreshQueue  g_q;
bool                             g_begun = false;
char                             g_running[16] = {};   // the refresh the verify task runs now
const char*                      g_why[nimbus::orch::kProviderSlotCount] = {};

// The passive check: first at boot plus 10 minutes (the first turns after a restart
// go first), then every 6 hours on a device that stays up for weeks.
constexpr uint32_t kPassiveFirstMs = 10u * 60u * 1000u;
constexpr uint32_t kPassiveEveryMs = 6u * 3600u * 1000u;

int slotIndex(const char* slug) {
  for (size_t i = 0; slug && i < nimbus::orch::kProviderSlotCount; ++i)
    if (std::strcmp(slug, nimbus::orch::kProviderSlots[i].slug) == 0) return (int)i;
  return -1;
}

bool keyed(const char* p) {
  if (!std::strcmp(p, "openai")) return store::hasOpenaiKey();
  if (!std::strcmp(p, "anthropic")) return store::hasAnthropicKey();
  if (!std::strcmp(p, "mistral")) return store::hasMistralKey();
  if (!std::strcmp(p, "zai")) return store::hasZaiKey();
  if (!std::strcmp(p, "cumulo")) return store::hasCumuloKey();
  return false;
}

uint32_t nowEpoch() {
  const time_t t = time(nullptr);
  return t > 1700000000 ? (uint32_t)t : 0;   // 0 until the clock is set
}

std::string stampPath(const char* slug) { return std::string("/data/mstamp_") + slug + ".txt"; }

nimbus::orch::ModelsStamp readStamp(const char* slug) {
  nimbus::orch::ModelsStamp s;
  if (slotIndex(slug) < 0 || !flashMounted()) return s;
  const std::string path = stampPath(slug);
  if (!LittleFS.exists(path.c_str())) return s;
  File f = LittleFS.open(path.c_str(), "r");
  if (!f) return s;
  char buf[128];
  const size_t n = f.readBytes(buf, sizeof(buf) - 1);
  f.close();
  buf[n] = 0;
  nimbus::orch::parseModelsStamp(std::string(buf, n), s);
  return s;
}

// A catalog row's selector: the router's rows carry the bare id plus the upstream.
std::string selectorOf(const std::string& provider, const nimbus::orch::ModelInfo& mi) {
  if (provider != "cumulo" || mi.upstream.empty() || mi.id.rfind(mi.upstream + "/", 0) == 0) return mi.id;
  return mi.upstream + "/" + mi.id;
}

nimbus::orch::ModelRow rowFor(const std::string& provider, const std::string& id,
                              const nimbus::orch::ModelInfo& mi) {
  nimbus::orch::ModelRow r;
  r.id = id;
  r.line = mi.family;
  r.size = mi.size;
  r.ctx = mi.ctxTokens;
  r.lifecycle = mi.deprecated ? "deprecated" : "active";
  r.roles = mi.roles;
  const nimbus::orch::EffRate rate = store::modelRate(provider, id);
  r.priceIn = (uint32_t)(rate.in > 0xFFFFFFFFull ? 0xFFFFFFFFull : rate.in);
  r.priceOut = (uint32_t)(rate.out > 0xFFFFFFFFull ? 0xFFFFFFFFull : rate.out);
  r.priceSource = nimbus::orch::rateSrcName(rate.src());
  return r;
}

// The rows models.list shows for `provider`: the ids the orch_model and spawn checks
// accept (the effective choice list), so every id the head reads applies as named,
// with the capability catalog's details where it has the model. Flash reads only.
bool loadRows(const std::string& provider, nimbus::orch::ProviderModels& out) {
  const String p(provider.c_str());
  const String harvested = store::modelChoices(p);
  const std::string choices =
      nimbus::orch::effectiveModelChoices(provider.c_str(), std::string(harvested.c_str()));
  std::vector<nimbus::orch::ModelInfo> cat;
  const String blob = store::modelCatalogJson(p);
  if (blob.length()) {
    JsonDocument d(&PsramJsonAllocator::instance());
    if (!deserializeJson(d, blob)) {
      JsonArrayConst arr = d.as<JsonArrayConst>();
      nimbus::orch::modelsFromJson(arr, cat);
      if (arr.size()) out.more = arr[arr.size() - 1]["more"] | 0;
    }
  }
  out.fetched = harvested.length() > 0 || blob.length() > 0;
  out.fetchedAt = readStamp(provider.c_str()).fetchedAt;
  size_t start = 0;
  while (start < choices.size()) {
    size_t comma = choices.find(',', start);
    if (comma == std::string::npos) comma = choices.size();
    std::string id = choices.substr(start, comma - start);
    start = comma + 1;
    id.erase(0, id.find_first_not_of(" \t"));
    id.erase(id.find_last_not_of(" \t") + 1);
    if (id.empty()) continue;
    const nimbus::orch::ModelInfo* found = nullptr;
    for (const auto& mi : cat)
      if (selectorOf(provider, mi) == id) { found = &mi; break; }
    if (found && !found->usable) continue;   // a model the key cannot use is never offered
    out.rows.push_back(rowFor(provider, id, found ? *found : nimbus::orch::classifyCatalogEntry(provider, id)));
  }
  return true;
}

// The build that fetched a list: NIMBUS_FW_BUILD (git describe), so any new image -
// a release or a bench flash - counts as an update; a release build reads its tag.
const char* runningFw() { return NIMBUS_FW_BUILD; }

int passiveCheck() {
  const uint32_t now = nowEpoch();
  int queued = 0;
  for (const nimbus::orch::ProviderSlot& slot : nimbus::orch::kProviderSlots) {
    // A rejected key is the owner's to fix: it is not asked again every 6 hours.
    if (!keyed(slot.slug) || store::verifyResult(slot.slug) == 0) continue;
    const nimbus::orch::PassiveWhy why =
        nimbus::orch::passiveRefreshDue(readStamp(slot.slug), runningFw(), now);
    if (why == nimbus::orch::PassiveWhy::None) continue;
    const RefreshAsk a = request(slot.slug, "passive");
    queued += a == RefreshAsk::Queued;
    alogif("models: %s passive refresh due (%s) -> %s", slot.slug, nimbus::orch::passiveWhyToken(why),
           nimbus::orch::refreshAskToken(a));
  }
  return queued;
}

}  // namespace

RefreshAsk request(const char* provider, const char* why) {
  if (!provider || slotIndex(provider) < 0) return RefreshAsk::NoKey;
  const bool k = keyed(provider);
  RefreshAsk a;
  {
    std::lock_guard<std::mutex> lk(g_mx);
    a = g_q.request(provider, k, (uint32_t)millis());
    if (a == RefreshAsk::Queued) g_why[slotIndex(provider)] = why;
  }
  if (a == RefreshAsk::Queued) alogif("models: %s list refresh queued (%s)", provider, why ? why : "-");
  return a;
}

std::string missText(const std::string& provider, const std::string& model) {
  (void)model;
  const RefreshAsk a = request(provider.c_str(), "miss");
  return nimbus::orch::modelMissText(provider, fetchedAt(provider.c_str()), a);
}

uint32_t fetchedAt(const char* provider) { return readStamp(provider).fetchedAt; }

std::string stampFw(const char* provider) { return readStamp(provider).fw; }

int passiveCheckNow() { return g_begun ? passiveCheck() : 0; }

bool pending(const char* provider) {
  std::lock_guard<std::mutex> lk(g_mx);
  return g_q.queued(provider) || (g_running[0] && provider && !std::strcmp(g_running, provider));
}

void noteFetched(const char* provider) {
  if (slotIndex(provider) < 0) return;
  const std::string stamp = nimbus::orch::formatModelsStamp(nowEpoch(), runningFw());
  if (!writeFileAtomic(stampPath(provider).c_str(), stamp, "mstamp"))
    alogif("models: %s list stamp not stored", provider);
}

void begin() {
  if (g_begun) return;
  g_begun = true;
  nimbus::orch::ModelsToolHandlers h;
  h.keyedProviders = [] {
    std::vector<std::string> out;
    for (const nimbus::orch::ProviderSlot& slot : nimbus::orch::kProviderSlots)
      if (keyed(slot.slug)) out.push_back(slot.slug);
    return out;
  };
  h.load = [](const std::string& p, nimbus::orch::ProviderModels& out) { return loadRows(p, out); };
  h.requestRefresh = [](const std::string& p) { return request(p.c_str(), "head"); };
  nimbus::orch::registerModelsTool(memory::registry(), h);
}

void tick(uint32_t nowMs) {
  if (!g_begun) return;
  static uint32_t s_next = 0;
  static uint32_t s_passiveAt = 0;
  static bool s_init = false;
  if (!s_init) {
    s_init = true;
    s_passiveAt = nowMs + kPassiveFirstMs;
  }
  if ((int32_t)(nowMs - s_next) < 0) return;   // a 1 s gate: the loop runs far faster
  s_next = nowMs + 1000;
  if ((int32_t)(nowMs - s_passiveAt) >= 0) {
    s_passiveAt = nowMs + kPassiveEveryMs;
    passiveCheck();
  }
  const bool busy = provider_verify::pending();
  const bool inTurn = orchestrator::turnInFlight();
  const bool online = WiFi.status() == WL_CONNECTED;
  const bool restingOk = nimbus::heapwatch::restingLargest() >= provider_verify::deferFloor();
  const char* due;
  const char* why;
  {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!busy) g_running[0] = 0;
    due = g_q.due(inTurn, online, busy, restingOk);
    why = due ? g_why[slotIndex(due)] : nullptr;
  }
  if (!due || !provider_verify::requestRefresh(due)) return;   // stays queued until it starts
  {
    std::lock_guard<std::mutex> lk(g_mx);
    g_q.started(due, nowMs);
    std::strncpy(g_running, due, sizeof(g_running) - 1);
  }
  alogif("models: %s list refresh started (%s)", due, why ? why : "-");
}

}  // namespace model_refresh
}  // namespace agent
