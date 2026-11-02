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
passed or failed on a marked known defect. `python3 tools/persona-rig/test_persona_rig.py` is the free self test (no docker, no
network, no keys); it runs under `make -C nimbusd test` and in the `gate-logic` CI job.

Options:

| Option | Meaning |
|---|---|
| `--persona FILE` | a persona or matrix file; repeat for several |
| `--names a,b` | run only these persona names from the files |
| `--only id,id` | run only these scenario ids, plus the scenarios they name in `needs` |
| `--parallel N` | run N personas at once, one container each |
| `--provider-limit N` | at most N turns at once per head provider, however many personas run (default 2) |
| `--max-cents N` | stop starting paid turns and judge calls at this total for the run (default 500). A step already running finishes, so a run can pass it by that step's cost, per parallel persona |
| `--retries N` | run a turn again after a provider's 429 or 5xx, with a growing wait (default 2), only when no tool ran yet, so a retry repeats no side effect. The failed message stays in the conversation. Every try is kept in the step file |
| `--resume` | skip personas this evidence directory finished with the same persona and image tag; run the rest from their start |
| `--judge` | add the independent LLM judge from `tests/hil/qa_judge.py`, on a provider other than the persona's head. A recorded soft signal that never changes a verdict (needs `requests`) |
| `--keep` | leave the containers and volumes for inspection |

`persona_rig.py regrade --evidence DIR` re-grades every stored step with today's graders and
exits 1 when a stored verdict would change, or when it found no step or an unreadable one. Checks
that read a live instance are skipped, and so are moved-aside runs.

A rerun into the same evidence directory moves the persona's older run aside as
`<persona>.prev-<time>`, so old step files never read as new ones.

## Persona format

A persona is one JSON object. Everything except `name` and `scenarios` is optional. An unknown
field or a wrong type anywhere in the file stops the run when it loads, so a typo can never
run on defaults and pass.

```json
{
  "name": "nora-bakery-owner",
  "display_name": "Nora's Nimbus",
  "timezone": "Europe/London",
  "directive": "You help Nora run a small bakery.",
  "monthly_cap_cents": 500,
  "keys": {"mistral": "MISTRAL_API_KEY"},
  "key_expect": {"mistral": "verified"},
  "instance_env_keys": ["TAVILY_API_KEY"],
  "env": {"NIMBUSD_TOOL_LOOP": "1"},
  "models": {"head": {"mistral": "mistral-small-latest"}, "sub": {"mistral": "mistral-small-latest"}},
  "host": "mistral",
  "members": [{"chat": "2001", "name": "Sam", "role": "user"}],
  "scenarios": [{"id": "owner-day", "steps": []}]
}
```

- `keys` maps a provider slug (`mistral`, `zai`, `anthropic`, `openai`, `cumulo`, or `tavily` for the web search key) to the
  **name** of a key in the env file. The value is posted the way the web app posts a key.
- `key_expect` is the verdict a key must get before any turn runs: `verified` (the default),
  `rejected`, or `nocredits`. A key name starting `RIG_FAKE_` needs no env entry: the rig makes
  up a key the provider will refuse, so a rejected-key persona holds no secret.
- `monthly_cap_cents` defaults to 200, not the daemon's 2000. The daemon's floor is 100.
- `instance_env_keys` lists keys that reach the container as environment instead, the way
  the platform passes them. Tavily is one: the web app does not accept it.
- `env` is plain, non-secret container environment.
- A setting the instance refuses stops the persona before anything is spent. Every setting is
  then read back (models, host, ceiling, directive, time zone, name, members), so a setting that
  answered ok but did not take stops it too. The readback is in `summary.json`.

A **matrix file** runs one template many times:

```json
{"vars": {"marker": "4417"},
 "matrix": [{"name": "a", "model": "mistral-small-latest"}],
 "persona": {"name": "cap-${name}", "scenarios": []}}
```

`${x}` fills from the matrix row, then `vars`, in values and object keys. A string that is
exactly one `${x}` takes the value with its JSON type, so numbers (`"repeat": "${n}"`), lists and a
whole scenario list (`"scenarios": "${s}"`) can vary per row. An unknown `${x}` is refused at load.
A bare `$` is plain text, so regexes and prices need no escaping.

A matrix file may read a known answer when the run starts, instead of holding a typed-in value
that goes stale:

```json
{"truth": {"py_latest": {"url": "https://www.python.org/downloads/", "regex": "Download Python (3\\.\\d+\\.\\d+)"}}}
```

The value fills `${py_latest}`, and the raw body and its hash go to `<evidence>/truth/`. The source is
a plain public page: http or https, no query string. A source that does not answer stops the run
before anything is spent.

A scenario takes:

| Field | Meaning |
|---|---|
| `id`, `steps` | required |
| `needs` | earlier scenario ids it builds on; `--only` brings them along |
| `continue_conversation` | `true` keeps the conversation of the scenario before. By default each scenario after the first starts with the page's Clear, so one scenario's chat cannot answer the next. Long-term memory and files stay |

## Known defects

Marking a failure as known keeps a suite green while a reported defect stands, and keeps the
defect-hitting persona in the repo. An id must be a row of `known_defects.json`.

```json
{"tool_called": "files_read", "known_defect": "glm-no-tools"}
```

- A marked check that fails is recorded under `known_failures`, the persona still passes, and the
  scenario's verdict reads `known_defect`.
- A marked check that passes **fails the persona** (`xpass`): the defect looks fixed, so remove the
  marker. Add `"may_pass": true` where the defect depends on what the model chose to do.
- An empty `known_defect` marks nothing, so a matrix row can switch a marker off.
- A marker sits on one check, never on a scenario. A refused turn, a turn that ran out of time, a
  check that broke (no trace, a dead instance) and a persona that stopped are never excused.

## Steps

A step has exactly one action, plus an `expect` list (required on a `say`). A turn that is
refused or runs out of time fails on its own, whatever its list says. A key name missing from
the env file, or a key the provider rejects, stops the persona before any turn is paid for.

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
| `att` | none | a list of `project/name` Files entries the message brings as photos |

A member's turn has no turn id, so `turn_resolved` is refused on it.

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
| `tool_called`, `tool_not_called` | name | a head tool of this step (`files_read`, not `files.read`) |
| `tool_args_regex`, `tool_result_regex` | `{"tool", "regex"}` | a call of that tool whose arguments or result match |
| `log_regex`, `log_not_regex` | regex | the daemon log lines this step added |
| `http_status`, `body_contains`, `body_not_contains`, `body_regex` | value | the `http` or `upload` response |
| `tokens_max` | number | input plus output tokens of the step |
| `file_contains`, `file_absent` | `{"project", "name", "text"}` | read through the Files route |
| `api_min` | `{"path", "field", "min"}` | a number an API route reports, `a.b` for nested fields |
| `api_equals` | `{"path", "field", "value"}` | a value an API route reports |

Prefer a side effect (a file, a memory count) over the wording of a reply. Limits of the tool trace,
which comes from `/api/lastturn`:

- It shows head turns only. A sub-agent's tool calls are not visible on a hosted instance, so check
  a sub-agent by its result, a file it wrote, and its log line.
- The daemon folds each line at 200 characters, so a result is its first 180 or so characters.
  `tool_result_regex` can only match that start. The full result never leaves the daemon.
- It is one slot that the next turn overwrites. The rig reads it on every poll and keeps each turn
  it saw (`tools` lists them all, `lastturn_earlier` holds the older text), but a turn shorter than
  the one-second poll can still be missed.

## Evidence

```
<evidence>/
  run-summary.json         every persona's summary
  run-totals.json          the run's spend (turns and judge) against --max-cents, known defects
  truth/                   known answers read at the start, with their raw source
  <persona>/
    summary.json           verdict per scenario, failures, known failures, judge, tokens by provider
    config.json            every setting posted and its answer
    orch-after-config.json GET /api/orch after configuration
    <scenario>/reset.json  the Clear that started the scenario
    <scenario>/step-NN.json request, reply, every message, tool calls, log delta, spend, the turn
                           anatomy, assertion results, judge verdict, earlier tries
    turn-log.txt           the daemon's log for the container's whole life
    api-log.txt            GET /api/log at the end: since the last restart only
    container.log          docker logs: entrypoint start and stop lines only, no turns
```

Read `turn-log.txt` for what a turn did. The daemon keeps its turn log in memory and a restart
empties it, so the rig copies it after every step and before a restart.

A run never overwrites earlier evidence: a persona directory or `run-summary.json` already
at that path moves aside as `<name>.replaced-<time>`. Keys and the instance token are
scrubbed from every file. Spend comes from the daemon's own
`turn spend:` lines, which carry provider-reported tokens, and `spend.spentCents` in `/api/state`.
The judge's spend comes from the usage its provider reports.

## Reaching into the daemon

`CONTRACT` in `persona_rig.py` lists every route, form field, JSON field and log format the rig
relies on. The self test checks each against the daemon's source, so a rename fails CI. Add to
it when the rig starts relying on something new. `personas/contract.json` runs the same routes
against a real image, with no key and no turn: run it when you build a new image.

## Persona files here

| File | What it is |
|---|---|
| `personas/worked_example.json` | one small complete persona: read this first |
| `personas/capability_matrix.json` | the capability script (chat, memory, restart, files, sub-agent, web, image, a binary file read, routine, a long conversation) on six models, with reported defects marked |
| `personas/web_with_tavily.json` | the web lookup on an instance that also holds a Tavily key |
| `personas/first_run.json` | a new instance with no key, then with a rejected key (costs nothing) |
| `personas/first_run_rejected_key.json` | a rejected key through `keys` and `key_expect` (costs nothing) |
| `personas/glm_memory_cause.json` | whether a GLM head's memory writes land when embeddings work |
| `personas/cumulo_router.json` | one Cumulo router key, funded and with no credits. Needs `CUMULO_TEST_KEY` and `CUMULO_NOCREDIT_KEY` |
| `personas/contract.json` | every route the rig relies on, against a real image (costs nothing) |
| `personas/embed_switch.json` | an OpenAI-only owner moves memory to OpenAI embeddings, then stores a memory |
| `personas/vn_parity/*.json` | what a hosted instance had to gain to match the device: the conversation fold, the chat that never hangs, memory that follows the key, routines, wake-ups, skills, photos, the tool trace, the web search key |
