#include "degraded_device.h"

#include <mutex>
#include <new>

#include "files_subsystem.h"
#include "memory_subsystem.h"
#include "store.h"
#include "telegram.h"

namespace agent {
namespace degraded {

namespace {

using namespace nimbus::orch;

constexpr uint32_t kTickMs = 2000;
// The first seconds are the boot's: the park's file loads, Wi-Fi joins.
constexpr uint32_t kBootQuietMs = 15000;

struct State {
  ServingTracker tracker;
  DegradedDebounce debounce;   // guarded by mx
  std::mutex mx;
  uint32_t lastTickMs = 0;
  bool ticked = false;
  // The last embedding answer: a busy card lock keeps it.
  memory::EmbedHealth embed;
};

State* state() {
  static State* s = new (std::nothrow) State();
  return s;
}

ParkedFact factOf(const ParkEntry& e) {
  ParkedFact p;
  p.provider = e.provider;
  p.kind = e.kind;
  p.httpStatus = e.httpStatus;
  p.seamMask = e.seamMask;
  p.paused = e.paused;
  p.cause = e.cause;
  return p;
}

// Is the head held off for the chat, as a turn would meet it? A refusal park that covers
// the chat seam and whose re-check is not due (a due re-check is the next request), or the
// provider's own limit. The monthly spend ceiling is not here: it pauses every provider
// and refuses the turn, so no fallback turn is served, and it has its own line.
bool headHeldOff(const ParkEntry& e, ParkedFact& why) {
  const bool parkedForChat = e.kind != ParkKind::None && (e.seamMask & (1u << (uint8_t)PaidSeam::Chat)) &&
                             !(e.recheckInMs == 0 && !e.probing);
  const uint8_t own = e.paused & uint8_t(~kPauseSpendCeiling);
  if (!parkedForChat && !own) return false;
  why = factOf(e);
  why.paused = own;
  if (!parkedForChat) why.kind = ParkKind::None;
  return true;
}

void collectProviders(State& s, DegradedFacts& f, uint32_t nowMs) {
  ParkEntry entries[ProviderPark::kMaxProviders];
  const size_t n = providerPark().snapshot(entries, ProviderPark::kMaxProviders, nowMs);
  for (size_t i = 0; i < n; ++i) {
    // A park of a provider whose key is gone is history, not news.
    if (!store::providerHasKey(String(entries[i].provider))) continue;
    f.parks.push_back(factOf(entries[i]));
  }
  FallbackFact& fb = f.fallback;
  fb = s.tracker.fact(nowMs);
  if (!fb.active) return;
  for (size_t i = 0; i < n; ++i) {
    if (fb.head != entries[i].provider) continue;
    ParkedFact why;
    if (!headHeldOff(entries[i], why)) break;
    const ParkReason r = degradedParkReason(why);
    fb.headClosed = true;
    fb.headWhy = r.phrase;
    fb.headFix = r.fix;
    break;
  }
}

void collectMemory(State& s, DegradedFacts& f) {
  memory::EmbedHealth h;
  if (memory::embedHealth(h)) s.embed = h;   // busy: the last answer stands
  f.embedParked = s.embed.parked;
  f.pendingEmbeds = s.embed.pending;
  f.storeFull = files::storeFullActive();
  f.flashFull = memory::flashFull();
  f.clockSynced = memory::clockSynced();
}

void collectTelegram(DegradedFacts& f) {
  const telegram::Health h = telegram::health();
  f.telegram.configured = h.tokenSet;
  f.telegram.taskAlive = h.taskAlive;
  f.telegram.authRejected = h.authRejected;
  f.telegram.fails = h.fails;
  f.telegram.idleMs = h.idleMs;
  f.telegram.turnBusy = h.busy;
  f.telegram.wifiUp = f.wifiUp;
}

}  // namespace

void noteTurn(const std::string& requested, const std::string& served, const std::string& path, bool billed) {
  if (State* s = state()) s->tracker.noteTurn(requested, served, path, billed, (uint32_t)millis());
}

bool tick(uint32_t nowMs, bool orchestrator, bool wifiUp) {
  State* s = state();
  if (!s) return false;
  if (nowMs < kBootQuietMs) return false;
  if (s->ticked && nowMs - s->lastTickMs < kTickMs) return false;
  s->ticked = true;
  s->lastTickMs = nowMs;

  DegradedFacts f;
  f.orchestrator = orchestrator;
  f.uptimeMs = nowMs;
  f.wifiUp = wifiUp;
  if (orchestrator) {
    collectProviders(*s, f, nowMs);
    collectMemory(*s, f);
    collectTelegram(f);
  }
  const DegradedSet now = computeDegraded(f);
  std::lock_guard<std::mutex> lk(s->mx);
  return s->debounce.update(now, nowMs);
}

DegradedSet shown() {
  State* s = state();
  if (!s) return DegradedSet();
  std::lock_guard<std::mutex> lk(s->mx);
  return s->debounce.shown();
}

uint32_t generation() {
  State* s = state();
  if (!s) return 0;
  std::lock_guard<std::mutex> lk(s->mx);
  return s->debounce.generation();
}

void into(JsonObject out) {
  State* s = state();
  if (!s) {
    degradedInto(DegradedSet(), 0, out);
    return;
  }
  std::lock_guard<std::mutex> lk(s->mx);
  degradedInto(s->debounce.shown(), s->debounce.generation(), out);
}

}  // namespace degraded
}  // namespace agent
