#pragma once
#include <ArduinoJson.h>

#include <cstdint>
#include <functional>
#include <string>

#include "nimbus/harness/fabric.h"      // Directive / ResultEnvelope / FabricErr
#include "nimbus/harness/head_tools.h"  // HeadTools - the multi-turn tool-use loop glue
#include "nimbus/harness/http.h"        // HttpTransport - the device/host seam
#include "nimbus/orch/body_budget.h"    // bodyBudgetError - the refusal the tool loop reads back
#include "nimbus/orch/head_loop.h"      // StepErr - the typed step error
#include "nimbus/orch/provider_park.h"  // the one per-provider park (the host park's store)
#include "nimbus/orch/token_usage.h"
#include "nimbus/orch/turn_spend.h"     // TurnSpend - per-host spend of a fabric turn
#include "nimbus/orch/wire_limits.h"    // kJsonStrGuard, foldDigestCap - the string ceilings

// providers - the PORTABLE provider wire layer (Stage H of the harness
// extraction). Everything that used to live inside the device adapters except
// the socket: request URL/header/body construction, structured-output config,
// the head tool-use loop steps (via nimbus::orch::runHeadLoop), response
// parsing, usage extraction, and error mapping - all in std::string space over
// the agent::HttpTransport contract, so the full provider wire is host-tested
// with FakeHttpTransport (test/test_harness_wire_*).
//
// The DEVICE keeps only: the TLS transport (src/agent/transport_tls.cpp -
// WiFiClientSecure + tls_arbiter + the one-buffer-single-write rule) and thin
// String<->std::string wrappers in src/agent/adapters/*.cpp (public surfaces
// unchanged).
//
// Every AGENTS.md wire lesson moved verbatim - the comments ride with the code:
// Anthropic ADVISORY schema + echo-assistant-only-with-tool_use, OpenAI strict
// json_schema + previous_response_id + the F20 chain-poison guard, Mistral
// tool_choice "required" (NOT "any") + no built-in connectors on loop turns,
// custom keyless-on-http. Bodies are built as ONE contiguous std::string and
// handed to the transport whole (on the device, malloc >=128 B spills to PSRAM
// via heap_caps_malloc_extmem_enable(128) - see main.cpp - so large bodies
// never sit on internal SRAM; the transport then does a single client.write).
namespace agent {
namespace providers {

// R3 (22 Oct prompt-size item): the whole request body stays under this stated
// budget. A one-word owner turn on a bench unit with weeks of history sent a
// 56,504 B body (v4.5.15 soak); the composed parts are each capped now (the
// system block by its budget, the window by recentConvBytes, recall and
// scratchpad by the same budget), and the measured worst HEALTHY case - the
// full 42-tool manifest on the wire, a device-scale system budget, a full
// recent window, a full recall - assembles at 62.8-64.9 KB across the loop
// wires, dominated by the tools array (~32 KB, the API contract); a LONG
// tool loop adds its replay budget on top (16 KB of tool results, CUM-642),
// for a measured worst of about 80 KB mid-loop. The 85 KB budget sits above
// that case (test_prompt_layout asserts it, per wire and mid-loop) and
// a body over it is REFUSED before the send (fail closed, named, the way
// serializeChecked refuses an over-long string): a component that escapes
// its cap costs one honest turn, not a TLS stall (lane O3's memory pressure).
// Inside a tool loop the refusal is not the end of the turn: nothing was sent,
// so the loop folds older tool results to one-line stubs and runs the same
// round again (head_loop.h), and the owner still gets an answer.
// Photos splice in AFTER the check: they are their own bounded channel.
inline constexpr size_t kWireBodyMax = 85000;

// The last step of a body build, after serializeChecked: hold the assembled
// body under kWireBodyMax. False + err (the provider refuses the send) when
// the turn outgrew the budget; err is nimbus::orch::bodyBudgetError's wording.
inline bool bodyWithinBudget(const std::string& body, std::string& err) {
  if (body.size() <= kWireBodyMax) return true;
  err = nimbus::orch::bodyBudgetError(kWireBodyMax, body.size());
  return false;
}

// The dependency bundle every portable provider function takes. All closures
// are nullable unless noted; a missing closure reads as "" / false / no-op.
// The device builds ONE of these (src/agent/transport_tls.cpp
// deviceProviderDeps()) over store:: + connectors:: + PsramJsonAllocator;
// tests build it over fakes.
struct ProviderDeps {
  HttpTransport* http = nullptr;   // REQUIRED

  // Credentials + models (host = "openai" | "anthropic" | "mistral").
  std::function<std::string(const char* host)> key;
  std::function<std::string(const char* host)> orchModel;

  // The head tool-loop master switch (store::orchToolLoop). The engine also
  // gates by passing tools=nullptr; both must be on for a loop turn (the
  // pre-split adapters checked store::orchToolLoop() themselves - preserved).
  std::function<bool()> toolLoopOn;

  // Anthropic managed-agents NVS caches (sub-sessions only): the shared cloud
  // environment id + the per-model agent map "model=agentid;...".
  std::function<std::string()> antEnvId;
  std::function<void(const std::string&)> setAntEnvId;
  std::function<std::string()> antAgentMap;
  std::function<void(const std::string&)> setAntAgentMap;

  // Custom/proxy endpoint (backend "custom"): base URL ("http://host:port[/..]"
  // = plain HTTP + keyless; "https://"/bare host = TLS), key, wire convention
  // ("openai"|"mistral"|"anthropic"), model.
  std::function<std::string()> customBase, customKey, customConv, customModel;
  // Optional API base path that REPLACES the default "/v1" segment of the request
  // path (parseBase strips any path from customBase, so a router under a sub-path
  // carries it here). Unset/"" = the plain "/v1/...". So the Cumulo head sets
  // "/router/openai/v1" (-> /router/openai/v1/chat/completions) and the Z.ai head
  // sets "/api/paas/v4" (-> /api/paas/v4/chat/completions, which has no /v1).
  std::function<std::string()> customPathPrefix;
  // Optional structured-output mode for the custom HEAD (orchTurnCustom).
  // Unset/"" = response_format json_schema (strict) carrying the orch schema;
  // "object" = response_format {type:"json_object"} plus the JSON-only nudge,
  // for backends that answer 200 but IGNORE json_schema (Z.ai GLM answered in
  // Markdown, 2 of 2 live turns, while json_object returned valid turns - CUM-495).
  std::function<std::string()> customJsonMode;
  // Optional reasoning effort for the custom HEAD body (orchTurnCustom). Non-empty
  // = "reasoning_effort":"<value>" on every attempt; unset/"" = no field (LAN and
  // custom endpoints, byte-identical). CUM-544: Z.ai GLM always reasons and
  // cannot switch it off, and with no bound it out-thinks Z.ai's ~60 s gateway cut
  // on a substantive turn (glm-5.3 replay: no answer at 61 s; "low": 13.5 s).
  std::function<std::string()> customReasoningEffort;
  // Optional per-request budget for the custom HEAD in ms; 0 = the 120 s head
  // default (CUST_HEAD_TIMEOUT_MS). The Z.ai heads set kZaiHeadTimeoutMs.
  uint32_t customHeadTimeoutMs = 0;

  // Cumulo router HEAD override (CUM-242 tool loop). When routerBase() is
  // non-empty, the three native head-turn wire helpers (antRequest / the OpenAI
  // Responses exchange / mistralRequest) rewrite their request to travel THROUGH
  // the router instead of the direct provider host: host = bare routerBase(),
  // scheme/port taken from routerBase (http:// = plain 80, else TLS 443), path =
  // "/router/<upstream>" + path, and the client auth (x-api-key / Bearer) is
  // replaced by a single "Authorization: Bearer routerKey()" (the router injects
  // the upstream's real key server-side and drops the client's). This lets a
  // Cumulo-only device run the SELECTED upstream's native tool loop over the
  // router with no per-provider loop duplication. Unset/"" = direct provider
  // hosts, byte-identical to the pre-router wire (so every existing wire test and
  // every direct-key turn is untouched). Only the cumulo head closure sets these,
  // on a pd it builds per turn - deviceProviderDeps() leaves them null, so a
  // direct anthropic/openai/mistral turn on a device that ALSO holds a Cumulo key
  // is never silently rerouted.
  std::function<std::string()> routerBase;
  std::function<std::string()> routerKey;

  // Provider-side connector/MCP attach (device: connectors::attach* append tool
  // entries to the request doc). Nullable = none. attachOpenAI rides the OpenAI
  // Responses head + sub-agent dispatch; attachMistral the Conversations
  // single-shot head; attachAnthropic the managed-agent CREATION body
  // (mcp_servers[], sub-agents only).
  std::function<void(JsonDocument&)> attachOpenAI;
  std::function<void(JsonDocument&)> attachMistral;
  std::function<void(JsonDocument&)> attachAnthropic;

  // Platform (loop deadline/heap re-gate). nowMs required for loop turns.
  std::function<uint32_t()> nowMs;
  std::function<uint32_t()> freeHeap;
  // Optional: largest contiguous free INTERNAL block (fragmentation trace for the
  // per-round headloop log - free-vs-largest divergence IS fragmentation).
  std::function<uint32_t()> largestBlock;
  // Optional: block the calling task for ms (device: vTaskDelay - yields, no new
  // task). Used to let the internal heap recover before the one same-host resend
  // of a request the device could not finish sending (CUM-477). Null = no wait:
  // the resend runs only if the heap is already back past the round floor.
  std::function<void(uint32_t)> delayMs;

  // JsonDocument allocator for the big loop docs (device: PsramJsonAllocator).
  // nullptr = ArduinoJson's default (host tests; also fine on device since
  // malloc spills to PSRAM at >=128 B, but the explicit allocator keeps the
  // node pools off internal SRAM deterministically).
  ArduinoJson::Allocator* alloc = nullptr;
};

// ---- R1: the turn's photos as image parts (nimbus/orch/turn_images.h) -----------
// Every head loop seeds its transcript with the photos of the turn in flight
// (activeTurnImages) and its renderer puts them into the seeded user message as the
// provider's image parts, or their machine transcriptions when the model cannot see
// them. On an Anthropic head with a direct key each photo is uploaded once per turn
// (the Files API) and referenced by file_id in every round; when the loop ends the
// uploads are queued for deletion, which the device runs one at a time on its idle
// step (deletePendingUploads), never inside a turn and never before the reply.
size_t pendingUploadDeletes();
// Delete up to `max` queued uploads; returns how many were settled (deleted, gone,
// or refused for good). A transient failure keeps the id for a later call.
int deletePendingUploads(const ProviderDeps& pd, int max = 1);

// ---- head-orchestrator turns (std::string space) ----------------------------
// Same contract as the engine's ProviderTurnFn: send system(instructions) +
// user(inputs), get the strict orch_turn JSON in outJson. convId is in-out
// provider continuity; tools non-null (AND toolLoopOn) => the bounded
// multi-turn tool-use loop.
bool orchTurnAnthropic(const ProviderDeps& pd, std::string& convId,
                       const std::string& instructions, const std::string& inputs,
                       std::string& outJson, std::string& err,
                       const HeadTools* tools, nimbus::orch::TokenUsage* usage);
bool orchTurnOpenAI(const ProviderDeps& pd, std::string& convId,
                    const std::string& instructions, const std::string& inputs,
                    std::string& outJson, std::string& err,
                    const HeadTools* tools, nimbus::orch::TokenUsage* usage);
bool orchTurnMistral(const ProviderDeps& pd, std::string& convId,
                     const std::string& instructions, const std::string& inputs,
                     std::string& outJson, std::string& err,
                     const HeadTools* tools, nimbus::orch::TokenUsage* usage);
// Mistral reserved-connector-name guard (web_search collides with our Tavily
// tool → 422). safeName renames on the wire; unsafeName inverts before dispatch.
std::string mistralSafeName(const std::string& n);
std::string mistralUnsafeName(const std::string& n);

// ---- per-provider STEP FACTORIES (Stage 2 phase 5) --------------------------
// Each returns a HeadStepFn performing ONE model turn on that provider, rendered
// from (and paired with) the SHARED canonical transcript - the building block of
// the engine's mid-turn failover: a failed round on host A re-runs on host B
// against the same transcript (results are data; nothing re-dispatches).
// ⚠ Lifetime: `instructions`/`ht`/`usage`/`tr` are captured BY REFERENCE - the
// caller's frame must outlive every call of the returned step. `pd` is copied.
nimbus::orch::HeadStepFn antLoopStep(const ProviderDeps& pd, const std::string& instructions,
                                     const HeadTools& ht, nimbus::orch::TokenUsage* usage,
                                     nimbus::orch::Transcript& tr);
nimbus::orch::HeadStepFn oaiLoopStep(const ProviderDeps& pd, const std::string& instructions,
                                     const HeadTools& ht, nimbus::orch::TokenUsage* usage,
                                     nimbus::orch::Transcript& tr);
nimbus::orch::HeadStepFn misLoopStep(const ProviderDeps& pd, const std::string& instructions,
                                     const HeadTools& ht, nimbus::orch::TokenUsage* usage,
                                     nimbus::orch::Transcript& tr);

// True iff the fabric loop can drive this host (anthropic/openai/mistral - the
// custom LAN backend is single-shot only). The engine consults this before
// routing a turn to the fabric; keep in sync with loop_common's makeStep.
bool fabricSupports(const std::string& host);

// The engine-owned multi-provider fabric loop (impl: providers/loop_common.cpp).
// One canonical Transcript; a failed round retries same-host once, then switches
// to the next host in `hostList` (bounded ≤2 switches), re-running the SAME
// round - executed results ride the transcript, nothing re-dispatches. `notify`
// fires once per switch (fromHost, toHost); lastFabricSwitch() says why.
// CUM-477: wrap a loop step with ONE same-host resend for a request the device
// could not finish SENDING (a short head/body write - the provider never saw it,
// so the resend duplicates nothing). Under heap pressure (free internal heap
// below cfg.roundMinHeap, the largest block below cfg.roundMinLargest, or the
// transport's "low memory" tag) it first waits, bounded, for the heap to recover
// past that floor; if it does not, the step fails with "low memory: ..." so the
// owner hears the device ran out of working memory, not "check the connection".
// The per-host loops (runOaiLoop/runAntLoop/runMisLoop - the Cumulo router head
// runs these, never the fabric) use it, and so does every host step of the fabric
// (runFabricLoop), whose own same-host retry and switch come after it.
// The caller keeps the context (and what it points at) alive for the whole loop;
// the returned step holds only the pointer.
struct ResendCtx {
  const ProviderDeps* pd;
  const nimbus::orch::HeadLoopConfig* cfg;
  const nimbus::orch::HeadStepFn* inner;
  const char* who;
  // OUT: the last step's request was not sent and this wrapper settled it (resent
  // it once, or refused to for memory), so a caller's own same-host retry must not
  // send it again (the fabric loop).
  mutable bool settled = false;
};
nimbus::orch::HeadStepFn resendUnsentOnce(const ResendCtx* ctx);

// ---- the fabric's typed failover (lane K) --------------------------------------
// Every failed round is classified (nimbus::orch::StepErr) and the fabric acts on
// the kind (fabricRuleFor): one same-host resend only for a cause in transit
// (Transport, RateLimited, ServerError); an immediate switch for a refusal of this
// request on this host (Timeout, Rejected, Cut, Auth, Billing); neither for a
// request that is unreadable anywhere (BadRequest) or a device that could not make
// or send it (LowMemory, Unsent). A billing or auth refusal also PARKS the host:
// later turns start on the next host with no request, retry or notice spent on it,
// until its key changes, a request there succeeds, or kParkProbeMs passes (then one
// probe request; a second refusal re-parks it quietly).
//
// The park is the Chat seam of the one per-provider park (nimbus/orch/provider_park.h):
// a refusal learned on any seam (embeddings, the caption step, a spawn, speech, the
// download scan) parks the host here too, and the state is kept on flash across a
// restart. A host closed by a refusal (out of credit, or a refused key) gets no
// request outside its re-check or a key change: with no open host the turn ends with
// parkedRefusalError. A PAUSED host (the pause switch: the owner, the spend ceiling)
// is never asked at all.
inline constexpr uint32_t kParkProbeMs = nimbus::orch::ProviderPark::kRecheckMs;
// Why a host is parked (hostParked). kind None = not parked; kind Paused = the pause
// switch holds it (`paused` = its causes, provider_park.h PauseCause).
struct HostPark {
  nimbus::orch::StepErr kind = nimbus::orch::StepErr::None;
  int httpStatus = 0;
  uint8_t paused = 0;
};
// Closed now? `why` (optional) receives the refusal or pause that closed it. A host
// whose key changed since it was parked is open again (a new start).
bool hostParked(const ProviderDeps& pd, const std::string& host, HostPark* why = nullptr);
void parkHost(const ProviderDeps& pd, const std::string& host, nimbus::orch::StepErr kind,
              int httpStatus);
// A failed round there: parks on a billing or auth refusal; re-arms an existing
// park when the failed round was its probe (whatever the failure).
void noteHostFailed(const ProviderDeps& pd, const std::string& host, nimbus::orch::StepErr kind,
                    int httpStatus);
void unparkHost(const std::string& host);   // a request there succeeded
void clearHostParks();                      // tests: every park, latch and pause

// The last provider switch the fabric made (or a parked head it started past),
// read by the engine's notify callback for the owner notice. Single-task by
// design, so the record read inside notify is exact.
struct FabricSwitch {
  std::string from, to;
  nimbus::orch::StepErr kind = nimbus::orch::StepErr::None;
  int httpStatus = 0;
  bool parkedStart = false;   // the turn started past a parked head: nothing new to tell
};
const FabricSwitch& lastFabricSwitch();

// Test rigs only (host tests): pin the record the engine's notify callback reads
// inside the notify call, so a scripted fabric can say WHY a host changed without
// the real provider loop. Single-task exact, like the record it replaces.
void setTestFabricSwitch(const FabricSwitch& sw);

// Test builds (the bench billing leg): every request to API host `host`
// ("api.anthropic.com") is answered locally with `status` and `body` - nothing is
// sent - until cleared. The test console wires it. Defined only in test and host
// builds (NIMBUS_TEST / NIMBUS_NATIVE): the production image has neither the
// setter nor the check. Paired with setTestFirstAttemptCap (output_caps.h). Both
// return true when the override took effect: one signature for every declaration
// (a void here and a bool in another header is two functions with one mangled name).
bool setTestStatusOverride(const std::string& host, int status, const std::string& body);
bool clearTestStatusOverride();
// The override status for "the request could not be sent": the first write left 0
// bytes (L19-14), so the step fails Unsent with no request to the provider.
inline constexpr int kTestUnsentStatus = -1;
bool testStatusOverride(const std::string& host, int& status, std::string& body);

// Spend (lane T): each host bills into its own counter, and on EVERY exit each host
// that billed is filed into `spend` once, under the model configured for it then and
// its own served echo - never one sum filed under the last host. No default on
// purpose: a caller still passing a TokenUsage* fails to compile. Every failed
// attempt is reported to ht.onStepError with its host, including the early exits for
// a kind that is neither retried nor switched (BadRequest, LowMemory); a parked head
// the turn starts past was not asked, so it reports nothing. `err` stays the LAST
// error, as the classifiers expect.
bool runFabricLoop(const ProviderDeps& pd, const std::vector<std::string>& hostList,
                   const std::string& instructions, const std::string& inputs,
                   std::string& outJson, std::string& err, const HeadTools& ht,
                   nimbus::orch::TurnSpend* spend,
                   const std::function<void(const std::string& fromHost,
                                            const std::string& toHost)>& notify);
// Normalize a tool_call id to chat-completions' required 9-alphanumeric shape.
// Mistral's own ids pass through; a FOREIGN id (an Anthropic/OpenAI id in a
// transcript carried over by mid-turn failover) maps to a deterministic 9-char
// digest - applied consistently to a call and its paired tool message.
std::string mistralCallId(const std::string& id);
// NEW (Stage H): the head on a custom/LAN endpoint - ONE single-shot structured
// turn over the OpenAI chat-completions dialect (response_format json_schema
// when the backend accepts it, or json_object when pd.customJsonMode says so;
// on a 400 it retries once schema-less with a JSON-only nudge, leaning on the
// engine's lenient parseHeadReply + salvage). Each request may take up to the
// head budget: pd.customHeadTimeoutMs, else 120 s (the OpenAI/Anthropic ceiling).
// The Z.ai heads share this path with their own 50 s budget (kZaiHeadTimeoutMs).
// v1 has NO tool loop (`tools` advertises nothing; when the engine passes it,
// its round floor drives the one resend of a request that never left, CUM-477);
// customConv "anthropic" is refused (the head speaks chat-completions only).
// Registered in orchestrator.cpp ONLY when store::hasCustom().
// A request that reached the provider but got no answer inside the budget fails
// with err "timeout: ..." (the engine reads it as a Timeout - no same-host retry,
// an honest "did not answer in time" notice); any other transport failure is
// "network".
bool orchTurnCustom(const ProviderDeps& pd, std::string& convId,
                    const std::string& instructions, const std::string& inputs,
                    std::string& outJson, std::string& err,
                    const HeadTools* tools, nimbus::orch::TokenUsage* usage);

// ---- one provider time policy (lane K) ------------------------------------------
// Every request is non-streamed: nothing arrives until the model has finished, so
// the socket deadline is the whole round. One table for every head and every
// synchronous sub (a test walks every adapter against it). Mistral ran at 60 s
// while every other provider had 120 s, and a slow Mistral round read as a dead
// network, was retried for another 60 s, then failed over.
inline constexpr uint32_t kHeadRoundCeilingMs = 120000;   // anthropic, openai, mistral, custom heads
inline constexpr uint32_t kSyncSubCeilingMs   = 120000;   // a sub that runs inside its POST
inline constexpr uint32_t kMinRoundMs         = 8000;     // a near-exhausted final round still gets a shot

// ---- Z.ai (GLM) wire policy (CUM-544) ---------------------------------------
// ONE place for every Z.ai request path: the direct BYOK head, the Cumulo
// router's zai upstream head, and both sub-agent dispatches. GLM always reasons;
// "low" bounds it (the owner has no effort knob - none exists to reuse, and NVS
// keys are frozen). The head budget sits under Z.ai's ~60 s gateway cut, and a
// timeout fails over without a same-host retry, so an owner waits about 50 s
// before the fallback provider answers - never the old 2 x 120 s.
inline constexpr const char* kZaiReasoningEffort = "low";
inline constexpr uint32_t    kZaiHeadTimeoutMs   = 50000;
// The reasoning effort a request to this upstream/backend must carry ("" = none).
// "zai" -> kZaiReasoningEffort; every other name -> "".
const char* reasoningEffortFor(const std::string& upstream);
// Apply the Z.ai head policy to a pd already pointed at a Z.ai endpoint:
// json_object mode (CUM-495), kZaiReasoningEffort, kZaiHeadTimeoutMs.
void applyZaiHeadPolicy(ProviderDeps& pd);
// Point pd at the Cumulo router's single-shot chat-completions head for an
// upstream the native fabric does not drive (zai, or any unknown name):
// base = pd.routerBase(), path prefix "/router/<upstream>" (no /v1 - the zai
// upstream path has none), key = pd.routerKey(), convention openai, json_object
// mode, the given bare model, and the Z.ai policy when upstream is "zai".
void configureRouterCompatHead(ProviderDeps& pd, const std::string& upstream,
                               const std::string& model);

// ---- sub-session dispatch/poll/cancel/answer --------------------------------
// Anthropic Managed Agents (cloud sandbox sessions; env/agent cached via the
// ant* closures). defaultModel = the device's ANT_MODEL fallback. The cached ids
// are loaded once per boot into RAM and written through to the closures; a
// write that does not read back (a full NVS partition) is logged and RAM keeps
// serving the id until restart, so no spawn re-creates them (CUM-481). A non-2xx
// on any call of the spawn chain logs the provider's reason (<= 150 chars) and
// classifies: 429 -> RateLimited (+ Directive::rateLimit), 401/403 -> Auth,
// 404 -> NotFound, other 4xx -> BadRequest, 5xx -> RemoteFail (CUM-474). A
// session create that names a cached id dead (404, or a 400 "... is archived")
// drops both cached ids so the next dispatch re-creates them.
FabricErr antDispatch(const ProviderDeps& pd, const char* defaultModel,
                      const Directive& d, char outJobId[72]);
FabricErr antPoll(const ProviderDeps& pd, const char* jobId, ResultEnvelope& env);
FabricErr antCancel(const ProviderDeps& pd, const char* jobId);
FabricErr antAnswer(const ProviderDeps& pd, const char* jobId, const char* userText);
// Drop the boot-lifetime RAM copy of the managed-agent ids; the next dispatch
// reloads them from the closures. Host suites call it between cases.
void antForgetIds();

// OpenAI Responses background mode, host/key/model-parameterized (kept generic
// exactly like the pre-split openai_responses:: so a proxy could reuse it).
FabricErr oaiDispatch(const ProviderDeps& pd, const char* host, const std::string& key,
                      const char* model, const char* backend, const Directive& d,
                      char outJobId[72]);
FabricErr oaiPoll(const ProviderDeps& pd, const char* host, const std::string& key,
                  const char* backend, const char* jobId, ResultEnvelope& env);
FabricErr oaiCancel(const ProviderDeps& pd, const char* host, const std::string& key,
                    const char* jobId);

// Mistral sub-session: ONE synchronous chat/completions call, result cached in
// a RAM slot; poll serves it as Done. model = the resolved sub model.
FabricErr mistralDispatch(const ProviderDeps& pd, const std::string& model,
                          const Directive& d, char outJobId[72]);
FabricErr mistralPoll(const ProviderDeps& pd, const char* jobId, ResultEnvelope& env);
FabricErr mistralCancel(const ProviderDeps& pd, const char* jobId);

// Custom/proxy sub-session: synchronous chat-completion per the configured wire
// convention (openai/mistral -> /v1/chat/completions; anthropic -> /v1/messages).
FabricErr customDispatch(const ProviderDeps& pd, const Directive& d, char outJobId[72]);
FabricErr customPoll(const ProviderDeps& pd, const char* jobId, ResultEnvelope& env);
FabricErr customCancel(const ProviderDeps& pd, const char* jobId);

// Generic OpenAI-compatible sub-session (Z.ai GLM, Cumulo router, or any other
// chat-completions endpoint). Synchronous: dispatch runs the call inline against
// host+basePath (basePath+"/chat/completions", Bearer key) and caches the answer;
// poll returns it. jobId = "<backendTag>:<id>"; the SAME backendTag routes poll/
// cancel back here via the device adapter. A small multi-slot RAM cache lets Z.ai
// and Cumulo coexist. The endpoint is passed explicitly (not via the pd.custom*
// closures) so several such providers run at once.
// The upstream's native wire. A router (Cumulo) proxies each upstream on ITS OWN
// wire under /router/<upstream>/v1/: OpenAI/Mistral/Z.ai speak chat-completions,
// Anthropic speaks Messages. Auth is Bearer either way (the router key).
enum class CompatWire { OpenAIChat, AnthropicMessages };

struct CompatEndpoint {
  const char* host = nullptr;
  uint16_t    port = 443;
  bool        tls = true;
  std::string basePath;     // e.g. "/api/paas/v4" or "/router/anthropic/v1"
  std::string key;          // Bearer token ("" = no auth header)
  std::string model;
  const char* backendTag = "compat";  // jobId prefix + adapter route key
  CompatWire  wire = CompatWire::OpenAIChat;
  // "" = no field; else "reasoning_effort":"<value>" on the chat-completions body
  // (the Z.ai sub-agents: reasoningEffortFor("zai"), CUM-544).
  std::string reasoningEffort;
};
FabricErr openaiCompatDispatch(const ProviderDeps& pd, const CompatEndpoint& ep,
                               const Directive& d, char outJobId[72]);
// The read deadline of that one POST: kZaiHeadTimeoutMs for a Z.ai sub (backend
// "zai", or the router's zai upstream at basePath /router/zai), else 120 s.
uint32_t compatSubDeadlineMs(const CompatEndpoint& ep);
FabricErr openaiCompatPoll(const ProviderDeps& pd, const char* backendTag, const char* jobId,
                           ResultEnvelope& env);
FabricErr openaiCompatCancel(const ProviderDeps& pd, const char* jobId);

}  // namespace providers
}  // namespace agent
