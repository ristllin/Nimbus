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
  than the bill. A custom endpoint is counted too: see
  [A custom endpoint](#a-custom-endpoint).
- **From the first paid call**, even on a new device whose clock is not set yet: the
  count starts at once, the ceiling applies at once, and the count joins the month
  as soon as the clock is set. Until the clock is set the count cannot start a new
  month either. On a network that blocks the time service, the date in Telegram's or a
  provider's answers stands in for the clock (never a custom endpoint's, and never a
  date years ahead), so the month still changes on time.
- **Calendar month in the device's time zone** (set it under **Device**; UTC when it
  is not set). The count starts again on the 1st.
- **At the ceiling, paid calls pause.** The device's side stops them before any
  request leaves: it never relies on the provider to stop, so a provider account on
  auto-recharge cannot run away.
- **You change it** on the web page under **Assistant > Usage**, in the
  **Monthly spend ceiling** row of Budgets. Any amount from 1 USD to 100,000 USD.
- **Kept on the device's flash**, not in its settings store, so a restart or a full
  settings store changes nothing. After a restart the count is never lower than this
  month's estimate on the **Usage** page.

## What pauses

Everything that calls a paid model:

| Feature | While paused |
|---|---|
| Replies to your messages (Telegram, the web chat, voice, the device) | No reply is generated. The web chat's send button answers at once that paid calls are paused. |
| Helpers (sub-agents) | Not started. A helper still running on a provider's side (OpenAI, Anthropic) is stopped, what it uses until it stops counts, and its result is not delivered; one message names the helpers being stopped. Raising the ceiling does not restart them. A helper that ran inside its one request (Mistral, Z.ai, Cumulo Nimbus, a custom endpoint) had already finished and keeps its result. Stopping a helper yourself always works. |
| Photos and images | Not described or generated. |
| Voice | Dictation and spoken replies are off. |
| Routines and the nightly dream | Skipped for as long as the pause lasts. They are not counted as runs and they are not switched off; the Routines list shows them as skipped. |
| Reminders and wakeups | Kept. Each one runs a few minutes after paid calls resume. |
| Web search, memory search by meaning | Off. Memory is kept, and searching it by keyword still works. |
| Screening of guests' messages | Off. A guest's message gets a short line that it could not be processed. |

Things that cost nothing keep working: the ring, the screen, the web page, your files,
your memory and settings.

Checking a provider key also keeps working. A full check ends with one tiny paid test
request that confirms your chosen models run (a fraction of a cent, counted). The
device's own scheduled checks skip it while paid calls are paused or that provider is
paused; pressing **Verify** yourself still sends it.

## What counts

Everything that calls a paid model or service counts toward the ceiling:

- replies to your messages, routines, reminders, wakeups and the nightly dream;
- helpers (sub-agents) on every provider, including the web searches they run;
- memory: saving a memory and searching it by meaning;
- photo descriptions, speech to text, spoken replies, generated images;
- the safety check on downloaded files, screening of guests' messages, and web search;
- a custom endpoint (see below), and the tiny test request a key check sends to confirm
  your chosen models work (a fraction of a cent).

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
| A request that was sent and got no answer (it timed out, or the answer was cut): replies, helpers, photo descriptions, the download scan, memory, moderation and web searches | one token per two characters sent (a photo counts 16,000 tokens), on the provider and model it named: the provider may well have run it. A reply or helper request also counts the answer a model could have written while it waited, 200 tokens a second up to the request's own limit, and a helper start (an OpenAI background run, a Mistral run) at least 50,000 input tokens. Generated images count their per-image price, voice its usual price, a web search one call |
| A helper whose stop is not confirmed 15 minutes after it was asked, or that cannot be followed at all | 50,000 input tokens at its model's price, at once (what its provider reports later counts only above that) |

Helpers' tokens also show on the **Usage** page under their provider. Everything else
in this list (the web searches helpers run included) counts toward the ceiling only.

### A custom endpoint

A custom endpoint is counted at the price you set for its model under **Prices** on
the **Usage** page (set both the input and the output price: a price left empty stays
at the estimate below). Until you set one:

- **On a paid service** (any other address, for example OpenRouter or Together, or any
  endpoint with a key): every token it reports is
  counted at the highest built-in price of any provider (10 USD per million input
  tokens and 50 USD per million output tokens), above what almost every model costs.
  The **Usage** page marks the figure as an estimate. A service that charges more than
  that for your model needs its real price set, or the estimate reads low.
- **On your own network with no key** (a private address: `10.x.x.x`,
  `172.16.x.x` to `172.31.x.x` or `192.168.x.x`, a loopback or link-local address,
  `localhost`, or a `.local` name; for example Ollama): a local model, counted as free.
  Other names (a bare name such as `nas`, `.lan`, `.home.arpa`), IPv6 addresses and
  VPN addresses count as paid, and so does a local endpoint with a key, which usually
  means a proxy in front of a paid service.

## How you hear about it

Once per pause, on each channel:

- **Telegram**: one message in your owner chat. A message of yours that the pause
  leaves unanswered also gets a short reply saying it was not answered and how to
  raise the ceiling (at most once every 30 minutes), so you hear about it even if that
  one message did not arrive. Any other chat gets one plain line per pause saying the
  assistant cannot answer right now, with no amounts.
- **The web chat**: one message in the conversation. A message you send while the
  pause lasts is answered at once with the reason instead of waiting.
- **The device screen**: a line in place of **Hold to talk** that says paid AI is
  paused.

The **Providers** row on the **Health** page also says paid requests to every provider
are paused while it lasts.

The message names the ceiling, this month's estimate, and how to raise it. The full
message is not repeated for every message you send while the pause lasts (each gets
only the short reply), and a restart does not send it again.

A heads-up goes out once a month when the estimate reaches **80%** of the ceiling.

## Lifting the pause

- **Raise the ceiling**: open the device's web page, go to **Assistant > Usage**, and
  set a higher **Monthly spend ceiling**. Paid calls resume at once.
- **Or wait for the month to end**: on the 1st (device time) the count starts again and
  paid calls resume, with no message.

## Monthly limits per provider

You can also give each provider its own monthly limit, in dollars, under
**Assistant > Usage > Budgets** (the **$ per month** field of **Set a budget**). Each
limit is a hard stop for that provider alone:

- **Counted the same way as the ceiling**: every paid call of that provider (replies,
  helpers, memory, photo descriptions, speech, images, file checks, screening), at the
  usage it reports or the high estimate in [What counts](#what-counts), over the
  budget's month (from its reset day, 1 for a calendar month, in the device's time
  zone).
- **At its limit, that provider pauses** for every paid call, and the others keep
  working: a reply that would have used it runs on the next provider in your order, a
  helper meant for it starts on another provider, a helper still running on it is
  stopped (one message names it; raising the limit does not restart it), and its memory,
  voice and photo calls stop. A provider with no limit is never paused by one.
- **One message per channel** (Telegram and the web chat) names the provider, the
  limit and how to raise it; a heads-up goes out at 80% of the limit. The Usage page
  lists each limit with this month's count and marks a paused one.
- **Raising the limit lifts the pause at once**; the next month starts a new count
  with no message.

**With provider limits set, the overall ceiling is off** unless you chose one: your
limits are the stops you picked, and a provider with no limit stays unlimited. To keep
an overall ceiling as well, set one under **Monthly spend ceiling**; to turn it off on
a device without provider limits, clear **Pause paid AI calls at an overall monthly
ceiling** and save.

## Daily spend warning

Under **Assistant > Usage**, **Daily spend warning** sends one message a day once
today's spend across all providers passes the amount you set (device date, from
$0.01). It is a heads-up only: spending continues. Leave it empty for the built-in warning, which
tells you once a day when a provider's spend passes 5 USD or three times its usual day.

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
- a message delivered twice is answered once, and the device answers at most 60
  messages an hour from Telegram, 12 of them always kept for your own chat.

## Devices that were set up before this update

- The models you picked stay as they are.
- The ceiling applies to you too, at the 20 USD default, counted from the start of
  this calendar month (the same estimate the **Usage** page showed).
- The first start after the update sends one message about the ceiling and where to
  change it. If this month's estimate is already over 20 USD, paid calls pause right
  away and that message says so; raise the ceiling to keep going.
- If the assistant runs on a premium model (for example Claude Opus 5), the message
  says how much sooner the ceiling is reached and how much a standard model saves.
- If you had set provider budgets with a **$ per month** amount, those become hard
  monthly limits, the overall ceiling stays off, and the first-start message lists the
  limits that apply (every other provider has none). A device already on the release
  with the overall ceiling gets that message once too, on its first start after this
  update.

## Virtual Nimbus

A Virtual Nimbus instance has the same ceiling, counted the same way (replies,
helpers and memory). Its default models are set by the hosted service. If its saved
count ever cannot be read back, the instance pauses paid calls (it never starts again
from zero) until you raise the ceiling or the month ends. Router usage
is also limited by your Cumulo Nimbus balance; the ceiling is a second limit on top
of it.

## Questions

**Is the estimate exact?** No: it is the device's estimate from the usage each
provider reports, at the prices on the **Usage** page. Your provider's bill is the
final word. Set a provider's price under **Prices** on the **Usage** page if it
differs.

**Can I turn the ceiling off?** Yes: clear **Pause paid AI calls at an overall monthly
ceiling** and save. It is off already when you set provider limits and never chose a
ceiling.

**Do the per-provider budgets still work?** Yes. A **$ per month** budget is a hard
monthly limit for that provider (see [Monthly limits per provider](#monthly-limits-per-provider));
token and call limits keep working as before. When you set an overall ceiling as well,
whichever is reached first applies.
