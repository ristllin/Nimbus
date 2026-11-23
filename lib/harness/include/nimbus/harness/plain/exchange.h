#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/harness/http.h"
#include "nimbus/harness/plain/deliver.h"   // StatusLine: the one status line of a failed exchange
#include "nimbus/harness/plain/wire.h"
#include "nimbus/orch/head_qual.h"

namespace agent {
struct Platform;
}  // namespace agent

// plain/exchange.h - one exchange (PRD section 7): render, post through the kept
// transport, read, the output cap, the retry policy for a direct head (same-host retry
// after 2 s then 8 s on transport, 5xx and 429, at most two per round), cut handling
// (one retry at the largest cap the window allows; cut tool arguments are never
// dispatched) and the checked body writer over providers/wire.h.
//
// Seam declared by lane RB-B0a; the body (plain_exchange.cpp) is lane RB-EX's.
//
// What one call does, in order:
//   1. The output cap. The request is rendered once with the table's cap
//      (providers::tableCap), measured (text bytes and spliced photos), and rendered again
//      with main's load-aware cap for that body inside the round's time
//      (providers::outputCapFor with a RoundLoad): RenderInput::maxTokens is the
//      exchange's, whatever the caller set. A round with tool choice none is a final round.
//   2. The budget. A body over the head's cap (bodyCap) is never sent: the caller's ladder
//      climbs a rung (climb(Climb::OverCap)) and the body is rendered again, until it fits
//      or no rung is left (then ContextRefused with httpStatus 0: nothing was sent). The climb
//      is told the body's size and the cap (ClimbAsk).
//   3. The body check: every JSON string of the body at most 60,000 B and the whole body
//      valid UTF-8. A wire that breaks it is refused before the send (InvalidArgument):
//      plain_body.h's writer keeps a wire inside it.
//   4. The post, with the read window bounded by the turn's remaining soft wall clock, and
//      the error class of the answer:
//        transport (no answer), 5xx, 429: the same request again after 2 s, then after
//          8 s, at most two such retries in the exchange, each only when the wait and a
//          request still fit the wall clock; then one plain line;
//        context refused (413, or a 400 or 422 naming the context): the ladder climbs
//          (climb(Climb::Refused): the caller goes to rung 3) and the request is rendered
//          and sent once more; a second refusal is one plain line;
//        billing or key refused (401, 402, 403, a billing marker): parkHost, one plain line;
//        a 2xx that does not parse: Malformed, one plain line;
//        a cut answer (StopReason::MaxTokens): one retry at the retry cap (more room, and a
//          lower effort where the table says so) when the window allows one, else or when
//          the retry is cut too: the text is kept, every call of the cut answer is marked
//          incomplete (never dispatched), err Cut and the CutAnswer status line;
//        anything else: one plain line.
//      A routed head follows the router refusal contract (router_refusals.h): the clamp
//      resends once with the output cap the router names, a short retry_after waits and
//      resends once, park_billing and park_auth park, fail_over sets failOver (the caller's
//      ladder moves to its next qualified target), request_too_large is a context refusal.
//   5. Every request of the exchange renders the same stored state, so a retry never runs
//      a tool again: the exchange dispatches nothing, and the caller stores and dispatches
//      only what the returned step holds.
//
// Status: ok when `out` holds the exchange's outcome (out.err says how it ended, None when
// out.step is the model's answer); InvalidArgument for missing inputs or a body that
// breaks the check; the render's own status when the wire refused to render.
namespace agent {
namespace plain {

// The error class of an exchange. Slugs ride the e item: frozen.
enum class ExchangeErr : uint8_t {
  None = 0,
  Transport = 1,       // connect, TLS, timeout
  Server = 2,          // 5xx
  RateLimited = 3,     // 429
  ContextRefused = 4,  // the provider refused the size: ladder rung 3 and one retry
  KeyRefused = 5,      // billing or key: park the host
  Cut = 6,             // the answer was cut after its retry
  Malformed = 7,       // the answer did not parse
  Other = 8,           // anything else: one plain line
  Blocked = 9,         // a moderation block (the router's): the moderation status line
};
inline const char* exchangeErrSlug(ExchangeErr e) {
  switch (e) {
    case ExchangeErr::None: return "ok";
    case ExchangeErr::Transport: return "transport";
    case ExchangeErr::Server: return "server";
    case ExchangeErr::RateLimited: return "rate-limited";
    case ExchangeErr::ContextRefused: return "context";
    case ExchangeErr::KeyRefused: return "key";
    case ExchangeErr::Cut: return "cut";
    case ExchangeErr::Malformed: return "malformed";
    case ExchangeErr::Other: return "other";
    case ExchangeErr::Blocked: return "blocked";
  }
  return "other";
}

// The retry policy's numbers (PRD section 7).
inline constexpr uint32_t kRetryWaitMs[] = {2000, 8000};   // before the first and the second retry
inline constexpr uint8_t kMaxSameHostRetries = 2;          // transport, 5xx, 429 together
// A retry is taken only when, after its wait, at least this much of the wall clock is
// left for the request: a request with less time cannot be answered, it only spends.
inline constexpr uint32_t kMinRequestMs = 5000;
// How many rungs one exchange may climb for a body over its cap (the ladder has four).
inline constexpr uint8_t kMaxCapClimbs = 5;   // and compaction first (lane RB-C3, ruling 105 item 3)

// Why the ladder is asked to climb one rung.
enum class Climb : uint8_t {
  OverCap = 0,   // the rendered body is over the head's body cap: the next rung
  Refused = 1,   // the provider refused the size: rung 3 (PRD section 7)
};
// What the ladder is told: why, and for a body over its cap the body's size and the cap, so a
// rung that omits frees only what the request needs (ruling 44). A refusal knows no size:
// bodyBytes is 0 there.
struct ClimbAsk {
  Climb why = Climb::OverCap;
  size_t bodyBytes = 0;   // the rendered body, photos included (0: not known)
  size_t bodyCap = 0;     // the cap it must fit
};
// The caller's ladder: climb one rung by changing the state its RenderInput renders (the
// stub-through point, the omission span); false when no rung is left. The exchange renders
// again after every true.
using ClimbFn = std::function<bool(const ClimbAsk& ask)>;
// A request refused for something it named that the provider no longer holds (a photo's
// file id that expired or is unknown: HTTP 400 or 404 naming a file). The caller forgets it
// (imageparts::forgetRefusedFileId) and answers true: the exchange renders once more, the photo
// going inline or as its stub, so the turn never fails on an id. False: the refusal stands.
using RefusedRefFn = std::function<bool(int httpStatus, const std::string& body)>;

struct ExchangeDeps {
  HttpTransport* transport = nullptr;
  const Platform* platform = nullptr;   // nowMs and delayMs are required
};

struct ExchangeRequest {
  const WireCodec* wire = nullptr;
  const RenderInput* render = nullptr;   // turn and head are required; maxTokens is the exchange's
  uint32_t timeoutMs = 0;      // the host's read window (Anthropic 120 s, ...); 0 = readWindowMsFor
  uint32_t deadlineMs = 0;     // the turn's remaining soft wall clock; required
  uint32_t bodyCap = 0;        // the head's body cap (tx::budgetFor); 0 = its released row (requestBudgetFor)
  ClimbFn climb;               // the overflow ladder; empty = no rung to climb
  RefusedRefFn refusedRef;     // a refused file reference, once per exchange; empty = none
};

struct ExchangeResult {
  ParsedStep step;             // the answer; usage is the sum of every answered request
  ExchangeErr err = ExchangeErr::None;
  int httpStatus = 0;          // the last answer's status (0: no answer, or nothing sent)
  uint8_t retries = 0;         // requests sent beyond the first
  uint8_t requests = 0;        // requests sent (head requests for the e item)
  uint8_t climbs = 0;          // ladder rungs climbed in this exchange
  uint32_t requestBytes = 0;   // the last request's body, photos included
  uint32_t ms = 0;             // the whole exchange, waits included
  uint32_t waitedMs = 0;       // of which waits before retries
  uint32_t maxTokens = 0;      // the output cap the last request carried (0 = none sent)
  bool cutRetried = false;     // a cut answer was asked again with the retry cap
  bool failOver = false;       // the router said fail over: the ladder's next target
  bool parkHost = false;       // billing or key refused: the caller parks the host
  StatusLine status = StatusLine::None;   // CutAnswer or ProviderError when err != None
  std::string errorLine;       // the one plain line for the owner when err != None
  std::string providerBody;    // the provider's own refusal body (first 4 KB) when err is Other (lane E2-FIX7)
  nimbus::orch::tx::ExchangeFacts ref;   // the last request sent, by reference (RB-E3, recordRequest)
};

Status runExchange(const ExchangeDeps& deps, const ExchangeRequest& req, ExchangeResult& out);

// ---- qualification: who may answer instead (PRD sections 7 and 9) ------------------------
// One head a fallback order or the router ladder names.
struct HeadTarget {
  std::string host;            // the qualification table's provider slug ("anthropic", ...)
  std::string model;
  nimbus::orch::headqual::Wire wire = nimbus::orch::headqual::Wire::Any;
  bool parked = false;         // the provider park holds this host
};

// True when the table qualifies `t` on `line` (assumed and held rows do not).
bool targetQualified(const HeadTarget& t, nimbus::orch::headqual::Line line);

struct FallbackFacts {
  bool freshAttempt = false;   // the turn's attempt has not sent its first request
  bool effectRun = false;      // an effect tool has run in this turn
  bool headParked = false;     // the turn's head is parked
  nimbus::orch::headqual::Line line = nimbus::orch::headqual::Line::E2;
  const std::vector<HeadTarget>* order = nullptr;   // the fallback order after the head
};
// The fallback host that answers instead of a parked head: only a fresh attempt with no
// effect run, only a target that is not parked and that the table qualifies on `line`;
// index -1 when none may (the owner gets one plain line and no model is called).
Status pickFallback(const FallbackFacts& facts, int& index);

// The router ladder's targets the table qualifies on `line`, in their order; the rest are
// skipped.
Status qualifiedTargets(const std::vector<HeadTarget>& ladder, nimbus::orch::headqual::Line line,
                        std::vector<HeadTarget>& out);

// ---- the parts of the policy, for the tests and the e item -----------------------------
// The class of an answer that is not a usable 2xx: HTTP status and body, routed or not
// (ExchangeErr::None for a 2xx; Transport for status 0). A routed answer that names a
// reason of the router contract is classed by that reason, never by its status.
ExchangeErr classifyAnswer(int status, const std::string& body, bool routed);
// The provider slug of a turn's head (lane RB-T4b): TurnCtx::host when it is a slug, else a known API
// host (the endpoint's, else the turn's), else "cumulo" when routed, else the configured dialect:
// Anthropic Messages "anthropic" and OpenAI Responses "openai" (`byDialect` set: a fallback the engine
// logs), a compatible dialect "custom" (what a custom endpoint is configured as). Empty when the turn
// names no host and the endpoint none: nothing to size. The budget and the exchange's caps read it.
std::string headSlugOf(const TurnCtx& turn, const HeadEndpoint& head, bool* byDialect = nullptr);
// The provider an owner's line names: headSlugOf, but "custom" where only the dialect named it (a
// proxy is not the provider).
std::string providerSlugOf(const TurnCtx& turn, const HeadEndpoint& head);
// The owner's plain line for an exchange that ended with `err` (empty for None).
std::string exchangeLine(ExchangeErr err, const std::string& providerSlug, int httpStatus);

}  // namespace plain
}  // namespace agent
