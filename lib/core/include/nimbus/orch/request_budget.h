#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// request_budget - what one request to a head may carry: its model's window, the prompt
// it aims at, the bytes per token that turn tokens into body bytes, the body budget it
// is refused over, the round's deadline and the rates its time fits at. Declared by lane
// P0 of the history fix (16 Nov 2026) with v5.1.1's figures; lane B derives them per host
// and model from one formula (plan v3 protocols, RULINGS-v3). The host and model that
// send a request decide its budget. Pure functions: nothing reads them until lanes W, F
// and H switch their consumers. Arduino-free, host-tested (test_request_budget, which
// also writes docs/reference/request-budgets.md).
//
// The formula, per (host, model, routed):
//   window   headWindowTokens (the listed window, else the corrected table);
//   ratios   rNom bytes per token (anthropic 2.5, openai 3.6, mistral-large-4 3.6, other
//            Mistral 3.1, GLM 3.3, custom 3.0) and the dense rSafe (Claude 2.0, else 2.5);
//   T_lat    the largest T (1k steps) with connect 1.5 s + T x rNom at 70,000 B/s + the
//            fragmented-heap pace (2 s + 0.1 s per 4 KB slice, no deadline/10 cap)
//            + T / prefill floor + minFinal / decode floor at T  <=  the read window
//            (a Server link, nimbusd, has no upload or pace term);
//   bytes    floor to 1,000 of min(100,000 x rNom, T_lat x rNom, 0.75 x window x rSafe,
//            0.9 x price tier x rSafe), never under min(85,000, 0.85 x window x rSafe);
//            a custom endpoint, and a routed head while kRoutedBudgetLift is false, gets
//            exactly that floor; a head held at v5.1.1 (its live leg failed) gets 85,000;
//   target   bytes / rNom.
// A tighten-only memo of observed bytes per token (noteObservedRatio) can only lower the
// window and tier bounds.
namespace nimbus {
namespace orch {

// The one body budget every wire has in v5.1.1 (providers::kWireBodyMax; test_engine_seams
// pins the two equal) and the floor no head goes under (RULINGS-v3).
inline constexpr uint32_t kLegacyWireBodyBytes = 85000;

// Cumulo-routed heads above the floor (and the cumulo ledger's cache split, usage_ledger)
// wait for the router to meter cached tokens (cumulo CUM-R1, verified on prod). v5.1.2
// ships false: routed heads keep the 85,000 B floor (RULINGS-v3 release shape).
inline constexpr bool kRoutedBudgetLift = false;

// Ruling 1: no target above 100k tokens.
inline constexpr uint32_t kBudgetTargetClampTokens = 100000;

// The device link the fits assume (plan v3 protocols; A10 measures it on the bench).
inline constexpr uint32_t kBudgetConnectMs = 1500;         // connect and TLS
inline constexpr uint32_t kBudgetUploadBytesPerS = 70000;  // Lumi's heaptrace gives 74 KB/s or better
// The fragmented-heap pace of one body: the transport's per-request base and its stall
// window per 4 KB slice (http.h kRequestPaceMaxMs, kSliceStallMs; exchange_trace.h
// kBodySliceBytes; test_output_caps pins them equal), with no deadline/10 cap: the
// prediction counts every slice (RULINGS-v3 item 1).
inline constexpr uint32_t kBudgetPaceBaseMs = 2000;
inline constexpr uint32_t kBudgetPaceSliceMs = 100;
inline constexpr uint32_t kBudgetPaceSliceBytes = 4096;
// A routed request's read window: the device gives up before the router's 95 s egress cut.
inline constexpr uint32_t kRoutedReadWindowMs = 90000;

// Where the budget's request is sent from. Device: an ESP32 on Wi-Fi (upload at 70 KB/s,
// fragmented-heap pacing). Server: nimbusd, a Linux VN with neither (RULINGS-v3: nimbusd
// budgets use no upload-rate or pacing terms).
enum class BudgetLink : uint8_t { Device = 0, Server };
// The process's link: Device unless nimbusd installs Server at start.
void installBudgetLink(BudgetLink link);
BudgetLink budgetLink();

// Which bound set the bytes.
enum class BudgetBound : uint8_t { Clamp = 0, Latency, Window, Tier, Floor, Held };
const char* budgetBoundSlug(BudgetBound b);   // "clamp" | "latency" | "window" | "tier" | "floor" | "held"

struct RequestBudget {
  uint32_t windowTokens = 0;     // the head model's input window (headWindowTokens)
  uint32_t targetTokens = 0;     // the prompt the budget aims at: wireBodyBytes / rNom
  uint16_t rNomX100 = 400;       // nominal body bytes per token, x100
  uint16_t rSafeX100 = 400;      // dense (worst-case) body bytes per token, x100
  uint32_t wireBodyBytes = kLegacyWireBodyBytes;   // a body over this is refused before the send
  uint32_t roundMs = 0;          // the round's socket deadline (routed: at most 90 s)
  uint32_t prefillTokPerS = 0;   // the prompt's slowest read rate (the prefill floor)
  uint32_t decodeTokPerS = 0;    // the answer's slowest write rate at the target's context
  bool routed = false;           // the Cumulo router relays the request
  bool flat = false;             // the flat single-string wire (the custom endpoint)
  // Lane B's derivation (P0 declared the fields above).
  uint32_t readWindowMs = 0;     // the effective read window every fit uses (gateway, router)
  uint32_t minFinalTokens = 0;   // the answer a round must be able to carry
  uint32_t latencyTokens = 0;    // T_lat, walked up to the 100k clamp (0 = no 1k step fits)
  uint32_t tierTokens = 0;       // the model's price tier (a prompt over it bills higher); 0 = none
  uint32_t floorBytes = 0;       // min(85,000, 0.85 x window x rSafe)
  BudgetBound bound = BudgetBound::Floor;
  BudgetLink link = BudgetLink::Device;
  uint32_t v511CtxTokens = 0;    // v5.1.1's window for the head (legacyCtxTokens): the no-shrink baseline
  uint32_t v511ReplayBytes = 0;  // the sending wire's v5.1.1 replay cap (Anthropic 16,384, else 24,576)
};

// The budget of a request `host` sends on `model` (`routed`: through the router; host
// "cumulo" is always routed and reads its selector's upstream). The installed link.
RequestBudget requestBudgetFor(const std::string& host, const std::string& model, bool routed);
// The same on an explicit link (tests and the generated table).
RequestBudget requestBudgetFor(const std::string& host, const std::string& model, bool routed,
                               BudgetLink link);
// The same with the routed lift given: what a routed head gets once kRoutedBudgetLift
// flips (tests and the generated table; every consumer uses the forms above).
RequestBudget requestBudgetFor(const std::string& host, const std::string& model, bool routed,
                               BudgetLink link, bool routedLift);

// ---- the rates and the round's time (shared with output_caps' load-aware fit) --------
// Each takes the sending host and model; a router selector or an upstream-prefixed id
// reads its upstream's figures.
uint32_t prefillFloorTokPerS(const std::string& host, const std::string& model);
// The slowest decode at `contextTokens` of prompt: Medium and Small 130 tok/s up to 32k
// falling linearly to 70 at 64k and above; GLM 55 to 48 the same way; Large 4 a flat 50
// (15 Nov measurements); Sonnet 75, Haiku 120, Opus 50, other Claude 40; OpenAI and
// custom 50 (the target check only: they are never capped).
uint32_t decodeFloorTokPerS(const std::string& host, const std::string& model, uint32_t contextTokens);
// 3,000 tokens; Z.ai 1,600; a forced-reasoning Mistral head (Large 4, GLM on Mistral) 1,500.
uint32_t minFinalTokensFor(const std::string& host, const std::string& model);
// Mistral 60 s (its gateway), Z.ai 50 s, every other host 120 s; routed: at most 90 s.
uint32_t readWindowMsFor(const std::string& host, const std::string& model, bool routed);
// A round's time before its first output token: connect, the upload of `bodyBytes`
// `uploads` times (a router resend after a 402 clamp uploads twice), the pace of each
// upload when `fragmented`, and the prefill of `uncachedTokens`. A Server link has no
// upload or pace term.
uint32_t roundOverheadMs(BudgetLink link, size_t bodyBytes, bool fragmented, uint32_t uncachedTokens,
                         uint32_t prefillTokPerS, uint8_t uploads = 1);
// The fragmented-heap pace of one body (no deadline cap).
uint32_t predictedPaceMs(size_t bodyBytes);

// ---- the tighten-only memo (RAM, per host and model) ---------------------------------
// After a 2xx answer the wire reports the text body it sent and the prompt tokens billed
// (photo tokens taken out). A ratio under the head's rSafe lowers its window and tier
// bounds from then on; a higher one never raises anything. Bodies under 2,048 prompt
// tokens are ignored (their JSON framing dominates), and the memo never goes under
// 1.5 B/token (a malformed usage object cannot shrink a budget to nothing).
void noteObservedRatio(const std::string& host, const std::string& model, size_t textBytes,
                       uint32_t promptTokens);
uint16_t observedRatioX100(const std::string& host, const std::string& model);   // 0 = none
void clearObservedRatios();   // tests

}  // namespace orch
}  // namespace nimbus
