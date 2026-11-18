# Image allowance per lane

Every lane that can move a device image carries `<lane>.json` here, for example
`RB-L1b.json`. `check_image_size.py --lane <lane>` reads it and fails when the lane's
image grows by more than its allowance in code bytes.

```json
{
  "lane": "RB-L1b",
  "allowance_bytes": 34816,
  "envs": {"esp32s3": 34000},
  "why": "ledger tools and their device binding, from the map delta of a30b12fb"
}
```

- `lane` is the file's own name without `.json`.
- `allowance_bytes` is the most the code bytes may grow against `../image_baseline.json`,
  for every env that has a baseline row. `envs` overrides it per env. Integers only.
- A negative allowance is a promised deletion: the image must shrink by at least that much.
- `why` says where the figure comes from: the PRD estimate first (section 10, the flash
  ledger), then the map delta measured in the lane's PR.
- No file means an allowance of 0 B. A lane that adds only data files or Python tools
  needs no file.

What is counted:

- Code bytes are `firmware.bin` minus the docs pack. The docs pack is read from
  `firmware.elf`: the tables named in the baseline's `docs_pack_tables` and every string
  they point at. It is counted apart, so regenerating the pack never spends a lane's
  allowance, and a cut in the docs cannot hide code growth.
- Every image must still fit its app slot (the same script's partition check).
- Every env with a baseline row must be built, from this commit with a clean tree: the
  build id (`git describe`) must be inside both `firmware.bin` and `firmware.elf`, so a
  stale `.pio`, a stale ELF or a `-dirty` build fails. A built env with no baseline row is
  printed as NOT GATED until the baseline records it.
- A new docs pack (the engine-2 pack of RB-D0) adds its tables to the baseline's
  `docs_pack_tables`, or its bytes count as code.
- The baseline is the integration branch at its last merge. After a merge the
  orchestrator builds the merged commit and runs
  `python3 tools/release_gate/check_image_size.py --record-baseline`, then commits
  `image_baseline.json`. A baseline whose sha is not an ancestor of the lane's HEAD fails, and so does one that is
  stale: a firmware source changed between its sha and the lane's merge base with rb-main.

Never raise an allowance to make a merge pass: an image over its allowance is cut, or
the figure in the PRD's flash ledger is changed by an orchestrator ruling first.
