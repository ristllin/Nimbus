# File replay

Real file shapes through the shared file aids (`lib/core` `file_aids.h`), with no model calls: the day total a read
with `column`/`from`/`to`/`sum` gives, the line a line-picked edit changes, and the facts an append states. Both
engines and nimbusd call this same code, so the table is what any of them would answer.

It replaced the food replay (lane RB-REC7, owner ruling 41): the food tools, the keyed append, the legacy fold and the
reply checks it replayed are gone from the product. The owner-shaped day files from that replay (`corpus.json`,
scrubbed when it was written) are kept as `sum` cases; the cases that needed the removed code (the fold, the keyed
append's same-food note and the 13 reply-check cases) are dropped.

- `replay_driver.cpp` reads one JSON case per line on stdin and prints what the file aids do with it. It compiles
  against a tree's `orch_file_aids.cpp`, `orch_csv_sum.cpp` and `orch_psram_alloc.cpp` (a few seconds) and uses
  `test/support/fake_aid_card.h` as the card, with its refusing allocator for the `refuse_at` cases.
- `replay.py` builds the driver, runs every case and grades each answer against an oracle written in Python from the
  rules in `file_aids.h`. Exit 1 when a case misses.
- The cases: the owner-shaped `sum` cases of `corpus.json`, and for each of three invented domains (food, expenses,
  training; `tests/evals/families/domains.py`) a 15-row day with the owner's row classes (rows with more fields than
  the header, an unknown left empty, a comment line, stray blank lines) read as a day and as a range, a re-logged
  row, a torn tail, an identical row removed by line, a stale line number, a row moved to another day, and an edit
  and a total under a refusing allocator.

What a case requires (ruling 49): a stated total covers exactly the rows the result says it covers, every other row
of the range is named by line, an edit changes exactly the picked line or nothing, an append fences a torn tail and
names an identical line, and a refused allocation changes nothing and says so.

```bash
python3 tests/evals/file_replay/replay.py                     # this tree; --before <tree> adds a column
python3 -m pytest tests/evals/file_replay -q                  # the replay plus its counter-tests
```

`test_file_replay.py` also proves the grader catches each wrong answer it exists for: a total that silently misses
a named row, an unknown counted as 0, an append written twice, a torn tail not fenced, a stale line that edits
another row, a refused edit that changed the file.
