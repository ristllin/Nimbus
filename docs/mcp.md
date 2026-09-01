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
per-device access token (`/mcp?t=<token>`). `tools/list` returns the device's
tool catalog; `tools/call` runs one. This is the same registry the Orchestrator
uses on its own turns, so what a LAN client can call and what the assistant can
call never drift apart.

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

Beyond tools, the device reads the other things a modern MCP server offers, so a
real server works fully rather than partially:

- **Resources and resource templates** - readable items the server exposes (a
  file, a record, a report), including ones named by a fill-in-the-blank pattern.
- **Prompts** - reusable, named request templates the server publishes.
- **Pagination** - a large tool, resource, or prompt list is read across pages.
- **Change signals** - when a server announces its tools, resources, or prompts
  changed, the device picks up the new set on the next turn.
- **Progress** - progress updates during a long call are understood rather than
  read as an error.

Two parts of the MCP spec are deliberately left out, because the device cannot
serve them in a way that helps you: **sampling** (a server asking the device to
run its own model mid-call) and **elicitation** (a server prompting you for input
mid-call). The device runs one request at a time with no room to answer a model
question inside another call, and it has no way to put an interactive form in front
of you during a tool call. The device tells servers up front that it does not offer
these, so a well-behaved server simply does not ask.

### Setting one up

In the web UI under **Assistant -> Connectors**, add a server with:

- a **name** (for example `linear`),
- its **MCP endpoint URL** (for example `https://mcp.linear.app/mcp`, or a
  server on your own network like `http://192.168.1.20:3111/mcp`),
- a **token** if the server needs a fixed one (sent as `Authorization: Bearer`),
- **device-dialed** turned on (this is the client path, not provider-attach),
- **approved** turned on (see below).

The assistant is told, every turn, which device MCP servers are ready, which are
waiting for your approval, and which are temporarily unreachable.

### Signing in to a hosted server (OAuth)

Some hosted servers, such as `https://mcp.linear.app/mcp`, do not take a pasted
token at all: they use a sign-in you complete in your browser. The device has no
browser of its own, so it borrows one you already have. It shows a link on the
Connectors page; you open that link on a phone or laptop on the same network,
approve access there, and the browser hands the result back to the device. The
device keeps the resulting access on its own from then on. Your login and password
never touch the device.

#### Connecting Linear, step by step

Linear's hosted server (`https://mcp.linear.app/mcp`) is the worked example; Notion,
Sentry, and Atlassian connect the same way.

1. In the web UI under **Assistant -> Connectors**, add a server with the name
   `linear` and the endpoint `https://mcp.linear.app/mcp`. Leave the token field
   empty (this server does not use one). Turn on **device-dialed**.
2. On the new connector, choose **Sign in**. The device now shows a short link, a
   short code, and a QR code on the Connectors page.
3. On your phone or laptop, open the link (or scan the QR). It takes you to Linear's
   own sign-in page. Sign in and approve access for the device.
4. Linear sends the browser back to the device, which finishes the exchange and
   stores a lasting sign-in. The connector flips to connected, and Linear's tools
   appear on your next turn.

To disconnect, remove the connector.

#### What is headless, and the one step that is not

Everything except step 3 happens on the device with no help:

- **Headless (the device, on its own):** finding the server's sign-in details,
  registering itself as a client, minting the sign-in link and code, completing the
  token exchange when the browser returns, storing the result, and refreshing access
  quietly before it expires on every turn after that.
- **Needs your browser (once):** approving access at the server's sign-in page in
  step 3. This is a deliberate consent, so it is your call, not the model's, and it
  cannot be automated away. It happens one time; the device handles the refreshes
  after that without asking again.

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

> Confirming a live end-to-end sign-in against a hosted server is a bench step. The
> sign-in machinery is verified on the host against recorded sign-in details from a
> real server, and the on-device sign-in screen and web flow are built; the live
> "approve on a phone, tools appear" round trip against `mcp.linear.app` is checked on
> real hardware, not claimed from a host build.

### A local server stays on the device

A server on your own network (a `192.168.x.x`, `10.x`, `172.16-31.x`, `.local`,
or `localhost` address) is reachable from the device but not from a provider's
cloud. Turning on **device-dialed** keeps that server on the device's own client,
where it belongs: the device connects to it directly and never hands the URL to a
provider. This is the default for a device-dialed server, so you do not have to
pick a provider for it.

A provider only ever receives a connector whose URL its cloud can actually dial.
If a local URL is pointed at a provider by mistake, the device leaves it off that
provider's request rather than letting one bad entry fail the whole turn: that one
tool is simply skipped, and the fix is to turn on device-dialed for it, or to use
a public `https` address.

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
- **A slow or dead server cannot wedge a turn.** Each call has a short timeout,
  and a server that keeps failing is put on a brief cooldown (a circuit breaker)
  and skipped until it recovers, so one bad server does not stall your requests.
  When a call does fail, the message says what happened and the one next step.
- **Bounded size.** A response is capped: a server that sends far more than the
  device can hold is refused with a clear message that says so, never a crash.
- **Tool budget.** Up to a few dozen tools per server are surfaced, within the
  assistant's context budget. A server that offers more has the rest left off,
  and which tools are kept is the same every time.

## Servers worth connecting

A short, honest starter set of remote MCP servers a Nimbus owner tends to get real
use from, grouped by how you connect today. The endpoint is what you paste into the
connector's **MCP endpoint URL**; verify the current URL on the server's own page
before relying on it, since hosted endpoints move. How you sign in matters more than
the tool list: the three groups below are the three sign-in paths the device
supports right now.

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
cloud, so turn on **device-dialed** for it (the default for a private address) and
give it a fixed token. These keep your data on your network.

| Server | What it does for you | Endpoint | Connect with |
|---|---|---|---|
| Home Assistant | Control lights, scenes, and climate, and read sensors, by voice, on your own network. | `http://homeassistant.local:8123/api/mcp` | A long-lived access token from Home Assistant. |
| Your own server | Reach files, notes, or a database on a machine at home without sending anything to a provider. | `http://192.168.1.20:3111/mcp` (your address) | A fixed token you set on that server. |

### Hosted servers that need a browser sign-in

These are OAuth-only: they do not take a pasted token, so you connect them with the
**Sign in** button (see [Signing in to a hosted server](#signing-in-to-a-hosted-server-oauth)),
approving on a phone or laptop on the same network. Sign-in works with a server that
offers automatic client registration; a server that needs a hand-registered client is
not supported this way.

| Server | What it does for you | Endpoint | Connect with |
|---|---|---|---|
| Linear | Create and update issues and projects. | `https://mcp.linear.app/mcp` | Sign in (browser). |
| Notion | Search and update your pages and databases. | `https://mcp.notion.com/mcp` | Sign in (browser). |
| Sentry | Look up errors and issues from your projects. | `https://mcp.sentry.dev/mcp` | Sign in (browser). |
| Atlassian | Read and update Jira issues and Confluence pages. | `https://mcp.atlassian.com/v1/sse` | Sign in (browser). |

## Using MCP with a Cumulo key

You do not need a public address to reach the device's MCP server from outside
your home. Pair the device for [cloud access](cloud-relay.md) and reach `/mcp`
through the tunnel; the device stamps its own access token onto the forwarded
request, so the token never leaves the device. See
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
- **Sign-in does not finish** - open the link on a device on the same network as
  the Nimbus, and finish approving within a couple of minutes. If it still stalls,
  start the sign-in again from the connector.
