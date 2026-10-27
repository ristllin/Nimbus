<!-- audience: user -->
# People and privacy - roles, quotas, what stays private

This page is for an admin who shares the device with other people (family, a
team, a group chat). It covers the roles each person can hold, the storage
limits each role gets, what stays private between people, and where those
boundaries are enforced.

The short version: **everyone gets their own memory, their own files, and
their own conversation history. Nothing crosses between people unless a file
is explicitly shared, and vector memory never crosses at all.**

As an admin you can:

- Approve someone and pick their role, in the web UI or by asking the
  assistant ("approve 12345 as a guest").
- Raise or lower a person's storage limits ("give Alex more storage").
- Revoke someone. They lose access immediately; their data stays so they can
  be restored later.

## Roles

Every chat the device knows about has exactly one role.

| Role | What it means |
|---|---|
| **Unknown** | Messaged the device but was never approved. No data, no control. This is the default, and it is also what revoking someone returns them to. |
| **Guest** | Approved, with the tightest limits. Own memories and files, memories expire soonest, cannot share files, cannot pin anything permanently. |
| **User** | Approved. Own memories and files, a small budget of permanent pins, and may share their own files. |
| **Admin** | Runs the device. Sees every namespace and every file, manages people, changes settings, installs updates. Not subject to quotas. |

Three rules the device enforces for you:

- **There is always at least one admin.** Demoting or removing the last one is
  refused, in the web UI and in conversation alike, so the device can never
  be left without someone able to administer it.
- **The owner is the admin from the first message.** While no one has the admin
  role yet, a chat on the Telegram allowlist with no role takes the owner rule: the
  chats named as owners (or, when none are named, the first chat on the allowlist)
  are admins, everyone else on the allowlist is a user. Once an admin exists, a chat
  with no role is a user, wherever it sits on the allowlist. The owner-only commands
  follow the role too: a demoted or revoked chat loses them.
- **An upgrade changes nobody's access.** On the first start after upgrading
  from firmware without roles, existing Telegram owners become admins and
  everyone else on the allowlist becomes a user, matching the access they
  already had.

### Setting roles

Two surfaces, one source of truth:

- **Assistant > Connectors > Telegram**: each person's chip shows a role
  badge. Click it to cycle admin > user > guest. Their limits are shown next
  to it.
- **In conversation**: an admin can ask "approve 12345 as a guest", "what can
  Sam do?" or "give Alex more storage". The assistant uses `tenant.list`,
  `tenant.set_role` and `tenant.set_quota`; each refuses any caller who is not
  an admin.

The model can never set a role for itself or change its own limits, and none
of these tools run in a scheduled routine: a role change always comes from a
person in the conversation.

### What only the owner can do

Members and guests talk to the assistant and keep their own memories and files.
Everything that changes the device itself stays with the owner: the main model,
settings, the light ring, the speaker and music, routines, skills, standing rules,
people, and stopping sub-agents. When a member asks for one of these, the assistant
says that only the owner can, and nothing changes.

A sub-agent belongs to the chat that started it. One a member or a guest starts can
search the web and use the provider's own tools, never the owner's connected accounts
(mail, calendar, documents, tool servers), and its results reach only that chat. The
owner's sub-agents and their results stay the owner's, and a member sees only its own
running sub-agents.

Routines, wakeups, the nightly memory pass and the summary of the owner's sub-agent
results run for the owner, but nobody is writing them at that moment. They can do the everyday
things a routine needs, such as dimming the ring at night or playing an alarm.
Anything that changes the main model, a voice or provider setting, a safety
override, the device's name, routines, skills or people needs a message the owner
writes. The full table is in [Security posture](./security.md#who-may-run-each-action).

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

The web UI shows this at a glance. Under **Memory > Long-term memory**, "Usage
by conversation" lists each conversation with how many memories and pins it
holds against its limit, and every memory in the browse list is tagged with the
conversation it belongs to. Only an admin sees this view, and it is read-only:
one conversation's memories are still never visible to another.

## What is private, and what can be shared

| | Crosses between people? |
|---|---|
| Vector memory (what the device recalls) | **Never** |
| Conversation history | **Never** |
| Files | Only when their owner shares them |

**Vector memory is never shared, not even by opt-in.** Recall is automatic:
the device pulls relevant memories into its context before it answers. A
shared memory pool would let anyone who can write a memory put words into
everyone else's conversations, including an admin's, which is a prompt
injection channel. Device-level facts live in the admin's namespace instead.

**Files may be shared, and sharing is read-only.** A person can mark their own
file shared, which makes it visible and readable to everyone. It does not
become writable: another person cannot overwrite it, and cannot un-share it
either. Only the owner controls their file. Guests cannot share at all.
Admins can read every file regardless.

**Revoking access suspends, it does not destroy:** a revoked person
immediately loses both reads and writes, and if they are restored later their
data is still there.

## Photos and files sent to the device

**The assistant sees the photo itself.** When someone sends a picture, the
assistant's model receives the image and keeps a one-line note of what matters in
it; that note is what the conversation keeps. The original is kept on the SD card,
and the assistant can view it again later. If the model in use cannot see images, a
second model describes the photo instead, and the assistant is told it has not seen
it. With an Anthropic key, a recent photo is kept at Anthropic so later replies can
still see it: it is deleted an hour after it was last used, and Anthropic deletes it by
itself two hours after the upload in any case.

**Media needs an SD card.** With no card the device says so and suggests
describing the file in words instead. It will not half-accept something it
cannot keep.

If no provider can read images, the photo is still stored and acknowledged:
the conversation simply does not gain a description.

## Viewing files in the web UI

**Memory > Files** lists everything stored, with a "view" link next to files
that can be previewed in place: images render inline, text opens in a panel.

Previews are limited on purpose. Files arrive from everyone the device talks
to, and the web UI holds the device sign-in code. An HTML or SVG file rendered
inside the UI could run its own script on the device's origin with that code
in reach, so the device renders only what cannot execute (images and plain
text) and everything else downloads instead.

## Where this is enforced

Boundaries are enforced in the code path that touches the data, not in the
UI, so no surface (the web UI, Telegram, a LAN MCP client, the assistant's
own tools) can reach around them:

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
the single `permsFor()` table every surface consults, `TenantStore` (including
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
- [Memory: conversation compaction](./memory.md) - how long history is kept
- [Security posture](./security.md) - the device's open security items
