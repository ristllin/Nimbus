#include "model_refresh.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <time.h>

#include <cstdio>
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
#include "nimbus/orch/served_model.h"   // the concrete model behind an answer's alias
#include "nimbus/orch/turn_images.h"    // installImageInputLookup - photos only to a seeing head
#include "nimbus/orch/usage_ledger.h"   // rateSrcName
#include "orchestrator.h"               // turnInFlight
#include "provider_verify.h"
#include "store.h"
#include "telegram.h"                   // taskAlive - the poll task a refresh can run on
#include "version.h"                    // NIMBUS_FW_VERSION - the store header's firmware

namespace agent {
namespace model_refresh {

namespace {

using nimbus::orch::RefreshAsk;

std::mutex                       g_mx;   // the queue: turn task, web task, main loop
nimbus::orch::ModelRefreshQueue  g_q;
bool                             g_begun = false;
char                             g_running[16] = {};   // the refresh under way (its task or the poll task)
char                             g_staged[16] = {};    // handed to the poll task, not yet run there
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
    // A rejected key is the owner's to fix: it is not asked again every 6 hours. A
    // paused or parked provider is asked: its list GET is free (passiveWants).
    const nimbus::orch::ModelsStamp stamp = readStamp(slot.slug);
    if (!nimbus::orch::passiveWants(keyed(slot.slug), store::verifyResult(slot.slug), stamp, runningFw(), now))
      continue;
    const nimbus::orch::PassiveWhy why = nimbus::orch::passiveRefreshDue(stamp, runningFw(), now);
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
    // A key verify (a key saved or checked again): the list may differ, so the hour does not hold it.
    a = g_q.request(provider, k, (uint32_t)millis(), why && !std::strcmp(why, "verify"));
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
  // A refresh waiting out a failure's retry delay is not under way: the page does not wait on it.
  const bool waiting = g_q.queued(provider) && !g_q.retryWaiting(provider, (uint32_t)millis());
  return waiting || (g_running[0] && provider && !std::strcmp(g_running, provider));
}

bool stagedPending() {
  std::lock_guard<std::mutex> lk(g_mx);
  return g_staged[0] != 0;
}

void runStaged() {
  char slug[sizeof(g_staged)];
  {
    std::lock_guard<std::mutex> lk(g_mx);
    std::memcpy(slug, g_staged, sizeof(slug));
  }
  // A refusal runs nothing: the next pass settles it as a failed run (the retry delay applies).
  // A run that gave way to a waiting owner message is queued again after a short hold and
  // counts no try (ModelRefreshQueue::yielded): it is not settled as a run at all.
  using provider_verify::InlineRun;
  const InlineRun r = slug[0] ? provider_verify::runRefreshHere(String(slug)) : InlineRun::Refused;
  std::lock_guard<std::mutex> lk(g_mx);
  if (r == InlineRun::Yielded) {
    g_q.yielded(slug, (uint32_t)millis());
    g_running[0] = 0;
  }
  g_staged[0] = 0;   // cleared after the run: tick reads "busy" until here
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
  // A turn's record names the concrete model each host answered with: a "-latest" echo
  // (Mistral's answers) is looked up in the stored lists this module keeps fresh.
  nimbus::orch::installServedModelResolver([](const char* host, const char* echo, char* out, size_t cap) {
    const std::string id = store::concreteModelId(String(host), echo ? std::string(echo) : std::string());
    if (!id.empty()) std::snprintf(out, cap, "%s", id.c_str());
  });
  // Photos go to the head as images only when its model takes them: the provider's own
  // list decides when it says (Mistral's and Anthropic's do), else the name rule.
  nimbus::orch::installImageInputLookup([](const char* host, const char* model) {
    return store::modelImageInput(String(host), model ? std::string(model) : std::string());
  });
}

// The refresh the verify task ran for `slug` has ended: when the stored list is still
// due (no answer, a cut or empty list, a failed write) it is queued again on the
// queue's growing delay. A key the refresh found rejected, or a removed key, is not
// retried (the owner's to fix).
static void noteFinished(const char* slug, uint32_t nowMs) {
  const bool stillDue = nimbus::orch::passiveWants(keyed(slug), store::verifyResult(slug), readStamp(slug),
                                                   runningFw(), nowEpoch());
  uint32_t retryMs;
  {
    std::lock_guard<std::mutex> lk(g_mx);
    retryMs = g_q.finished(slug, stillDue, nowMs);
  }
  if (retryMs) alogf("models: %s list refresh did not land - next try in %u min", slug, (unsigned)(retryMs / 60000u));
}

// Whether a refresh is under way (on its task, or handed to the poll task and not yet
// run there); one that ended since the last pass is settled here (noteFinished).
static bool settleRun(bool pollAlive, uint32_t nowMs) {
  char ended[sizeof(g_running)] = {};
  bool busy;
  {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!pollAlive) g_staged[0] = 0;   // the poll task is gone: what it held ends as a failed run
    busy = provider_verify::pending() || g_staged[0];
    if (!busy && g_running[0]) {
      std::memcpy(ended, g_running, sizeof(ended));
      g_running[0] = 0;
    }
  }
  if (ended[0]) noteFinished(ended, nowMs);   // flash reads: outside the queue lock
  return busy;
}

// Start `due` on its route: its own task, or handed to the poll task's staged work.
static void startDue(const char* due, const char* why, nimbus::orch::RefreshRoute route, bool pollAlive,
                     uint32_t nowMs) {
  bool onPoll = route == nimbus::orch::RefreshRoute::Poll;
  // Its own task refused after all (the block moved): the poll task, when there is one.
  if (!onPoll && !provider_verify::requestRefresh(due)) {
    if (!pollAlive) return;   // stays queued until it starts
    onPoll = true;
  }
  {
    std::lock_guard<std::mutex> lk(g_mx);
    g_q.started(due, nowMs);
    std::strncpy(g_running, due, sizeof(g_running) - 1);
    if (onPoll) std::strncpy(g_staged, due, sizeof(g_staged) - 1);
  }
  alogif("models: %s list refresh started (%s, %s)", due, why ? why : "-", onPoll ? "poll" : "task");
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
  const bool inTurn = orchestrator::turnInFlight();
  const bool pollAlive = telegram::taskAlive();
  const bool busy = settleRun(pollAlive, nowMs);
  const char* due;
  const char* why;
  {
    std::lock_guard<std::mutex> lk(g_mx);
    due = g_q.due(inTurn, WiFi.status() == WL_CONNECTED, busy, /*restingOk=*/true, nowMs);
    why = due ? g_why[slotIndex(due)] : nullptr;
  }
  if (!due) return;
  // Its own task when the 8 KB stack fits, else the poll task, else it waits (model_refresh.h
  // refreshRoute: the memory gate, judged only when something is due).
  const nimbus::orch::RefreshRoute route = nimbus::orch::refreshRoute(
      inTurn, pollAlive, (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
      (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), nimbus::heapwatch::restingLargest());
  if (route != nimbus::orch::RefreshRoute::Wait) startDue(due, why, route, pollAlive, nowMs);
}

}  // namespace model_refresh
}  // namespace agent
