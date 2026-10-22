<!-- audience: user -->
# Monthly spend ceiling and default models

Nimbus spends money on your provider accounts every time it calls an AI model. Out of
the box it keeps that spend small and bounded: it starts on cost-conscious models, and
it stops making paid calls once a month's estimated spend reaches a ceiling you
control. This page explains both, in plain words.

## The ceiling at a glance

- **Default: 20 USD per calendar month**, across **all** providers combined
  (Anthropic, OpenAI, Mistral, Z.ai, Cumulo Nimbus, a custom endpoint and web search).
- **Counted on the device**, from the usage each provider reports with every answer,
  priced with the same per-model prices the **Usage** page shows. Every paid call
  counts, not only replies: see [What counts](#what-counts). A model with no known
  price is counted at its provider's highest price, so the estimate never reads lower
  than the bill.
- **Calendar month in the device's time zone** (set it under **Device**; UTC when it
  is not set). The count starts again on the 1st.
- **At the ceiling, paid calls pause.** The device's side stops them before any
  request leaves: it never relies on the provider to stop, so a provider account on
  auto-recharge cannot run away.
- **You change it** on the web page under **Assistant > Usage**, in the
  **Monthly spend ceiling** row of Budgets. Any amount from 1 USD to 100,000 USD.
- **Kept on the device's flash**, not in its settings store, so a restart or a full
  settings store changes nothing.

## What pauses

Everything that calls a paid model:

| Feature | While paused |
|---|---|
| Replies to your messages (Telegram, the web chat, voice, the device) | No reply is generated. The web chat's send button answers at once that paid calls are paused. |
| Helpers (sub-agents) | Not started. Helpers that were already running finish, and their results reach you. |
| Photos and images | Not described or generated. |
| Voice | Dictation and spoken replies are off. |
| Routines and the nightly dream | Skipped for as long as the pause lasts. They are not counted as runs and they are not switched off; the Routines list shows them as skipped. |
| Reminders and wakeups | Kept. Each one runs a few minutes after paid calls resume. |
| Web search, memory search by meaning | Off. Memory is kept, and searching it by keyword still works. |
| Screening of guests' messages | Off. A guest's message gets a short line that it could not be processed. |

Things that cost nothing keep working: the ring, the screen, the web page, your files,
your memory, settings, and checking a provider key.

## What counts

Everything that calls a paid model or service counts toward the ceiling:

- replies to your messages, routines, reminders, wakeups and the nightly dream;
- helpers (sub-agents) on every provider, including the web searches they run;
- memory: saving a memory and searching it by meaning;
- photo descriptions, speech to text, spoken replies, generated images;
- the safety check on downloaded files, screening of guests' messages, and web search.

Where the provider reports the usage of a call, that usage is counted at the model's
price. Where it reports none, the device estimates on the high side, so the count
never reads lower than the bill:

| Call | Estimate when the provider reports no usage |
|---|---|
| Text (memory, photo descriptions, file checks, screening, helpers) | one token per two characters sent, and the whole answer allowance |
| Speech to text | the recording's length at the model's per-minute price: 0.006 USD a minute unless the model is listed below |
| `gpt-4o-mini-transcribe` / Voxtral Mini | 0.003 / 0.002 USD a minute |
| Spoken replies | 25 USD per million characters on `gpt-4o-mini-tts`, 30 USD on any other voice model |
| Generated images | the largest size at the requested quality: 0.016 (low), 0.063 (medium), 0.25 USD (high or other models) |
| A web search a helper runs on its provider | 0.03 USD each |
| An OpenAI helper's code interpreter | 0.03 USD a run |

Helpers' tokens also show on the **Usage** page under their provider. Everything else
in this list (the web searches helpers run included) counts toward the ceiling only.

## How you hear about it

Once per pause, on each channel:

- **Telegram**: one message in your owner chat.
- **The web chat**: one message in the conversation. A message you send while the
  pause lasts is answered at once with the reason instead of waiting.
- **The device screen**: a line in place of **Hold to talk** that says paid AI is
  paused.

The message names the ceiling, this month's estimate, and how to raise it. It is not
repeated for every message you send while the pause lasts, and a restart does not
send it again.

A heads-up goes out once a month when the estimate reaches **80%** of the ceiling.

## Lifting the pause

- **Raise the ceiling**: open the device's web page, go to **Assistant > Usage**, and
  set a higher **Monthly spend ceiling**. Paid calls resume at once.
- **Or wait for the month to end**: on the 1st (device time) the count starts again and
  paid calls resume, with no message.

## Default models

A new setup uses each provider's standard model for the assistant and its cheapest
model for everything that runs in the background:

| Provider | The assistant | Helpers, photo captions, routines and the dream |
|---|---|---|
| Anthropic | Claude Sonnet 5 (`claude-sonnet-5`) | Claude Haiku 5.5 (`claude-haiku-5-5`) |
| OpenAI | the mini class (`gpt-5.6-luna`) | the nano class (`gpt-6-luna`) |
| Mistral | Mistral Medium (`mistral-medium-latest`) | Mistral Small (`mistral-small-latest`) |
| Z.ai | GLM-5.3 (`glm-5.3`) | GLM-5.3 Flash (`glm-5.3-flash`) |
| Cumulo Nimbus | the router's default (`gpt-5.6`) | the router's cheapest tier (`gpt-5.6-luna`) |

The stronger models stay one click away: **Assistant > Models** lists every model a
provider offers, with its price, and you can pick any of them for the assistant or
for helpers. Routines, reminders, wakeups and the nightly dream always run on the
cheapest model with light reasoning, whatever the assistant runs on: nobody waits on
them, and a routine that has nothing to say still pays for its thinking.

When a provider retires a default model, the device picks the cheapest model of the
same class that the provider still lists.

## Bounded work

Other defaults keep a single request from turning into a long bill:

- one reply runs at most 8 tool rounds and 10 minutes;
- the assistant can start at most 6 helpers per reply, with at most 4 running at
  once and 8 waiting;
- routines run at most 24 times a day each, and use at most 120,000 tokens a day
  each (400,000 for all routines together);
- a message delivered twice is answered once, and a burst of messages is answered
  with a bounded number of replies per hour.

## Devices that were set up before this update

- The models you picked stay as they are.
- The ceiling applies to you too, at the 20 USD default, counted from the start of
  this calendar month (the same estimate the **Usage** page showed).
- The first start after the update sends one message about the ceiling and where to
  change it. If this month's estimate is already over 20 USD, paid calls pause right
  away and that message says so; raise the ceiling to keep going.
- If the assistant runs on a premium model (for example Claude Opus 5), the message
  says how much sooner the ceiling is reached and how much a standard model saves.

## Virtual Nimbus

A Virtual Nimbus instance has the same ceiling, counted the same way (replies,
helpers and memory). Its default models are set by the hosted service. Router usage
is also limited by your Cumulo Nimbus balance; the ceiling is a second limit on top
of it.

## Questions

**Is the estimate exact?** No: it is the device's estimate from the usage each
provider reports, at the prices on the **Usage** page. Your provider's bill is the
final word. Set a provider's price under **Prices** on the **Usage** page if it
differs.

**Can I turn the ceiling off?** Set it high enough for your use. A very high ceiling
keeps the safety net without getting in the way.

**Do the per-provider budgets still work?** Yes. A provider budget limits one
provider; the monthly ceiling limits all of them together. Whichever is reached first
applies.
