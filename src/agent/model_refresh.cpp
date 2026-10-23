#include "model_refresh.h"

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include <cstring>
#include <mutex>
#include <vector>

#include "../sys/agent_log.h"
#include "../sys/heap_watch.h"          // restingLargest - the handshake floor gate
#include "memory_subsystem.h"           // registry() - where models.list lives
#include "nimbus/orch/model_catalog.h"  // lane F: the model store, rowOffered, modelPrice
#include "nimbus/orch/model_choices.h"  // staticModelChoices - before the first fetch
#include "nimbus/orch/model_refresh.h"
#include "nimbus/orch/provider_slots.h"
#include "nimbus/orch/usage_ledger.h"   // rateSrcName
#include "orchestrator.h"               // turnInFlight
#include "provider_verify.h"
#include "store.h"
#include "version.h"                    // NIMBUS_FW_VERSION - the store header's firmware

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

uint32_t nowEpoch() { return nimbus::orch::calendarNowOrZero(time(nullptr)); }   // 0 until the clock is set

// The stamp: the header of lane F's model store for `slug` (fetch time, the firmware
// that wrote it). No store yet: present=false.
nimbus::orch::ModelsStamp readStamp(const char* slug) {
  if (slotIndex(slug) < 0) return nimbus::orch::ModelsStamp();
  return nimbus::orch::stampFromStore(store::modelStoreText(String(slug)));
}

// What one store row costs and where the figure comes from: the ledger's own price
// when it has one for this model (the owner's, the router's billed rate, the
// built-in model estimate), else the store row's (lane F: the router's per-row price
// or the built-in estimate). A provider-wide default is never shown as a model's price.
void priceRow(const std::string& provider, const nimbus::orch::ModelInfo& m, nimbus::orch::ModelRow& r) {
  using nimbus::orch::RateSrc;
  const nimbus::orch::EffRate rate = store::modelRate(provider, r.id);
  const RateSrc src = rate.src();
  if (src == RateSrc::Owner || src == RateSrc::Router || src == RateSrc::BuiltinModel) {
    r.priceIn = (uint32_t)(rate.in > 0xFFFFFFFFull ? 0xFFFFFFFFull : rate.in);
    r.priceOut = (uint32_t)(rate.out > 0xFFFFFFFFull ? 0xFFFFFFFFull : rate.out);
    r.priceSource = nimbus::orch::rateSrcName(src);
    return;
  }
  const nimbus::orch::ModelPrice mp = nimbus::orch::modelPrice(provider, m);
  if (!std::strcmp(mp.src, "unknown")) {
    r.priceSource = "none";
    return;
  }
  r.priceIn = mp.in;
  r.priceOut = mp.out;
  r.priceSource = !std::strcmp(mp.src, "builtin") ? "est" : mp.src;
}

nimbus::orch::ModelRow rowFor(const std::string& provider, const nimbus::orch::ModelInfo& m) {
  nimbus::orch::ModelRow r;
  r.id = nimbus::orch::modelSelector(m);
  r.name = m.name;
  r.line = m.line.empty() ? m.family : m.line;
  r.size = m.size;
  r.released = m.created;
  r.ctx = m.ctxTokens;
  r.lifecycle = !m.life.empty() ? m.life : (m.deprecated ? "deprecated" : "active");
  r.roles = m.roles;
  priceRow(provider, m, r);
  return r;
}

// The rows models.list shows for `provider`, from lane F's model store: every row the
// one model check offers now (rowOffered: usable, a chat role, not past its retire
// date), in the store's order, so every id the head reads applies as named. Before
// the first fetch: the compiled cold-start list. Flash (RAM-cached) reads only.
bool loadRows(const std::string& provider, nimbus::orch::ProviderModels& out) {
  const std::string text = store::modelStoreText(String(provider.c_str()));
  nimbus::orch::ModelStoreMeta meta;
  const uint32_t now = nowEpoch();
  if (!text.empty() && nimbus::orch::parseModelStoreHeader(text, meta)) {
    out.fetched = true;
    out.fetchedAt = meta.fetchedAt;
    out.more = meta.more;
    nimbus::orch::forEachStoreRow(provider, text, [&](const nimbus::orch::ModelInfo& m) {
      if (nimbus::orch::rowOffered(m, 0, now)) out.rows.push_back(rowFor(provider, m));
      return true;
    });
    if (!out.rows.empty()) return true;
  }
  out.fetched = false;   // no store row can run a chat role: the cold-start list stands
  const std::string cold = nimbus::orch::staticModelChoices(provider.c_str());
  size_t start = 0;
  while (start < cold.size()) {
    size_t comma = cold.find(',', start);
    if (comma == std::string::npos) comma = cold.size();
    const std::string id = cold.substr(start, comma - start);
    start = comma + 1;
    if (!id.empty()) out.rows.push_back(rowFor(provider, nimbus::orch::classifyCatalogEntry(provider, id)));
  }
  return true;
}

// The firmware version lane F's store header records (NIMBUS_FW_VERSION): a list
// written by another version is due once after an update.
const char* runningFw() { return NIMBUS_FW_VERSION; }

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
