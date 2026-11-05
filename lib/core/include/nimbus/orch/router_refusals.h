#pragma once
#include <cstdint>
#include <string>

// router_refusals - what the Cumulo router means when it does not serve a call, and what
// a device does about it. The table mirrors the cloud repo's router refusal contract
// (docs-internal/router-refusal-contract.md, packages/shared/src/router-refusals.ts);
// test/test_router_refusal_contract walks it against the contract's exact bodies.
//
// The rules, in the contract's words:
//   - switch on `reason`, never on the HTTP status: a 402 cannot separate an empty wallet
//     from a call too large for the wallet, a 429 a throttled key from an upstream out of
//     quota;
//   - park Cumulo only for park_billing or park_auth; everything else leaves it usable;
//   - an unknown reason is a per-call failure, never a park.
namespace nimbus {
namespace orch {

enum class RouterAction : uint8_t {
  None = 0,      // no reason, or one this image does not know: a per-call failure
  FixRequest,    // do not resend it unchanged; fail this call or change the request
  RetryLater,    // the same request after retryAfterMs
  RetryCapped,   // the clamp: resend once with the output cap lowered
  FailOver,      // cool this upstream, use the next provider or seam
  ParkBilling,   // park as billing, tell the owner once
  ParkAuth,      // park as auth, tell the owner once
};
const char* routerActionSlug(RouterAction a);

struct RouterReasonRow {
  const char* reason;
  int http;
  bool retryable;
  RouterAction action;
};

// Every reason the contract names. Exposed so a test can walk them.
inline constexpr RouterReasonRow kRouterReasons[] = {
    {"missing_router_key", 401, false, RouterAction::ParkAuth},
    {"invalid_router_key", 401, false, RouterAction::ParkAuth},
    {"unknown_provider", 404, false, RouterAction::FixRequest},
    {"router_not_configured", 501, false, RouterAction::FailOver},
    {"rate_limited", 429, true, RouterAction::RetryLater},
    {"request_too_large", 413, false, RouterAction::FixRequest},
    {"invalid_request", 400, false, RouterAction::FixRequest},
    {"router_internal_error", 500, true, RouterAction::RetryLater},
    {"router_disabled", 503, true, RouterAction::FailOver},
    {"provider_disabled", 503, true, RouterAction::FailOver},
    {"ceiling_reached", 429, true, RouterAction::FailOver},
    {"funding_cap_reached", 402, true, RouterAction::FailOver},
    {"endpoint_not_allowed", 403, false, RouterAction::FixRequest},
    {"invalid_json", 400, false, RouterAction::FixRequest},
    {"invalid_body", 400, false, RouterAction::FixRequest},
    {"agents_blocked", 400, false, RouterAction::FixRequest},
    {"inline_tools_only", 400, false, RouterAction::FixRequest},
    {"model_not_priced", 403, false, RouterAction::FixRequest},
    {"native_tool_not_metered", 403, false, RouterAction::FixRequest},
    {"audio_duration_unknown", 422, false, RouterAction::FixRequest},
    {"unsupported_media_type", 415, false, RouterAction::FixRequest},
    {"streaming_not_supported", 400, false, RouterAction::FixRequest},
    {"account_suspended", 403, false, RouterAction::ParkAuth},
    {"credits_or_subscription_required", 402, false, RouterAction::ParkBilling},
    {"insufficient_credits", 402, false, RouterAction::ParkBilling},
    {"insufficient_credits_for_call", 402, false, RouterAction::RetryCapped},
    {"credits_held_in_flight", 429, true, RouterAction::RetryLater},
    {"budget_exceeded", 402, false, RouterAction::ParkBilling},
    {"upstream_rate_limited", 429, true, RouterAction::FailOver},
    {"upstream_quota_exhausted", 429, true, RouterAction::FailOver},
    {"upstream_auth_failed", 502, true, RouterAction::FailOver},
    {"upstream_error", 502, true, RouterAction::FailOver},
    {"fallback_exhausted", 502, true, RouterAction::FailOver},
    {"provider_timeout", 504, true, RouterAction::FailOver},
    {"provider_unreachable", 502, true, RouterAction::FailOver},
};

// The row of `reason`, null when the table has none.
const RouterReasonRow* routerReasonRow(const std::string& reason);

// One refusal, as the router wrote it.
struct RouterAnswer {
  std::string reason;                 // "" = the answer names none (a provider's own body)
  const RouterReasonRow* row = nullptr;   // null = no reason, or one this image does not know
  bool retryable = false;
  uint32_t retryAfterMs = 0;          // 0 = none given
  int64_t maxAffordableOutputTokens = -1;   // -1 = none given; 0 = no cap would fit
  RouterAction action() const { return row ? row->action : RouterAction::None; }
};

// Reads a refusal from anything a seam holds of it: the JSON body (the top-level `reason`,
// else a top-level string `error`, as the router writes both), a raw HTTP answer (headers
// then that body), or a device error line ("HTTP 402 insufficient_credits_for_call"),
// where only a whole word of the table counts, so a provider echoing a prompt that
// quotes a reason inside its message reads as no reason.
RouterAnswer readRouterAnswer(const std::string& text);

// A refusal that shows the account stands: the router reached the wallet and held it
// funded, or passed it and failed further on. It ends a billing or key park whatever else
// it says about this one call, so a parked seam never waits on a call that could be
// served. (rate_limited, router faults and admin pauses come before the wallet and show
// nothing either way.) The router reads a key's own monthly budget after the wallet, so
// the clamp and a held wallet say nothing about it: with `keyBudgetPark` only an answer
// from past the budget counts (an upstream's failure).
bool routerAnswerProvesAccount(const RouterAnswer& a, bool keyBudgetPark = false);

// The smallest output a capped resend may ask for (the contract's floor for a fold, and
// the headroom of the shortest head reply). Below it the call fails.
inline constexpr uint32_t kRouterCapFloorTokens = 256;
// The longest a request waits on the wire for retryAfterMs. Longer fails the call now and
// the fallback ladder or the owner decides, so a turn never hangs on a router's backoff.
inline constexpr uint32_t kRouterInlineRetryMaxMs = 6000;

// Every cause a head reports for a router refusal starts with this. classifyStepError
// reads it as a per-call refusal, never as a park.
inline constexpr const char* kRouterCausePrefix = "the Cumulo router ";

// Lowers the output cap of a JSON request `body` bound for `path` to `cap` for the
// contract's capped resend: every top-level spelling already declared (the router reads
// the largest), completion_args.max_tokens on a Mistral conversation, and when none is
// declared, the spelling that path takes. A value already at or under `cap` stays. False
// when the path takes no output cap (embeddings, speech, images) or the body is not a
// JSON object. `edits` receives what moved, latest offset first, for a caller whose
// photo splices hold offsets into the body (applyCapEdits).
struct CapEdit {
  size_t at = 0;
  long delta = 0;
};
inline constexpr size_t kCapEditMax = 3;
bool capOutputTokens(std::string& body, const std::string& path, uint32_t cap, CapEdit edits[], size_t& editCount,
                     size_t editCap = kCapEditMax);
// The new offset of `at` after those edits (an offset at or past an edit moves with it).
size_t applyCapEdits(size_t at, const CapEdit edits[], size_t editCount);

}  // namespace orch
}  // namespace nimbus
