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

## Memory embeddings

Which providers the device can embed long-term memories with (Memory >
Embedding model). Generated from the embeddings route table and request
builder (`lib/core/include/nimbus/orch/embedding.h`), so each row shows the
request the device really sends. `Width` says what sets the vector width:
`Dimensions` when the request carries the saved Dimensions, `model` when the
provider has no width field and the model's own width comes back
(mistral-embed: 1024), so Dimensions must equal it. A vector of any other
width is refused, never truncated.

| Provider | Memory embeddings | Endpoint | Request fields | Width |
|---|---|---|---|---|
| cumulo | yes | `/router/openai/v1/embeddings` (router key) | `model`, `input`, `dimensions`, `encoding_format` | Dimensions |
| openai | yes | `/v1/embeddings` | `model`, `input`, `dimensions`, `encoding_format` | Dimensions |
| anthropic | no | - | - | - |
| mistral | yes | `/v1/embeddings` | `model`, `input` | model |
| zai | no | - | - | - |
