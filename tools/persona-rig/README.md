# Persona rig

Runs scripted people against isolated `nimbusd` containers. Each persona gets its own
container on its own throwaway volume, is configured over `/api/*` the way the web app
does it, then runs scripted steps. Every step is graded with hard assertions, and the raw
evidence of every step is written to disk.

```bash
docker build -f nimbusd/Dockerfile -t nimbusd:dev .

DOCKER_BIN=/opt/homebrew/bin/docker python3 tools/persona-rig/persona_rig.py run \
  --persona tools/persona-rig/personas/worked_example.json \
  --image nimbusd:dev --evidence /tmp/persona-evidence --env-file nimbus/.env
```

The run is paid: real provider turns. It exits 0 only when every assertion of every persona
passed. `python3 tools/persona-rig/test_persona_rig.py` is the free self test (no docker, no
network, no keys); it runs under `make -C nimbusd test` and in the `gate-logic` CI job.

Options:

| Option | Meaning |
|---|---|
| `--persona FILE` | a persona or matrix file; repeat for several |
| `--names a,b` | run only these persona names from the files |
| `--only id,id` | run only these scenario ids |
| `--parallel N` | run N personas at once, one container each (providers sharing a key can rate limit) |
| `--judge` | add the independent LLM judge from `tests/hil/qa_judge.py`, a recorded soft signal that never changes a verdict (needs `requests`) |
| `--keep` | leave the containers and volumes for inspection |

## Persona format

A persona is one JSON object. Everything except `name` and `scenarios` is optional.

```json
{
  "name": "nora-bakery-owner",
  "display_name": "Nora's Nimbus",
  "timezone": "Europe/London",
  "directive": "You help Nora run a small bakery.",
  "monthly_cap_cents": 500,
  "keys": {"mistral": "MISTRAL_API_KEY"},
  "instance_env_keys": ["TAVILY_API_KEY"],
  "env": {"NIMBUSD_TOOL_LOOP": "1"},
  "models": {"head": {"mistral": "mistral-small-latest"}, "sub": {"mistral": "mistral-small-latest"}},
  "host": "mistral",
  "members": [{"chat": "2001", "name": "Sam", "role": "user"}],
  "scenarios": [{"id": "owner-day", "steps": []}]
}
```

- `keys` maps a provider slug (`mistral`, `zai`, `anthropic`, `openai`, `cumulo`) to the
  **name** of a key in the env file. The value is posted the way the web app posts a key.
- `instance_env_keys` lists keys that reach the container as environment instead, the way
  the platform passes them. Tavily is one: the web app does not accept it.
- `env` is plain, non-secret container environment.
- A setting the instance refuses stops the persona before anything is spent.

A **matrix file** runs one template many times:

```json
{"vars": {"marker": "4417"},
 "matrix": [{"name": "a", "model": "mistral-small-latest"}],
 "persona": {"name": "cap-${name}", "scenarios": []}}
```

`${x}` fills from the matrix row, then `vars`, in values and object keys. An unknown `${x}`
is refused at load.

## Steps

A step has exactly one action, plus an optional `expect` list.

| Action | Meaning |
|---|---|
| `{"say": "text"}` | a web chat turn as the owner, polled the way the page polls |
| `{"say": "text", "as": "2001"}` | a turn from an approved member, through `POST /api/message` |
| `{"http": {"method": "POST", "path": "/api/orch", "form": {}}}` | a call to any route; also takes `json` |
| `{"upload": {"project": "chat", "name": "a.png", "png_rgb": [0, 200, 0]}}` | a Files upload; also takes `text` or `b64` |
| `{"restart": true}` | restart the container, keeping the volume |
| `{"sleep": 5}` | wait, in seconds |

A `say` step also takes:

| Field | Default | Meaning |
|---|---|---|
| `timeout_s` | 240 | give up after this long |
| `settle_s` | 3 | quiet time required after the last reply |
| `await_replies` | 0 | wait for at least this many assistant messages (sub-agent results arrive later) |
| `await_reply_regex` | none | wait until an assistant message matches |
| `repeat` | none | expand into N steps; `$i` in the text counts from 1 |
| `pad` | none | with `repeat`: add this many bytes of distinct filler to each turn |
| `judge` | true | false leaves this step out of `--judge` |

## Assertions

Each `expect` item is one name and one value. An unknown name is refused at load, so a typo
can never read as a pass. The table is `ASSERTIONS` in `grade.py`, and the self test holds
one passing and one failing case for every entry.

| Name | Value | Passes when |
|---|---|---|
| `reply_nonempty` | `true` | an assistant message arrived |
| `no_error_reply` | `true` | no message carries failure wording, a raw JSON dump, or a bare `Done.` or `placeholder` |
| `reply_contains`, `reply_not_contains` | text | case-insensitive, over every message of the step |
| `reply_regex`, `reply_not_regex` | regex | same scope |
| `no_fabricated_action` | `true` | a completed-action claim has a tool of the matching family (the HIL suite's `FAB_DONE_MAP`) |
| `turn_resolved` | bool | the turn's own poll answered, which the page's spinner needs |
| `tool_called`, `tool_not_called` | name | a tool of the last turn (`files_read`, not `files.read`) |
| `log_regex`, `log_not_regex` | regex | the daemon log lines this step added |
| `http_status`, `body_contains`, `body_not_contains`, `body_regex` | value | the `http` or `upload` response |
| `tokens_max` | number | input plus output tokens of the step |
| `file_contains`, `file_absent` | `{"project", "name", "text"}` | read through the Files route |
| `api_min` | `{"path", "field", "min"}` | a number an API route reports, `a.b` for nested fields |
| `api_equals` | `{"path", "field", "value"}` | a value an API route reports |

Prefer a side effect (a file, a memory count) over the wording of a reply. `tool_called`
reads `/api/lastturn`, one global slot that a sub-agent's synthesis turn overwrites, so check
a sub-agent by its result and its log line instead.

## Evidence

```
<evidence>/<persona>/
  summary.json             verdict per scenario, the failed checks, tokens, spend
  config.json              every setting posted and its answer
  orch-after-config.json   GET /api/orch after configuration
  <scenario>/step-NN.json  request, reply, every message, tool calls, log delta, spend, the
                           turn anatomy, assertion results, judge verdict
  container.log            docker logs
  api-log.txt              GET /api/log at the end
```

Keys and the instance token are scrubbed from every file. Spend comes from the daemon's own
`turn spend:` lines, which carry provider-reported tokens, and `spend.spentCents` in `/api/state`.

## Persona files here

| File | What it is |
|---|---|
| `personas/worked_example.json` | one small complete persona: read this first |
| `personas/capability_matrix.json` | the capability script (chat, memory, restart, files, sub-agent, web, image, a binary file read, routine, a long conversation) on six models |
| `personas/web_with_tavily.json` | the web lookup on an instance that also holds a Tavily key |
| `personas/first_run.json` | a new instance with no key, then with a rejected key (costs nothing) |
| `personas/embed_switch.json` | an OpenAI-only owner moves memory to OpenAI embeddings, then stores a memory |
