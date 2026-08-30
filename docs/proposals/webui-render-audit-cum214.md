# Web app render-audit (CUM-214)

Status: findings for owner review. One fix landed (the Safety checkbox exemplar);
everything else is a proposal, not a change. Taste calls are the owner's to rule.

## Why this exists

Pages were snapshot-tested for byte regressions, never looked at the way a person
sees them. A byte-diff passes even when the snapshot itself encodes a wrong layout,
which is exactly how the Safety tab shipped with centered checkboxes. This audit
renders every page, subtab, and first-run state and reviews each one for the four
questions in CUM-214: too much text or bad formatting, unclear for a new user,
annoying for an experienced user, and alignment or consistency bugs.

## Method

The served page is the ordered concatenation of the PROGMEM fragments in
`include/web/ui_*.h`. The host harness rebuilds that exact page
(`tools/webui_harness/concat.mjs`, byte-identical to the device), serves it with
mock `/api` responses, and drives it with Chromium at two widths: desktop
(1280 x 900) and phone (Pixel 7).

Regenerate every screenshot (32 images) into `screenshots/audit/`:

```bash
cd tools/webui_harness
npm install                        # first time only; chromium is cached on the bench
npx playwright test 95_audit_shots
```

Output names: `screenshots/audit/<desktop|phone>-<surface>.png`. The audit spec
(`tests/95_audit_shots.spec.mjs`) expands every collapsible section and opens the
tap-? hints so the shots show the real surface area, not just closed summaries.

## The exemplar (landed)

**Assistant -> Safety: checkboxes rendered centered, not left-aligned.**

Root cause: the design system's field rule
`input,select,textarea{width:100%}` also matched checkboxes and radios. A
full-width checkbox centers its glyph in the row and pushes the label to the next
line. The controls that looked right (for example the Tools tab toggles) were
saved only because they sit inside a `.pr` wrapper that already had a
`.pr input{width:auto}` override. Every checkbox and radio outside `.pr` was
broken: the Safety guest-moderation toggles, the Device battery-mode radios, the
Display flip toggle, and the routine day-picker.

Fix (commit on branch `lane/TF-A1`): make the override apply to the whole class,
not one wrapper.

```css
input[type=checkbox],input[type=radio]{width:auto;margin:0}
```

This is a root-cause class fix, not a per-instance patch, so it also corrects the
other unwrapped checkboxes and radios in the same stroke. Before and after are in
`screenshots/audit-before/` and `screenshots/audit/` (`*-safety-focus.png`,
`*-assistant-safety.png`, `*-expanded-device.png`). Verified at both widths.

## Per-surface verdict

Read these against the regenerated screenshots. Verdict is one of: clean,
proposal (a taste call for the owner), or bug (a defect worth a follow-up issue).

### Home  -  clean

Stat tiles, quick actions, active sessions, and the health panel read well at both
widths. One small redundancy proposal below (tiles and the info line repeat RAM,
PSRAM, and battery).

### Chat  -  clean, one small proposal

Empty state is clear and the drop-zone hint is good. Proposal: Attach and Send sit
at the top of a tall multi-line composer; bottom-aligning them reads better as the
box grows.

### Memory  -  clean

Collapsible groups (Memory, Long-term memory, Files) keep a long page manageable.
See the defensive-rendering proposal for the directive field on a fresh device.

### Assistant / Models  -  proposal

The first thing a new user needs here, Providers and keys, is collapsed by default
behind an accordion. Someone setting up their first provider has to guess that the
key lives one click inside a closed section. Proposal: open Providers and keys by
default, or auto-open it whenever no provider is verified yet.

### Assistant / Connectors  -  proposal

The Telegram readiness line concatenates three not-set-up states into one dense red
paragraph joined by middots ("No bot token ... No one approved yet ... No openai
key ..."). For a new user who simply has not set up Telegram, a wall of red reads
as three errors, which the copy guide warns against (no alarm for a normal state).
Proposal: neutral tone for setup states, or a short checklist, and reserve red for
a real failure.

### Assistant / Tools  -  clean

Dense but well grouped. Checkboxes now left-align correctly.

### Assistant / Skills  -  clean, one nit

The status line can render a trailing separator with nothing after it
("summarize  .") when there is one item. Minor formatting nit.

### Assistant / Routines  -  clean

Clear structure: routines list, wake-ups policy, new-routine form.

### Assistant / Usage  -  proposal

On a device with no history yet (every new device), Spend over time renders a large
empty black panel (about 350 px tall) above its caption. Proposal: collapse the
empty chart to a slim empty-state card until there is data to draw.

### Assistant / Safety  -  fixed

Checkbox alignment corrected (the exemplar above).

### Device  -  clean

The accordion pattern (Mode and identity, Display, Battery mode, and so on) keeps a
very long settings page approachable; the collapsed default is the right call. When
every group is expanded the page is about 6,500 px tall, which is expected for an
all-in-one settings page and fine because groups are closed by default.

### Search palette (Ctrl K)  -  clean

Grouped results across settings, memory, and docs; keyboard reachable. Good.

## Cross-cutting proposals

1. **Defensive rendering for missing fields.** Several render helpers assign an API
   field straight into a control without a fallback, for example
   `set('directive', d.directive)`. If the firmware ever returns that field as null
   or omits it (a fresh device with no directive set is the obvious case), the user
   sees the literal text "undefined" in the box. This audit could not confirm on
   real hardware whether the firmware always populates these fields, so this is a
   robustness proposal, not a confirmed live bug: guard user-facing assignments
   with `|| ''` (or a placeholder) so a missing value never surfaces as "undefined".

2. **Home tile / info redundancy.** RAM, PSRAM, and battery appear both as stat
   tiles and again as text in the info line. Consider trimming the info line to the
   things the tiles do not already show (mode, access point, mDNS).

## Harness quality (root-cause adjacent)

While rendering, three fixtures in `tools/webui_harness/fixtures.mjs` proved to be
shape-mismatched with the current JS, which made the harness render text a real
device would never show:

- `/api/health` fixture sends `ok: true` (a boolean); the JS treats `ok` as a
  count, so the header rendered "true ok".
- `/api/mem/stats` fixture omits `scratchItems`, so the badge rendered
  "undefined scratch".
- the budget fixture shape does not match `renderBudgets`, so a provider name
  rendered as "undefined".

None of these are device bugs. They matter because they are the same class as the
bug that started CUM-214: a harness whose render does not match the device's render
cannot guard a per-page visual pass. Proposal: bring these fixtures back in line
with the live API shapes so the harness render equals the device render, then the
standing visual pass below actually catches layout drift.

## Standing guard (from the CUM-228 RCA asks)

1. A per-page visual capture now exists (`tests/95_audit_shots.spec.mjs`), both
   widths, every page and subtab. Run it before a release and look at the diff.
2. Recommend adding an explicit alignment assertion for form controls (checkboxes
   and radios left-aligned) so a future regression fails a named assertion, not
   just a snapshot re-bless.

## What landed vs what waits

- Landed: the Safety checkbox class fix and its snapshot re-bless; the audit
  capture spec.
- Waiting on the owner's ruling: every proposal above. None were changed.
