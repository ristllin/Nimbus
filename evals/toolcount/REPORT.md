# Tool count vs. quality: inventory, literature, and a controlled experiment

This report answers a single question for the on-device Orchestrator: **how many
tools should the model see per turn, and how should the rest be reached?** It has
four parts - an inventory of the real tool surface, a cited literature review, a
controlled experiment run through `evals/toolcount/`, and a recommendation plus a
benchmark that stays in the tree to track future changes.

## 1. Inventory - the real surface

The device advertises one global `ToolRegistry` on every Orchestrator turn
(`memory::registry().toolSpecsFor(who)`, wrapped into the provider request
`tools[]` alongside a synthetic terminal tool `orch_turn`).

| surface | tools advertised |
|---|---|
| Orchestrator, owner/admin | **49** registry tools + `orch_turn` + dynamic `mcp.<slug>.<tool>` connectors |
| Orchestrator, member/guest | **39** (10 admin-only tools dropped by `toolSpecsFor`) + `orch_turn` |
| Notifier mode | **0** (no LLM tool-loop) |
| Sub-agent turn | device registry not exposed; provider-hosted agent gets `web_search` / built-in sandbox + server connectors |

The 49 break down as: memory 7, sessions 5, results 2, web 1, schedule 4, reply 2,
skills/docs 7, health 1, tenant 3, device 4, media 4, files 9. Ten are admin-only.
This confirms the owner's recollection: the turn sits near 50 tools before any MCP
connector adds more.

There is no separate fleet-orchestrator tool surface in this repo; `lib/harness`
consumes the same registry, so the device Orchestrator turn is the surface in
question. Existing deferral patterns are for *knowledge*, not tools: `skill.*` and
`docs.*` pull bodies on demand, `results.*` page overflowed output. The function
tools themselves have no tool-search, no lazy schema loading, and no per-request
cap - every non-admin-filtered spec is sent in full on every tool-allowed round.

The inventory is pinned as data in `catalog.json`, with `core` (curated target),
`always` (lazy base), and `admin` flags, so the experiment runs against the real
surface and a host test fails if it drifts.

## 2. Literature - what current evidence says

**Bottom line.** Vendor guidance and 2025-26 research converge that tool-selection
accuracy and answer quality begin to erode in the **~20-50 tool** range, driven by
two coupled effects: schema/context bloat (dozens of tools cost tens of thousands
of tokens before the user speaks) and a selection "readout" problem where many
similar tools shrink the model's decision margin. The established fix past the
ceiling is retrieval / progressive disclosure, which cuts token overhead ~85-98%
and roughly triples selection accuracy in the reported studies.

**Vendor ceilings.**
- OpenAI recommends "fewer than 20 functions available at the start of a turn," a
  small initial set for accuracy, and evaluating at different counts. (Function
  calling guide, https://developers.openai.com/api/docs/guides/function-calling)
- Anthropic reports selection degrading past ~30-50 tools and large gains from
  deferring tool loading behind a Tool Search Tool: MCP-eval accuracy 49% -> 74%
  (Opus 4) and 79.5% -> 88.1% (Opus 4.5) at ~85% fewer tokens; a five-server MCP
  setup is "58 tools consuming ~55K tokens before the conversation starts."
  (Advanced tool use, https://www.anthropic.com/engineering/advanced-tool-use ;
  Code execution with MCP, 150K -> 2K tokens,
  https://www.anthropic.com/engineering/code-execution-with-mcp)

**Measured degradation and mitigation.**
- RAG-MCP swept the pool from N=1 to 100: success stayed >90% below ~30 tools and
  collapsed toward 100; the all-tools-in-prompt baseline selected correctly only
  13.6%, vs 43.1% with retrieval. (https://arxiv.org/abs/2505.03275)
- ScaleMCP stresses retrieval at 5,000 servers / 334,685 tools (Recall@5 ~0.94,
  MAP@5 ~0.58). (https://arxiv.org/pdf/2505.06416)
- ToolShed RAG-tool fusion reports +46-56% Recall@5 on ToolE/Seal-Tools.
  (https://www.scitepress.org/Papers/2025/133030/133030.pdf)
- Patterns: Tool RAG (embed + top-k), `search_tools` deferred loading, code-
  execution-over-MCP (import defs on demand), role/mode-scoped and hierarchical
  sub-agent toolsets, dynamic per-turn tool memory (MemTool,
  https://arxiv.org/html/2507.21428v1).

**Skeptical notes.** The cleanest accuracy-vs-N curves come from papers proposing a
retrieval fix, so they have an incentive to show steep decay. Mechanistic
degradation studies (attention-segment; the "tool-use tax") mostly use sub-frontier
open models. The effect is model-dependent (one report saw little impact on gpt-4o
but more on Claude 3.5). No public vendor-neutral "accuracy vs exact N" table exists
for current closed frontier models - which is part of why the experiment below is
worth having in-tree.

**Benchmarks referenced:** BFCL v4 (https://gorilla.cs.berkeley.edu/leaderboard.html),
MetaTool/ToolE (https://arxiv.org/abs/2310.03128), ToolSandbox
(https://arxiv.org/pdf/2408.04682), RAG-MCP, ScaleMCP, tau-bench, API-Bank
(https://aclanthology.org/2023.emnlp-main.187.pdf), ToolBench
(https://arxiv.org/abs/2307.16789).

## 3. Experiment - method

The benchmark under `evals/toolcount/` puts a provider model through a fixed task
set under three tool-exposure conditions and scores it two ways (see `README.md`
for the mechanics).

- **Conditions:** `full` (all 49 catalog tools), `curated` (the 17 `core` tools),
  `lazy` (5 always-on tools + a `search_tools` meta-tool that reveals matching
  tools on demand).
- **Tasks:** 22 fixed tasks - 17 whose ideal tool is in the curated set (the fair
  cross-condition comparison), 3 whose ideal tool was deliberately cut from the
  curated set (to measure the cost of over-curation), and 2 that need no tool (to
  measure over-triggering).
- **Scoring:** a deterministic selection check (did the first real tool call equal
  the ground-truth tool?) is the hard signal; an independent cross-provider judge
  scores appropriateness 0..1 as a soft signal that never gates.
- **Models:** `openai:gpt-4o-mini` and `zai:glm-4.6`, temperature 0.7, 4
  repetitions per task x condition. Judge is cross-provider (a model never judges
  its own rows).

## 4. Results

528 turns (2 models x 3 conditions x 22 tasks x 4 reps), 0 errors, ~$0.37 total
spend. Raw numbers are in `results.summary.json` and `results.report.txt`
(committed); the corpus is under `runs/` (gitignored). Selection accuracy is the
hard signal; the judge (soft) ran on the glm-4.6 rows only, scored by gpt-4o-mini.

| metric (core subset = fair compare) | model | full (49) | curated (17) | lazy (5+search) |
|---|---|---|---|---|
| selection accuracy | gpt-4o-mini | 76% | **82%** | 35% |
| selection accuracy | glm-4.6 | 71% | **76%** | 66% |
| coverage-subset accuracy | gpt-4o-mini | 100% | 0% | 0% |
| coverage-subset accuracy | glm-4.6 | 100% | 0% | 92% |
| mean prompt tokens | gpt-4o-mini | 1271 | 520 | 319 |
| mean prompt tokens | glm-4.6 | 2892 | 1127 | 1008 |
| lazy rows that called search_tools | gpt-4o-mini | - | - | 4 / 88 |
| lazy rows that called search_tools | glm-4.6 | - | - | 46 / 88 |

**Four findings, in order of confidence.**

1. **Curation to ~17 core tools is a clear win.** On the tasks the core set
   covers, trimming from 49 to 17 tools *raised* selection accuracy for both models
   (+6pp gpt-4o-mini, +5pp glm-4.6) while cutting prompt tokens ~59-61%. This is the
   central result and it validates the owner's 15-20 instinct directly: fewer,
   well-chosen tools selected better and cost less.

2. **Full (49) is measurably worse than curated and 2-2.5x the tokens.** The extra
   32 tools are pure distractor cost on the covered tasks - lower accuracy, higher
   wrong-call rate (gpt-4o-mini 9% vs 1%), and a per-turn token floor of ~1.3K
   (gpt-4o-mini) / ~2.9K (glm-4.6) before the user's words. glm-4.6 spends roughly
   double the tokens of gpt-4o-mini for the same tool set.

3. **Over-curation has a real, sharp cost.** Tasks whose ideal tool was cut from the
   curated set collapse to 0% accuracy under `curated` for both models (100% under
   `full`). A curated set only helps if it actually covers the task distribution;
   anything cut needs a fallback path, not silence.

4. **A tool-search / lazy tier is the right fallback, but it is model-sensitive.**
   `lazy` had the lowest token floor and let glm-4.6 recover the cut tools to 92%
   (it called `search_tools` in 46/88 lazy rows). But gpt-4o-mini almost never
   triggered the meta-tool (4/88 rows) - it fell back to an always-on tool or gave
   up, dropping to 35% core and 0% coverage. So progressive disclosure works only
   when the model reliably invokes the search step; a weaker model needs a nudge
   (a forced first search, or a stronger always-on set) or it silently regresses.
   This matches the literature's caveat that the deferral gains are model-dependent.

**Honest limits.** 4 reps x 22 tasks x 2 models at temperature 0.7 gives ~176
selection points per model - enough for the large effects above (token floor,
curation lift, the lazy split, the coverage cliff) but not for reading a 2-3pp
difference as real. The judge covered only the glm-4.6 half and never gated. Two
mid/upper-mid models, not the full frontier tier. The benchmark is built to be
re-run cheaply as models and the tool set change, which is the point.

## 5. Recommendation

**Target: a ~15-20 tool always-loaded core, everything else behind a tool-search
tier, MCP connectors always deferred.** The experiment and the literature agree,
and it maps cleanly onto patterns already in this codebase (`skill.*` / `docs.*`
pull-on-demand, `results.*` paging).

Concretely:

1. **Keep an always-advertised core of the ~17 tools flagged `core` in
   `catalog.json`** (memory.write/search, session.spawn/poll, results.get,
   web.search, reply.speak/telegram, skill.list, docs.search, device.status/control,
   media.play, files.read/search, artifact.save, image.generate). These selected
   *better* than the full set, so this is a quality win, not just a token win.

2. **Move the other ~32 tools behind a `tools.search` meta-tool** (the same shape
   as the `lazy` condition and as Anthropic's Tool Search Tool). Add a per-tool
   `deferred` flag to the registry; `toolSpecsFor(who)` advertises core + a single
   `tools.search`, and a `tools.search` call reveals matching specs for the rest of
   the turn (reuse the `docs.search` keyword matcher). Group the tail by namespace
   for retrieval: memory-admin, session-mgmt, schedule, files-admin, media-transport,
   tenant, device-diagnostics, skills-authoring, docs.

3. **Always defer MCP connector tools** (`mcp.<slug>.<tool>`) through the same
   search tier rather than advertising them all up front. This is the only
   unbounded growth path in the surface today; it is where the tool count silently
   climbs past 49 in the field.

4. **Guard against the two failure modes the experiment exposed.** (a) Over-curation:
   because a cut tool is unreachable without search, the core set must cover the
   common task distribution - treat "which tools are core" as a tracked decision,
   not an accident. (b) Weak-model search avoidance: since gpt-4o-mini rarely
   triggered `tools.search`, keep the always-on core genuinely sufficient for the
   frequent cases, and consider prompting the model to search when no core tool
   fits. The device's own provider (a frontier-class model) behaves more like
   glm-4.6 here than gpt-4o-mini, but the harness lets us verify that per model
   rather than assume it.

5. **Keep RBAC advertisement filtering as-is.** The admin-only drop composes with
   the core/deferred split; it is orthogonal.

**Tracking.** This benchmark stays in `evals/toolcount/`. A host test pins the core
set to the 15-20 band and fails if the surface drifts; re-running `run_toolcount.py`
after any tool-surface change reproduces the table above, so a regression in
selection accuracy or a jump in the token floor shows up as a number, not a
surprise in the field. New tools should be classified `core` or `deferred` at
registration, in the spirit of the existing capability-matrix rule.
