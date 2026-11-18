## What changed and why

<!-- The owner-visible problem, its root cause (why the product did the wrong thing; "missing check" is not a root cause), and the change. -->

## Model-facing text (PRD section 11: a fix never adds prompt text)

- [ ] This change adds no prompt text, guard, or instruction inside a tool result. A fix changes a tool, the transcript, or the model and profile.
- [ ] `python3 tools/release_gate/gen_model_text_catalog.py` is green, or the catalog was regenerated with `--write` and its diff read.
- [ ] Any class whose text changed has its entry in `coverage/PROMPT-CHANGES.md` (growth fails; a byte-neutral swap names its red scenario, the commit where it was red, and the parity rates before and after).
- [ ] `python3 tools/release_gate/check_no_prompt_patch.py` and `check_tool_result_imperatives.py` end GREEN (paste the last lines).

## Scenario first

- [ ] An owner-visible failure has a red multi-turn scenario (id: ...) recorded before the fix, and a register row that names it.

## Battery (paste the real marker lines)

```
```

## Cross-lane edits, risks, what dependents must know
