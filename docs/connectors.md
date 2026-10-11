<!-- audience: user -->
# Connectors - external tools, per provider

Connectors let the Orchestrator (and the sub-agents it spawns) reach the
outside world. With a connector enabled you can ask, over Telegram or voice,
things like:

- "Any new issues on my repo?" - GitHub returns the live list.
- "How many unread emails do I have?" - Gmail answers.
- "Publish that summary to my Notion workspace." - a page appears.

**How it works:** Nimbus does not call Gmail, Notion or GitHub itself. It
attaches a small connector descriptor to the model request, and the provider
runs the tool in its own cloud. What you can do is whatever your provider
hosts; the credential either lives with the provider (Mistral Studio) or is
pasted once into the device web UI.

> **Scope note.** This page is the *provider-attach* model - the AI lab executes
> the tool in its own cloud. Nimbus can *also* dial a remote MCP server itself
> (the device as an MCP client): see [MCP](mcp.md) for that path, its approval
> model, and limits. Google **Sheets/Slides** have no hosted MCP anywhere, so
> they are not reachable either way today (they need direct REST - a future
> pass). Gmail, Calendar, and Drive *are* reachable.

## Setting up a connector

Every connector is a card under **Assistant → Connectors** in the web UI.
Paste the credential once; the device stores it write-only and never shows it
again. Google connectors on OpenAI are the one exception: they need an OAuth
block (token URL, client id, client secret, refresh token) rather than a single
token, entered through the **Advanced - edit raw JSON** editor below the cards
or pushed from a repository `.env` with `tools/connectors_setup.py`:

1. Create the credential once in a browser: a GitHub personal access token, a
   Notion integration token, a Slack bot token, an OAuth refresh token, or a
   Studio-side connection.
2. Open the device web UI → **Assistant → Connectors**.
3. Pick the provider on the connector's card, paste the credential (or, for
   Mistral, just enable it), toggle **enabled**, **Save**.
4. The card's status badge confirms whether it is live on your turns, via
   sub-agents, or idle until you change the host.

The model can never set a connector or read a secret - that rail is enforced
in firmware ([Security](#security)).

## What each provider runs, and where

| | On the Orchestrator's **own turns** | On **sub-agents** it spawns |
|---|---|---|
| **OpenAI** | ✅ remote MCP (`server_url` + token) and first-party connectors (Gmail, Calendar, Drive…) | ✅ same |
| **Anthropic** | - (the head turn is a single forced tool; MCP cannot run mid-turn) | ✅ remote MCP by URL (`mcp_servers`); a pasted token or a completed Sign in is forwarded as the server's authorization token |
| **Mistral** | ✅ Studio built-ins + Studio-named connectors - **single-shot turns** | ✅ sub-agents run over the Conversations API; built-ins *and* Studio connectors attach to the spawn dispatch (server-side) |

Two consequences:

- **The current provider matters.** A Mistral connector is only live when
  Mistral is the Orchestrator's host. An Anthropic connector only runs on
  sub-agents. Assistant → Connectors badges each card with what applies
  *right now*, and the model is told the same thing every turn.
- **Mistral requires the least setup.** Its connectors are authenticated in
  **Mistral Studio** and referenced by name - Nimbus stores *no secret* for
  them.

## How Nimbus knows a connector is available

Nimbus does not blindly trust that a pasted key works. **Capability validation**
(Assistant → Tools → Tool use) controls how it confirms provider access
before it tells the assistant a connector is available:

- **Off** - trust the key as-is, and make no "verified" claim.
- **Passive** (default) - mark a provider verified once a real call, or a manual
  **Verify**, succeeds; the result is cached.
- **Active** - passive, plus a periodic re-verify on a slow schedule so a revoked
  key or changed access is caught. The re-check is a free metadata call
  (`GET /v1/models`, roughly $0), one provider per wake, spread so each keyed
  provider refreshes about every *re-check* interval (default 24 hours, range
  1–168).

Active mode runs only in Orchestrator mode (the re-check needs the TLS heap)
and shares the single verify worker with the web **Verify** button.

**Per-connector credential state.** Enabling a connector is a checkbox, not
proof it works. The catalog the assistant reads marks the two states that need
you: a connector whose OAuth sign-in failed on its last refresh ("sign-in
FAILED - tell the owner") and a remote MCP or first-party connector with no
credential at all ("NO credential - not usable until the owner adds one"),
which is skipped rather than sent a request that cannot succeed. A bare name
means no known problem; a successful OAuth refresh since the last restart is
positive proof the credential works.

**Mistral Studio connectors: the workspace listing decides.** A Studio connector
(Google Calendar, Notion, Slack, ...) has no credential on the device, so its
state comes from Mistral. The device asks Mistral which connectors the key's
workspace offers (`GET /v1/connectors`) on each Mistral **Verify**, and on its own
when you **Save** a connector card and after a restart, whenever a Studio
connector is not listed yet. That check needs no click from you:

- **Listed and active:** usable. The assistant is told it can use it, and the
  Connectors tab shows where it runs.
- **Not listed, or listed as inactive:** not usable; the Connectors tab shows
  **connect it in Mistral**.
- **Being checked** (right after a save or a restart, including a re-check of a
  connector Mistral did not list before): the card shows **checking your Mistral
  workspace** and updates by itself when the check lands.
- **Not checked** (no Mistral key yet, or the check could not run): the card
  shows **Mistral workspace not checked**, and the assistant is told the
  workspace has not been checked rather than sent to Mistral for nothing.

Mistral's own "signed in" flag (`is_authenticated`) is a hint, not the rule: it
can read false for connectors that work. So a listed connector is used, and the
assistant is told that an empty result may mean the account still needs
connecting in Mistral. A Virtual Nimbus runs the same check at
startup, after a Mistral key is saved, and when its first Studio connector is
added (the hosted page has no Verify button).

**When a connector did not run, the result says so.** A sub-agent can answer a
calendar request from its own guess without ever calling the calendar connector.
For a Mistral sub-agent the device reads Mistral's own record of the run (one
entry per tool that ran). If the task asked for an attached connector (by its
name, its id, or a phrase such as "my calendar" or "calendar events") and that
connector never ran, the result opens with:

*"The Google Calendar connector was NOT called. The text below is the model's own
guess, not your calendar."*

That note rides into the saved document, the assistant's summary, the activity
record and the raw results, and the assistant saves nothing to long-term memory
in the turn that reads it, so the guess is not stored as a fact on its own. A
result where the connector ran is delivered unchanged. The check matches words,
so a task that never names the connector (say, "is Friday free?") is not marked;
the sub-agent is also told never to invent records when a tool does not run.

> **What validation does not do.** It confirms the *provider key* works, not each
> connector's individual tools. Per-connector *functional* probing - actually
> calling a connector, which would spend tokens - is not implemented. A connector
> can be attached and still fail on a specific call (a private repo, a missing
> scope); the assistant is guided to verify writes by reading them back.

Whatever the mode, the model is told the truth every turn: its
`[PROVIDERS & CONNECTORS]` block marks each provider **available, verified** /
**key present, not yet verified** / **key present but rejected on last check** /
**no key** - and with validation off, **key present (validation off - trusting
key presence)**.

**The Telegram bot token is watched live, not just at save time.** The token is
verified once (a `getMe` call) when you save it, and that verdict is cached. If
the token is later revoked - a key rotation, say - the device keeps long-polling
and Telegram answers every request with an authorization failure. Rather than
keep reporting the bot as verified and live, Nimbus watches the poll: a short run
of consecutive auth failures flips the token to rejected on the Home tab health
panel ("Telegram token rejected - set a new bot token") and in the status the web
app reads, and the next successful poll clears it. A one-off blip does not trip
it, and a conflict with another device using the same token (a different
condition) is not treated as a rejected token.

---

## GitHub {#github}

- **Providers:** OpenAI / Anthropic (remote MCP by URL), Mistral (Studio).
- **Credential:** a **Personal Access Token** (`ghp_…`) - GitHub → Settings →
  Developer settings → Personal access tokens. A PAT avoids the OAuth flow
  entirely: paste it once and the connector works.
- **MCP URL (OpenAI/Anthropic):** `https://api.githubcopilot.com/mcp/`
- **Mistral:** connect GitHub in Studio, then enable it by name here (no token
  on device).
- **Unlocks:** issues, PRs and reviews, code and commit search, **creating
  repositories**, and pushing file contents through the GitHub API (releases are
  read-only; there are no Actions or Gists tools). Verified against the
  OpenAI/Anthropic MCP endpoint; the Mistral Studio connector's toolset may
  differ. For repo creation and file pushes the token needs the `repo` scope
  (or a fine-grained token with Administration and Contents read/write).
- **Try it:** once the card is saved, ask the assistant to *"create a private
  repo named test-repo and add a README"* - it should come back with the new
  repo's URL.

## Gmail {#gmail}

- **Providers:** OpenAI (first-party connector `connector_gmail`), Mistral (Studio).
- **Credential:** an **OAuth refresh token** created off-device (Google's
  device flow excludes Gmail scopes, so do the one-time auth-code consent in a
  browser and paste the refresh token). For Mistral, connect Google in Studio
  instead.
- **Unlocks:** read, search, draft, and label email.

## Google Calendar {#google-calendar}

- **Providers:** OpenAI (first-party `connector_googlecalendar`), Mistral (Studio).
- **Credential:** OAuth refresh token (as Gmail), or Studio-side for Mistral.
- **Unlocks:** read and manage events; calendar digests.

## Google Drive {#google-drive}

- **Providers:** OpenAI (first-party `connector_googledrive`), Mistral (Studio).
- **Credential:** OAuth refresh token (as Gmail), or Studio-side for Mistral.
- **Unlocks:** read and write Drive files - report and research-corpus storage.
- **Note:** Google **Sheets/Slides** are *not* covered by the hosted connector;
  they need direct REST (out of scope today).

## Notion {#notion}

- **Providers:** Mistral (Studio) requires the least setup; OpenAI/Anthropic
  via a remote MCP URL work too.
- **Credential:** a Notion **Internal Integration** token - Notion → Settings
  → Connections → Develop integrations. Share each target page with the
  integration once. Notion's hosted MCP (`https://mcp.notion.com/mcp`, the
  card's default URL) signs in through the browser instead: save the card, then
  use its **Sign in** button and approve on a phone or laptop on the same
  network.
- **Unlocks:** create and query pages and databases - report publishing,
  research corpora.

## Slack {#slack}

- **Providers:** Mistral (Studio); elsewhere needs a small REST adapter (future).
- **Credential:** a **bot token** (`xoxb-…`) - create a Slack app once in a
  browser, install it to the workspace, copy the token. No on-device OAuth.
- **Unlocks:** read channel/DM history, search, post messages, reactions.

## Linear {#linear}

- **Providers:** OpenAI / Anthropic (remote MCP `https://mcp.linear.app/mcp`), Mistral (Studio).
- **Credential:** a static **Linear API key** (Bearer) that bypasses OAuth
  entirely - Linear → Settings → API.
- **Unlocks:** find, create, and update issues, projects, and comments.

## Mistral built-ins {#mistral-builtins}

Hosted tools that run inside Mistral with **no device secret** - just toggle
them on here (provider = Mistral). They attach as `{type:"<name>"}` to the
Orchestrator's own single-shot Mistral turns.

- **web_search** - hosted web search with citations.
- **code_interpreter** - sandboxed Python execution (also available on OpenAI
  with no setup; the OpenAI sandbox can produce PDF and CSV files, the Mistral
  one images and text only).
- **image_generation** - hosted image generation.
- **document_library** - RAG over your Studio document library.

## Mistral Studio connectors {#mistral-studio}

Third-party connectors (GitHub, Gmail, Notion, and custom MCP servers) are
**authenticated in Mistral Studio**
([console.mistral.ai/build/connectors](https://console.mistral.ai/build/connectors)),
not on the device - no secret is stored here. Two kinds:

- **Built-in connectors** (GitHub, Gmail/Calendar/Outlook): pre-registered -
  you authorize the account in Studio, then reference the connector by its
  **name** here.
- **Custom MCP** (e.g. Notion, DeepWiki): register the MCP server once via
  `client.beta.connectors.create(...)` in the Mistral SDK → you get a
  **connector name/UUID** → reference it here.

Either way, on the device: add the connector with **provider = Mistral**, and
put the Studio connector **name or UUID** in the connector-id field. It
attaches as `{type:"connector", connector_id:"<name-or-uuid>"}` on single-shot
Mistral turns.

> The agentic **tool-loop** turns cannot carry Mistral built-ins or Studio
> connectors: the loop forces tool-choice (the model must call a tool every
> round), which Mistral rejects alongside built-in connectors - a documented
> provider limitation. Those attach on the single-shot Conversations turn and on
> sub-agent dispatch instead. So on loop turns the Orchestrator has only its
> registry tools (the Tavily `web.search` tool when configured) and should
> delegate connector work to a Mistral sub-agent.

---

## Operational notes

- **Mistral connectors attach on single-shot turns** (tool loop off) **and on
  sub-agent dispatch** - not on the agentic tool-loop, which forces tool-choice
  (rejected alongside built-in connectors). Set the Orchestrator host to Mistral
  and the tool loop off to use them on your own turns, or spawn a Mistral
  sub-agent for connector work while the loop is on.
- **Studio connectors are most reliable on a Mistral sub-agent.** A single-shot
  turn that carries a Studio connector together with the structured reply
  format tends to time out, while the same request without the structured
  format (which is what a sub-agent sends) answers in seconds. A Virtual Nimbus
  therefore attaches Studio connectors only to Mistral sub-agents, and its
  assistant is told to spawn one for connector work.
  On the device, a single-shot Mistral turn that carries a Studio connector
  leaves the structured format off (the model gets the same reply format in
  its instructions instead), so it answers rather than timing out. When the
  model answers in plain text, that text is the whole reply: no device action,
  memory write or sub-agent rides that turn. Built-in connectors alone keep the
  structured format.
- **A Mistral key's Studio connectors have their own quota.** Mistral reports
  it in the `x-ratelimit-limit-custom-minute` / `-custom-day` headers (one
  individual plan allowed 5 connector requests per minute and 50 per day, with
  the daily count resetting at midnight UTC). Past it, every connector call
  answers HTTP 429.
- **A refused turn says which limit it hit.** When a provider answers HTTP 429,
  the device's reply names the window from the provider's rate-limit headers
  or error text: a per-minute limit ("wait a minute and ask again"), a daily
  limit (for Mistral Studio connectors, "it resets at midnight UTC"), a plan
  that allows no requests of that kind ("waiting won't help"), or a spent
  quota ("check the plan and billing"). When the provider does not say, the
  reply makes no promise about when. A Virtual Nimbus reads the same headers.
  A sub-agent the provider refuses at start says it couldn't start on that
  provider and names the window the same way, so a Mistral sub refused by a
  spent connector day says it resets at midnight UTC.
- **Keep it to about 2 enabled Mistral connectors at a time.** One or two work
  reliably. With around 4 enabled at once the Conversations response can come
  back with no message output and the turn fails over to another provider: no
  crash, but the connector call does not run.
- **Changing a connector takes effect on the next turn** - the device resets
  the Mistral conversation whenever you add, toggle, or remove a connector
  (connectors are pinned at conversation creation, so this is required for a
  change to apply).
- **Private repos:** a Studio GitHub connector only sees repos its authorized
  account or org can access - a private repo returns "not accessible" even
  though the connector itself is working.
- **Heavy connector writes are streamed, not buffered.** The device parses
  every provider response *off the socket* (only the filter-retained fields
  are stored), so a large connector-write response no longer exhausts the
  board's memory - the Notion page-create that used to restart it now
  completes. For heavy writes and large fetches, prefer **spawning a
  sub-agent** (the Orchestrator is guided to): the work runs on the provider's
  compute and only a short result returns.

## Security

- Secrets are **write-only**: the web UI never echoes a token back (it shows a
  "saved" state); editing a card and leaving the credential blank keeps the
  stored secret.
- The model **cannot** configure connectors or read their secrets - the
  `connector` config key is a protected key, refused at the wire.
- Everything is behind the per-device access token (identify once per browser).

## Finance connectors

Any broker or finance connector (e.g. Alpaca) must stay **read-only** - Nimbus
never places trades or moves money on its own. Keep write scopes off.
