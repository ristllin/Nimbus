# The fold ladder and its driver

How the conversation summary (the fold) heals itself on every provider, from v5.1.2.
Read it when you change the fold, its schedule, or Summarize now. The owner's view is
[../compaction.md](../compaction.md); the summary's place in the device's memory is
[../memory.md](../memory.md#3-conversation-compaction---the-fold).

## Why

On 2 Nov 2026 three folds on Lumi failed in 95 seconds, all on the same host and
model, and the third paused automatic summaries. Nothing lifted the pause for 13 days.
Owner ruling 2 (15 Nov): the summary can never stay off for days again, a failure
walks every provider in the owner's priority list, and the bound on cost is a daily cap,
not a pause.

## One driver, two hosts

`agent::FoldDriver` (`lib/harness/{include/nimbus/harness/fold_driver.h,
src/fold_driver.cpp}`) is the only fold logic. The device's `compactTick()`
(`src/agent/orchestrator.cpp`) and the Virtual Nimbus's `foldTick()`
(`nimbusd/src/rig.h`) are bindings: they pass the clock, the network and quiet gates,
the trigger, the fold step, the slice, and where notices go.

```mermaid
flowchart LR
  P["pump pass<br/>(tg_poll / engine thread)"] --> G{"gates: no turn,<br/>heap, network"}
  G -->|no| W["Deferred<br/>(nothing counted)"]
  G -->|yes| C{"cycle running?"}
  C -->|no| S["start: the next manual request,<br/>else the first due chat<br/>(quiet gate, daily cap)<br/>freeze slice + rungs"]
  C -->|yes| R
  S --> R["run ONE rung"]
  R -->|summary| OK["store FIRST, reset the chain,<br/>ev:compact row"]
  R -->|failed, rungs left| PEND["Pending<br/>(no notice)"]
  R -->|last rung failed| CF["CycleFailed once:<br/>backoff, maybe the daily alert"]
```

Each pump pass runs at most one rung (one request plus that wire's own retry), so an
owner message waits at most one rung. A later automatic rung re-checks the owner-first
quiet gate; a manual one does not wait for quiet.

## The ladder

`TurnEngine::foldRungs()` (`lib/harness/src/engine_fold.cpp`), declared in
`fold_ladder.h`:

1. the head host on its fold model: the head model, or the host's non-reasoning model
   when the head is a forced reasoner (Mistral Large 4 folds on `mistral-small-latest`
   at effort none, orchestrator ruling of 16 Nov);
2. every other keyed, unparked, in-budget provider of the owner's priority list, in its
   order, and nothing else: a keyed provider the owner left unchecked (a key kept for
   voice, embeddings or sub-agents) and a custom endpoint not in the list never receive
   the digest (privacy ruling, v5.1.2 review);
3. the router fallback (Cumulo, then Z.ai) only on a router-only device, one with no direct
   provider key at all (CUM-288). Cumulo the owner put in the list is a rung of step 2 (with
   a Cumulo key, RULINGS-v3 item 15); one left out of it never receives the digest (privacy
   ruling, v5.1.2 fix review);
4. the head host on its Sub model (a direct host only; none for a forced-reasoner head, whose
   first rung already stepped down).

No rung ever runs a forced reasoner: lane W's live legs (16 Nov) found Large 4 folding on
itself cut at its fit (2,118 and 2,824 tokens) while Small folds fine, so on any rung (the
head, another provider, the Sub model) such a model is replaced by `mistral-small-latest`,
and on Cumulo's router `mistral/<forced reasoner>` by `mistral/mistral-small-latest`. A rung
on another model than the host's pick runs under a model pin (`ScopedModelOverride`): the
direct hosts and the Cumulo head read it, on the fold's own task only.
"Forced reasoner" is one predicate, `nimbus::orch::mistralForcedReasoner` (`model_family.h`):
Large 4 under every id Mistral serves it as (`mistral-large-4*` and the alias
`mistral-large-latest`, nimbusd's default Mistral model) and the hosted GLM models. The
class test runs it against every id and alias of the recorded model list, with the list's
canonical name as the oracle. No (host, model) appears twice. A rung that answered empty
or thin, or was cut twice, goes last in the next cycle. A rung whose host left the ladder since the cycle started (parked, capped)
is passed over. Every failed rung logs one line the persona rig reads:

```
fold(<chat>): rung i/n <host>/<model> failed -> <kind> (<detail>)
```

with `<kind>` one of transport, timeout, 4xx, 402 park, 5xx, cut twice, empty, thin.
A success keeps `fold(<chat>): ok on <host> (...)`; a failed cycle logs
`fold(<chat>): cycle failed, next try <date time>`.

**The quality gate** (`foldAnswerThin`): for digests of 8 KB or more, a 2xx answer
shorter than min(800 B, 30% of the previous summary) or naming fewer than 2 of the
prompt's 5 sections (case-insensitive keywords) is thin: the next rung gets the job.
The matcher passes every live fold output recorded in
`test/support/fixtures/fold_outputs_2026-11/`. When every rung failed, and one or more
answered at least 800 B but named too few sections (a summary in another language, or
with renamed headings), the longest of those is stored instead of failing the cycle.

## Trigger and step

The fold reads lane B's request context for the head (`deriveRequestContext` over
`turnBudgetFor`, the head's turn budget in the form its turn goes out in; the VN on the
server link): `foldTriggerBytes(budget, compactAtKB)`
makes a chat due (0 off; 48, the default, the derived soft mark; another value that mark
clamped inside the history; on the floor branch v5.1.1's reading, compactAtKB KB) and
`foldCycleBytes(budget)` = min(`foldSliceBytes`, `foldDigestCap(chatSummaryBytes)`) is what
one cycle may take: the slice stops at the kept tail, so a steady-state fold takes about
`foldStepBytes` (soft - keep) and only a backlog takes more per paid request.

On a head with room the trigger compares the soft mark with the history after `thru`,
`bytesAfterThru()`: the conversation the last fold kept after the new anchor (`keptAtFold`,
from `applyFold`'s `keptBytes`) plus the bytes captured since, or the bytes the turn's window
rendered (`noteHistoryBytes`, RAM only), whichever is more. So after the first fold the next
one is due at the soft mark, before the window reaches its maximum and shifts.
`bytesSinceFold` keeps v5.1.1's meaning (bytes since the last fold, restarting at 0): the
thrash wait counts it, and the blob's bytes field holds it, with `keptAtFold` as a `;` suffix
that v5.1.1's number parser stops before. A rollback therefore reads the bytes since the fold,
never the kept tail, and is not due on its first pass because of a figure v5.1.2 wrote; a
restart reads the kept tail back. The floor branch keeps v5.1.1's reading (nothing kept). The VN reads
`kHostedCompactKb` (48) the same way. A head whose turn is one flat prompt string (a
custom endpoint; Mistral or Z.ai with the tool loop off) is on the floor branch: the fold
keeps the tail that string can carry.

## The slice and its four outcomes

`TurnEngine::historyFoldSlice` reads the rows after `thru` forward (the after cursor, the
card's cold region included), oldest first, up to the kept tail. A slice with no digest says
why (`SliceState`), and only one outcome may give rows up:

| outcome | when | the driver |
|---|---|---|
| Ready | a digest to summarize | runs the cycle |
| Nothing | the read reached the kept tail or the end (and only then) and found nothing worth a summary | restarts the counters; gives up hidden rows only up to where the read reached (`readTo`) |
| Wait | the rows are not readable yet: the cache has not read far enough to know the kept tail (with rows hidden the slice takes those rows alone instead), a store read budget, a failing card, the scan for the newest `/clear` of a chat with no anchor, a read that stopped short holding nothing to summarize, a stale cursor | Deferred, the store untouched |
| Unread | the store named a span after the anchor it will not give back | Deferred, the store untouched; after 24 h of it, the owner's daily alert (fail-closed: the anchor never passes the span) |

So `thru` moves only past rows a landed summary took. A `/clear` anchors the chat at its
boundary row (device and VN); for a chat cleared before that (no anchor), only a read that
reached its bound is folded, so nothing before the newest `/clear` is ever summarized. A read
budget that found none of the chat's rows is resumed from where the store stopped on the next
pass. Such a cursor is a byte position (`day:off`) that only fits the store state it came from:
when the store answers it with an empty page and nothing more (its index completed meanwhile),
or with rows at or before the anchor, it is dropped and the read starts again from the anchor
(Wait, never the end). The cache's gap probe (one store read a call) treats its own cursor the
same way. A cold no-clock day-stream blocks only cursors it could hold rows after. The slice runs inside a `PsramFirstScope` and spills each kept row as it is read: the
fold runs outside a turn, where a block of 128 B or less would otherwise stay in internal SRAM.

## The schedule (FoldStore and FoldCtl)

Per chat, in `/data/chatsum.txt` (`nimbus::orch::FoldStore`, `compact.h`):
`failedCycles`, `firstFail` and `nextTry`, written as `;` suffixes of the record's
failures field. The failures and pause fields themselves are always written `0`, so a
v5.1.1 image reads a chat that is neither failing nor paused.

| event | effect |
|---|---|
| a cycle fails | `nextTry` = now + 15 min, 1 h, 4 h, then 12 h; the next slice is half the last (floor 8 KB) |
| a summary lands | the failure count and a backoff wait clear |
| a refill within 2 turns of a fold (thrash) | due once, `nextTry` = now + 15 min; never a pause |
| a legacy pause (`paused=1` or `2`) | ignored on load: due on the first pass |
| a slice that cannot read its rows (`SliceState::Wait` or `Unread`) | nothing in the store changes; the next look is the next pass when the read moved forward, else 1 min doubling to 1 h (RAM) |
| a new firmware (`fwAtLastFail` differs) | every chat's wait and failure count restart |
| no clock | the wait is kept in RAM on the monotonic clock; paid requests count under day 0 in RAM, never over a real day's count, and that count restarts 24 h after its first request; a chat with a persisted wait is held 10 minutes after boot for the clock; no alert |
| a wait longer than 12 h 15 min (the clock stepped back) | read as due |

Device-wide, in `/data/fold_ctl.txt` (`nimbus::orch::FoldCtl`; on the VN
`<data>/mem/fold_ctl.txt`): `paidDay`, `paidCount` and `guestPaid` (at most 12 paid
automatic fold requests a local day, every chat together, of which chats that are not the
owner's spend at most 6; the owner's due chats fold first; 2xx answers and timeouts after
the upload count, refusals do not), `lastAlertDay` (one owner alert a day, after 24 h of
failing, sent to the owner's Telegram chat or the web chat, by role on the VN: the first
Admin chat; another chat's failure is only logged and does not use up the day),
`manualPendingChat` (up to 4 queued manual requests) and `fwAtLastFail`. Because this
lives outside the per-chat blob, evicting a chat's record (the blob keeps the 16 most
recently used chats inside 64 KB, about ten with a full summary) never resets the cap
or loses a request.

## Manual requests

`/compact`, `POST /api/fold/run` and the Memory page's **Summarize Now** stage the chat
(any task), and the pump moves it into `manualPendingChat`. Manual cycles run one at a
time, two minutes apart, skip the daily cap (they are counted in it) and the quiet wait,
and take a keep-0 slice. A manual request is a catch-up: while the chat still has rows
the history hides behind the anchor, or its last slice stopped short of the newest row,
its cycles chain oldest first (at most 64) and one
confirmation names the whole span. A manual request never waits behind an automatic cycle:
one that is between rungs when a manual request is ready ends uncounted, its failed
rungs going last. Every manual request is answered: the confirmation,
a failure with the next try time, or "Nothing to compact yet."

## Status

`FoldDriver::status()` and `foldStatusJson()` build `GET /api/fold/status`. The device
publishes a copy for the web task under a mutex whenever the driver's state moves and
every 5 s; the VN builds it on the engine thread. The CTX echo (`CTX?`,
`/api/test/ctx`) appends `nextTry`, `firstFail`, `rung`, `paidToday`, `cap` and
`queued`; `paused` is always 0.

## Summary size

The head's cap is lane B's `chatSummaryBytes` (`deriveRequestContext`): 6,144 B for a
request target of 60k tokens or more, else 4,096 B; a head whose turn is one flat prompt
string never takes the larger cap (the input names the larger budget in a
`[SUMMARY BUDGET]` line). `capSummary()` keeps every
section header and trims each body in proportion to its size, whole lines from the
end.

## Tests

| suite | what it holds |
|---|---|
| `test_orch_compact` | backoff, thrash wait, legacy lift, firmware reset, rollback round trip (the v5.1.1 parser), no reachable state stays off past 12 h, FoldCtl codec, trigger math, proportional cap |
| `test_fold_backlog` | a long chat's catch-up, a refill that only waits, a failure that schedules a retry |
| `test_fold_ladder` | every failure kind at every rung is answered by the next; no (host, model) twice; Large 4 on Small at effort none on the real wire; the matcher fixtures |
| `test_fold_driver` | Pending per rung, one CycleFailed, the never-lost manual request, keep 0, the device-wide cap, a failing week on five chats, no clock, fresh device, the Lumi blob folding forward with no id gap |
| `nimbusd/tests/test_fold_driver_vn` | the VN reaches the same outcomes as the device; the routes |
| `tools/release_gate/check_fold_health.py` | no reachable pause, fold control present, the blobs round-trip |

Live: `harness-lab --scenario foldladder` (one real fold per provider on a Lumi-shaped
digest of that host's fold step).
