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

| reason                             | HTTP | retryable | device action  | meaning                                                                                                                                                                                             |
| ---------------------------------- | ---- | --------- | -------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `missing_router_key`               | 401  | no        | `park_auth`    | No bearer key on the request.                                                                                                                                                                       |
| `invalid_router_key`               | 401  | no        | `park_auth`    | The bearer key is unknown or revoked.                                                                                                                                                               |
| `unknown_provider`                 | 404  | no        | `fix_request`  | The path names an upstream the router does not have.                                                                                                                                                |
| `router_not_configured`            | 501  | no        | `fail_over`    | This deployment has no provider gateway.                                                                                                                                                            |
| `rate_limited`                     | 429  | yes       | `retry_later`  | This key passed the per-minute burst bound. The live wait is in retryAfterMs.                                                                                                                       |
| `request_too_large`                | 413  | no        | `fix_request`  | The body is over the router's size limit, so it was not read.                                                                                                                                       |
| `large_body_busy`                  | 413  | yes       | `retry_later`  | Every slot for bodies over 1 MiB on this server stayed busy for the short wait. The live wait is in retryAfterMs. A 413, so firmware that predates this reason resends the turn without its photos. |
| `body_read_timeout`                | 408  | yes       | `retry_later`  | A body over 1 MiB stopped arriving, so the router stopped waiting for it.                                                                                                                           |
| `invalid_request`                  | 400  | no        | `fix_request`  | The HTTP server could not read this request, for example a malformed Content-Type.                                                                                                                  |
| `router_internal_error`            | 500  | yes       | `retry_later`  | The router itself failed, for example a database fault. The wallet was not debited.                                                                                                                 |
| `router_disabled`                  | 503  | yes       | `fail_over`    | An admin switched the whole router off.                                                                                                                                                             |
| `provider_disabled`                | 503  | yes       | `fail_over`    | An admin switched this upstream off.                                                                                                                                                                |
| `ceiling_reached`                  | 429  | yes       | `fail_over`    | The platform's own monthly spend ceiling was hit. Not this account's wallet.                                                                                                                        |
| `funding_cap_reached`              | 402  | yes       | `fail_over`    | This upstream hit the platform's funding limit. Not this account's wallet.                                                                                                                          |
| `endpoint_not_allowed`             | 403  | no        | `fix_request`  | The path is not an allowlisted, meterable endpoint.                                                                                                                                                 |
| `invalid_json`                     | 400  | no        | `fix_request`  | The body did not parse as JSON, so it was not forwarded.                                                                                                                                            |
| `invalid_body`                     | 400  | no        | `fix_request`  | A Mistral conversations body was not a JSON object.                                                                                                                                                 |
| `agents_blocked`                   | 400  | no        | `fix_request`  | A Mistral conversations body named a stored agent or conversation.                                                                                                                                  |
| `inline_tools_only`                | 400  | no        | `fix_request`  | A Mistral conversations body used a tool that is not inline.                                                                                                                                        |
| `model_not_priced`                 | 403  | no        | `fix_request`  | The model has no confirmed price, or the request names no model.                                                                                                                                    |
| `native_tool_not_metered`          | 403  | no        | `fix_request`  | A provider-native tool has a surcharge that cannot be metered on a paying key.                                                                                                                      |
| `audio_duration_unknown`           | 422  | no        | `fix_request`  | The audio's billable duration could not be read from the file.                                                                                                                                      |
| `unsupported_media_type`           | 415  | no        | `fix_request`  | The upload is a container the router cannot measure.                                                                                                                                                |
| `streaming_not_supported`          | 400  | no        | `fix_request`  | Streaming is not available on this path. Resend without stream:true.                                                                                                                                |
| `image_source_not_allowed`         | 400  | no        | `fix_request`  | An image is a file id on the shared provider account, or a URL that is not https.                                                                                                                   |
| `too_many_images`                  | 400  | no        | `fix_request`  | More images than this upstream and model take in one request.                                                                                                                                       |
| `image_too_large`                  | 413  | no        | `fix_request`  | One image is over the router's per-image size limit.                                                                                                                                                |
| `image_unsupported_type`           | 400  | no        | `fix_request`  | An image is not JPEG, PNG, WebP or GIF by its own bytes, or not in canonical base64.                                                                                                                |
| `model_no_image_input`             | 400  | no        | `fix_request`  | The model does not take image input. A model whose vision is unknown is forwarded.                                                                                                                  |
| `file_source_not_allowed`          | 400  | no        | `fix_request`  | A document, file, audio or video part is a file id on the shared provider account, or a URL that is not https.                                                                                      |
| `account_suspended`                | 403  | no        | `park_auth`    | An admin suspended this account. Credits do not lift a suspension.                                                                                                                                  |
| `credits_or_subscription_required` | 402  | no        | `park_billing` | No credits, no subscription and no complimentary access.                                                                                                                                            |
| `insufficient_credits`             | 402  | no        | `park_billing` | The wallet is empty or below zero. The only refusal that means an empty wallet.                                                                                                                     |
| `insufficient_credits_for_call`    | 402  | no        | `retry_capped` | The wallet has credits but cannot cover this call's worst case. retryable turns true when finishing in-flight calls would let the same request through.                                             |
| `credits_held_in_flight`           | 429  | yes       | `retry_later`  | The wallet has credits, but other in-flight calls hold all of them for now.                                                                                                                         |
| `budget_exceeded`                  | 402  | no        | `park_billing` | This key hit its own monthly budget. The wallet may still hold credits.                                                                                                                             |
| `upstream_rate_limited`            | 429  | yes       | `fail_over`    | Every key slot of this upstream answered 429.                                                                                                                                                       |
| `upstream_quota_exhausted`         | 429  | yes       | `fail_over`    | The platform's account at this upstream is out of quota or credit.                                                                                                                                  |
| `upstream_auth_failed`             | 502  | yes       | `fail_over`    | This upstream rejected the platform's key. The caller's Cumulo key is fine.                                                                                                                         |
| `upstream_error`                   | 502  | yes       | `fail_over`    | This upstream answered 5xx on a path with no fallback (streaming or media).                                                                                                                         |
| `fallback_exhausted`               | 502  | yes       | `fail_over`    | Retries and fallback rules ran out without a clean quota or throttle answer.                                                                                                                        |
| `provider_timeout`                 | 504  | yes       | `fail_over`    | The upstream did not answer before the deadline.                                                                                                                                                    |
| `deadline_too_short`               | 504  | no        | `fix_request`  | The request's x-cumulo-deadline-ms left less than 5 seconds, so nothing was sent. Not about the upstream: do not cool it.                                                                           |
| `provider_unreachable`             | 502  | yes       | `fail_over`    | The router could not reach the upstream at all.                                                                                                                                                     |

## Image input

The router forwards each upstream's own image parts unchanged (Anthropic image blocks,
including those inside a `tool_result`; OpenAI chat `image_url`; Responses `input_image`;
Mistral `image_url` as a string or an object, in chat and in conversations; Z.ai
`image_url`). Before any credit is reserved it checks every image, in this order:

1. The source: a provider file id (`file_id`, an Anthropic `file` source) is refused,
   because the provider account is shared. A remote URL must be https.
   `image_source_not_allowed`.
2. The count, per upstream and model (the Limits table below). `too_many_images`.
3. Each image's decoded size against the per-image limit. `image_too_large`.
4. Each image's magic bytes: JPEG, PNG, WebP or GIF, base64 in the body.
   `image_unsupported_type`.
5. The model: when the price feeds, or else the router's name rule, say the model takes
   no images, `model_no_image_input`. A model whose vision is unknown is forwarded.

Documents, files, audio and video (OpenAI `file` and `input_file` parts, Anthropic `document`
blocks, Mistral `document_url`, Z.ai `file_url` and `video_url`) follow rule 1 too, from cumulo
v0.4.44: a provider file id or a URL that is not https is `file_source_not_allowed`, whose message
says `file input`; inline ones are forwarded unchanged, and a request carrying any is never
substituted by the fallback engine.

Every one of the image messages contains the words `image input`, every image reason is a 400 or
a 413, and for these reasons `error` is an object, `{"type": "<reason>", "message": "..."}`, next to
the plain-string `reason`. That is what firmware older than these reasons needs: v5.1.1's
Anthropic and OpenAI heads read only `error.message`, its Mistral head reads `message`, and each
re-sends the turn once without its photos when `refusedImages` matches a 413 or a 400 naming
`image input` (a 415, or a bare string `error`, failed the turn instead; tested by replaying
v5.1.1's parser in `apps/api/src/firmware-v511-replay.test.ts`). Firmware that knows the reasons
does the same on any of them. None of them is retryable unchanged and none charges anything. A served call that carried images answers `x-cumulo-images: <count>`.

## Limits

Generated from `packages/shared/src/router-images.ts` (`renderRouterLimitsTable`); a test
fails when this section and the code disagree. The image limits are checked by the router
before any spend. The body cap is the HTTP route's: cumulo v0.4.44 raises the router route
from 1 MiB to this value (CUM-R2), and every other route keeps 1 MiB.

<!-- limits:begin -->

- Request body on `POST /router/:provider/*`: at most 4,194,304 bytes (`ROUTER_INFERENCE_BODY_MAX`).
- One image, decoded: at most 3,145,728 bytes (`ROUTER_IMAGE_BYTES_MAX`).

| upstream    | models            | images per request | bytes per image (decoded) | source                                                                  |
| ----------- | ----------------- | ------------------ | ------------------------- | ----------------------------------------------------------------------- |
| `openai`    | every model       | 1,500              | 3,145,728                 | documented: 1,500 per request                                           |
| `anthropic` | every model       | 100                | 3,145,728                 | documented: 100 per request on 200k-context models (600 on larger ones) |
| `mistral`   | `mistral-large*`  | 100                | 3,145,728                 | measured: mistral-large takes 100                                       |
| `mistral`   | every other model | 8                  | 3,145,728                 | measured: small, medium and ministral answer 400 above 8                |
| `zai`       | every model       | 100                | 3,145,728                 | measured: glm-4.6v took 101; not documented                             |

<!-- limits:end -->

A declared `Content-Length` over the cap answers `request_too_large` before a byte or the key is
read; a body with no declared length is read until it passes the cap (its key is checked when it
passes 1 MiB), then answers the same. One image over the
per-image limit is over the body cap too once it is base64, so in practice it is the body cap that
answers. Image counts are
the provider's documented limit, or where it documents none, the limit measured live on
16 Nov 2026: Mistral's small, medium and ministral models answer 400 above 8 images, while
`mistral-large` takes 100.

### Large bodies take turns

Each api pod reads at most 2 bodies over 1 MiB at once, and at most 1 per account: its device
keys and portal keys share it, because a device key is minted without hardware. A second album
from the same account is refused `large_body_busy` at once rather than waiting behind its own. A
body is admitted when its bytes pass 1 MiB, and only for a valid key on an entitled account
(otherwise `missing_router_key`, `invalid_router_key` or the entitlement refusal). With both
slots taken, a request that carries `x-cumulo-deadline-ms` waits for one, its upload paused, up
to 10 seconds and only while its deadline still leaves 5 seconds; the wait comes out of the
deadline. A request without the header (firmware up to v5.1.1) does not wait. Then it answers
`large_body_busy` (413, retryable, `retryAfterMs` taken from the real hold times, a matching
`Retry-After`). Every such refusal is answered only after the rest of
the body has been read and dropped (never buffered): the device finishes its upload, then reads
the answer, so a refused album costs one full upload. Firmware that knows the reason may wait
`retryAfterMs` or resend without photos; firmware that predates it reads the 413 as "photos
refused" and resends the turn without them (`refusedImages(413)`), as it did under the old 1 MiB
cap. A body holding a slot that stops arriving (15 seconds without a byte, or under 32 KiB/s
overall) answers `body_read_timeout` (408) and frees the slot. An account with 3 such stalls in an
hour on one api pod has its bodies over 1 MiB refused `large_body_busy` on that pod until the
hour from its first stall ends (`retryAfterMs` says when); smaller bodies are not affected.

## The deadline: `x-cumulo-deadline-ms`

A routed request may carry `x-cumulo-deadline-ms: <n>`, a positive whole number of
milliseconds the device will wait, counted from when the router holds the whole request, less
any time a body over 1 MiB waited for its slot (firmware sends its socket ceiling less a small
margin, and never under 5000: a shorter value is refused, see below). From cumulo v0.4.44 the router strips it
before any provider sees it, starts no retry or fallback substitute that cannot finish before
it (each needs at least 5 seconds, or as long as the slowest attempt so far), and aborts the
upstream call when it passes, in the api and in the egress. That abort answers
`provider_timeout` (504, `charged: false`, no usage recorded). A deadline that leaves less than
5 seconds is answered `deadline_too_short` at once and nothing is sent: 504, not retryable, no
`retryAfterMs`, device action `fix_request`. It is about the request, not the upstream, so a
device must not cool the upstream for it (`provider_timeout` is `fail_over`, which would). An account may make 10
deadline calls that end cut off per hour on each api pod (a call takes its place when it starts
and gives it back if it is not cut off); past that its header is ignored and its calls run to the
end and are billed, so a device must not rely on the header to stop paying for a slow answer. When the deadline only stops a
retry or a substitute from starting, the answer is the one the walk would have given anyway
(an upstream refusal or `fallback_exhausted`). Malformed values are ignored, and older routers
ignore the header, so sending it is always safe.

## The clamp: `insufficient_credits_for_call`

Below 500 available credits, the router refuses a call whose worst case the wallet cannot
cover. The worst case is the byte-derived input estimate plus the request's declared output
cap, or the model's true maximum output when the request declares none, at the rate the call
would be billed. Input is priced at the input rate, or at the dearest cache write it can
bill at once the operator turns that provider's write billing on (OpenAI 1.25x, 2.5x with
extended retention, under `ROUTER_BILL_OPENAI_CACHE_WRITES`; Anthropic 1.25x or 2x, under
`ROUTER_BILL_ANTHROPIC_CACHE`). Each image counts its provider's image-token formula for its real pixel size, not its
base64 bytes; an image whose size cannot be read counts its bytes like text. The wallet was **not** empty, and the body says what would fit:

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

Photos never move to a blind model (cumulo v0.4.44): a request with image parts is substituted
only to a model whose image input is known true, within that model's image limits, with its
`image_url` parts in that upstream's form. A Responses, Messages or Conversations request is
never substituted. When no target qualifies the walk ends as above, before any spend on a
substitute.

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
