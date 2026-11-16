# Conversation summary

The assistant keeps a running summary of each conversation, so it still knows what
you talked about once older messages leave its view. Making that summary is called
compacting (the code calls it a **fold**). This page is the owner's view: when it
runs, how to run it now, what the Memory page shows, and what happens when a
provider has trouble. How it is built is in
[architecture/fold-ladder.md](architecture/fold-ladder.md); where it sits in the
device's memory is in [memory.md](memory.md#3-conversation-compaction---the-fold).

## When it runs

- **By itself**, once a conversation has grown enough since its last summary
  (Settings, Assistant, "Compact conversations after": 48, the default, follows
  the model in use and the page shows the size in effect; 0 turns automatic
  summaries off), after 200 different messages, or when the history has rows that
  neither the summary nor the recent messages hold yet.
- **It waits for a quiet moment**: two minutes with no message, or at most 10
  minutes or 6 turns, so it never starts between the messages of a conversation.
- **On demand**: send `/compact` in the chat, or press **Summarize Now** on the
  Memory page (Conversation summary). The device answers `/compact` at once with
  "Compacting this conversation in the background", and when the summary is done
  the chat gets one message: "✓ Compacted. Summary updated: covered N messages
  from D1 to D2". A conversation with a long backlog (the first summary after an
  update) catches up in one request: the device summarizes it piece by piece,
  oldest first, and confirms once at the end.

## The Memory page

**Memory, Conversation summary** shows, for the conversation you pick:

- a badge: **Fresh** (updated in the last 24 hours), **Stale** (24 to 72 hours),
  **Behind** (older, or some older messages are not in it yet) or **Not made yet**;
- when it was last updated and how far it reaches;
- while it runs, which provider it is on ("provider 2 of 5");
- after a try that did not finish, when the next one is;
- how many summary requests ran today.

**Summarize Now** is off while a summary you asked for runs or waits, and for two
minutes after one finishes (summaries you ask for run one at a time).

The same is available on the API, behind the device sign-in code like every
owner route:

| route | what it does |
|---|---|
| `GET /api/fold/status[?chat=<id>]` | the summary's health: `summaryAgeH` (-1 = none), `lastOk`, `thru`, `thruDate`, `uncovered`, `failedCycles`, `nextTry`, `paidToday`, `cap`, `rung` ("2/5" while running), `queued`, `cooldownS`, `state`, `canRun`, `why`, and `chats` (every conversation with its badge) |
| `POST /api/fold/run` (form `chat=<id>`) | queue a summary now: `202 {"queued":true}`; `404` for a conversation the device does not know; `401` without the sign-in code |

With no `chat`, both take the owner's Telegram chat, else the web chat (on a
Virtual Nimbus, the hosted web chat).

## When a provider has trouble

- **It tries every provider you have a key for**, one per pass: the model you chat
  with first (Mistral Large 4 summarizes on Mistral Small, which does not spend the
  answer on thinking), then your other providers in your priority order, then the
  head provider's sub-agent model. A provider that answered with nothing usable
  goes last next time.
- **It never pauses for good.** When every provider failed, it tries again after
  15 minutes, then 1 hour, then 4 hours, then every 12 hours, and each try after a
  failure takes a smaller piece (half, down to 8 KB). An update to new firmware
  restarts the wait. Versions up to v5.1.1 paused summaries after three failures
  until you sent `/compact`; that pause lifts by itself on the first start of
  v5.1.2.
- **You hear about it once a day at most**, and only after a day of failing:
  "This chat's summary hasn't updated since 2026-11-02. It keeps retrying on its
  own; send /compact to try now."
- **Cost stays bounded**: at most 12 paid summary requests a day on the whole
  device for automatic summaries (every request a provider answers counts, and one
  that timed out after it was sent; a refused request costs nothing and does not
  count). Summaries you ask for are not held to
  that limit but are counted in it. The spend ceiling and each provider's own
  limits apply as for any turn.

## Size

A summary is at most 6,144 bytes for a model with a window of 60k tokens or more,
else 4,096. When a summary runs long, every section keeps its heading and each
section is shortened in proportion to its size.

## Going back to v5.1.1

The summaries stay readable: v5.1.1 reads the same file, with nothing paused. It
shortens any summary over 4,096 bytes when it loads it, and it writes the file back
at no more than 48 KB, so with many long conversations it can drop the oldest
conversations' summaries. On v5.1.1, three failures in a row pause automatic
summaries again until `/compact`.
