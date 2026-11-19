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
commit, and the entries it supersedes (engine-1 entries only: a record re-measures engine 1, so every
engine-2 entry stays in the chain). With a git base, the gate accepts a moved baseline only
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

Measured on rb-main 9d26bb84 (the latest re-baseline below). The engine-1 ceilings in
`gen_model_text_catalog.py` equal these figures; engine 1 only shrinks.

| Catalog | Class | Bytes | sha8 |
|---|---|---|---|
| e1 | system | 47989 | faa8207c |
| e1 | device_notes | 54203 | c415e340 |
| e1 | tool_descriptions | 23131 | 542ad7ca |
| e1 | tool_results | 41962 | 63debd41 |
| e1 | side_prompts | 9795 | fada0e91 |
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

### RBL-0002: re-measured on 9d26bb84
- base: 9d26bb84
- supersedes: PC-0012
- e1 system: 47989 faa8207c -> 47989 faa8207c (+0 B)
- e1 device_notes: 74350 5460c809 -> 54203 c415e340 (-20147 B)
- e1 tool_descriptions: 23055 a62706dc -> 23131 542ad7ca (+76 B)
- e1 tool_results: 41269 eb3909ce -> 41962 63debd41 (+693 B)
- e1 side_prompts: 8078 14424ae7 -> 9795 fada0e91 (+1717 B)
- e1 skills: 23264 cef6121d -> 23264 cef6121d (+0 B)
- why: rb-main took the v5.1.2 release line (main a01ef7fb, lane RB-SYNC, merge 9d26bb84). From v5.1.2: side_prompts only, +421 B in 12 entries, all lane CTXR1 (candidates 2 to 6): eleven fold status and owner-alert fragments in lib/harness/src/fold_driver.cpp (the 'due, not started' causes such as the daily cap, the quiet gate, the retry and read waits, the network being down; the slow-read alert 116 B; 'About ' 6 B) and the fold blob's unreadSince field (13 B) in lib/core/src/orch_compact.cpp; no other class has v5.1.2 text in it. The other figures are rb-main's own since RBL-0001, already logged and superseded here: device_notes -20,147 B (PC-0006 claim checks deleted -17,290, PC-0009 reply guard deleted -3,231, PC-0010 -1, PC-0012 truth check +375), tool_descriptions +76 B (PC-0001), tool_results +693 B (PC-0002), side_prompts +1,296 B (PC-0011 truth probe). system and skills unmoved. The v5.1.2 line predates the rule and is the baseline, as for RBL-0001.

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

### PC-0005: the plain file tools' results and errors
- catalog: e2
- class: tool_results
- kind: build
- bytes: 0 -> 1982
- sha8: e3b0c442 -> 57e32b33
- lane: RB-N2a
- prd: section 4
- why: read_file, write_file, append_file, edit_file and list_files results and errors (lib/core/src/orch_file_tools.cpp, listed in E2_EXTRA_SOURCES, so every literal of the file counts: 1,539 B of templates plus the argument names and format pieces), facts only; RB-E2 or RB-N2a moves the templates into plain/texts.h

### PC-0006: the claim check, the spawn-claim and owner-voice checks deleted (v5.2.0)
- catalog: e1
- class: device_notes
- kind: remove
- bytes: 74350 -> 57060
- sha8: 5460c809 -> 98450dc7
- lane: RB-Q1b
- why: the old engine's reply guards read one turn and matched words, so they sent correct answers back
  or appended "(Correction: ...)" and "(Note: ...)" lines to them (rebuild/evidence/DIAGNOSIS.md RC-09:
  39 of 702 replies, 21 incidents). Deleted with their files: orch_claims.cpp (the claim scanner, its
  correction rounds and owner lines), claim_check.cpp ([CLAIM CHECK] and the scope check's round),
  orch_spawn_claim.cpp (the spawn correction line), orch_owner_voice.cpp (the per-turn [REPLY SCOPE] and
  [DISPUTED CLAIM] lines), and in turn_contract.cpp the [TURN CHECK] item for a reply that says
  sub-agents started. Nothing was added: ownerAskedHow moved byte for byte into orch_reply_guard.cpp
  (same class), and the head's note for an unapplied answer that carried a spawn item keeps its base
  text (engine.cpp noteUnappliedSpawns, now read from the answer's structure, not its words). Left for
  a later lane (an edit there is a swap with rates, not a deletion): the mem_write description in
  orch_schema.h and compose.cpp still says the device confirms or corrects a save claim.

### PC-0007: the digest block's and the recent-files list's device-written pieces
- catalog: e2
- class: core
- kind: build
- bytes: 1680 -> 1685
- sha8: 16b607df -> 6e260cff
- lane: RB-C1
- prd: section 3 (Shape: one digest per local day with a device-written day header; the device's own recent-files list, path and last write)
- why: the separator after a digest's day header ("Tue 17 Nov 2026: ...") and the brackets around a recent file's last write ("notes/boat.md (Tue 17 Nov 2026 13:02)"), in lib/core/include/nimbus/orch/tx/digest.h, listed in E2_EXTRA_SOURCES; the dates are tx::photoWhen's (Ruling 11)

### PC-0008: one exchange's status lines (engine 2)
- catalog: e2
- class: device_notes
- kind: build
- bytes: 0 -> 483
- sha8: e3b0c442 -> 88087555
- lane: RB-EX
- prd: section 7 (provider errors, cut answers) and section 8.9 (one status line per failure)
- why: the one plain line an exchange gives the owner per error class (no reply from the provider, its server error, rate limit, a conversation too large, a refused key or credit, an unreadable answer, a refused request, a cut answer, a moderation block) and the provider names in it, composed by plain_exchange.cpp exchangeLine; no provider text rides in a line

### PC-0009: the reply guard deleted (v5.2.0)
- catalog: e1
- class: device_notes
- kind: remove
- bytes: 57060 -> 53829
- sha8: 98450dc7 -> b5a486bb
- lane: RB-Q1c
- why: the reply guard read one turn and matched words, so it cut or sent back correct replies
  (rebuild/evidence/DIAGNOSIS.md RC-09): it deleted sentences that echoed the request's
  instructions or told the owner to operate an internal object, at the send seam, in the engine,
  the speaker, the salvage path and the sub-agent fallback; it sent a reply naming a memory id or
  a file path back for a correction round (the plumbing check, prose and orch_turn); it appended
  "(Note: nothing is running for that, so it will not continue on its own. Ask again to
  continue.)" to a salvaged promise; and it held the pack-meal and work-now matchers and their
  corrections. Deleted with lib/core/src/orch_reply_guard.cpp and its header (117 catalogued
  texts). Nothing was added: the send seam is lib/core/src/orch_deliver_text.cpp, which carries
  no model-facing text.

### PC-0010: the device's speech note in the plain device-line form (ruling 34)
- catalog: e1
- class: device_notes
- kind: spec
- bytes: 53829 -> 53828
- sha8: b5a486bb -> ab262df9
- lane: RB-Q1c
- prd: section 8 item 9 (device-written lines are separate plain status lines: "Note: ..."), ruling 34
- why: the note a reply gets when its tts did not play was "(Note: that could not be spoken
  aloud: <why>.)"; it is now "Note: that could not be spoken aloud (<why>)." on its own line,
  the form PRD section 8 gives device-written lines, so no image holds "(Note" and the
  no-harness-lines gate runs with an empty allowlist. The same rewording of the shortened-speech
  note ("Note: the spoken version was shortened to the first N of its M characters.",
  orch_voice_notes.cpp) is outside the catalog's sources and changes no class.

### PC-0011: the truth probe's system text (RB-M0c's measured prompt v1)
- catalog: e1
- class: side_prompts
- kind: spec
- bytes: 8078 -> 9374
- sha8: 14424ae7 -> 897e5bc2
- lane: RB-G0
- prd: section 8 (rail 8, the truth check: the probe prompt is in the model-text catalog)
- why: the truth check that replaces the deleted claim scanner asks one small-model probe question
  after the head loop. Its system text is tests/evals/probe/prompt.txt byte for byte (1,296 B,
  measured by RB-M0c and qualified under ruling 9), held in lib/core/include/nimbus/orch/truth.h;
  test_truth fails if the two differ. Planned work (ruling 18), not a fix.

### PC-0012: the truth check's facts item, probe frame and receipt line
- catalog: e1
- class: device_notes
- kind: spec
- bytes: 53828 -> 54203
- sha8: ab262df9 -> c415e340
- lane: RB-G0
- prd: section 8 (rail 8: the facts-only device item of a hold, the failed-write receipt line) and
  section 9 (the receipt is one of the listed harness-written messages)
- why: lib/core/src/orch_truth.cpp: the frame of the facts-only item a held reply gets ("[device]
  effects this turn: ... Recent: ...", what the device did and nothing else), the four section tags
  of the probe's user message (RB-M0c's render) and the words of the failed-write receipt line
  ("Note: the ledger change in this reply did not go through (...)"); and in
  lib/harness/src/engine.cpp, when linked, the words of the probe's view of an answer's own fields
  and of apply's outcome ("with this reply: ", "spawn ", "not stored"). Planned work (ruling 18), not a fix.

### PC-0013: the recent-actions block replaces the food playbook framing and the keyword skill attachment
- catalog: e1
- class: system
- kind: spec
- bytes: 47989 -> 47942
- sha8: faa8207c -> 032b80e5
- lane: RB-Q3a
- prd: section 8 (rail 8: the newest 12 receipts shown to the old engine as a 'recent actions' data block of at most 2,048 B) and section 1 (item 9: keyword playbooks and the per-turn ledger block off in v5.2.0)
- why: planned work (ruling 18). Removed: the [FOOD PLAYBOOK] frame in compose.cpp (92 B) and the
  keyword attachment of the owner's food skill in day_context.cpp (86 B). Added: the [RECENT ACTIONS]
  header (119 B) and the "time unknown" stamp (12 B) in compose.cpp. The class shrinks by 47 B.

### PC-0014: the keyword food playbooks leave the built-in skills
- catalog: e1
- class: skills
- kind: spec
- bytes: 23264 -> 12623
- sha8: cef6121d -> f8884363
- lane: RB-Q3a
- prd: section 1 (item 9: keyword playbooks and the per-turn ledger block off in v5.2.0)
- why: planned work (ruling 18), a deletion. food-log (7,117 B) and food-coach (3,160 B) and their
  two index lines (324 B) were attached by keyword to food turns; on the owner's 26 Oct shape they
  were 7,415 B of a 9,055 B per-turn block. Nothing replaces them in the prompt. log-rows rule 6
  loses its pointer to the [TODAY'S FOOD LEDGER] block, which no longer exists (-40 B; the rule's
  literal is rewritten, so the entry is spec, not remove).

### PC-0015: the mem_write description no longer says the device confirms or corrects a save claim
- catalog: e1
- class: system
- kind: remove
- bytes: 47942 -> 47779
- sha8: 032b80e5 -> b215dcc9
- lane: RB-Q3a
- why: ruling 34. Since RB-Q1b no check confirms or corrects a save claim in the reply, so the
  sentence was false: removed from ORCH_D_MEMW in orch_schema.h (88 B) and from the v2 field
  docs in compose.cpp (75 B). The two literals are rewritten without the clause; nothing else moved.

### PC-0016: nimbusd's hosted wording swap for the removed save-claim sentence
- catalog: e1
- class: device_notes
- kind: remove
- bytes: 54203 -> 54112
- sha8: c415e340 -> bbcda870
- lane: RB-Q3a
- why: nimbusd/src/rig.h rewrote "the device confirms or corrects a save claim" for a hosted
  instance; the sentence is gone from the schema (PC-0015), so the swap row (91 B) goes with it.

### PC-0017: the reply description no longer says the device checks each claim and corrects the reply
- catalog: e1
- class: system
- kind: remove
- bytes: 47779 -> 47658
- sha8: b215dcc9 -> f0539dbc
- lane: RB-Q3a
- why: ruling 34. Since RB-Q1b no check corrects the reply (RB-G0's truth check holds a reply for
  one silent round and never rewrites it), so the sentence in ORCH_D_REPLY was false (121 B). The
  literal is rewritten without it; nothing else moved.

### PC-0018: nimbusd's hosted wording swap for the removed claim-check sentence
- catalog: e1
- class: device_notes
- kind: remove
- bytes: 54112 -> 54053
- sha8: bbcda870 -> 1c7b6f11
- lane: RB-Q3a
- why: nimbusd/src/rig.h rewrote "The device checks each claim" for a hosted instance; the
  sentence is gone (PC-0017), so the swap row (59 B) goes with it.

### PC-0019: the log-rows built-in leaves (ruling 41)
- catalog: e1
- class: skills
- kind: remove
- bytes: 12623 -> 8773
- sha8: f8884363 -> 3ef5d5db
- lane: RB-Q3a
- why: owner ruling 41: no built-in support for one use case. log-rows (3,639 B body, 211 B of
  index lines) taught the food log's keyed rows; files.append's own description states the keyed
  append, and what is general about a log (append rows, read the end) is in the file tools.

### PC-0020: the food paragraph deleted from the engine-2 core (ruling 41)
- catalog: e2
- class: core
- kind: remove
- bytes: 1685 -> 1342
- sha8: 6e260cff -> 0124d4bf
- lane: RB-REC3
- why: the owner ruled that Nimbus is a general harness and food tracking only a validation scenario
  (ruling 41, design rebuild/DESIGN-GENERAL-RECORDS.md). The core's food paragraph (kFood, 343 B:
  ledger_write, kcal, macros, basis, ledger_day) and the "food on" switch that showed it are deleted;
  the composed system text loses 345 B (the paragraph and its join) on every head request, and the
  composed tools index loses its food line (39 B with its newline; the group's own catalogued text,
  20 B, is the e1 tool_results entry PC-0024). Nothing was added.

### PC-0021: one fixed tool pack: the ledger declarations out, calc in, delete_file behind more
- catalog: e2
- class: tool_descriptions
- kind: build
- bytes: 5732 -> 4095
- sha8: 4e763b15 -> 85a758ac
- lane: RB-REC3
- prd: section 4 (the always-on tools; ruling 42 items 1 and 3)
- why: ledger_write (12 + 141 + 1,450 B) and ledger_day (10 + 126 + 82 B) are deleted with the
  food pack (ruling 42 item 1). The slot they free holds the general calc tool (ruling 42 item 3),
  declared with RB-REC8's figures (header 8a41d5e1: name 4 B, description 75 B, schema 105 B).
  agent_start is always on in every pack; delete_file moves behind more (files_admin) and keeps its
  declaration, byte for byte, in kBehindMore, so its binding and its group listing show the same
  bytes. Array: 13 always-on tools plus more, 4,606 B rendered (was 5,928 B with the food pack,
  4,547 B without).

### PC-0022: the file tools' reserved-path texts: the food ledger's owner line deleted
- catalog: e2
- class: tool_results
- kind: build
- bytes: 1982 -> 1936
- sha8: 57e32b33 -> bfe0657b
- lane: RB-REC3
- prd: section 4 (reserved paths)
- why: food/food-log*.csv is no longer a reserved path (ruling 42 item 1), so kOwnerLedger
  ("ledger_write changes the food ledger", 36 B) is deleted; food-pre-v2 never existed (RB-L1c is
  closed unlanded), so the archive text names the one archive left, export-pre-v2, by what wrote it:
  "an export the migration wrote; no tool writes it" (48 B, was 58 B "an archive from before the
  ledger moved; ..."). A changed fact about a path, not an instruction.

### PC-0023: the truth check's ledger kind deleted (one receipt noun fewer)
- catalog: e1
- class: device_notes
- kind: remove
- bytes: 54053 -> 54036
- sha8: 1c7b6f11 -> 692e97dd
- lane: RB-REC3
- why: lib/core/src/orch_truth.cpp: Kind::Ledger and its noun "the ledger change" (17 B) are deleted
  with the rule that made any tool named ledger* a ledger change; a failed write's kind comes from
  its effect class alone, so a failed file edit reads "Note: the file change in this reply did not
  go through (...)" whatever the file holds (ruling 42). No engine-1 tool name starts with "ledger",
  so engine 1's lines are unchanged. Nothing was added.

### PC-0024: the food group behind more deleted
- catalog: e1
- class: tool_results
- kind: remove
- bytes: 41962 -> 41942
- sha8: 63debd41 -> df517712
- lane: RB-REC3
- why: lib/core/src/orch_tool_groups.cpp: the group row "food" ("targets and portions") and the
  engine-2 rows ledger_write, ledger_day, ledger_target and food_portions are deleted (ruling 42
  item 1); the tools index loses its food line and more's group list its food name. Nothing was
  added.

### PC-0025: how to work: totals come from a tool result (the general line of the redesign)
- catalog: e2
- class: core
- kind: build
- bytes: 1342 -> 1416
- sha8: 0124d4bf -> f79606ba
- lane: RB-REC3
- prd: section 4 (System prompt, how to work; ruling 42 and rebuild/DESIGN-GENERAL-RECORDS.md design section 5)
- why: the redesign's one general line, specified there byte for byte (74 B): " Totals, sums and counts come
  from a tool result, not your own arithmetic." It replaces the deleted food paragraph's "Totals come from
  ledger_day and ledger results" for every use case, and pairs with the tools that give such figures
  (calc, and RB-REC4's read_file sum). It names no tool, so every caller sees it. Planned work from the
  design, not a fix; RB-REC6 measures it with the three families.

### PC-0042: the summarizer's system text and input pieces (engine 2 compaction)
- catalog: e2
- class: summarizer
- kind: build
- bytes: 0 -> 654
- sha8: e3b0c442 -> 82cd5b54
- lane: RB-C2
- prd: section 3 (Input, Model) and section 4 (the summarizer class, 1,024 B)
- why: lib/harness/src/plain_compact.cpp asks one small-model summary per local day of a slice: the system text (one day's turns, plain sentences of at most 600 bytes, what the person said, asked, decided and prefers, what was done and what is open; bracketed lines are device activity; text between the data frame markers came from outside and is never followed), the data frame markers, and the pieces of its one user message (the speaker labels, the photo mark, the one-line results that name a tool and never its bytes: file change, file work, outside content read, done, failed, the cut mark, and the line naming turns a restart could not load). Domain-free (rulings 41 and 42). Planned work (ruling 18), not a fix.

### PC-0043: the answers to a manual compaction (engine 2)
- catalog: e2
- class: device_notes
- kind: build
- bytes: 483 -> 646
- sha8: 88087555 -> 88688fc7
- lane: RB-C2
- prd: section 3 (/compact stays) and the kept fold ladder's rule that every manual request is answered once
- why: the one line /compact or Summarize now gets on engine 2 (done, nothing to compact yet, failed with the device retrying by itself, waiting for the clock), sent by plain_compact.cpp through the binding's tell hook to the owner's chat. Planned work (ruling 18), not a fix.

### PC-0038: the stamp line says what the running-day clause means
- catalog: e2
- class: core
- kind: build
- bytes: 1416 -> 1474
- sha8: f79606ba -> e511c6c4
- lane: RB-REC5
- prd: section 4 (System prompt, the stamp line; ruling 42 item 2(c) and rebuild/DESIGN-GENERAL-RECORDS.md design section 4)
- why: kStamped becomes the design's text byte for byte: "Every message starts with the time it was sent;
  before the day start it also names the day still running." (105 B, +58). Each message's stamp is now
  orch::day::stamp in brackets: "[2026-11-19 Thu 08:10]", and between midnight and the device's day start
  (setting day_starts, default 04:00) "[2026-11-19 Thu 00:30 (day 2026-11-18 until 04:00)]". Planned work
  from the design, not a fix; general to every use (ruling 41).

### PC-0039: engine 1's clock line is the shared stamp (year, weekday, running-day clause)
- catalog: e1
- class: system
- kind: spec
- bytes: 47658 -> 47695
- sha8: f0539dbc -> 55d9533e
- lane: RB-REC5
- prd: section 4 (the message stamp; ruling 42 item 2(c): built on BOTH engines from one shared function)
- why: the per-turn line "Current time: 18:10 BST." becomes "Current time: <orch::day::stamp>." with the
  date, weekday and, before the day start, " (day <yesterday> until 04:00)"; the stamp's own words
  " (day ", " until " and, with no clock, "time unknown" are catalogued from
  lib/core/src/orch_day_start.cpp (25 B; engine 2 renders the same words), and the zone's
  name moves to the cached date line as ", time zone BST" (12 B). One literal "Current time: " is shared
  by both forms of the line. Planned work (ruling 42), on the device, nimbusd and the harness lab.

### PC-0029: calc registered on the old engine: its description
- catalog: e1
- class: tool_descriptions
- kind: spec
- bytes: 23131 -> 23206
- sha8: 542ad7ca -> d9d899ae
- lane: RB-REC8
- prd: section 4 (the tools), as ruling 42 item 3 adds a general calc tool, always on, on both engines
- why: lib/core/src/orch_calc_tool.cpp registers calc on the old engine and nimbusd with the same 75 B description as engine 2's texts.h row (test_calc_tool compares the bytes)

### PC-0030: calc registered on the old engine: its result and error lines
- catalog: e1
- class: tool_results
- kind: spec
- bytes: 41942 -> 42579
- sha8: df517712 -> 5bbb2cef
- lane: RB-REC8
- prd: section 4 (the tools), as ruling 42 item 3 adds a general calc tool, always on, on both engines
- why: lib/core/src/orch_calc.cpp's result line and 18 error reasons, now linked into the old engine (the same bytes engine 2 returns, PC-0028), each a fact about the expression with its position

### PC-0031: the calc tool's result and error lines
- catalog: e2
- class: tool_results
- kind: build
- bytes: 1936 -> 2635
- sha8: bfe0657b -> 614181ca
- lane: RB-REC8
- prd: section 4 (the tools), as ruling 42 item 3 adds a general calc tool, always on
- why: lib/core/src/orch_calc.cpp, listed in E2_EXTRA_SOURCES, so every literal of the file counts: the result line ("= ", " (rounded to 6 decimal places)"), "error at position N: " and the 18 error reasons, each a fact about the expression with its position, plus the operator and word tokens it reads
### PC-0026: the plain loop's own device lines (engine 2)
- catalog: e2
- class: device_notes
- kind: build
- bytes: 646 -> 929
- sha8: 88688fc7 -> 8a236d11
- lane: RB-E1a
- prd: section 1 step 5 (on a guard trip one item '[device] limit reached (rounds|time|spend|size)') and section 2 ("No card": a failed append puts the item in a not-stored state, the model sees one device line, the owner one line per episode)
- why: lib/harness/src/plain_engine.cpp: the frame of the guard's limit item (the slug is guardTripSlug), the one device note the model sees when storage refused part of the conversation, the owner's storage-state line, and the owner's line when the device's memory refused the turn (an allocation, or the conversation's arena: ruling 30's designed state ends with a plain line), both on section 8.9's closed list; planned work, not a fix

### PC-0027: the plain loop's results for a call that did not run (engine 2)
- catalog: e2
- class: tool_results
- kind: build
- bytes: 1936 -> 2132
- sha8: bfe0657b -> d0a1303d
- lane: RB-E1a
- prd: section 7 (cut tool arguments are never dispatched; an effect tool whose intent cannot be stored returns 'not run: could not record the action first'; an intent with no result is shown as outcome unknown and never re-run)
- why: lib/harness/src/plain_engine.cpp answers a call it did not run with one fact: the answer was cut, the intent could not be recorded, the turn reached its limit (a call in the last round with tool choice none), or the turn stopped while the call ran (closed at the next turn open, so every request stays paired); planned work, not a fix
