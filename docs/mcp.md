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

### Setting one up

In the web UI under **Capabilities -> Connectors**, add a server with:

- a **name** (for example `linear`),
- its **MCP endpoint URL** (for example `https://mcp.linear.app/mcp`, or a
  server on your own network like `http://192.168.1.20:3111/mcp`),
- a **token** if the server needs one (sent as `Authorization: Bearer`),
- **device-dialed** turned on (this is the client path, not provider-attach),
- **approved** turned on (see below).

The assistant is told, every turn, which device MCP servers are ready, which are
waiting for your approval, and which are temporarily unreachable.

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
- **Bounded size.** Responses are read into the device's larger memory and
  capped; a server that sends far more than the device can hold is refused
  cleanly rather than crashing.
- **Tool budget.** Up to a few dozen tools per server are surfaced, within the
  assistant's context budget.

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
