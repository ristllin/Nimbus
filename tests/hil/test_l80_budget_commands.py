"""L80 - the owner's spending commands on a real board (lane BUD, owner request 26 Oct 2026).

With every provider paused by a limit the device answered no message until the 1st
unless the owner reached the web page. Slash commands never call a model, so /usage,
/budget and /model keep working while everything is paused. This leg proves it on the
TEST build with the dedicated test bot (``locks/tg-bot``), owner messages fed through
the real receive path (``TGINJECT``) and the replies read from the device's Telegram
record (``/api/tg/archive``, outbound rows of the owner chat):

  1. a micro cap: Anthropic's monthly limit set to 1.00 USD and its count seeded over it
     (``SPENDSET prov anthropic``): Anthropic is paused;
  2. ``/usage`` names Anthropic over its limit and paused; ``/model`` names the owner's
     choice (Anthropic, ``claude-haiku-5-5``) and the pause;
  3. one owner message runs on Mistral (the failover); ``/model`` then names Mistral as the
     last reply and says it started on Anthropic;
  4. a member and a group chat are refused: neither can propose a change, and a member's
     ``/budget confirm`` does not apply the owner's waiting change; a group that is on
     the owner list is refused too (the device log says so);
  5. ``/budget anthropic 3`` proposes, ``/budget confirm`` applies: Anthropic's pause ends,
     the Usage page lists the change, the web chat holds its copy, and the next owner
     message runs on Anthropic; ``/model`` names Anthropic;
  6. a raise of more than 10x is refused with the force command named; a change followed
     by any other message is cancelled.

Everything changed is put back in a finally: the Anthropic budget entry, the Anthropic
and overall counts (plus what the leg spent), the head model, the provider keys, the
allowlist and the bot token. Left behind by design: the change record's row on the
Usage page (/data/budget_log.txt keeps the newest 8) and its copy in the web chat's
history. Real calls: two short owner turns (Anthropic ``claude-haiku-5-5``, and Mistral
on the unit's configured model), about a cent each.

Run (unit A or L under its device lock; the leg takes ``locks/tg-bot`` itself):
NIMBUS_STORM_TG=1 NIMBUS_BENCH_LOCKS=<program>/locks NIMBUS_STORM_UNIT=A \\
    NIMBUS_L80_EVIDENCE=<lane>/evidence/l80.json \\
    python3 -m pytest tests/hil/test_l80_budget_commands.py -m "hil or net" --allow-hardware -s

Markers: ``hil`` + ``net``. Needs --allow-hardware, NIMBUS_TEST_IP, NIMBUS_TEST_TOKEN, the
bench .env (provider keys, NIMBUS_TEST_TG_TOKEN and NIMBUS_TEST_TG_CHAT).
"""

from __future__ import annotations

import contextlib
import json
import os
import re
import time
from typing import Dict, Iterator, List

import pytest

pytestmark = [pytest.mark.hil, pytest.mark.net]

LEG = ""  # not a storm leg: the storm fixture writes no storm evidence for it
CAP_CENTS = 100  # the micro cap: 1.00 USD
MEMBER_CHAT = 4747474791
GROUP_CHAT = -1004747474792
GROUP_USER = 4747474793
REPLY_WAIT_S = 60.0
ALLOW_SETTLE_S = 5.0  # the poll task re-reads the allowlist on its next loop
EVIDENCE = os.environ.get("NIMBUS_L80_EVIDENCE", "")
KEEP = ("anthropic", "mistral")
TURN_END_RE = re.compile(r"turn end: (ok|FAIL) host=(\w+)")


# ---- the bench --------------------------------------------------------------------


def spend(storm) -> dict:
    """The spend store's state (SPENDSET ?: the same JSON as /api/orch usage.ceiling)."""
    line = storm.verb("SPENDSET ?", "SPENDSET {", timeout=8.0)
    return json.loads(line[line.index("{") :])


def prov_row(st: dict, p: str) -> dict:
    return next((r for r in st.get("providers", []) if r.get("p") == p), {"capCents": 0, "spentCents": 0})


def budget_entry(storm, p: str) -> str:
    """The provider's budget field as the web page posts it ("prov:tok:call:resetDay:cents")."""
    rows = ((storm.orch().get("usage") or {}).get("byProvider")) or []
    e = next((r for r in rows if r.get("prov") == p), {})
    return f"{p}:{int(e.get('tokenLimit') or 0)}:{int(e.get('callLimit') or 0)}:{int(e.get('resetDay') or 1)}:{int(e.get('centsLimit') or 0)}"


def with_cents(entry: str, cents: int) -> str:
    return ":".join(entry.split(":")[:4] + [str(cents)])


def inject_page(storm, page: str, timeout: float = 240.0) -> List[str]:
    """One raw getUpdates page through TGINJECT; the console lines until it has run."""
    line = ""
    for _ in range(60):
        line = storm.cmd_paced("TGINJECT " + page, "TGINJECT", timeout=15.0)
        if "busy" not in line:
            break
        time.sleep(3.0)
    assert "queued" in line or "injected=1" in line, f"the page was not taken: {line}"
    if "queued" not in line:
        return [line]
    out, end = [], time.time() + timeout
    while True:
        got = storm.device.expect_re(r".", timeout=max(1.0, end - time.time())).string
        out.append(got)
        if "telegram: injected page:" in got:
            return out


def group_page(update_id: int, text: str) -> str:
    msg = {
        "message_id": update_id % 1000000,
        "from": {"id": GROUP_USER, "is_bot": False, "first_name": "Guest"},
        "chat": {"id": GROUP_CHAT, "type": "supergroup", "title": "Bench group"},
        "date": int(time.time()),
        "text": text,
    }
    return json.dumps({"ok": True, "result": [{"update_id": update_id, "message": msg}]}, separators=(",", ":"))


class Owner:
    """The owner's Telegram chat: send a message, read the device's replies to it."""

    def __init__(self, storm, kit):
        self.storm, self.kit = storm, kit
        self.uid = kit.first_update_id()
        self.log: List[Dict[str, str]] = []

    def next_uid(self) -> int:
        self.uid += 1
        return self.uid

    def send(self, text: str) -> List[str]:
        return self.storm.inject(text, self.next_uid())

    def ask(self, text: str, want: str) -> str:
        """Send `text` and wait for the reply that contains `want`; returns it."""
        seen = len(self.storm.outbound() or [])
        lines = self.send(text)
        end = time.time() + REPLY_WAIT_S
        while time.time() < end:
            rows = (self.storm.outbound() or [])[seen:]
            for r in rows:
                if want in (r.get("text") or ""):
                    self.log.append({"sent": text, "reply": r["text"]})
                    print(f"[l80] {text!r} ->\n{r['text']}\n")
                    return r["text"]
            time.sleep(2.0)
        got = [r.get("text", "")[:200] for r in (self.storm.outbound() or [])[seen:]]
        raise AssertionError(
            f"{text!r}: no reply with {want!r} in {REPLY_WAIT_S:.0f} s; got {got}; console {lines[-3:]}"
        )

    def turn(self, text: str) -> str:
        """One owner message that runs a paid turn: the host it ended on."""
        lines = self.send(text)
        hosts = [m.group(2) for m in map(TURN_END_RE.search, lines) if m]
        assert hosts, f"no turn end line for {text!r}: {lines[-5:]}"
        print(f"[l80] turn {text!r} ended on {hosts[-1]}")
        return hosts[-1]


# ---- what the leg changes, put back however it ends ----------------------------------


@contextlib.contextmanager
def leg_keys(storm, kit) -> Iterator[None]:
    """Anthropic and Mistral keyed (from the bench .env when missing), every other provider
    key cleared; each put back as it was."""
    provs = storm.orch().get("providers") or {}
    assert not (provs.get("cumulo") or {}).get("hasKey"), "a cloud router key is on the unit: run on a unit without one"
    pushed, cleared = [], []
    try:
        for p, (fld, envname) in kit.KEY_FIELDS.items():
            has = bool((provs.get(p) or {}).get("hasKey"))
            if p in KEEP and not has:
                assert storm.env.get(envname), f"no {envname} in the bench .env"
                storm.post_orch({fld: storm.env[envname]})
                pushed.append(p)
            elif p not in KEEP and has:
                assert storm.env.get(envname), f"cannot restore {p} afterwards: no {envname} in the bench .env"
                cleared.append(p)
                storm.post_orch({"clr_" + fld: "1"})
        yield
    finally:
        for p in cleared:
            storm.post_orch({kit.KEY_FIELDS[p][0]: storm.env[kit.KEY_FIELDS[p][1]]}, ok=(200, 507))
        for p in pushed:
            storm.post_orch({"clr_" + kit.KEY_FIELDS[p][0]: "1"}, ok=(200, 507))
        print(f"[l80] keys put back (pushed {pushed}, cleared {cleared})")


@contextlib.contextmanager
def leg_models(storm, kit) -> Iterator[None]:
    """The head on Anthropic claude-haiku-5-5 (Mistral keeps its configured model)."""
    o = storm.orch()
    prior = {"orchHost": o.get("orchHost") or ""}
    m = ((o.get("providers") or {}).get("anthropic") or {}).get("orchModel")
    if m:
        prior["orchM_anthropic"] = m
    try:
        storm.post_orch({"orchHost": "anthropic", "orchM_anthropic": kit.MODEL, "convReset": "1"})
        yield
    finally:
        storm.post_orch(prior, ok=(200, 400, 507))


@contextlib.contextmanager
def leg_counts(storm) -> Iterator[dict]:
    """The Anthropic budget entry and the Anthropic and overall counts, put back after the
    leg plus what the leg itself spent. The overall count is seeded to 0 when the overall
    ceiling would pause every provider (a bench unit's month carries earlier lanes' spend)."""
    st = spend(storm)
    entry = budget_entry(storm, "anthropic")
    ant0 = int(prov_row(st, "anthropic")["spentCents"])
    all0 = int(st["spentCents"])
    seeded_all = bool(st.get("active", True)) and (st.get("frozen") or st["limitCents"] - all0 < 300)
    state = {"entry": entry, "ant0": ant0, "all0": all0, "antSeed": None}
    try:
        if seeded_all:
            storm.verb("SPENDSET 0", "SPENDSET", timeout=6.0)
        yield state
    finally:
        storm.post_orch({"budget": entry}, ok=(200, 507))
        now = spend(storm)
        ant_now = int(prov_row(now, "anthropic")["spentCents"])
        leg_ant = max(0, ant_now - (state["antSeed"] if state["antSeed"] is not None else ant0))
        storm.verb(f"SPENDSET prov anthropic {ant0 + leg_ant}", "SPENDSET", timeout=6.0)
        if seeded_all:
            storm.verb(f"SPENDSET {all0 + int(now['spentCents'])}", "SPENDSET", timeout=6.0)
        print(
            f"[l80] counts put back: anthropic {ant0}+{leg_ant} c, overall {'reseeded' if seeded_all else 'untouched'}"
        )


@contextlib.contextmanager
def leg_chats(storm) -> Iterator[None]:
    """The owner first on the allowlist, then a member and a group (members). The owner
    chat context restores the allowlist."""
    storm.post_orch({"tgAllow": f"{storm.chat},{MEMBER_CHAT},{GROUP_CHAT}"})
    time.sleep(ALLOW_SETTLE_S)
    yield


# ---- the legs -----------------------------------------------------------------------


def freeze_anthropic(storm, state: dict) -> None:
    storm.post_orch({"budget": with_cents(state["entry"], CAP_CENTS)})
    storm.verb(f"SPENDSET prov anthropic {CAP_CENTS + 1}", "SPENDSET", timeout=6.0)
    state["antSeed"] = CAP_CENTS + 1
    row = prov_row(spend(storm), "anthropic")
    assert row["capCents"] == CAP_CENTS and row["frozen"], f"Anthropic not paused by the micro cap: {row}"


def leg_paused_answers(owner: Owner, kit) -> None:
    u = owner.ask("/usage", "Spending this month")
    assert "Anthropic: $1.01 of its $1.00 limit" in u, u
    assert "Anthropic (monthly limit reached)" in u, u
    m = owner.ask("/model", "The assistant runs on")
    assert f"Anthropic, {kit.MODEL} (your choice)" in m, m
    assert "Paused now: Anthropic (monthly limit reached)" in m, m


def leg_failover(owner: Owner) -> None:
    host = owner.turn("l80 failover: reply with the single word pong")
    assert host == "mistral", f"the paused head's turn ended on {host}, not mistral"
    m = owner.ask("/model", "Last reply")
    assert "Last reply (" in m and "): Mistral," in m, m
    assert "It started on Anthropic and moved to Mistral." in m, m


def leg_strangers(storm, owner: Owner) -> bool:
    """A member and a group (members) cannot propose; a group on the owner list is refused
    by the spending commands' own rule; a member's confirm does not apply the owner's
    waiting change. The owner's proposal is made last, so its 2-minute window holds only
    the member's confirm. Returns whether the group-as-owner probe ran."""
    member = lambda text: inject_page(storm, kit_text_page(owner, MEMBER_CHAT, text))  # noqa: E731
    member("/budget anthropic 50")
    inject_page(storm, group_page(owner.next_uid(), "/budget@NimbusBenchBot anthropic 50"))
    probed = bool(group_owner_probe(storm, owner))
    assert prov_row(spend(storm), "anthropic")["capCents"] == CAP_CENTS, "a member or group changed the limit"
    owner.ask("/budget anthropic 3", "Send /budget confirm within 2 minutes")
    member("/budget confirm")
    assert prov_row(spend(storm), "anthropic")["capCents"] == CAP_CENTS, "a member's confirm applied the change"
    return probed


def kit_text_page(owner: Owner, chat: int, text: str) -> str:
    return owner.kit.text_page(owner.next_uid(), chat, text, int(time.time()))


def group_owner_probe(storm, owner: Owner) -> List[str]:
    """A group first on the allowlist is an owner by the default rule (no explicit owners
    list): /usage from it is refused by the spending commands' own rule."""
    storm.post_orch({"tgAllow": f"{GROUP_CHAT},{storm.chat},{MEMBER_CHAT}"})
    time.sleep(ALLOW_SETTLE_S)
    try:
        roles = {
            str(a.get("id")): a.get("owner") for a in storm.net.get_json("/api/telegram", timeout=8.0).get("allow", [])
        }
        if not roles.get(str(GROUP_CHAT)):
            print("[l80] group-as-owner probe skipped: the unit has an explicit owners list")
            return []
        lines = inject_page(storm, group_page(owner.next_uid(), "/usage"))
        assert any("budget: /usage refused from a group chat" in ln for ln in lines), lines[-6:]
        return lines
    finally:
        storm.post_orch({"tgAllow": f"{storm.chat},{MEMBER_CHAT},{GROUP_CHAT}"})
        time.sleep(ALLOW_SETTLE_S)


def leg_raise(storm, owner: Owner, kit) -> None:
    """The owner's confirm (still within the window of the proposal in leg_strangers)."""
    done = owner.ask("/budget confirm", "Done.")
    assert "The Anthropic monthly limit is now $3.00 (it was $1.00)." in done, done
    assert "Paid calls to Anthropic resume now." in done, done
    row = prov_row(spend(storm), "anthropic")
    assert row["capCents"] == 300 and not row["frozen"], f"Anthropic still paused after the raise: {row}"
    changes = ((storm.orch().get("usage") or {}).get("ceiling") or {}).get("changes") or []
    assert changes and changes[0]["src"] == "telegram", changes
    assert changes[0]["text"] == "Anthropic monthly limit changed from $1.00 to $3.00", changes
    web = storm.net.get_json("/api/mem/episodic?session=web&limit=20", timeout=10.0).get("messages", [])
    assert any(
        "Changed from Telegram: Anthropic monthly limit changed from $1.00 to $3.00." in (r.get("text") or "")
        for r in web
    ), "no web chat copy"
    host = owner.turn("l80 resumed: reply with the single word pong")
    assert host == "anthropic", f"the turn after the raise ended on {host}"
    m = owner.ask("/model", "Last reply")
    assert f"): Anthropic, {kit.MODEL}." in m, m


def leg_guards(storm, owner: Owner) -> None:
    f = owner.ask("/budget anthropic 40", "If you mean it")
    assert "send /budget anthropic 40 force." in f, f
    owner.ask("/budget anthropic 2", "Send /budget confirm within 2 minutes")
    owner.ask("/usage", "Spending this month")
    c = owner.ask("/budget confirm", "nothing changed")
    assert "cancelled by your next message" in c, c
    assert prov_row(spend(storm), "anthropic")["capCents"] == 300, "a cancelled change applied"


def test_l80_budget_commands(storm):
    import kit

    assert not storm.control, "a candidate-only leg (TGINJECT, SPENDSET)"
    assert storm.tg, "run in bot mode: NIMBUS_STORM_TG=1 with the test bot in the bench .env (locks/tg-bot)"
    owner = Owner(storm, kit)
    result = {"build": storm.build, "unit": storm.unit, "started": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    try:
        with (
            storm.owner_chat(),
            leg_keys(storm, kit),
            leg_models(storm, kit),
            leg_counts(storm) as state,
            leg_chats(storm),
        ):
            freeze_anthropic(storm, state)
            leg_paused_answers(owner, kit)
            leg_failover(owner)
            result["groupProbe"] = leg_strangers(storm, owner)
            leg_raise(storm, owner, kit)
            leg_guards(storm, owner)
        result["grade"] = "PASS"
    finally:
        result.setdefault("grade", "FAIL")
        result["exchanges"] = owner.log
        if EVIDENCE:
            with open(EVIDENCE, "w", encoding="utf-8") as f:
                json.dump(result, f, indent=1)
            print(f"[l80] evidence: {EVIDENCE} grade={result['grade']}")
