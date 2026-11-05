# Router refusal contract (for firmware)

What the Cumulo router answers when it does not serve a call, and what a device does
about it. The table below is generated from, and tested against,
`packages/shared/src/router-refusals.ts` (`apps/api/src/router-refusals.test.ts` fails if
they disagree). Tracks CUM-888, CUM-862 and CUM-569.

## Rules

1. **Switch on `reason`, never on the HTTP status.** Status 402 alone cannot separate an
   empty wallet from a call that is too large for the wallet. Status 429 alone cannot
   separate a throttled key from an upstream out of quota.
2. **Park Cumulo only for `park_billing` or `park_auth`.** Everything else leaves the lane
   usable. Only `insufficient_credits`, `credits_or_subscription_required` and
   `budget_exceeded` may produce an "add credits" or "budget reached" message.
3. **`charged` is false on every refusal.** The wallet moves only on a metered, served call.
4. **`retryable` means the same request, unchanged, can succeed later without the owner
   doing anything.** When true, the body carries `retryAfterMs` and the response carries a
   matching `Retry-After` header. Wait at least that long.
5. **A body with no `reason` is the provider's own answer** to the request (400, 404, 422),
   passed through so the device can fix the request. Treat it as a per-call failure.
6. **An unknown `reason` is a per-call failure.** New reasons may appear. Never park on one.

## Device actions

| action         | what the device does                                                                                                                                |
| -------------- | --------------------------------------------------------------------------------------------------------------------------------------------------- |
| `fix_request`  | Do not resend it unchanged and do not park. Fail this call, or change the request.                                                                  |
| `retry_later`  | Same request after `retryAfterMs`. Do not park.                                                                                                     |
| `retry_capped` | The clamp. See below. Do not park.                                                                                                                  |
| `fail_over`    | Cool this upstream for `retryAfterMs` and use the next provider or seam. The account is fine. Do not park and do not tell the owner to add credits. |
| `park_billing` | Park Cumulo as billing and tell the owner once. Re-check on the normal park interval.                                                               |
| `park_auth`    | Park Cumulo as auth and tell the owner once.                                                                                                        |

## The table

| reason                             | HTTP | retryable | device action  | meaning                                                                                                                                                 |
| ---------------------------------- | ---- | --------- | -------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `missing_router_key`               | 401  | no        | `park_auth`    | No bearer key on the request.                                                                                                                           |
| `invalid_router_key`               | 401  | no        | `park_auth`    | The bearer key is unknown or revoked.                                                                                                                   |
| `unknown_provider`                 | 404  | no        | `fix_request`  | The path names an upstream the router does not have.                                                                                                    |
| `router_not_configured`            | 501  | no        | `fail_over`    | This deployment has no provider gateway.                                                                                                                |
| `rate_limited`                     | 429  | yes       | `retry_later`  | This key passed the per-minute burst bound. The live wait is in retryAfterMs.                                                                           |
| `request_too_large`                | 413  | no        | `fix_request`  | The body is over the router's size limit, so it was not read.                                                                                           |
| `invalid_request`                  | 400  | no        | `fix_request`  | The HTTP server could not read this request, for example a malformed Content-Type.                                                                      |
| `router_internal_error`            | 500  | yes       | `retry_later`  | The router itself failed, for example a database fault. The wallet was not debited.                                                                     |
| `router_disabled`                  | 503  | yes       | `fail_over`    | An admin switched the whole router off.                                                                                                                 |
| `provider_disabled`                | 503  | yes       | `fail_over`    | An admin switched this upstream off.                                                                                                                    |
| `ceiling_reached`                  | 429  | yes       | `fail_over`    | The platform's own monthly spend ceiling was hit. Not this account's wallet.                                                                            |
| `funding_cap_reached`              | 402  | yes       | `fail_over`    | This upstream hit the platform's funding limit. Not this account's wallet.                                                                              |
| `endpoint_not_allowed`             | 403  | no        | `fix_request`  | The path is not an allowlisted, meterable endpoint.                                                                                                     |
| `invalid_json`                     | 400  | no        | `fix_request`  | The body did not parse as JSON, so it was not forwarded.                                                                                                |
| `invalid_body`                     | 400  | no        | `fix_request`  | A Mistral conversations body was not a JSON object.                                                                                                     |
| `agents_blocked`                   | 400  | no        | `fix_request`  | A Mistral conversations body named a stored agent or conversation.                                                                                      |
| `inline_tools_only`                | 400  | no        | `fix_request`  | A Mistral conversations body used a tool that is not inline.                                                                                            |
| `model_not_priced`                 | 403  | no        | `fix_request`  | The model has no confirmed price, or the request names no model.                                                                                        |
| `native_tool_not_metered`          | 403  | no        | `fix_request`  | A provider-native tool has a surcharge that cannot be metered on a paying key.                                                                          |
| `audio_duration_unknown`           | 422  | no        | `fix_request`  | The audio's billable duration could not be read from the file.                                                                                          |
| `unsupported_media_type`           | 415  | no        | `fix_request`  | The upload is a container the router cannot measure.                                                                                                    |
| `streaming_not_supported`          | 400  | no        | `fix_request`  | Streaming is not available on this path. Resend without stream:true.                                                                                    |
| `account_suspended`                | 403  | no        | `park_auth`    | An admin suspended this account. Credits do not lift a suspension.                                                                                      |
| `credits_or_subscription_required` | 402  | no        | `park_billing` | No credits, no subscription and no complimentary access.                                                                                                |
| `insufficient_credits`             | 402  | no        | `park_billing` | The wallet is empty or below zero. The only refusal that means an empty wallet.                                                                         |
| `insufficient_credits_for_call`    | 402  | no        | `retry_capped` | The wallet has credits but cannot cover this call's worst case. retryable turns true when finishing in-flight calls would let the same request through. |
| `credits_held_in_flight`           | 429  | yes       | `retry_later`  | The wallet has credits, but other in-flight calls hold all of them for now.                                                                             |
| `budget_exceeded`                  | 402  | no        | `park_billing` | This key hit its own monthly budget. The wallet may still hold credits.                                                                                 |
| `upstream_rate_limited`            | 429  | yes       | `fail_over`    | Every key slot of this upstream answered 429.                                                                                                           |
| `upstream_quota_exhausted`         | 429  | yes       | `fail_over`    | The platform's account at this upstream is out of quota or credit.                                                                                      |
| `upstream_auth_failed`             | 502  | yes       | `fail_over`    | This upstream rejected the platform's key. The caller's Cumulo key is fine.                                                                             |
| `upstream_error`                   | 502  | yes       | `fail_over`    | This upstream answered 5xx on a path with no fallback (streaming or media).                                                                             |
| `fallback_exhausted`               | 502  | yes       | `fail_over`    | Retries and fallback rules ran out without a clean quota or throttle answer.                                                                            |
| `provider_timeout`                 | 504  | yes       | `fail_over`    | The upstream did not answer before the deadline.                                                                                                        |
| `provider_unreachable`             | 502  | yes       | `fail_over`    | The router could not reach the upstream at all.                                                                                                         |

## The clamp: `insufficient_credits_for_call`

Below 500 available credits, the router refuses a call whose worst case the wallet cannot
cover. The worst case is the byte-derived input estimate plus the request's declared output
cap, or the model's true maximum output when the request declares none, at the rate the call
would be billed. The wallet was **not** empty, and the body says what would fit:

```json
{
  "error": "insufficient_credits_for_call",
  "reason": "insufficient_credits_for_call",
  "message": "This request could cost up to 525 credits and 309 are available. Nothing was charged. Limit the output to 74904 tokens or fewer, or top up.",
  "charged": false,
  "retryable": false,
  "balanceCredits": 309,
  "availableCredits": 309,
  "heldCredits": 0,
  "estimatedCredits": 525,
  "maxAffordableOutputTokens": 74904,
  "retryHint": "lower_max_output"
}
```

- `balanceCredits`: the wallet.
- `availableCredits`: the wallet minus what other in-flight calls hold. The bound uses this.
- `heldCredits`: what other in-flight calls hold now.
- `estimatedCredits`: this call's worst case as sent.
- `maxAffordableOutputTokens`: the largest output cap that clears the clamp right now, 0 when
  no cap can (the input alone is too expensive).
- `retryHint`: `lower_max_output`, `wait` (only in-flight calls stand in the way) or `top_up`.
- `retryable`: true only when `heldCredits` is above 0 and finishing calls would let the same
  request through: `estimatedCredits` is at or below `balanceCredits`, or `balanceCredits` is at
  or above 500, where the clamp stops applying. Then `retryAfterMs` is set too.

**What a device does:**

1. If `maxAffordableOutputTokens` is at or above the smallest output this seam can use (256 is
   a sensible floor for a fold), resend **once** with the output cap set to that number. Use
   `max_output_tokens` on `/v1/responses`, `max_tokens` on Anthropic and on Mistral and Z.ai
   chat, `max_completion_tokens` or `max_tokens` on OpenAI chat. If the body already declares
   a cap under another spelling, lower them all: the router reads the largest.
2. The bound already leaves room for the added field, so the retry clears the clamp unless the
   wallet changed in between. A second refusal fails the call. Do not loop.
3. Reasoning models count reasoning inside the output cap, so a low cap can end in a truncated
   reply. Treat a truncated reply as that seam's normal short-output case.
4. Tell the owner, plainly: this request is larger than the remaining credits allow. Do not
   say "add credits" as if the wallet were empty. Do not park.
5. Otherwise fail this call or fail over. Do not park.

## The other wallet answers, exactly

The only empty-wallet refusal:

```json
{
  "error": "insufficient_credits",
  "reason": "insufficient_credits",
  "message": "Your credit balance is empty. Top up to continue.",
  "balanceCredits": 0,
  "charged": false,
  "retryable": false
}
```

The wallet is funded and other calls hold it (transient, HTTP 429):

```json
{
  "error": "credits_held_in_flight",
  "reason": "credits_held_in_flight",
  "message": "Your other requests in progress are holding your credits. Nothing was charged. This clears when they finish, so try again in a moment.",
  "balanceCredits": 100,
  "availableCredits": 0,
  "heldCredits": 100,
  "charged": false,
  "retryable": true,
  "retryAfterMs": 5000
}
```

## Upstream refusals

An upstream 402, 429 or 5xx, or a quota answer on 400, 401 or 403, is about the platform's account
or the upstream's health. It is never this account's wallet. All are retryable and carry
`provider`, `upstreamStatus` and `retryAfterMs` (the upstream's own `Retry-After` when it sent
one, capped at one hour):

```json
{
  "error": "upstream_quota_exhausted",
  "reason": "upstream_quota_exhausted",
  "provider": "openai",
  "upstreamStatus": 429,
  "message": "The platform's openai account is out of quota. This is not your balance, and you were not charged. Try again later or use another provider.",
  "charged": false,
  "retryable": true,
  "retryAfterMs": 600000
}
```

`fallback_exhausted` remains for a walk that ended on a mix of failures, for example one target
throttled and one answering 503. When every target answered 429 or quota, the answer is the
typed refusal of the last target tried.

An upstream 401 or 403 with no quota marker arrives as `upstream_auth_failed` (502), so a device
never reads the platform's dead key as its own bad Cumulo key.

## Server-side fallback and the wallet

The fallback engine can serve a call with a different model when the requested one fails, and the
bill follows the model that served. On a paying key a substitute must clear the wallet at its own
rate first, through the same clamp. One that cannot is skipped, and the walk ends on the next
target or on the upstream's own refusal above, with `charged:false`. A device sees no new reason
for this.

## Faults in the router itself

`router_internal_error` means the router failed, usually a database fault. It is retryable and
says nothing about the wallet or the provider. Do not cool the upstream and do not tell the owner
to add credits. A provider reason (`provider_unreachable`, `provider_timeout`) is only ever about
the upstream.

## The other endpoints a router key reaches

`GET /router/models`, `GET /router/fallback` and `POST /devices/safety-report` answer the same
table for a missing or invalid key. Both `GET` reads also answer `account_suspended`,
`credits_or_subscription_required`, `rate_limited` and `router_internal_error` with the same body
as the passthrough, so one key gets one verdict everywhere.

## Which firmware versions see this

Firmware before the v5.0.0 firmware half of this lane reads every 402 as a billing park. Until
it updates, the clamp and `funding_cap_reached` still park that device. The router's status codes
for those two were kept at 402 on purpose: `credits_held_in_flight` and the upstream refusals are
new reasons on 429 or 502, so those devices at least do not park them as billing. How v4.6.x
handles each of them was not checked from this repo; the firmware half should confirm it.
