# lumi_state_prompt: a synthetic twin of a busy owner device

Input of `test/test_e1_request_ceiling` (the old engine's request on an owner text turn, measured
on the real build) and of `tools/release_gate/check_prompt_ceiling.py` (lane RB-Q3b).

Every word here is invented. The counts, sizes and classes follow the owner's device as pulled on
17 Nov 2026; nothing was copied from that pull. No use case gets a privileged place (owner rulings
41 and 42): the invented content is general (plans, a garden, a bike).

| Class | The owner's device (17 Nov) | This twin |
|---|---|---|
| Owner preferences | 4 lines, about 790 B | 4 lines, 790 B |
| Standing rules | 6 rules, about 1,030 B | 6 rules, 1,030 B |
| Open questions | none | none |
| Stored skills (the owner's) | 2 approved | 2 approved (index lines of the same length; bodies 3,700 and 1,900 B, not attached) |
| Running memory | about 780 B | 779 B |
| Scratchpad | active line + 1 short item (70 B) | the same |
| History | the fold's summary (3,765 B) and one exchange of 516 B | the same sizes |
| Recalled memories | 0 on the sampled turn (retrieval count 10) | 10 (the busier case) |
| Recent actions | not on v5.1.1 | 6 receipts of general tools |
| Connectors | 5 (gmail, notion, web_search, image_generation, document_library) | the same 5 |
| Keyed providers | anthropic, openai, mistral; head Sonnet on Anthropic | the same |
| Device tools | 61 registered (58 listed on an owner turn) | `device_tools.json`, 58 |
| Files | 343 (262 jpg, 69 md, 12 csv) in 11 projects | none: the old engine's prompt carries no file list |

`device_tools.json` holds the device's registrations (registry name, description, schema) in
registration order, from the bench `TOOLS?` capture of unit F1 (`tool_manifest_v5_1_1.json`).
`check_prompt_ceiling.py` fails when a description or schema there is not the text the source
registers, or when the source registers a device tool the file does not hold, so the
measurement cannot go stale while a tool grows. After a deliberate tool change, update the
entry to the registered text (or regenerate from a bench capture with
`tools/gen_tool_manifest_fixture.py`) and re-bless the golden with
`GOLDEN_UPDATE=1 pio test -e native -f test_e1_request_ceiling`.
