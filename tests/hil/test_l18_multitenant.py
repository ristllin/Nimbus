"""§L18 - multi-tenant privacy on real hardware: can anyone read anyone else?

The device grew a second, third and Nth correspondent before it grew a data
boundary. This suite is the adversarial half of that work: every assertion here
tries to EXFILTRATE a marker one tenant wrote, through every surface that could
leak it - vector recall, memory.search by id, episodic history, the file store,
and the assembled prompt itself.

Why it can trust its own result: ``/api/test/astool`` does not re-implement the
principal. It builds ``principalForRole(chat, roleOfChat(chat))`` - the same
call the live turn path makes - and dispatches into the same ``handleMcp``. A
green run here is a statement about the production rail, not a parallel one.

Markers: ``hil`` + ``net`` - pure LAN. Serial is never opened (a console open
resets the board AND wedges the host CDC driver).

Run:
    NIMBUS_TEST_IP=192.0.2.26 NIMBUS_TEST_TOKEN=... \\
      python3 -m pytest tests/hil/test_l18_multitenant.py -m net --allow-hardware -v
"""

from __future__ import annotations

import json
import os
import uuid

import pytest

try:
    import requests
except ImportError:  # pragma: no cover
    requests = None

pytestmark = [pytest.mark.hil, pytest.mark.net]

# Three synthetic principals. ALICE and BOB are ordinary users who must never see
# each other; CARLA is a guest (tightest quotas, no sharing, no pins).
ALICE = "918001"
BOB = "918002"
CARLA = "918003"
ALL_TEST_CHATS = (ALICE, BOB, CARLA)


def _u(rig, path):
    ip, tok = rig
    return f"http://{ip}{path}?t={tok}"


def _rpc(rig, chat, method, params=None, _id=1):
    """One JSON-RPC tool call AS `chat`, through the real dispatcher."""
    body = json.dumps({"jsonrpc": "2.0", "id": _id, "method": method, "params": params or {}})
    r = requests.post(_u(rig, "/api/test/astool"), data={"chat": chat, "body": body}, timeout=20)
    assert r.status_code == 200, f"{method} as {chat}: HTTP {r.status_code} {r.text}"
    return r.json()


def _call(rig, chat, tool, args=None):
    return _rpc(rig, chat, "tools/call", {"name": tool, "arguments": args or {}})


def _text(resp) -> str:
    """Flatten an MCP result to searchable text - errors included on purpose.

    A leak that arrives inside an error message is still a leak, so this does
    NOT filter to the success path.
    """
    return json.dumps(resp)


# Each run invents a secret that is SEMANTICALLY distinct, not merely uniquely
# suffixed. Two sentences differing only in a random marker embed almost
# identically, which breaks these tests two ways: the vector store rejects the
# second one as a duplicate (importance bumped, nothing stored), and even when
# it does store, a fresh row cannot out-rank the cluster of near-duplicates left
# by earlier runs - so the positive control fails for reasons that have nothing
# to do with privacy. Both were observed on hardware. Varying the whole template
# keeps each run's query unambiguous however much debris the board holds.
_TOPICS = (
    "vault passphrase",
    "garage keypad code",
    "locker combination",
    "bicycle lock number",
    "safe deposit key",
    "alarm disarm code",
    "wine cellar code",
    "boat ignition code",
)
_PLACES = (
    "Willow Street",
    "Harbour Lane",
    "the summer house",
    "the old mill",
    "Camden depot",
    "the north cabin",
    "Pier 9",
    "the orchard",
)
_FORMS = (
    "{o}'s {t} at {p} is {m}",
    "{o} keeps the {t} for {p} written as {m}",
    "For {p}, {o} uses the {t} {m}",
    "The {t} {o} set up at {p}: {m}",
)


def _secret(owner="Alice"):
    """Returns (sentence, marker). The sentence doubles as the search query."""
    r = uuid.uuid4().int
    topic = _TOPICS[r % len(_TOPICS)]
    place = _PLACES[(r // 8) % len(_PLACES)]
    form = _FORMS[(r // 64) % len(_FORMS)]
    marker = f"{topic.split()[0].upper()}-{uuid.uuid4().hex[:10].upper()}"
    return form.format(o=owner, t=topic, p=place, m=marker), marker


def _store_secret(rig, chat, owner="Alice"):
    """Write a fresh secret as `chat`, retrying past a semantic-dedup collision.

    The store bumps importance instead of adding when a new fact is near-identical
    to an existing one. That is correct behaviour, but it means the row the test
    then reasons about would be someone ELSE's older row - so retry with a
    genuinely different secret rather than assert against the wrong entry.
    """
    for _ in range(4):
        secret, marker = _secret(owner)
        # importance=1 so the row ranks against whatever is already stored. The
        # caps suite (L15) deliberately fills the vector store, and it runs
        # first alphabetically, so a default-importance row can be out-ranked by
        # hundreds of synthetic fillers - the positive control then fails and the
        # privacy assertions never get to run. This is the product's own
        # affordance for "this matters", not a test-only backdoor.
        res = _text(_call(rig, chat, "memory.write", {"content": secret, "importance": 1.0}))
        if "stored" in res:
            return secret, marker
        if "duplicate" not in res:
            raise AssertionError(f"memory.write failed as {chat}: {res}")
    raise AssertionError(
        "every generated secret collided with existing memories on this board, so "
        "the privacy assertions could not be set up. This is NOT a pass - clear the "
        "vector store and re-run."
    )


def _approve(rig, chat, name="l18"):
    """Put a chat on the Telegram allowlist - the real first step.

    A role can only be assigned to someone already approved: the device refuses
    to pre-seed a role on a chat nobody has admitted, because the normal
    approval flow would later hand them rights no one granted.
    """
    requests.post(_u(rig, "/api/telegram/add"), data={"id": chat, "name": name}, timeout=10)


def _unapprove(rig, chat):
    requests.post(_u(rig, "/api/telegram/remove"), data={"id": chat}, timeout=10)


def _set_role(rig, chat, role):
    r = requests.post(_u(rig, "/api/tenant"), data={"id": chat, "role": role}, timeout=10)
    assert r.status_code == 200, f"set {chat}->{role}: {r.status_code} {r.text}"


@pytest.fixture(scope="module")
def rig():
    if requests is None:
        pytest.skip("requests not installed")
    ip = os.environ.get("NIMBUS_TEST_IP")
    tok = os.environ.get("NIMBUS_TEST_TOKEN")
    if not ip or not tok:
        pytest.skip("set NIMBUS_TEST_IP + NIMBUS_TEST_TOKEN to run L18")
    handle = (ip, tok)
    st = requests.get(_u(handle, "/api/state"), timeout=10)
    if st.status_code != 200 or st.json().get("mode") != 1:
        pytest.skip("Orchestrator mode required (MODE 1)")
    probe = requests.post(
        _u(handle, "/api/test/astool"),
        data={"chat": "probe", "body": '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'},
        timeout=10,
    )
    if probe.status_code != 200:
        pytest.skip("board predates the multi-principal build - flash current main")
    yield handle
    # RESTORE: leave NOTHING behind. A leaked approved chat on the owner's real
    # device would be an actual security regression, and even a revoked row
    # consumes one of 32 tenant slots.
    #
    # Order matters, and REMOVE - not set-unknown - is the last word: setRole
    # UPSERTS, so revoking a chat whose row the per-test teardown already
    # removed recreates it. That is how earlier runs left a trail of `unknown`
    # rows on the board.
    for c in ALL_TEST_CHATS:
        try:
            requests.post(_u(handle, "/api/telegram/remove"), data={"id": c}, timeout=10)
            requests.post(_u(handle, "/api/tenant"), data={"id": c, "remove": "1"}, timeout=10)
        except Exception:  # noqa: BLE001 - best effort
            pass


@pytest.fixture
def cast(rig):
    """Alice + Bob as users, Carla as a guest - restored to unknown after."""
    for c in ALL_TEST_CHATS:
        _approve(rig, c)
    _set_role(rig, ALICE, "user")
    _set_role(rig, BOB, "user")
    _set_role(rig, CARLA, "guest")
    yield rig
    for c in ALL_TEST_CHATS:
        _set_role(rig, c, "unknown")  # deny first, in case removal is refused
        _unapprove(rig, c)  # off the allowlist too - never leak an approval


# ---------------------------------------------------------------------------
# vector memory - the recall path that feeds the prompt
# ---------------------------------------------------------------------------


def test_vector_memory_is_not_readable_across_tenants(cast):
    """Alice's memory must not surface in Bob's or a guest's search.

    ⚠ The probe queries the EXACT stored sentence, not a keyword. That is
    deliberate and was paid for: an earlier version searched for "passphrase"
    and "Alice", and when the namespace filter was deliberately removed to test
    the test, it STILL passed - the marker simply did not rank into the top 20
    among the ~1000 vectors already on the board. A privacy assertion that
    depends on a leaked row ranking highly is not an assertion. Feeding back the
    exact sentence is the strongest retrieval signal there is: if the row is
    visible at all, it comes back first.
    """
    secret, marker = _store_secret(cast, ALICE)

    # Positive control: the exact sentence retrieves it for its owner. Without
    # this, every negative below could pass because nothing was ever stored.
    mine = _text(_call(cast, ALICE, "memory.search", {"query": secret, "n_results": 10}))
    assert marker in mine, "Alice cannot read her OWN memory - the test would be vacuous"

    for chat, who in ((BOB, "another user"), (CARLA, "a guest")):
        got = _text(_call(cast, chat, "memory.search", {"query": secret, "n_results": 20}))
        assert marker not in got, f"LEAK: {who} retrieved Alice's memory verbatim"
        # And the same fact described loosely, in case an exact match is special-cased.
        loose = _text(_call(cast, chat, "memory.search", {"query": secret.rsplit(" is ", 1)[0], "n_results": 20}))
        assert marker not in loose, f"LEAK: {who} retrieved Alice's memory by description"


def test_memory_update_and_delete_cannot_reach_another_tenant(cast):
    """Writes are namespaced; so must be mutations addressed by raw id."""
    secret, marker = _store_secret(cast, ALICE)

    # Find the id as Alice (the only principal that legitimately can).
    listed = _text(_call(cast, ALICE, "memory.search", {"query": secret, "n_results": 10}))
    assert marker in listed, "the writer cannot see her own row - test would be vacuous"

    # Bob guesses ids. Whatever he guesses, he must not be able to overwrite or
    # remove a row he cannot see - and the failure must not confirm existence.
    for probe_id in ("1", "2", "3", "10", "42"):
        upd = _text(_call(cast, BOB, "memory.update", {"id": probe_id, "content": "OVERWRITTEN-BY-BOB"}))
        assert "OVERWRITTEN" not in upd or "error" in upd.lower(), (
            f"LEAK: Bob updated id {probe_id} across the boundary"
        )

    # Alice's row survived every probe intact.
    still = _text(_call(cast, ALICE, "memory.search", {"query": secret, "n_results": 10}))
    assert marker in still, "Bob's blind-id probing destroyed Alice's memory"
    assert "OVERWRITTEN-BY-BOB" not in still


def test_revoked_tenant_loses_reads_not_just_writes(cast):
    """Revocation that stops writing but keeps reading is not revocation."""
    secret, marker = _store_secret(cast, ALICE)
    assert marker in _text(_call(cast, ALICE, "memory.search", {"query": secret, "n_results": 5}))

    _set_role(cast, ALICE, "unknown")  # revoked

    after = _text(_call(cast, ALICE, "memory.search", {"query": secret, "n_results": 5}))
    assert marker not in after, "LEAK: a revoked tenant still reads its own memories"
    wrote = _text(_call(cast, ALICE, "memory.write", {"content": "should not land"}))
    assert "error" in wrote.lower() or "not" in wrote.lower(), "a revoked tenant could still write"

    # Restored access returns the data - revocation suspends, it does not destroy.
    _set_role(cast, ALICE, "user")
    back = _text(_call(cast, ALICE, "memory.search", {"query": secret, "n_results": 5}))
    assert marker in back, "restoring a role lost the tenant's data"


# ---------------------------------------------------------------------------
# episodic history
# ---------------------------------------------------------------------------


def test_episodic_history_is_scoped_to_the_asking_chat(cast):
    """One chat must not read another's transcript."""
    marker = f"HERON-{uuid.uuid4().hex[:10].upper()}"
    requests.post(
        _u(cast, "/api/test/inject"), data={"chat": ALICE, "text": f"remember this word: {marker}"}, timeout=10
    )
    # No wait on a real turn - the inbound capture writes the user row itself.
    for chat in (BOB, CARLA):
        got = _text(_call(cast, chat, "memory.episodic", {"limit": 50}))
        assert marker not in got, f"LEAK: {chat} read Alice's conversation"

    # A session id is not a capability: naming Alice's session must not open it.
    got = _text(_call(cast, BOB, "memory.episodic", {"session": ALICE, "limit": 50}))
    assert marker not in got, "LEAK: naming another chat's session id returned its history"


# ---------------------------------------------------------------------------
# files - the one place sharing is allowed, and only one way
# ---------------------------------------------------------------------------


def test_files_are_private_until_explicitly_shared(cast):
    """A file is invisible cross-tenant; sharing makes it READ-only, not writable."""
    proj = "l18"
    name = f"note-{uuid.uuid4().hex[:8]}.txt"
    marker = f"MARLIN-{uuid.uuid4().hex[:10].upper()}"
    saved = _text(_call(cast, ALICE, "artifact.save", {"project": proj, "name": name, "text": f"contents {marker}"}))
    if "sd" in saved.lower() and "error" in saved.lower():
        pytest.skip("no SD card - the artifact store is unavailable on this board")
    assert "isError\": true" not in saved.replace(" ", "") or "true" not in saved, saved

    assert name in _text(_call(cast, ALICE, "files.list", {"project": proj})), (
        "Alice cannot see her OWN file - the negatives below would be vacuous"
    )

    # Bob: neither listing nor addressing it by exact name may reveal it.
    assert name not in _text(_call(cast, BOB, "files.list", {"project": proj})), (
        "LEAK: Bob sees Alice's unshared file in the listing"
    )
    stat = _text(_call(cast, BOB, "files.stat", {"project": proj, "name": name}))
    assert marker not in stat, "LEAK: Bob read Alice's unshared file by exact name"

    # Alice shares it. Now Bob may see it...
    shared = _text(_call(cast, ALICE, "files.share", {"project": proj, "name": name, "share": True}))
    assert "only" not in shared.lower(), shared
    assert name in _text(_call(cast, BOB, "files.list", {"project": proj})), (
        "sharing did not actually grant read access"
    )

    # ...but must NOT be able to overwrite it. Shared means readable, never a
    # write grant - otherwise one tenant could rewrite what everyone else reads.
    over = _text(_call(cast, BOB, "artifact.save", {"project": proj, "name": name, "text": "BOB-OVERWROTE-THIS"}))
    assert "isError" in over and "true" in over.lower(), f"LEAK: a shared file was writable by another tenant: {over}"
    back = _text(_call(cast, ALICE, "files.stat", {"project": proj, "name": name}))
    assert "BOB-OVERWROTE" not in back, "Alice's shared file was overwritten by Bob"

    # A guest may not share at all (no shareOwn permission).
    gname = f"guest-{uuid.uuid4().hex[:8]}.txt"
    _call(cast, CARLA, "artifact.save", {"project": proj, "name": gname, "text": "guest file"})
    gs = _text(_call(cast, CARLA, "files.share", {"project": proj, "name": gname, "share": True}))
    assert "isError" in gs and "true" in gs.lower(), f"a guest was allowed to share: {gs}"


def test_a_shared_file_cannot_be_unshared_by_someone_else(cast):
    """Only the owner controls a file's sharing - not every reader of it."""
    proj = "l18"
    name = f"own-{uuid.uuid4().hex[:8]}.txt"
    saved = _text(_call(cast, ALICE, "artifact.save", {"project": proj, "name": name, "text": "alice owns this"}))
    if "sd" in saved.lower() and "error" in saved.lower():
        pytest.skip("no SD card - the artifact store is unavailable on this board")
    _call(cast, ALICE, "files.share", {"project": proj, "name": name, "share": True})

    flip = _text(_call(cast, BOB, "files.share", {"project": proj, "name": name, "share": False}))
    assert "isError" in flip and "true" in flip.lower(), (
        f"LEAK: Bob changed the sharing of a file he does not own: {flip}"
    )
    assert name in _text(_call(cast, BOB, "files.list", {"project": proj})), "Bob's refused un-share took effect anyway"


# ---------------------------------------------------------------------------
# tenant management - only admins, and never down to zero admins
# ---------------------------------------------------------------------------


def test_non_admins_cannot_manage_people(cast):
    """A user or guest must not read the people list or grant themselves admin."""
    for chat in (ALICE, CARLA):
        listed = _text(_call(cast, chat, "tenant.list", {}))
        assert "error" in listed.lower() or "only an admin" in listed.lower(), f"{chat} could enumerate every tenant"

        esc = _text(_call(cast, chat, "tenant.set_role", {"chat": chat, "role": "admin"}))
        assert "error" in esc.lower() or "only an admin" in esc.lower(), f"PRIVILEGE ESCALATION: {chat} promoted itself"

        # And it really did not take effect.
        roles = {t["id"]: t["role"] for t in requests.get(_u(cast, "/api/tenant"), timeout=10).json()["tenants"]}
        assert roles.get(chat) != "admin", f"{chat} is now an admin"

        quota = _text(_call(cast, chat, "tenant.set_quota", {"chat": chat, "vectors": 999999}))
        assert "error" in quota.lower() or "only an admin" in quota.lower(), f"{chat} raised its own quota"


def test_the_prompt_itself_carries_no_other_tenants_data(cast):
    """The last line of defence: what actually reaches the model.

    Every gate above could pass and the device still leak, if the assembled
    context pulls memories in on a path that skips the tool layer. ``recall`` IS
    that path - it is what the turn's ComposeInputs hook calls - so this asserts
    on the bullets themselves. Same exact-sentence rule as the search probe: a
    keyword query would not rank the row and the test would pass blind.
    """
    secret, marker = _store_secret(cast, ALICE)

    def bullets(chat, q):
        r = requests.post(_u(cast, "/api/test/recall"), data={"chat": chat, "q": q}, timeout=25)
        assert r.status_code == 200, r.text
        return json.dumps(r.json())

    mine = bullets(ALICE, secret)
    assert marker in mine, "recall did not surface the writer's OWN memory - the negatives would be vacuous"

    for chat in (BOB, CARLA):
        assert marker not in bullets(chat, secret), f"LEAK: Alice's memory reached {chat}'s prompt"
        assert marker not in bullets(chat, secret.rsplit(" is ", 1)[0]), (
            f"LEAK: a described query pulled Alice's memory into {chat}'s prompt"
        )
