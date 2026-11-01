# Food replay

Replays the owner's week of 22 Oct to 1 Nov 2026 through the device's receipt and claim checks, with no model calls: the
ledger as it stood and the reply as it was delivered. Built against two trees, it gives the before and after table of what
the device would have said.

- `corpus.json` holds the cases, scrubbed: the food words and names are changed, the shapes and figures are the real ones.
  A case is a stored log and the day (`sum`: what the receipt must say) or a reply with the facts at that moment (`reply`:
  what the checks must add).
- `replay_driver.cpp` reads the cases as JSON on stdin and prints what the tree's code does with each. It links the harness
  lab's objects, so it runs the same `lib/core` the firmware does.
- `replay.py` runs the driver on one or two trees, grades each case and prints the table. Exit 1 when the after tree
  misses a case.

```bash
make -C tools/harness-lab                                        # once per tree
python3 tests/evals/food_replay/replay.py --after . --before <tree of the base commit>
```

`--full <file.jsonl>` also runs every delivered reply of a private transcript through the checks and lists what they would
add, so a new rule is read against all the real replies before it ships. That file holds personal data and stays out of the repo.
