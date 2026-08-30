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

<!-- RESULTS: filled from the committed summary.json after the run. -->
_(populated below once the run completes; the numbers, the summary.json, and the
report table are committed alongside this file.)_

## 5. Recommendation

<!-- RECOMMENDATION: filled after results, but the design is literature-anchored. -->
_(populated below.)_
