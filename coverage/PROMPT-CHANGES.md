# Prompt changes: the no-prompt-patch ledger

The owner's rule (17 Nov 2026, PRD section 11): a fix may not add prompt text, a guard, or an
instruction inside a tool result. It changes a tool, the transcript, or the model and profile,
and passes the reference-loop parity suite (L3). Months of fixes each added prompt text or a
guard, and each pushed more of the conversation out of the request; the "Lane QFIX rulings"
in coverage/OWNER-ISSUES-REGISTER.md are a record of that pattern.

`tools/release_gate/check_no_prompt_patch.py` enforces it over every model-facing string that
`tools/release_gate/gen_model_text_catalog.py` extracts into the catalog goldens
(`test/golden/model_text_catalog_e1.txt` for the old engine, `test/golden/model_text_catalog.txt`
for the plain engine once its `texts.h` exists). This file is the gate's ledger:

- The **Baseline** holds each class's bytes and sha8 when the gate was introduced. It is
  written once, and equals `E1_BASELINE` in the generator (the engine-1 ceilings).
- Every later change to a class's text needs one entry under **Changes**, in merge order. Its
  `bytes` and `sha8` go from the class's last accepted state (the baseline or the previous
  entry for that class) to the class as the regenerated golden shows it. Entries chain, so an
  entry cannot be reused, and a merged entry is never edited.
- **Growth fails.** A class that ends larger than it started fails whatever the entry says.
  The one exception is kind `build` on a catalog in its build phase (the plain engine while it
  is written to its spec, until its candidate gate); it names the PRD section and stays under
  the class ceiling.
- Kind `swap`: text was added and at least as many bytes were removed from the same class. It
  needs the red scenario first: `scenario` (an id that resolves under
  `tests/evals/**/scenarios`), `red_at` (the commit where that scenario was red) and `rates`
  (the parity pass rate before and after the change, from the same suite and head; the after
  rate may not be lower).
- Kind `spec` (ruling 18): planned work, not a fix. A lane of the live plan (`rebuild/PLAN.json`;
  off that machine, `tools/engine/owners.json`) implements a tool or text the PRD specifies, so
  it may grow one class for one change: the entry names the lane (`lane:`) and the PRD section
  (`prd: section N`), needs no parity rates (the lane's own tests and the stage gate cover it),
  and raises that class's ceiling by exactly its growth. It chains like every other kind, so it
  covers one change only. A release line taken into the integration branch is not a spec
  entry: the orchestrator re-baselines (below). At the v5.2.0 release, engine 1's total bytes
  over all classes must be lower than the baseline total (`check_no_prompt_patch.py --release`, `release_total_problems()` for RB-R1).
- Kind `remove`: the class only shrank. With a git base the gate compares the class's texts
  with the base golden, and an added text fails the entry.

**Re-baseline (the orchestrator's step, not a lane's).** When a release line is taken into the
integration branch (v5.1.2 into rb-main, and its later rounds), the orchestrator re-measures the
baseline on the merged tree with one command, reviews the diff, and commits it:

```
python3 tools/release_gate/gen_model_text_catalog.py --rebaseline "<what moved, per class, and which lane it came from>"
python3 tools/release_gate/check_no_prompt_patch.py && python3 tools/release_gate/check_tool_result_imperatives.py
```

It regenerates the golden, rewrites `E1_BASELINE` (the engine-1 ceilings) and the baseline table,
and appends a record under **Re-baselines** with each class's old and new bytes and sha8, the
commit, and the entries it supersedes. With a git base, the gate accepts a moved baseline only
with such a record added in the same change and starting at the base's baseline; merged records
are never edited. If the imperative lint then flags a template the release line added, that row
is added to the imperative baseline in the same commit and named in the record's why.

How to make a change (normally only a deletion should ever need one):

1. Run `python3 tools/release_gate/gen_model_text_catalog.py --write` and read the golden's diff.
2. `python3 tools/release_gate/check_no_prompt_patch.py` names each class that changed with
   its before and after state; copy them into a new entry.
3. A swap: write the scenario first, record it red, then make the change and record the rates.

Entry form (one per class changed; the next free id):

```
### PC-0001: the change in a few words
- catalog: e1
- class: tool_results
- kind: swap
- bytes: 41178 -> 41170
- sha8: 165fc9f4 -> 0a1b2c3d
- lane: RB-XX
- scenario: lc-day-total
- red_at: 1a2b3c4d
- rates: before=0.60 after=0.98 (owner_regressions, claude-haiku-5-5, 5 reps each)
- why: what was wrong in the product, and why a text change was the fix
```

A `build` or `spec` entry has `prd: section N` in place of `scenario`, `red_at` and `rates`.

## Baseline

Measured on rb-main 9b17824d (the latest re-baseline below). The engine-1 ceilings in
`gen_model_text_catalog.py` equal these figures; engine 1 only shrinks.

| Catalog | Class | Bytes | sha8 |
|---|---|---|---|
| e1 | system | 47989 | faa8207c |
| e1 | device_notes | 74350 | 5460c809 |
| e1 | tool_descriptions | 23055 | a62706dc |
| e1 | tool_results | 41269 | eb3909ce |
| e1 | side_prompts | 8078 | 14424ae7 |
| e1 | skills | 23264 | cef6121d |

## Imperative baseline

Engine-1 tool result and error templates that give the model an order, found by
`tools/release_gate/check_tool_result_imperatives.py` when the rule was introduced. Results
state facts and never give orders (PRD section 4). This list only shrinks: a template not on
it that gives an order fails, and a row whose template is gone or reworded as a fact must be
deleted. Engine 2 has no baseline.

| Source | sha8 | Order | Template (start) |
|---|---|---|---|
| lib/core/include/nimbus/orch/result_envelope.h | 40c46ffd | read the | ; read the rest with files.read from an offset] |
| lib/core/src/orch_file_append.cpp | 1012588a | start it |  starts with # (a comment row) - start it with its first col |
| lib/core/src/orch_file_append.cpp | 16ed2d94 | send fewer |  B) - send fewer rows per call |
| lib/core/src/orch_file_append.cpp | 4ee45099 | Use the | . Use the earlier figure per unit unless the portion or reci |
| lib/core/src/orch_file_append.cpp | 604baf3a | put a |  has a broken quote (an unclosed quote, or text after a clos |
| lib/core/src/orch_file_append.cpp | 656c6fde | start the | this log has no date column, and a keyed write without one w |
| lib/core/src/orch_file_append.cpp | 7917432a | add fewer |  B) - add fewer rows per call |
| lib/core/src/orch_file_append.cpp | 8c3336d6 | write one | ' is not a plain number - write one number, like 450 |
| lib/core/src/orch_file_append.cpp | 94959a39 | try again | the card read failed - the log was not changed; try again |
| lib/core/src/orch_file_append.cpp | b4501c1e | set aside | - set aside  |
| lib/core/src/orch_file_append.cpp | b76f2b85 | read it |  columns; read it with files.read {project, name, tail: 1} |
| lib/core/src/orch_file_append.cpp | ba15659d | send the | creating a CSV needs its header row first (the column names, |
| lib/core/src/orch_file_append.cpp | c281956d | put a |  - put a field that holds a comma in double quotes |
| lib/core/src/orch_file_append.cpp | dfe382fb | send the | ) - send the stored header and its column order |
| lib/core/src/orch_file_append.cpp | eebf4dc6 | send one | ' - send one row per date and key |
| lib/core/src/orch_file_edit.cpp | 344f286e | make the |  per call) - make the find text longer |
| lib/core/src/orch_file_edit.cpp | 35c6a867 | split the |  B - split the edit |
| lib/core/src/orch_file_edit.cpp | 471f0eb6 | start a | the file is over 1 MB, too large to edit in place - start a  |
| lib/core/src/orch_file_edit.cpp | 6073de08 | edit a |  B - edit a smaller span |
| lib/core/src/orch_food_figures.cpp | 42eb4c5f | Check it | . Check it against your plan.) |
| lib/core/src/orch_food_figures.cpp | 647d2f4a | Say so, | ": a row of today's ledger cannot be read, so the day's tota |
| lib/core/src/orch_food_figures.cpp | 99e6aadd | do not | Your reply states food figures the device's records do not b |
| lib/core/src/orch_food_figures.cpp | a6da59a5 | Read it | , and nothing this turn holds it (no stored target, standing |
| lib/core/src/orch_food_figures.cpp | b7ee91fb | do not | , from its rows. Use the ledger's figure; do not add up a da |
| lib/core/src/orch_food_figures.cpp | cb1072de | Check it |  days old. Check it is still current.) |
| lib/core/src/orch_food_figures.cpp | d3c7eabe | Do not |  of today's rows have no protein figure, so the real figure  |
| lib/core/src/orch_food_figures.cpp | e36e0050 | Check it |  left. Check it against your plan.) |
| lib/core/src/orch_food_portions.cpp | 5f71f8a2 | give at | give at least one food |
| lib/core/src/orch_food_portions.cpp | 62de1e21 | call again | . A food of the meal the owner named that is missing here is |
| lib/core/src/orch_food_portions.cpp | d5a66ad2 | give amount_g | give amount_g (the portion you would choose in grams; the pr |
| lib/core/src/orch_memory_tools.cpp | 045eec69 | do not | . Nothing keeps or retries it: tell the owner it was not sav |
| lib/core/src/orch_memory_tools.cpp | 0612be8c | do not |  automatically when embeddings work again; keyword search fi |
| lib/core/src/orch_memory_tools.cpp | 149e80eb | ask the | ) - ask the device's admin to raise it, or remove an old mem |
| lib/core/src/orch_memory_tools.cpp | 30e2ae18 | add credit |  (add credit on the provider's billing page) |
| lib/core/src/orch_memory_tools.cpp | 31bfd090 | do not | . Until then it is found by its words, not by meaning. Tell  |
| lib/core/src/orch_memory_tools.cpp | 3eb4bcb6 | use search|restore|list) | unknown action (use search/restore/list) |
| lib/core/src/orch_memory_tools.cpp | 46a9c226 | search again |  Every word in that query is a common word that keyword sear |
| lib/core/src/orch_memory_tools.cpp | 6035d05f | Search again | No matching episodic messages: every word in that search is  |
| lib/core/src/orch_memory_tools.cpp | 88415764 | add the |  (add the embedding provider's key under Assistant > Models  |
| lib/core/src/orch_memory_tools.cpp | 8bc215ed | use view|update) | unknown action (use view/update) |
| lib/core/src/orch_memory_tools.cpp | a2a17600 | use view|set_active|add|replace|clear) | unknown action (use view/set_active/add/replace/clear) |
| lib/core/src/orch_memory_tools.cpp | b7ca0076 | use pin|unpin|delete) | unknown action (use pin/unpin/delete) |
| lib/core/src/orch_memory_tools.cpp | c68b31d9 | use memory.pin | ; it is NOT pinned - use memory.pin to pin it) |
| lib/core/src/orch_memory_tools.cpp | cb1fc78a | remove a | ) - remove a live memory before restoring one |
| lib/core/src/orch_memory_tools.cpp | d85b9bc2 | set Dimensions |  (set Dimensions to the model's size in Memory > Embedding m |
| lib/core/src/orch_memory_tools.cpp | e4b26a15 | save a | too long to keep while embeddings are down; save a shorter v |
| lib/core/src/orch_memory_tools.cpp | e90266c7 | do not | . Removing a memory also makes room. Tell the owner exactly  |
| lib/core/src/orch_models_tool.cpp | 4f4fa159 | ask with |  (built-in list); ask with refresh:true.  |
| lib/core/src/orch_models_tool.cpp | 4fb4fe19 | pick one |  is a cumulo router id; pick one of  |
| lib/core/src/orch_models_tool.cpp | 8992e611 | Call models.list | . Call models.list for the model ids each one offers before  |
| lib/core/src/orch_openq_tools.cpp | 11a21b81 | ask the | . It is shown to you every turn as [OPEN QUESTIONS]; ask the |
| lib/core/src/orch_prefs_tools.cpp | 22f106f0 | Never use | No standing rules yet. Add one with /prefs add <rule>, for e |
| lib/core/src/orch_prefs_tools.cpp | 3984f9cf | Tell the |  or the web page). Tell the owner it waits for approval. |
| lib/core/src/orch_prefs_tools.cpp | 3da5e2e5 | Make it |  Make it a rule again with /prefs restore  |
| lib/core/src/orch_prefs_tools.cpp | 83d4c6a9 | Do NOT |  Tell the owner plainly and ask which rule to remove. Do NOT |
| lib/core/src/orch_prefs_tools.cpp | a1b9a98b | Never use | Standing rules: /prefs lists them. /prefs add <rule> saves o |
| lib/core/src/orch_result_store.cpp | 09c7179b | try memory.episodic | ' is not in the recent-results ring - try memory.episodic wi |
| lib/core/src/orch_tool_registry.cpp | 30673602 | put your | put your answer to the owner there |
| lib/core/src/orch_tool_registry.cpp | 8f5f60fb | Use the | Use the  |
| lib/core/src/orch_tool_registry.cpp | 9409a251 | add a | add a search string and the results arrive next turn |
| lib/core/src/orch_tool_registry.cpp | a7874baf | put your | put your question for the owner there |
| lib/core/src/orch_tool_registry.cpp | bc19cac4 | set active/short/mid/long | set active/short/mid/long to update your plan |
| lib/core/src/orch_tool_registry.cpp | f148d4d3 | add the | add the device action there to apply it after this turn |
| lib/core/src/orch_turn.cpp | 74572a52 | reply must | reply must be a string |
| lib/harness/src/skill_store.cpp | 86a555f7 | do not |  - WAITING FOR THE OWNER'S APPROVAL - not active: do not fol |
| lib/harness/src/skill_store.cpp | b01cefab | pick a | ' is a built-in skill - pick a different id |
| lib/harness/src/skill_store.cpp | ed79a1ff | put the | '); put the description in desc: instead |
| lib/harness/src/skill_store.cpp | f28843d5 | ask them |  skill changes are already waiting for the owner's approval  |
| lib/harness/src/websearch.cpp | f02a401d | check the |  (check the Tavily API key) |
| nimbusd/src/hosted_loops.h | 32ade2ea | do not |  min you get one automatic turn with your note. It fires onc |
| nimbusd/src/hosted_loops.h | eb3b2bc3 | tell the | ), pending the owner's approval. It will NOT fire until they |
| nimbusd/src/hosted_photos.h | 89fc118d | answer from | This photo came with the current message and is already in f |
| nimbusd/src/hosted_skills.h | 32ecc0fd | call skill_list) | unknown skill id (call skill_list) |
| nimbusd/src/posix_files.h | 020f8309 | ask the | Files on this instance belong to its owner; ask the owner to |
| nimbusd/src/posix_files.h | 34648981 | pass offset |  files; pass offset to page)  |
| nimbusd/src/posix_files.h | dde4e371 | read the | short write - read the tail before adding the rows again |
| nimbusd/src/rig.h | 80bbe265 | try fewer | no sections match - try fewer keywords |
| src/agent/connectors.cpp | 38d1d613 | Start again | Sign-in did not complete. Start again from the connector. |
| src/agent/connectors.cpp | 5ea71fb2 | Use a | Browser sign-in works only with a hosted https server. Use a |
| src/agent/connectors.cpp | 64cd5e9a | Pick a | Pick a connector to connect. |
| src/agent/connectors.cpp | 68b983c9 | Fix it |  has an invalid URL. Fix it on the device web page. |
| src/agent/connectors.cpp | dce6bc02 | Try again |  is cooling down after repeated failures. Try again in about |
| src/agent/files_subsystem.cpp | 0b9b074a | try again | an upload is in progress - try again |
| src/agent/files_subsystem.cpp | 1a843fa9 | read the |  B landed - read the tail before adding the rows again |
| src/agent/files_subsystem.cpp | 220158e8 | do not | the download runs only if they approve. Tell the owner it is |
| src/agent/files_subsystem.cpp | 2f373de6 | try again | the card read failed - nothing was added; try again |
| src/agent/files_subsystem.cpp | 3890f2a2 | try again | the card read or write failed - nothing was added; try again |
| src/agent/files_subsystem.cpp | 5bb41fb8 | try again | Telegram media queue busy or Telegram not configured - try a |
| src/agent/files_subsystem.cpp | 61f62253 | set policy | ) - kept OUT of the store; set policy to approve for manual  |
| src/agent/files_subsystem.cpp | 688add0f | start a | the log is over 1 MB - start a new log file (like food-log-2 |
| src/agent/files_subsystem.cpp | 87e3b7b0 | try again | the file could not be read - try again |
| src/agent/files_subsystem.cpp | 94959a39 | try again | the card read failed - the log was not changed; try again |
| src/agent/files_subsystem.cpp | 9d0ffed3 | ask the | this conversation has reached its file-storage limit - ask t |
| src/agent/files_subsystem.cpp | a09a060f | use files.share | binary or non-text document - use files.share / the web view |
| src/agent/files_subsystem.cpp | a239f3eb | try again] | [no day total: the card read failed - try again]  |
| src/agent/files_subsystem.cpp | bd573da5 | set an | no image provider - set an OpenAI key |
| src/agent/files_subsystem.cpp | d3102b0e | check files.list | ). It runs in the background - check files.list shortly; onl |
| src/agent/files_subsystem.cpp | ee454775 | Send it |  Send it to the owner with files.send. |
| src/agent/loops_subsystem.cpp | a1433dd5 | Ask me | No scheduled loops. Ask me to set one up, e.g. "every mornin |
| src/agent/memory_subsystem.cpp | 1959bd04 | call docs.list | ' (call docs.list with no arguments for the file index) |
| src/agent/memory_subsystem.cpp | 320f8d4b | reply with | voice replies are switched OFF - reply with text; if the own |
| src/agent/memory_subsystem.cpp | 32ade2ea | do not |  min you get one automatic turn with your note. It fires onc |
| src/agent/memory_subsystem.cpp | 32ecc0fd | call skill_list) | unknown skill id (call skill_list) |
| src/agent/memory_subsystem.cpp | 34618a4c | call docs.list | ' (call docs.list or docs.search) |
| src/agent/memory_subsystem.cpp | 56ef9302 | edit it, | edit it, save there instead. |
| src/agent/memory_subsystem.cpp | 7d158dd0 | try different | no sections match - try different keywords, or browse docs.l |
| src/agent/memory_subsystem.cpp | bf9ef5bb | reply in | ; reply in text instead |
| src/agent/memory_subsystem.cpp | eb3b2bc3 | tell the | ), pending the owner's approval. It will NOT fire until they |

## Re-baselines

### RBL-0001: re-measured on 9b17824d
- base: 9b17824d
- supersedes: none
- e1 system: 47989 faa8207c -> 47989 faa8207c (+0 B)
- e1 device_notes: 74350 5460c809 -> 74350 5460c809 (+0 B)
- e1 tool_descriptions: 23055 a62706dc -> 23055 a62706dc (+0 B)
- e1 tool_results: 41269 eb3909ce -> 41269 eb3909ce (+0 B)
- e1 side_prompts: 7641 041badf6 -> 8078 14424ae7 (+437 B)
- e1 skills: 23264 cef6121d -> 23264 cef6121d (+0 B)
- why: rb-main 9b17824d took the v5.1.2 release line (main d5afa30a). Against 831986f9 only side_prompts moved (+437 B): lane CTXR1 added three owner alerts in lib/harness/src/fold_driver.cpp (older messages unreadable from storage: 175, 82 and 169 B) and the fold blob's guestPaid field (11 B) in lib/core/src/orch_compact.cpp. The rest of v5.1.2 (CTXR2, CTXE, REL512) changed no catalogued text. The v5.1.2 line predates the rule and is the baseline (coordinator, 18 Nov).

## Changes

### PC-0001: RB-N4's tools groups (more) catalogued: its description
- catalog: e1
- class: tool_descriptions
- kind: spec
- bytes: 23055 -> 23131
- sha8: a62706dc -> 542ad7ca
- lane: RB-N4
- prd: section 4 (groups behind more: the more tool and its group listing)
- why: RB-N4 (merged on rb-main before this entry) added lib/core/src/orch_tool_groups.cpp, which registers the
  more tool; the coverage check found it outside the catalog, so RB-B5 listed it under tool_results and logs its
  76 B description here. Planned work by a lane of the plan (ruling 18), not a fix.

### PC-0002: RB-N4's tools groups (more) catalogued: its results and group lines
- catalog: e1
- class: tool_results
- kind: spec
- bytes: 41269 -> 41962
- sha8: eb3909ce -> 63debd41
- lane: RB-N4
- prd: section 4 (groups behind more: device, agents, files_admin, media, food, people, models, docs, skills,
  connectors)
- why: the group one-liners, the tools-index lines and the more tool's results and errors of
  lib/core/src/orch_tool_groups.cpp (693 B), catalogued by RB-B5 with the file. Planned work (ruling 18).

### PC-0003: engine-2 system core, composed by plain_request.cpp
- catalog: e2
- class: core
- kind: build
- bytes: 0 -> 1680
- sha8: e3b0c442 -> 16b607df
- lane: RB-E2
- prd: section 4
- why: the plain engine's system core (identity and channel, time zone, how to work, what you see, outside content, the recall line, the food paragraph, the model line and the tools index header), RB-M0a's measured text made person-neutral

### PC-0004: the 14 always-on tools (16 declarations, both packs)
- catalog: e2
- class: tool_descriptions
- kind: build
- bytes: 0 -> 5732
- sha8: e3b0c442 -> 4e763b15
- lane: RB-E2
- prd: section 4
- why: names, descriptions and schemas of the always-on tools and the pack pair agent_start and delete_file, RB-M0a's measured shapes (Ruling 11)
