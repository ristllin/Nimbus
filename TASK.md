# TASK: Per-key access-validation path (harness reliability)

## Definition of Done
The harness can, for each provider API key, query that provider's OWN API to enumerate
what the key/account actually reaches, and surface it as a diagnostic (API + web card) +
use it to validate/self-heal device connector config — instead of ASSUMING access.
- Mistral: `GET /v1/connectors` (real names + UUIDs) + `GET /v1/models`.
- OpenAI / Anthropic: `GET /v1/models` + a live probe of the configured MCP (BYO-MCP, no
  account-side connector list).
- Validated live against all 3 providers incl. the new Mistral token; its access documented.
- Host tests cover the parse/merge logic; a live probe confirms end-to-end.
- No regressions (`pio test -e native`, both device builds, website). Tree clean. prism-reviewed.

## Findings (grounded, 2026-08-19)
- **Mistral `/v1/connectors` EXISTS** and is the "active connections per account/key" API.
  This account (new token) has **17 connectors incl `github_app`** (UUID
  `019d8b52-6f1c-72db-97bf-7284f2bf5ead`), atlassian, notion, slack, linear, gmail, gdrive,
  gcal, stripe, box, sharepoint, bigquery, document_library, outlook. Also `/v1/agents`
  (200, 1 agent "nuage-solide") and `/v1/libraries` (200).
- Token is healthy: **55 models** incl coding (`codestral`, `devstral`, `mistral-code-agent`).
- **ROOT CAUSE of earlier Mistral-GitHub failure**: `attachMistralWire`
  (`lib/core/src/orch_connectors_wire.cpp:148-159`) sends `connector_id: "github_app"` (the
  NAME) with no explicit cid; the account's real id is the UUID above. First-party connector
  entries also "REQUIRE authorization" (comment `:102-103`). So the harness assumes an id/auth
  it may not have. The fix is to FETCH the account connectors and use the real id.
- Provider verify piping today: `src/agent/provider_verify.cpp` does a cheap `GET /v1/models`
  per key (verify=works). Connector config parse: `src/agent/connectors.cpp:42,49`
  (`cid`, `oauth.cid`). No account-connector enumeration exists → the gap.

## Plan
1. **lib/core (portable, host-tested)**: a `provider_audit` parser — given a provider's raw
   API responses (Mistral /v1/connectors + /v1/models; OpenAI/Anthropic /v1/models), produce a
   normalized "access report" struct (models[], connectors[{name,id,protocol}], caps). Pure,
   no Arduino. Host test in test/.
2. **src/agent (device seam)**: fetch the responses (reuse provider_verify's TLS path), run the
   portable parser, cache the report. For Mistral, cross-check configured github connector's
   cid against the fetched `github_app` UUID and flag/self-heal a mismatch.
3. **src/net (surface)**: `GET /api/keyaudit` returns the per-provider access report; web card
   under Harness → (Connectors or a new "Key access" panel).
4. **Docs + parity ledger**; **HIL/verify**: live probe against all 3 keys.
5. **prism** last.

## Guardrails
- Token handling: NEVER commit/log/echo any API key; read-only validation only; device Mistral
  key change is owner-only (web UI). Recommend owner ROTATE the chat-pasted token after.
- No scope creep beyond the validation path. No faked-green tests.

## Status
- [x] DoD + findings captured (this file)
- [x] worktree key-access-audit off main (v4.2.0)
- [x] lib/core provider_audit parser + host test (test_provider_audit 4/4)
- [ ] device seam (fetch + cache + cid self-heal)
- [ ] /api/keyaudit + web card
- [ ] docs + live 3-provider probe
- [ ] prism

## Continuation notes (turnkey — device seam)
BLOCKED on owner: set nimbus-4 Mistral key to the new token (one-liner in chat) so the
live device audit runs against the account that HAS github_app.

Device seam (src/agent/keyaudit.cpp, model on provider_verify.cpp):
- Per keyed provider, TLS GET (reuse the provider_verify runOne() shape: host+key+path,
  WiFiClientSecure, arbiter::acquireWork/releaseWork, readHttpBody):
    mistral: GET /v1/connectors  AND  GET /v1/models
    openai/anthropic: GET /v1/models  (+ later: live MCP probe)
- Feed bodies to core::parseMistralConnectors / core::parseModelsList (DONE, committed).
- Cache the report (RAM); expose GET /api/keyaudit (webui.cpp, token-gated) -> per-provider
  {models[], connectors[{name,id,protocol}]}.
- SELF-HEAL: for each configured connector whose prov can be mistral, if its cid != the
  fetched connectorIdByName(name-or-remap) UUID, update the blob cid (owner-confirmed or
  auto). NB attachMistralWire remaps github->github_app: match on "github_app".
- Web card: Harness -> "Key access" panel rendering /api/keyaudit.
- Verify: live probe all 3 keys; then a Mistral github create_repository test should work
  once cid = 019d8b52-... (confirm whether Mistral accepts name vs UUID for connector_id).

## Status (updated)
- [x] lib/core provider_audit parser + host test (test_provider_audit 4/4)
- [x] token validated live (55 models, 17 connectors incl github_app UUID)
- [ ] BLOCKED: owner sets new token on nimbus-4 -> then device seam + live test
- [ ] /api/keyaudit + web card ; docs ; prism
