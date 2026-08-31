# Cloud access (remote tunnel)

Cloud access lets you reach a Nimbus device from anywhere, without port forwarding,
through the CumuloNimbus service at `app.cumulo-nimbus.ai`. It is optional, off by
default, and available in Orchestrator mode.

## How it works

The device dials one outbound secure WebSocket to the relay and keeps it open. When
you open the device from the web app, the relay forwards that HTTP request down the
tunnel to the device, which answers it from its own local web server and streams the
response back. You see the device's real web UI, remotely. There is no second app to
keep in sync: the device is the app.

```
browser  ->  app.cumulo-nimbus.ai  ->  relay  ==WSS==>  device  ->  its local web UI
             (sign in, entitlement,       (tunnel)
              ownership checks)
```

The untrusted device UI is served on a separate origin (`d.cumulo-nimbus.ai`) so it
can never touch the signed-in session on the main origin.

## Enable and pair

1. Put the device in Orchestrator mode.
2. Start pairing from either surface:
   - On the device screen, open Settings > Connectivity > Cloud link code.
   - Or on the device web UI, open Settings > Cloud access and choose "Pair with the cloud".

   The device shows a **Cloud link code** (8 characters, and a QR) on its screen. The
   console command `CLOUDPAIR` does the same on a test build.
3. Sign in at `app.cumulo-nimbus.ai`, open the pairing page, and enter the Cloud link
   code (or scan the QR) to pair the device.
4. The device connects and appears in your account as online. "Open" serves its UI
   through the tunnel.

To stop remote access, use "Unpair" (keeps cloud access on, drops this pairing) or
"Turn off" (disables cloud access). Either can also be done from the account page.

## Security

- The relay connection always validates the server certificate against the device's
  built-in certificate bundle. It never falls back to an unvalidated connection.
- The credential the device uses to connect is issued by the service at pairing and is
  stored only on the device. Unpairing revokes it.
- The credential expires (30 days by default) and the device renews it on its own,
  quietly, before it lapses: it re-mints at a randomized point past the halfway mark of
  the credential's life, and only while the tunnel is idle, so a renewal never
  interrupts a request. An expired credential can still be renewed for a short grace
  period; past that, the device shows a fresh pairing code so you can sign in again.
  Older devices whose credential never expires keep working and pick up an expiring one
  the first time they renew or re-pair.
- Requests arriving over the tunnel are already authenticated by the service (the
  signed-in owner, an active subscription, and device ownership are all checked before
  anything is forwarded). The device treats the tunnel as trusted and stamps its own
  local access token onto the forwarded request; that token never leaves the device.
- The relay refuses a device connection, or drops a live one, when a pairing is revoked
  or a subscription lapses.

## Notes and limits

- Cloud access runs in Orchestrator mode only. In Notifier mode the device does not
  connect; switch modes to use it.
- One request is served at a time over the tunnel, which is ample for a single owner
  viewing the device UI.
- If the connection to the relay is interrupted, the device notices on its own within a
  few seconds and reconnects, so remote access recovers without a restart. While it
  reconnects, the web app shows a brief "Reaching your Nimbus" page and loads the device
  as soon as it is back.
- A future hardening step pins the relay's certificate issuer in addition to the
  bundle check; see [security.md](security.md).

## Virtual Nimbus (hosted instances)

A Virtual Nimbus is a hosted instance of the same assistant, with no physical
device. You reach it the same way, and it serves the **same web app** you already
know: Home, Chat, Memory, Assistant, and Device, with your chat, memory, and
provider settings all live. Because the relay session already signed you in, the
page opens without a second sign-in.

What is the same, and what differs, panel by panel (the parity a hosted instance
keeps):

| Panel | On a device | On a Virtual Nimbus |
|---|---|---|
| Chat | Real | Real (the same engine) |
| Memory (files, long-term memory, scratchpad) | Real | Real |
| Assistant (providers, models, tools, usage) | Real | Real |
| Ring, display, touch | Real hardware | Not present - shown honestly, no controls that do nothing |
| Battery | Real reading | Always on external power (hosted) |
| Sound (mic, speaker, tones) | Real hardware | Not present |
| Wi-Fi and network | Real radio | Managed by the platform |
| Software update | Device firmware update | The platform rolls the instance image; there is no device update to run |

The instance never shows a made-up hardware reading and never leaves a control
that does nothing: a panel that has no hardware behind it says so plainly.
