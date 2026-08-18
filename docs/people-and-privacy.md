<!-- audience: user -->
# People and privacy - roles, quotas, what stays private

Nimbus started as one person's device. It now talks to whoever the owner lets
in - family, a team, a group chat - and that changes what "remember this" has
to mean. This page covers who can do what, what each person can and cannot
see, and where the boundaries are actually enforced.

The short version: **everyone gets their own memory, their own files, and
their own conversation history. Nothing crosses between people unless a file
is explicitly shared, and vector memory never crosses at all.**

As an admin, the things you can do:

- Approve someone and pick their role - in the web UI, or by just asking the
  assistant ("approve 12345 as a guest").
- Raise or lower a person's storage limits ("give Alex more storage").
- Revoke someone - they lose access immediately; their data stays, in case you
  restore them later.

## Roles

Every chat the device knows about has exactly one role.

| Role | What it means |
|---|---|
| **Unknown** | Messaged the device but was never approved. No data, no control. This is the default, and it is also what revoking someone returns them to. |
| **Guest** | Approved, with the tightest limits. Own memories and files, memories expire soonest, cannot share files, cannot pin anything permanently. |
| **User** | Approved. Own memories and files, a small budget of permanent pins, and may share their own files. |
| **Admin** | Runs the device. Sees every namespace and every file, manages people, changes settings, installs updates. Not subject to quotas. |

Two rules the device enforces for you:

- **There is always at least one admin.** Demoting or removing the last one is
  refused, in the web UI and in conversation alike. A device nobody can
  administer is worse than any mistake that refusal prevents.
- **An upgrade changes nobody's access.** When a device running an older
  firmware first boots into this one, today's Telegram owners become admins
  and everyone else on the allowlist becomes a user - the access they already
  had.

### Setting roles

Two surfaces, one source of truth:

- **Capabilities → Connectors → Telegram** - each person's chip shows a role
  badge. Click it to cycle admin → user → guest. Their limits are shown next
  to it.
- **In conversation** - an admin can just ask: "approve 12345 as a guest",
  "what can Sam do?", "give Alex more storage". The assistant has
  `tenant.list`, `tenant.set_role`, and `tenant.set_quota`, and every one of
  them refuses a non-admin caller.

The model can never set a role for itself or change its own limits, and none
of these tools run in a scheduled routine - a role change is a thing a person
asks for while present.

## Quotas

Each person has a ceiling on what they can store. Set one explicitly to
override the default for their role; leave it at zero to inherit.

| Limit | Guest | User | Admin |
|---|---|---|---|
| Memories (vectors) | 100 | 500 | unlimited |
| Files | 1 MB | 8 MB | unlimited |
| How long a memory lives | 30 days | 1 year | unlimited |
| Permanent pins | none | 10 | unlimited |

**A request over the limit is clamped, not refused.** Someone asking the
device to remember something "forever" gets their maximum instead, and is told
so - the fact still lands. The exception is permanent pins, which are counted
rather than clamped, because a pin is exempt from both eviction and cleanup:
unbounded pins are storage a quota can never reclaim.

## What is private, and what can be shared

| | Crosses between people? |
|---|---|
| Vector memory (what the device recalls) | **Never** |
| Conversation history | **Never** |
| Files | Only when their owner shares them |

**Vector memory is deliberately never shared, not even opt-in.** Recall is
automatic - the device pulls relevant memories into its context before it
answers. A shared memory pool would mean anyone who can write a memory can put
words into the context of everyone else's conversations, including an admin's.
That is a prompt-injection channel with no upside, so the design forecloses
it. Device-level facts live in the admin's namespace instead.

**Files may be shared, and sharing is read-only.** A person can mark their own
file shared, which makes it visible and readable to everyone. It does not
become writable: another person cannot overwrite it, and cannot un-share it
either. Only the owner controls their file. Guests cannot share at all.
Admins can read every file regardless.

**Revoking access suspends, it does not destroy:** a revoked person
immediately loses both reads and writes, and if they are restored later their
data is still there.

## Photos and files sent to the device

**A photo becomes a description.** When someone sends a picture, the device
looks at it once, writes down what it sees, and the description is what enters
the conversation. The original is kept on the SD card, so it can be looked at
again later; the conversation carries a sentence, not several hundred
kilobytes of image that would crowd out everything else.

**Media needs an SD card.** With no card the device says so and suggests
describing the file in words instead. It will not half-accept something it
cannot keep.

If no vision provider is configured, the photo is still stored and
acknowledged - the conversation simply does not gain a description.

## Viewing files in the web UI

**Memory & Files** lists everything stored, with **view** next to files that
can be previewed in place: images render inline, text opens in a panel.

Previews are narrow on purpose. Files arrive from everyone the device talks
to, and the web UI holds the device access token. An HTML or SVG file rendered
inside the UI would run its own script on the device's origin, with that token
in reach. So the device renders only what cannot execute - images and plain
text - and everything else downloads instead, however the request is framed.

## Where this is enforced

Not in the UI. Every boundary is checked in the code path that touches the
data, so a different surface (the web app, Telegram, a LAN MCP client, the
assistant's own tools) cannot reach around it:

- **Recall and search** filter to the caller's namespace before ranking.
- **Reads by id** are checked too, not just writes - revocation that stops
  writing but leaves reading working is not revocation.
- **Conversation history** is scoped to the asking chat; naming someone else's
  session id does not open it.
- **Files** check ownership on read, write, and share separately.
- **Quotas** are enforced at the write, not by cleaning up afterwards - a
  limit applied later lets a burst win the race.

<details>
<summary>For developers - where the code lives</summary>

The RBAC core is portable and host-tested:
`lib/core/include/nimbus/orch/rbac.h` + `lib/core/src/orch_rbac.cpp` - roles,
the single `permsFor()` table every rail consults, `TenantStore` (including
the last-admin rule), and `adoptLegacy()` for the upgrade migration. A
`Principal{ns, owner, role, quota}` is threaded from the turn into every tool
call, and boundaries are enforced at the data path (VDB namespace filters,
episodic `sessionAllow`, file `owner`/`shared` fields), never in a surface.

The hardware suites `tests/hil/test_l17_rbac.py`, `test_l18_multitenant.py`,
`test_l19_file_viewer.py`, and `test_l20_degradation.py` try to read across
the boundary through every surface rather than merely confirming the happy
path. The privacy assertions are mutation-tested: with the namespace filter
deliberately removed, they fail.

</details>

## Related

- [Storage tiering](./orchestrator-storage.md) - where each kind of data lives
- [Conversation compaction](./compaction.md) - how long history is kept
- [Security posture](./security.md) - the device's open security items
