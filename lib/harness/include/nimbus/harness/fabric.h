#pragma once
#include <cstdint>

#include "nimbus/orch/result_envelope.h"   // nimbus::orch::ResultEnvelope + JobState

// fabric.h - the provider ADAPTER INTERFACE + heavy-fabric registry for the
// Nimbus Orchestrator, ported from Nuage-Solide src/agent/heavy_fabric.h (Head
// Orchestrator v2) and lifted verbatim from src/agent/adapter.h into the
// portable harness library (2026-07 harness extraction; only <Arduino.h> was
// dropped - the interface never needed it). Two roles per provider share one
// adapter file:
//
//   1. Sub-session adapter (ManagedAgentAdapter::dispatch/poll/cancel/answer) -
//      background heavy jobs on the ring. dispatch() fires and returns a jobId
//      ("<backend>:<remoteId>"); poll() re-attaches from that jobId alone (survives
//      reboot); the core loop never names a backend.
//   2. Orchestrator-host free function (orchTurn*) - one stateful head-orchestrator
//      turn: send system(role+memory) + user(context) and get a strict-JSON turn
//      string back. Declared in each adapter's own header.
//
// The result type is the PORTABLE nimbus::orch::ResultEnvelope (lib/core) - the
// core loop only ever sees that struct, never a provider type. Adding a backend =
// one subclass + one factory line + one config entry.
//
// COMPILE-CLEAN but LIVE-GATED: every device dispatch/poll opens a
// WiFiClientSecure and needs (a) provisioned STA WiFi, (b) the provider's API
// key. See the adapters. Host tests register scripted fakes.

namespace agent {

using nimbus::orch::JobState;
using nimbus::orch::ResultEnvelope;
using nimbus::orch::isTerminal;

enum class FabricErr : uint8_t {
  Ok = 0, Network, Auth, RateLimited, BadRequest,
  NotFound, Unsupported, Timeout, RemoteFail, ParseFail,
};

enum class RateLimit : uint8_t;   // the quota window a 429 refused on (rate_limit.h)

struct Capabilities {
  bool resultAsPR      = false;
  bool resultAsFile    = false;
  bool humanInLoop     = false;
  bool cancelable      = false;
  bool selfHosted      = false;
  uint16_t typicalLatencySec = 60;
};

struct Directive {
  const char* category    = nullptr;  // "code" | "research" | "ops"
  const char* instruction = nullptr;  // natural-language task
  const char* chatId      = nullptr;  // Telegram chat to deliver result to
  const char* tag         = nullptr;  // device correlation id
  const char* model       = nullptr;  // per-dispatch model override (else adapter default)
  const char* skill       = nullptr;  // skill hint, e.g. "deep_research" | "web"
  // The spawn's own task text, WITHOUT the date header, skill capsule, chat
  // [CONTEXT] or attachments that `instruction` carries (CUM-536): connector
  // provenance asks "did this task name an attached connector?" and must not be
  // fooled by an earlier message in the chat. nullptr => use `instruction`.
  const char* task        = nullptr;
  // OUT, optional (CUM-465): when dispatch() returns RateLimited, the adapter writes
  // the quota window that refused here (nothing is written otherwise), so the spawn
  // refusal names it the way a head-turn 429 does. FabricErr alone cannot carry it.
  // The caller owns the storage; every adapter passes the Directive through
  // unchanged, so the device and a Virtual Nimbus both carry it.
  RateLimit*  rateLimit   = nullptr;
};

class ManagedAgentAdapter {
 public:
  virtual ~ManagedAgentAdapter() {}
  virtual const char*  backendId()    const = 0;   // "openai"|"anthropic"|"mistral"|"custom"
  virtual Capabilities capabilities() const = 0;

  // Fire and forget: starts the remote agent, returns jobId immediately.
  // jobId format: "<backendId>:<remoteId>", written into outJobId[72].
  virtual FabricErr dispatch(const Directive& d, char outJobId[72]) = 0;

  // Cheap status check. Fills env (only valid fields on non-Done states).
  // Safe to call every N seconds. Re-attaches from jobId alone after reboot.
  virtual FabricErr poll(const char* jobId, ResultEnvelope& env) = 0;

  // Best-effort cancel. Idempotent.
  virtual FabricErr cancel(const char* jobId) = 0;

  // Resume a NeedsInput pause with a user reply. Default: unsupported.
  virtual FabricErr answer(const char* jobId, const char* userText) {
    (void)jobId; (void)userText; return FabricErr::Unsupported;
  }
};

// Registry: maps category -> adapter, and routes a jobId to its adapter by the
// backend prefix before ':'. One indirection the core loop holds.
class HeavyFabric {
 public:
  static constexpr int MAX_ADAPTERS = 6;   // the device backends below, all always registered

  void registerAdapter(ManagedAgentAdapter* a);
  void bindCategory(const char* category, const char* backendId);

  FabricErr dispatch(const Directive& d, char outJobId[72]);
  FabricErr poll(const char* jobId, ResultEnvelope& env);
  FabricErr cancel(const char* jobId);
  FabricErr answer(const char* jobId, const char* userText);

  // Resolve an adapter by a jobId ("backend:...") or a bare backend id.
  ManagedAgentAdapter* adapterFor(const char* jobIdOrBackend);

  struct Binding { char category[16]; char backendId[16]; };

 private:
  static constexpr int MAX_BINDINGS = 8;

  ManagedAgentAdapter* adapters_[MAX_ADAPTERS] = {};
  int                  adapterCount_ = 0;
  Binding              bindings_[MAX_BINDINGS] = {};
  int                  bindingCount_ = 0;
};

// The sub-agent backends a device registers at boot, keyed or not (CUM-541): each
// adapter's dispatch refuses until its key (or the custom base URL) is set, so one
// added later on the web page works on the next helper without a restart.
inline constexpr const char* kDeviceBackends[] = {"anthropic", "openai", "mistral",
                                                  "custom",    "zai",    "cumulo"};
inline constexpr int kDeviceBackendCount = (int)(sizeof(kDeviceBackends) / sizeof(kDeviceBackends[0]));
static_assert(kDeviceBackendCount <= HeavyFabric::MAX_ADAPTERS,
              "every device backend must fit the adapter table");
// How each device backend runs a sub-agent, one entry per kDeviceBackends slot
// in the same order (subagents F6.2, the honest [SPAWN CAPACITY] line). A
// SYNCHRONOUS backend runs the whole sub inside one blocking request on the
// device's only network task, so the device does nothing else meanwhile and the
// sub must finish within that request's wait; an asynchronous one creates a
// remote job and returns at once, and the device polls it.
inline constexpr bool kDeviceBackendSync[] = {false, false, true, true, true, true};
static_assert(sizeof(kDeviceBackendSync) / sizeof(kDeviceBackendSync[0]) ==
                  (unsigned)kDeviceBackendCount,
              "every device backend needs its dispatch mode");
// True when the named device backend runs its sub-agents synchronously.
inline bool backendRunsSync(const char* backend) {
  for (int i = 0; i < kDeviceBackendCount; i++) {
    const char* a = kDeviceBackends[i];
    const char* b = backend;
    while (*a && *a == *b) { a++; b++; }
    if (!*a && !*b) return kDeviceBackendSync[i];
  }
  return false;
}

// Registers one adapter per kDeviceBackends entry, in that order, then checks each
// id resolves to its adapter. Returns the first id that does not (a wrong adapter in
// a slot, a null, or a full table), or nullptr when every backend is reachable.
const char* registerDeviceBackends(HeavyFabric& fabric,
                                   ManagedAgentAdapter* const (&adapters)[kDeviceBackendCount]);

}  // namespace agent
