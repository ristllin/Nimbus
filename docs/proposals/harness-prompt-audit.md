<!-- audience: dev; status: proposal -->
# Harness prompt audit: Nimbus vs open-source agent harnesses

Status: proposal / investigation record. Not a published page.

This compares the Nimbus orchestrator system prompt and tool-description surface
against two public agent harnesses, identifies concrete simplification targets,
and proposes a flagged v2 prompt whose adoption is decided by the eval gate
(never by opinion). Numbers below are byte counts of the real composed output
(the prompt goldens), not estimates.

## References examined

Cloned to `~/Projects/reference-harnesses/` (shallow, read-only):

| Harness | Language | Role | Tool descriptions |
|---|---|---|---|
| `sst/opencode` | TypeScript | model-family system prompt, selected at runtime (`anthropic.txt`, `gpt.txt`, `gemini.txt`, ...) | one file per tool, attached to the function-calling schema |
| `charmbracelet/crush` | Go | single `coder.md.tpl` template | one `.md`/`.md.tpl` per tool, attached to the schema |

Both are general coding CLIs, not device assistants, so the comparison is about
*prompt architecture* (how the model's instructions are structured), not feature
parity.

## Size, measured

| Surface | Nimbus | opencode (anthropic) | crush (coder) |
|---|---|---|---|
| Static system prompt | see below | 8,212 B / 1,335 words | 21,523 B |
| Sum of tool descriptions | in-prompt (see below) | 16,342 B across 16 tools | 18,914 B across ~40 tools |
| Total model-visible instruction | ~19.4 KB system + ~5 KB connector catalog | ~24.5 KB (prompt + schemas) | ~40 KB (prompt + schemas) |

Nimbus composed World prompt (from the goldens, real bytes the model receives):

| Block | Bytes | Share |
|---|---|---|
| `orch_prompt_default.txt` (loop on, canned 5-tool fixture) | 14,451 | 100% |
| ...of which `ORCH_FIELD_DOCS` (the `orch_turn` contract, inlined as prose) | 6,347 | 44% |
| ...of which `## CAPABILITIES` (5-tool fixture) | ~3,500 | 24% |
| `[PROVIDERS & CONNECTORS]` catalog (separate turn-context path) | 4,973 | (added on top) |
| On a real ~42-tool device, `## CAPABILITIES` alone | ~10,000 | (per `orch_world.cpp` targets) |

The headline: **our prompt is not an outlier on length.** crush's system prompt
is larger than ours. The problem is not word count; it is *structure and
redundancy*, and where the bytes live.

## The core architectural difference

The two references make the same three splits; Nimbus makes none of them:

1. **Static role vs volatile world state.** opencode's system prompt is pure,
   stable policy (role, tone, workflow) with no per-turn data. The live world
   state (cwd, git, platform, date) is a *separate* short block injected at turn
   time:

   ```
   <env>
     Working directory: /path
     Is directory a git repo: yes
     Platform: darwin
     Today's date: ...
   </env>
   ```

   Seven lines. In Nimbus, `composeInstructions()` rebuilds one big string every
   turn that interleaves the never-changing contract (field docs, behavioral
   rails) with the volatile parts (identity/time, directive, running memory,
   live capability manifest, running sessions, scratchpad, recall). Mixing them
   defeats prompt caching: the stable 6-plus KB of field docs cannot be cached
   across turns because it sits in the same message as the per-turn state.

2. **Tool descriptions belong to the tool schema, not the prompt.** opencode and
   crush put each tool's description in the function-calling schema (opencode:
   `import DESCRIPTION from "./read.txt"` plus per-parameter `.annotate({
   description })`; crush: one `.md` per tool). The system prompt never restates
   them. Nimbus already ships a wire JSON schema built from the same `ORCH_D_*`
   macros to every provider (`ORCH_SCHEMA_BODY`), *and then re-states all of it*
   as `ORCH_FIELD_DOCS` prose in the system prompt. That 6,347-byte block (44% of
   the fixture prompt) is duplicated content: the provider already has the field
   descriptions in the structured-output schema.

3. **One statement per rule.** References state a rule once. Nimbus repeats
   several core rules many times (see below).

## Redundancy inventory (concrete)

Grepping the composed default prompt, the same principle is stated repeatedly:

- **"Only claim what a tool result confirms" (honesty rail)** appears in the
  `reply` field doc, the `device` delivery clause, the `mem_write` "prediction"
  note, the skills capability note, the memory-logging note, and the connector
  note. Six independent statements of one rule.
- **Memory tiers (running / scratchpad / long-term / episodic)** are explained in
  the `memory`, `mem_write`, `session_ops`, and `scratchpad` field docs, again in
  the `[HOW YOU RUN]` block, and a third time in `## HOW YOUR MEMORY WORKS`.
- **Sub-agent fire-and-forget + synthesis** is explained in `session_ops`, in
  `[HOW YOU RUN]`, and in `## CAPABILITIES`.

The single densest offender is `ORCH_D_DEVICE`: one run-on sentence enumerating
every config knob (line 5 of the golden), roughly 400 words, including the two
risk knobs. It is the largest schema macro and the least scannable text in the
prompt.

## Tool naming and capability nesting

- **Naming.** Nimbus uses dotted MCP-style names (`memory.write`,
  `session.spawn`) sanitized to underscores for the wire (`memory_write`). This
  is clean and consistent with MCP; no change proposed. opencode/crush use flat
  verbs (`read`, `edit`, `grep`).
- **Capability nesting / degradation.** Nimbus already does something the
  references do not: a rank-ordered, budget-aware tool render (`orch_world.cpp`
  `glossOf`, `kToolCoreBytes=200`, `kToolAuxBytes=88`, name-only roster past
  rank 4), plus a genuinely novel move for a device: tool *arguments* are not in
  the prompt at all. The model is told `Also callable (same call syntax; ask
  docs.search for their arguments): ...` and fetches argument schemas on demand.
  This is a good idea forced by ESP32 token/RAM limits and worth keeping. It also
  means the in-prompt tool list is already lean; the fat is in the field docs and
  the repeated behavioral prose, not the tool list.

## World-state presentation

opencode wraps volatile state in a single `<env>` block. Nimbus spreads world
state across identity, `[HOW YOU RUN]`, `## CAPABILITIES`, `## RUNNING SESSIONS`,
`## SCRATCHPAD`, `## RELEVANT MEMORIES`, and the separate `[PROVIDERS &
CONNECTORS]` catalog. The Nimbus split is defensible (these are genuinely
different kinds of state with different budgets and drop priorities), but the
*static contract* should not be tangled into it.

## What Nimbus does better (keep these)

- Honesty rails are unusually strong and specific; the failure modes they close
  (confabulated "sent", "I'll check back next turn") are real and device-specific.
  Do not weaken them; de-duplicate them.
- The budget-aware degradation ladder and the docs.search argument-fetch are the
  right answer to a 32 KB context ceiling on a microcontroller.
- The prompt-equals-wire-schema coupling (one `ORCH_D_*` source) prevents drift.
  Keep the coupling; stop *duplicating* the output into the prompt.

## Findings -> v2 targets

1. **Drop the inlined `ORCH_FIELD_DOCS` prose from the prompt; rely on the wire
   schema the provider already receives.** Replace with a 2-3 line pointer plus
   the field-specific rules that a JSON `description` cannot carry well. Expected
   saving: up to ~6 KB (44% of the fixture prompt). This is the single biggest,
   best-grounded reduction, and the exact thing both references do. Risk:
   providers attend to schema `description` fields unevenly. That risk is what the
   eval gate exists to measure, per model, before promotion.
2. **State each rule once.** Consolidate the six honesty statements into one
   labeled rail; the memory-tier explanation into one place; the sub-agent
   semantics into one place.
3. **Group the static contract above the volatile state** so a future
   prompt-cache boundary can fall between them.
4. **Keep** the tool-degradation ladder, docs.search argument fetch, risk-knob
   text, speaker-role data boundary, scheduled-turn rails, and connector honesty.
   These are safety-relevant and not redundant.

The v2 prompt implementing 1-3 lives behind an NVS flag (`promptV2`, default off,
so a fleet on defaults stays byte-identical). Its adoption is gated by the eval
suite: promote only at parity-or-better task success and tool-use correctness
with lower token cost. See `CUM-20` (flag + v2), `CUM-34` (eval gate), and
`docs/harness.md`.
