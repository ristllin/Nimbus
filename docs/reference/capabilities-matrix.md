# Provider capability matrix

Generated from the model catalog code (`lib/core/src/model_catalog.cpp`) over
recorded `/v1/models` fixtures, so it cannot drift from what the device
actually classifies. Do not edit by hand: re-run
`GOLDEN_UPDATE=1 pio test -e native -f test_capabilities_matrix`.

A cell is `yes` when at least one of that provider's live models carries the
role or capability. `source` is `api` when the provider's models endpoint
supplies capability fields, `heuristic` when the device infers them from the
model id family.

| Provider | Orchestrator | Sub-agent | Embedding | Vision | STT | TTS | Image | Tools | Streaming | Source |
|---|---|---|---|---|---|---|---|---|---|---|
| openai | yes | yes | yes | yes | yes | yes | yes | yes | yes | heuristic |
| anthropic | yes | yes | no | yes | no | no | no | yes | yes | api |
| mistral | yes | yes | yes | yes | yes | yes | no | yes | yes | api |
| zai | yes | yes | no | no | no | no | no | yes | yes | heuristic |
| cumulo | per upstream | per upstream | per upstream | per upstream | per upstream | per upstream | per upstream | per upstream | per upstream | router |

Cumulo Nimbus is a router: each role inherits the capabilities of the upstream
chosen for it (the model id is `<upstream>/<model>`), so its row is the union of
whichever upstreams the admin enables.
