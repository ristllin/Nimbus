<!-- audience: user -->
# Connectors - external tools, per provider

Connectors let the Orchestrator (and the sub-agents it spawns) reach the
outside world. With a connector enabled you can ask, over Telegram or voice,
things like:

- "Any new issues on my repo?" - GitHub returns the live list.
- "How many unread emails do I have?" - Gmail answers.
- "Publish that summary to my Notion workspace." - a page appears.

**How it works, in one sentence:** Nimbus does *not* speak to
Gmail/Notion/GitHub itself. It attaches a small *connector descriptor* to the
LLM request, and the **provider runs the tool server-side in its own cloud**.
So what you can do is whatever your provider hosts, and the credential lives
with the provider (Mistral Studio) or is pasted once into Nimbus's web UI.

> **Scope note.** This page is the *provider-attach* model - the AI lab executes
> the tool in its own cloud. Nimbus can *also* dial a remote MCP server itself
> (the device as an MCP client): see [MCP](mcp.md) for that path, its approval
> model, and limits. Google **Sheets/Slides** have no hosted MCP anywhere, so
> they are not reachable either way today (they need direct REST - a future
> pass). Gmail, Calendar, and Drive *are* reachable.

## Setting up a connector

Every connector is a **paste-a-credential card** under **Capabilities →
Connectors** in the web UI. Connector credentials are pasted once into that
card and stored write-only on the device - setup never involves the firmware
repository or any script:

1. Create the credential once in a browser: a GitHub personal access token, a
   Notion integration token, a Slack bot token, an OAuth refresh token, or a
   Studio-side connection.
2. Open the device web UI → **Capabilities → Connectors**.
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
| **Anthropic** | - (the head turn is a single forced tool; MCP can't run mid-turn) | ✅ BYO remote MCP by URL (`mcp_servers`) |
| **Mistral** | ✅ Studio built-ins + Studio-named connectors - **single-shot turns** | ✅ sub-agents run over the Conversations API; built-ins *and* Studio connectors attach to the spawn dispatch (server-side) |

Two consequences:

- **The current provider matters.** A Mistral connector is only live when
  Mistral is the Orchestrator's host. An Anthropic connector only runs on
  sub-agents. Capabilities → Connectors badges each card with what applies
  *right now*, and the model is told the same thing every turn.
- **Mistral requires the least setup.** Its connectors are authenticated in
  **Mistral Studio** and referenced by name - Nimbus stores *no secret* for
  them.

## How Nimbus knows a connector is available

Nimbus does not blindly trust that a pasted key works. **Capability validation**
(Capabilities → Models → Tool use) controls how it confirms provider access
before it tells the assistant a connector is available:

- **Off** - trust the key as-is, and make no "verified" claim.
- **Passive** (default) - mark a provider verified once a real call, or a manual
  **Verify**, succeeds; the result is cached.
- **Active** - passive, plus a periodic re-verify on a slow schedule so a revoked
  key or changed access is caught. The re-check is a free metadata call
  (`GET /v1/models`, roughly $0), one provider per wake, spread so each keyed
  provider refreshes about every *re-check* interval (default 24 hours, range
  1–168).

The settings persist as NVS `capProbe` (0/1/2) and `capProbeH` (hours,
`src/agent/store.cpp`). Active mode runs only in Orchestrator mode (the re-verify
needs the TLS heap) and reuses the same one-slot verify worker as the web
**Verify** button - no new concurrency.

**Per-connector credential state (v4.1).** Enabling a connector is a checkbox,
not proof it works. The catalog the assistant reads marks the two states that
need the owner: a connector whose OAuth sign-in **failed** on its last mint
("sign-in FAILED - tell the owner") and a remote MCP / first-party connector
that has **no credential at all** ("NO credential - not usable"), which the
attach layer skips rather than sending a doomed request. A bare name means no
known problem; a successful OAuth mint this boot is positive proof the
credential works (`src/agent/connectors.cpp` `authStateOf`, fed from the one
credential choke point).

> **What validation does not do.** It confirms the *provider key* works, not each
> connector's individual tools. Per-connector *functional* probing - actually
> calling a connector, which would spend tokens - is not implemented. A connector
> can be attached and still fail on a specific call (a private repo, a missing
> scope); the assistant is guided to verify writes by reading them back.

Whatever the mode, the model is told the truth every turn: its
`[PROVIDERS & CONNECTORS]` block marks each provider **available, verified** /
**key present, not yet verified** / **key present but rejected on last check** /
**no key** - and with validation off, **key present (validation off - trusting
key presence)**. That block is built in `catalogText`
(`lib/core/src/orch_connectors_wire.cpp`), fed the verify cache and the mode by
`connectors.cpp` `catalog()`.

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
- **Credential:** a Notion **Internal Integration** token (`secret_…`) -
  Notion → Settings → Connections → Develop integrations. Share each target
  page with the integration once. (Notion's *official* MCP mandates
  per-session OAuth and is unusable headless - prefer the static integration
  token.)
- **Unlocks:** create and query pages and databases - report publishing,
  research corpora.

## Slack {#slack}

- **Providers:** Mistral (Studio); elsewhere needs a small REST adapter (future).
- **Credential:** a **bot token** (`xoxb-…`) - create a Slack app once in a
  browser, install it to the workspace, copy the token. No on-device OAuth.
- **Unlocks:** read channel/DM history, search, post messages, reactions.

## Linear {#linear}

- **Providers:** OpenAI / Anthropic (remote MCP `https://mcp.linear.app`), Mistral (Studio).
- **Credential:** a static **Linear API key** (Bearer) that bypasses OAuth
  entirely - Linear → Settings → API.
- **Unlocks:** find, create, and update issues, projects, and comments.

## Mistral built-ins {#mistral-builtins}

Hosted tools that run inside Mistral with **no device secret** - just toggle
them on here (provider = Mistral). They attach as `{type:"<name>"}` to the
Orchestrator's own single-shot Mistral turns.

- **web_search** - hosted web search with citations.
- **code_interpreter** - sandboxed Python execution.
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

## Operational notes (live-verified on hardware)

- **Mistral connectors attach on single-shot turns** (tool loop off) **and on
  sub-agent dispatch** - not on the agentic tool-loop, which forces tool-choice
  (rejected alongside built-in connectors). Set the Orchestrator host to Mistral
  and the tool loop off to use them on your own turns, or spawn a Mistral
  sub-agent for connector work while the loop is on.
- **Keep it to about 2 enabled Mistral connectors at a time.** One or two work
  reliably (proven: GitHub returned a live issue count, Gmail a live unread
  count). Enabling around 4 at once made the Conversations response come back
  with no message output and the turn failed over to another provider. It
  fails *soft* (no crash), but the connector call won't run.
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
