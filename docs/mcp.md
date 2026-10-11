<!-- audience: user -->
# MCP - the device as client and server

[MCP](https://modelcontextprotocol.io) (the Model Context Protocol) is a common
language for tools. Nimbus speaks it both ways:

- **As a server.** The device exposes its own tools (memory, sessions, docs, and
  more) over MCP, so another app on your network can drive it.
- **As a client.** The device can dial a remote MCP server itself, discover the
  tools it offers, and use them on your turns - so "create a Linear issue" or
  "read that file from my file server" works from Telegram or voice.

This page covers both. For the provider-attached connectors (Gmail, GitHub, and
the rest, run in the AI provider's cloud), see [Connectors](connectors.md).

## The device as a server

The device serves MCP over your LAN at `POST /mcp` (JSON-RPC 2.0), behind the
device sign-in code sent as the `X-Nimbus-Token` request header (the body is
raw JSON-RPC, so the header is the only place the token goes). `tools/list`
returns the device's tool catalog; `tools/call` runs one. This is the same
registry the Orchestrator uses on its own turns, so what a LAN client can call and
what the assistant can call never drift apart.

## The device as a client

A device-dialed MCP server is a connector entry with a URL that the device
connects to directly, rather than handing to a provider. It speaks **Streamable
HTTP**: each request is a JSON-RPC POST, and the device reads the reply whether
the server answers with a single JSON body or a streamed one. A server that
needs a session is followed across requests automatically.

What happens once you add and approve one:

1. The device performs the MCP handshake and asks the server for its tools.
2. Each tool is added to the assistant's toolset, named `mcp.<server>.<tool>`
   (for example `mcp.linear.create_issue`), with its description and input shape.
3. On a turn, the assistant can call those tools like any other. The result
   comes back and is used in the reply.

The device reads the tool list across pages, so a server with a long catalog is
discovered in full up to the tool budget (see [Limits and safety](#limits-and-safety)).
Resources and prompts that a server publishes are not surfaced in this release.

Two parts of the MCP spec are left out: **sampling** (a server asking the device
to run its own model mid-call) and **elicitation** (a server asking you for input
mid-call). The device runs one request at a time and has no way to put a form in
front of you during a tool call, so it tells servers up front that it offers
neither, and a compliant server does not ask.

### Setting one up

The connector cards have no device-dialed switch, so a device-dialed server is
added in the raw editor:

1. In the web UI, open **Assistant → Connectors** and expand
   **Advanced - edit raw JSON** below the cards.
2. Enter the connector list as a JSON array, with the server as one entry:

   ```json
   [{"name":"linear","kind":"mcp","url":"https://mcp.linear.app/mcp","dev":1,"en":1}]
   ```

   - `name`: a short name (for example `linear`).
   - `url`: the server's MCP endpoint (for example `https://mcp.linear.app/mcp`,
     or a server on your own network like `http://192.168.1.20:3111/mcp`).
   - `tok`: a token, if the server needs a fixed one (sent as
     `Authorization: Bearer`).
   - `"dev":1` makes it device-dialed (the client path, not provider-attach);
     `"en":1` turns it on.
3. Choose **Save JSON**. Saving replaces the whole list, and stored tokens are
   never shown back, so include every connector you already have, with its token.
4. The server's card now shows **pending approval**. Choose **Approve** (see
   [Approval](#approval---you-decide-before-it-connects)).

The assistant is told, every turn, which device MCP servers are ready, which are
waiting for your approval, and which are temporarily unreachable.

### Signing in to a hosted server (OAuth)

Some hosted servers, such as `https://mcp.notion.com/mcp`, do not take a pasted
token at all, and others, such as `https://mcp.linear.app/mcp`, also accept one:
either way, they offer a sign-in you complete in your browser. The device has no
browser of its own, so it borrows one you already have. It shows a link on the
Connectors page; you open that link on a phone or laptop on the same network,
approve access there, and the browser hands the result back to the device. The
device keeps the resulting access on its own from then on. Your login and password
never touch the device.

#### Connecting Linear, step by step

Linear's hosted server (`https://mcp.linear.app/mcp`) is the worked example; Notion,
Sentry, and Atlassian connect the same way.

1. Add the server as in [Setting one up](#setting-one-up): name `linear`, URL
   `https://mcp.linear.app/mcp`, no token (to sign in through the browser instead;
   a Linear API key in `tok` also works and skips the steps below), and `"dev":1`.
   Then choose **Approve** on its card.
2. On the card, choose **Sign in**. The device shows a link, a short code, and a
   QR code on the Connectors page. The code only helps you match the link to this
   device; nothing asks you to type it.
3. On your phone or laptop, open the link (or scan the QR). It takes you to Linear's
   own sign-in page. Sign in and approve access for the device.
4. Linear sends the browser back to the device, which finishes the exchange and
   stores a lasting sign-in. The connector flips to connected, and Linear's tools
   appear on your next turn.

To disconnect, remove the connector.

#### What is headless, and the one step that is not

Everything except step 3 happens on the device:

- **On the device:** finding the server's sign-in details, registering itself as a
  client, minting the sign-in link and code, completing the exchange when the
  browser returns, storing the result, and refreshing access before it expires.
- **In your browser, once:** approving access at the server's sign-in page in
  step 3. This is a deliberate consent, so it is your call, not the model's.

The stored sign-in lives only on the device, is never shown or logged, and is used
the same way a token is: sent as `Authorization: Bearer` on each call.

#### What has to be true for it to work

- The phone or laptop you approve on must be able to reach the device by the address
  shown (a home network address or `nimbus.local`). The device registers that same
  address as where the server should return the browser, so a server that cannot be
  handed a private-network return address, or that only registers clients by hand
  rather than automatically, is not supported this way. Use a fixed token instead if
  that server offers one.
- The device must be on your Wi-Fi with a reachable address before you start; the
  sign-in link points at the device itself.

### A local server stays on the device

A server on your own network (a `192.168.x.x`, `10.x`, `172.16-31.x`, `.local`,
or `localhost` address) is reachable from the device but not from a provider's
cloud. Marking it device-dialed (`"dev":1`, see [Setting one up](#setting-one-up))
keeps that server on the device's own client, where it belongs: the device
connects to it directly and never hands the URL to a provider. A device-dialed
server with no provider chosen (no `prov` in its entry) stays device-side only, so
you do not have to pick one.

A provider only ever receives a connector whose URL its cloud can actually dial.
If a local URL is pointed at a provider by mistake, the device leaves it off that
provider's request rather than letting one bad entry fail the whole turn: that one
tool is simply skipped, and the fix is to mark it device-dialed (`"dev":1`), or to
use a public `https` address.

### Approval - you decide before it connects

A device-dialed server is **never contacted until you approve it**. An
unapproved server is listed as pending and none of its tools exist yet: nothing
is dialed, nothing is registered. This is deliberate - approving a server lets
the device send it your requests, so it is your call, not the model's. The model
can never approve a server or change a connector; that rail is enforced in
firmware.

Approval and use are also role-aware: only an owner-level account manages
connectors, only an approved account with write access can call a remote tool,
and remote tools are turned off entirely during automated (routine or
background) turns, so untrusted automated text can never reach an outside server.

### Limits and safety

- **One request at a time.** Like every network call on the device, an MCP call
  takes the single shared connection slot, in turn with provider calls. There is
  no on-device parallelism by design.
- **A slow or dead server cannot wedge a turn.** Discovery allows 8 seconds per
  request and a tool call 4 seconds. A server that keeps failing is put on a
  brief cooldown (a circuit breaker) and skipped until it recovers, so one bad
  server does not stall your requests. When a call fails, the message says what
  happened and the one next step.
- **Bounded size.** A response larger than 160 KB is refused with a message that
  says so, never a crash.
- **Tool budget.** Up to 6 device-dialed servers and up to 48 tools per server
  are surfaced. A server that offers more has the rest left off.

## Servers worth connecting

A starter set of remote MCP servers, grouped by how you sign in, which is the
three paths the device supports. The endpoint is what you paste into the card's
**MCP server URL**; check it on the server's own page first, since hosted
endpoints move.

### Works now with a token or no sign-in

These take a fixed token (pasted into the connector) or no sign-in at all, so they
connect the moment you approve them.

| Server | What it does for you | Endpoint | Connect with |
|---|---|---|---|
| GitHub | Read and triage issues, pull requests, and code across your repositories. | `https://api.githubcopilot.com/mcp/` | A personal access token in the token field. |
| Hugging Face | Search models, datasets, and papers, and run small hosted apps from a turn. | `https://huggingface.co/mcp` | A Hugging Face access token in the token field. |
| Context7 | Pull current, version-specific library and framework docs into an answer. | `https://mcp.context7.com/mcp` | No token needed; an optional free key raises the rate limit. |
| DeepWiki | Ask questions about any public GitHub project's documentation. | `https://mcp.deepwiki.com/mcp` | No sign-in; public repositories only. |

### On your own network (the best fit for a desk device)

A server on your own network is reachable from the device but not from a provider's
cloud, so add it device-dialed with `"dev":1` (see [Setting one up](#setting-one-up))
and give it a fixed token. Your data stays on your network.

| Server | What it does for you | Endpoint | Connect with |
|---|---|---|---|
| Home Assistant | Control lights, scenes, and climate, and read sensors, by voice, on your own network. | `http://homeassistant.local:8123/api/mcp` | A long-lived access token from Home Assistant. |
| Your own server | Reach files, notes, or a database on a machine at home without sending anything to a provider. | `http://192.168.1.20:3111/mcp` (your address) | A fixed token you set on that server. |

### Hosted servers that sign in through the browser

These connect with the **Sign in** button (see [Signing in to a hosted server](#signing-in-to-a-hosted-server-oauth)),
approving on a phone or laptop on the same network. Sign-in works with a server that
offers automatic client registration; a server that needs a hand-registered client is
not supported this way.

| Server | What it does for you | Endpoint | Connect with |
|---|---|---|---|
| Linear | Create and update issues and projects. | `https://mcp.linear.app/mcp` | Sign in (browser), or a Linear API key in the token field. |
| Notion | Search and update your pages and databases. | `https://mcp.notion.com/mcp` | Sign in (browser). |
| Sentry | Look up errors and issues from your projects. | `https://mcp.sentry.dev/mcp` | Sign in (browser). |
| Atlassian | Read and update Jira issues and Confluence pages. | `https://mcp.atlassian.com/v1/sse` | Sign in (browser). |

## Using MCP with a Cumulo key

You do not need a public address to reach the device's MCP server from outside
your home. Pair the device for [cloud access](cloud-relay.md) and reach `/mcp`
through the tunnel; the device stamps its own sign-in code onto the forwarded
request, so the code never leaves the device. See
[Cloud access](cloud-relay.md) for pairing and the security model.

## Troubleshooting

- **"pending approval"** - approve the server on the Connectors page.
- **"cooling down"** - the server failed repeatedly; the device retries after a
  short wait. Check the URL is the MCP endpoint and the server is up.
- **"rejected the credential"** - update the server's token on the Connectors
  page.
- **No tools appear after approval** - confirm the URL is reachable *from the
  device* (a server on `localhost` of another machine is not), and that it is a
  Streamable HTTP MCP endpoint.
- **Sign-in does not finish** - open the link on a phone or laptop on the same
  network as the device and finish approving promptly. If it still stalls, choose
  **Sign in** on the card again.
