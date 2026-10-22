"""Device-facing helpers of the storm suite: one ``StormBench`` per leg.

A leg drives the same storm on two images and grades both from what the owner and
the device log show:

* the **control image** (v4.5.13): it has none of the program's console verbs, so it
  is driven through the surfaces it has: the test inject route (``POST
  /api/test/inject``) delivers the same owner message again and again (on v4.5.13 a
  re-served page is processed again in full, so each delivery is one more turn), the
  LAN mock (``mock_llm.py``) answers as the head and carries every provider fault, and
  ``settings_fill.py`` fills the settings store through the web API. Every real
  provider key is cleared for the leg and restored from the bench ``.env`` after it,
  so the control run cannot spend;
* the **release candidate**: ``TGINJECT`` feeds the raw Telegram page through the real
  receive path, ``PROVFAULT`` answers for the head provider, ``NVSFILL`` fills the
  store past the reserve, ``FAULT sram_ballast`` pins internal memory down and
  ``PANIC ... arm=turn`` crashes inside a turn. The model-facing legs run the head on
  ``NIMBUS_STORM_MODEL`` (default ``claude-haiku-5-5``); every other provider key is
  cleared for the leg, so a refusal can never fail over to a provider with a zero
  budget.

The owner is a synthetic Telegram chat put first on the allowlist (restored after);
no bot token is on the unit, so nothing reaches Telegram: owner messages are read
from the chat's history (``/api/mem/episodic``) and the notices the device logs.
"""

from __future__ import annotations

import contextlib
import json
import os
import re
import time
from typing import Dict, Iterator, List, Optional, Tuple

import grading as g

MODEL = os.environ.get("NIMBUS_STORM_MODEL", "claude-haiku-5-5")
# Each leg's synthetic owner chat: its own hourly turn budget and its own history, so one
# leg's deliveries never count against the next leg's (the hourly ceiling is per chat).
OWNER_CHAT_BASE = int(os.environ.get("NIMBUS_STORM_CHAT") or 4747474700)


def owner_chat_for(leg: str) -> int:
    return OWNER_CHAT_BASE + (g.LEGS.index(leg) + 1 if leg in g.LEGS else 0)


# NIMBUS_STORM_TG=1: the leg runs with the dedicated test bot on the unit (locks/tg-bot
# held for the leg, the token cleared in a finally) and the test chat as its owner, so
# the device's Telegram record (/api/tg/archive, its outbound rows) is the owner channel
# the notices are counted on, and the notices only sent with a token (the storage-full
# notice) are in it.
TG_MODE = os.environ.get("NIMBUS_STORM_TG") == "1"


EVIDENCE_DIR = os.environ.get("NIMBUS_STORM_EVIDENCE") or os.path.join(
    os.environ.get("TMPDIR", "/tmp"), "nimbus-storm-evidence"
)
# Local bench state a leg must be able to put back by hand after an interruption.
STATE_DIR = os.path.join(os.environ.get("TMPDIR", "/tmp"), "nimbus-storm-state")
UNIT = os.environ.get("NIMBUS_STORM_UNIT", "")
ENV_FILE = os.environ.get("NIMBUS_ENV_FILE") or os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))), ".env"
)
# Provider key fields of /api/orch, and the bench .env name each is restored from.
KEY_FIELDS = {
    "anthropic": ("antKey", "ANTHROPIC_API_KEY"),
    "openai": ("oaiKey", "OPENAI_API_KEY"),
    "mistral": ("mistKey", "MISTRAL_API_KEY"),
    "zai": ("zaiKey", "Z_AI_TOKEN"),
}
CONTROL_BUILD_RE = re.compile(r"^v4\.5\.13\b")
BUILD_SHA_RE = re.compile(r"-g([0-9a-f]{7,40})")
CONTROL_COMMIT = "c1ab939e"
PACE_BYTES, PACE_GAP_S = 64, 0.01  # long console lines in small paced pieces (lane D, 22 Oct)
INJECT_DONE = "telegram: injected page:"


def read_env(path: str = ENV_FILE) -> Dict[str, str]:
    out: Dict[str, str] = {}
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                if "=" in line and not line.lstrip().startswith("#"):
                    k, _, v = line.strip().partition("=")
                    out[k.strip()] = v.strip().strip('"').strip("'")
    except OSError:
        pass
    return out


def commit_of(build: str) -> str:
    """The commit a build string names (``v4.5.16-26-geb50beb8`` -> ``eb50beb8``)."""
    if CONTROL_BUILD_RE.match(build or ""):
        return CONTROL_COMMIT
    m = BUILD_SHA_RE.search(build or "")
    return m.group(1) if m else ""


def text_page(update_id: int, chat: int, text: str, date: int) -> str:
    """One raw getUpdates page holding one owner text message. Telegram re-serves a page
    byte for byte, its date included, so a re-served copy must carry the first copy's
    date: the duplicate breaker keys on the message id, the date and the text."""
    msg = {
        "message_id": update_id % 1000000,
        "from": {"id": chat, "is_bot": False, "first_name": "Owner"},
        "chat": {"id": chat, "type": "private"},
        "date": date,
        "text": text,
    }
    return json.dumps({"ok": True, "result": [{"update_id": update_id, "message": msg}]}, separators=(",", ":"))


def drain_bot_queue(token: str) -> int:
    """Confirm the updates waiting in the test bot's queue (stale bench messages that a
    unit polling the bot would answer inside the leg's window). Only under locks/tg-bot:
    no other poller is running. Returns how many there were; never prints their content."""
    import requests

    url = f"https://api.telegram.org/bot{token}/getUpdates"
    try:
        ups = requests.get(url, params={"timeout": 0}, timeout=15).json().get("result", [])
        if ups:
            requests.get(url, params={"offset": ups[-1]["update_id"] + 1, "timeout": 0}, timeout=15)
        return len(ups)
    except Exception:  # noqa: BLE001 - the URL holds the token: report nothing of the error
        return -1


def port_mac(port: Optional[str]) -> str:
    """The MAC a serial port's USB serial number names (the ESP32-S3's own), or ''."""
    try:
        from serial.tools import list_ports

        return next((p.serial_number or "" for p in list_ports.comports() if p.device == port), "")
    except Exception:  # noqa: BLE001 - no pyserial listing: no targeted reset
        return ""


def first_update_id() -> int:
    env = os.environ.get("NIMBUS_TGINJECT_BASE")
    return int(env) if env else 910000000 + (int(time.time()) % 900000) * 100


class LogTap:
    """New device-log lines since the tap was armed, read over HTTP
    (``/api/errlog/recent``), in order, each once.

    Every durable-log line carries its boot's line number and uptime, both rising within
    a boot, so a line is taken only when it is newer than the last one taken. That keeps
    out the older lines a read can return when the log moves (a card that drops out puts
    the log on flash, whose tail holds earlier boots: unit A, 22 Oct) and a line cut at
    the start of a read. A restart the leg makes or expects (``restarted()``) starts the
    count again for the new boot."""

    def __init__(self, net, window: int = 16000):
        self.net = net
        self.window = window
        self.last = (-1, -1)  # (line number, uptime ms) of the newest line taken
        self.lines: List[str] = []
        self.skipped = 0  # older or cut lines a read returned
        self.new_boot: Optional[Tuple[float, int]] = None  # (host time, unit uptime ms) seen after a restart

    def _read(self) -> List[str]:
        try:
            r = self.net.get(f"/api/errlog/recent?bytes={self.window}", timeout=10.0)
            return r.text.splitlines() if r.status_code == 200 else []
        except Exception:  # noqa: BLE001 - busy or restarting: the next poll catches up
            return []

    @staticmethod
    def _key(line: str) -> Optional[Tuple[int, int]]:
        m = g.LOG_LINE_RE.match(line)
        return (int(m.group("seq")), int(m.group("ms"))) if m else None

    def arm(self) -> "LogTap":
        keys = [k for k in map(self._key, self._read()) if k]
        self.last = max(keys) if keys else (-1, -1)
        self.lines = []
        return self

    def restarted(self, uptime_ms: int = 0) -> None:
        """The unit restarted (seen at ``uptime_ms``): the next boot's lines start from
        line 0 again. A read right after a restart still holds the previous boot's tail
        first, so until a new-boot line is taken only the lines after the read's last boot
        boundary count, and none stamped later than the unit has been up."""
        self.last = (-1, -1)
        self.new_boot = (time.time(), uptime_ms)

    def _newest_boot(self, lines: List[str]) -> List[str]:
        """The lines after the read's last boot boundary (where the line number falls):
        a read that spans a restart, or a log that moved, holds earlier boots first."""
        keys = [self._key(ln) for ln in lines]
        start = 0
        for i in range(1, len(lines)):
            if keys[i] and keys[i - 1] and keys[i][0] < keys[i - 1][0]:
                start = i
        self.skipped += start
        return lines[start:]

    def take(self, lines: List[str]) -> List[str]:
        lines = self._newest_boot(lines)
        if self.new_boot is not None:  # right after a restart: nothing stamped later than the unit's uptime
            t0, up = self.new_boot
            cap = up + int((time.time() - t0) * 1000) + 5000
            lines = [ln for ln in lines if (self._key(ln) or (0, cap + 1))[1] <= cap]
        new = []
        for ln in lines:
            k = self._key(ln)
            if k is None or k[0] <= self.last[0] or k[1] < self.last[1]:
                self.skipped += 1
                continue
            self.last = k
            new.append(ln)
        if new:
            self.new_boot = None
        self.lines.extend(new)
        return new

    def poll(self) -> List[str]:
        return self.take(self._read())

    def parsed(self) -> List[g.LogLine]:
        return g.parse_log("\n".join(self.lines))


class StormBench:
    """One leg's bench: identity of the image, the owner chat, the head provider, the
    drives, the log tap and the evidence."""

    def __init__(self, device, net, mock=None, leg: str = ""):
        self.device = device
        self.leg = leg
        self.mac = port_mac(getattr(device, "port", None))
        self.env = read_env()
        self.tg = TG_MODE and bool(self.env.get("NIMBUS_TEST_TG_TOKEN") and self.env.get("NIMBUS_TEST_TG_CHAT"))
        self.chat = int(self.env["NIMBUS_TEST_TG_CHAT"]) if self.tg else owner_chat_for(leg)
        self.net = net
        self.mock = mock
        st = self.state()
        self.build = str(st.get("build") or st.get("fw") or "")
        self.commit = commit_of(self.build)
        self.role = g.CONTROL if CONTROL_BUILD_RE.match(self.build) else g.CANDIDATE
        self.unit = UNIT
        self.tap = LogTap(net)
        self.started = time.time()
        self.dates: Dict[int, int] = {}  # update id -> the date its first delivery carried
        self._out_texts: Dict[str, str] = {}  # archived outbound item -> its text
        self.guard0: Optional[Dict[str, int]] = None  # storm-guard counters when the leg began

    # ---- reads --------------------------------------------------------------------
    @property
    def control(self) -> bool:
        return self.role == g.CONTROL

    def state(self) -> dict:
        return self._retry(lambda: self.net.get_json("/api/state", timeout=8.0), "reading /api/state")

    def orch(self) -> dict:
        return self._retry(lambda: self.net.get_json("/api/orch", timeout=8.0), "reading /api/orch")

    def chat_rows(self, limit: int = 60) -> Dict[str, dict]:
        """The owner chat's messages by id (newest ``limit``)."""
        q = f"/api/mem/episodic?session={self.chat}&limit={limit}"
        page = self._retry(lambda: self.net.get_json(q, timeout=10.0), "reading the owner chat")
        return {str(r.get("id")): r for r in page.get("messages", []) if r.get("kind", "message") == "message"}

    @staticmethod
    def _retry(call, what: str, tries: int = 5):
        last = None
        for t in range(tries):
            try:
                return call()
            except Exception as e:  # noqa: BLE001 - a busy or restarting unit: try again
                last = e
                time.sleep(2.0 * (t + 1))
        raise AssertionError(f"{what} failed: {last}")

    def post_orch(self, fields: dict, ok=(200,)) -> int:
        r = self._retry(lambda: self.net.post("/api/orch", fields, timeout=10.0), f"POST /api/orch {sorted(fields)}")
        assert r.status_code in ok, f"POST /api/orch {sorted(fields)}: HTTP {r.status_code} {r.text[:120]}"
        return r.status_code

    # ---- console ------------------------------------------------------------------
    def verb(self, line: str, expect: str, timeout: float = 10.0) -> str:
        """A console verb's reply; a verb the image lacks answers with an error line."""
        return self.device.cmd(line, expect=expect, timeout=timeout)

    def has_verb(self, probe: str, expect: str, missing: str) -> bool:
        try:
            out = self.verb(probe, expect, timeout=6.0)
        except Exception:  # noqa: BLE001 - no answer at all: the image lacks it
            return False
        return missing not in out

    def cmd_paced(self, line: str, expect: str, timeout: float) -> str:
        self.device.drain(quiet=0.2)
        ser = self.device._require_open()
        data = (line + "\n").encode("utf-8")
        for i in range(0, len(data), PACE_BYTES):
            ser.write(data[i : i + PACE_BYTES])
            ser.flush()
            time.sleep(PACE_GAP_S)
        return self.device.expect(expect, timeout=timeout)

    # ---- the owner chat -------------------------------------------------------------
    @contextlib.contextmanager
    def owner_chat(self) -> Iterator[int]:
        """The synthetic owner chat first on the allowlist; the allowlist restored after.
        Refuses to run while a bot token is on the unit (it would poll the bot)."""
        o = self.orch()
        assert not o.get("hasTg"), (
            "a Telegram bot token is on the unit: clear it, or take locks/tg-bot and run the leg with the test bot"
        )
        before = str(o.get("tgAllow") or "")
        # Kept in a local state file, never printed and never beside the evidence (the
        # allowlist holds real chat ids), so an interrupted run can put it back by hand.
        os.makedirs(STATE_DIR, exist_ok=True)
        with open(os.path.join(STATE_DIR, f"allowlist-before-unit{self.unit or 'x'}.txt"), "w") as f:
            f.write(before + "\n")
        try:
            if self.tg:
                import bench_lock

                post = lambda f: self.net.post("/api/orch", f, timeout=10.0)  # noqa: E731
                token = self.env["NIMBUS_TEST_TG_TOKEN"]

                def drain():
                    n = drain_bot_queue(token)
                    print(f"[storm] bot mode: {n} stale update(s) in the test bot's queue confirmed before the leg")

                with bench_lock.bot_on_unit(post, token, str(self.chat), f"storm {self.leg}", before=drain):
                    time.sleep(5.0)  # the token swap lands on the poll task's next loop
                    yield self.chat
            else:
                self.post_orch({"tgAllow": str(self.chat)})
                yield self.chat
        finally:
            self.post_orch({"tgAllow": before}, ok=(200, 507))

    def outbound(self) -> Optional[List[dict]]:
        """Today's outbound Telegram rows for the owner chat (the device's archive), or
        None without a bot token (nothing is archived then)."""
        if not self.tg:
            return None
        rows, start = [], 0
        day = time.strftime("%Y-%m-%d", time.gmtime())
        while True:
            q = f"/api/tg/archive?chat={self.chat}&day={day}&limit=200&from={start}"
            page = self._retry(lambda: self.net.get_json(q, timeout=15.0), "reading the Telegram archive")
            rows += [r for r in page.get("rows", []) if r.get("dir") == "out"]
            if not page.get("more"):
                return [self._with_text(r, day) for r in rows]
            start = page["next"]

    def _with_text(self, row: dict, day: str) -> dict:
        """An outbound row with the text it carried (the row names its stored item)."""
        raw = row.get("raw")
        if not raw or "text" in row:
            return row
        if raw not in self._out_texts:
            q = f"/api/tg/archive/item?chat={self.chat}&day={day}&name={raw}&inline=1"
            try:
                item = self.net.get_json(q, timeout=10.0)
                self._out_texts[raw] = str(item.get("text") or item.get("caption") or "")
            except Exception:  # noqa: BLE001 - unreadable item: counted, its text unknown
                self._out_texts[raw] = ""
        return dict(row, text=self._out_texts[raw])

    @contextlib.contextmanager
    def session(self, keep: Optional[str], head) -> Iterator[None]:
        """Everything a leg runs inside, undone in reverse order however it ends: the
        owner chat, the one provider key it keeps, room under the spend ceiling, and its
        head (``head_for``)."""
        with self.owner_chat(), self.only_provider(keep), self.spend_room(), head:
            self.guard0 = guard_counts(self)
            yield

    # ---- the monthly spend ceiling ------------------------------------------------------
    SPEND_RE = re.compile(r'SPENDSET \{.*?"limitCents":(\d+),"spentCents":(\d+),"frozen":(true|false)')

    def spend_state(self) -> Optional[dict]:
        """The monthly spend ceiling (SPENDSET?), or None on an image without it."""
        try:
            out = self.verb("SPENDSET ?", "SPENDSET", timeout=6.0)
        except Exception:  # noqa: BLE001 - no answer: no ceiling seam
            return None
        m = self.SPEND_RE.search(out)
        if not m:
            return None
        return {"limitCents": int(m.group(1)), "spentCents": int(m.group(2)), "frozen": m.group(3) == "true"}

    @contextlib.contextmanager
    def spend_room(self, need_cents: int = 300) -> Iterator[Optional[dict]]:
        """Room under the monthly spend ceiling for the leg. A bench unit's month carries
        every earlier lane's bench spend; a ceiling already reached pauses every paid
        call and the leg would grade the pause, not its storm. When the room left is
        under ``need_cents`` this month's count is seeded to 0 for the leg and put back
        after it (SPENDSET, test builds). The ceiling itself stays: the legs prove the
        cap holds, they never lift it."""
        st = self.spend_state() if not self.control else None
        seeded = None
        try:
            if st and (st["frozen"] or st["limitCents"] - st["spentCents"] < need_cents):
                seeded = st["spentCents"]  # set first: a seed that raises after landing is still put back
                self.verb("SPENDSET 0", "SPENDSET", timeout=6.0)
                print(f"[storm] spend ceiling: {st}; this month's count seeded to 0 for the leg")
            yield st
        finally:
            if seeded is not None:
                # The month's count back, plus what the leg itself spent: the device keeps
                # counting the bench's real spend.
                now = self.spend_state() or {"spentCents": 0}
                back = seeded + now["spentCents"]
                self.verb(f"SPENDSET {back}", "SPENDSET", timeout=6.0)
                print(
                    f"[storm] spend ceiling: this month's count put back to {back} cents ({now['spentCents']} by the leg)"
                )

    # ---- provider isolation and the head -------------------------------------------------
    @contextlib.contextmanager
    def only_provider(self, keep: Optional[str]) -> Iterator[None]:
        """Every keyed provider but ``keep`` (None: all of them) has its key cleared for
        the leg, and pushed back from the bench .env after it, so no turn can fail over
        to a provider this leg has no budget for. The cloud router key is never touched:
        a unit with one refuses to run the leg."""
        provs = self.orch().get("providers") or {}
        assert not (provs.get("cumulo") or {}).get("hasKey"), (
            "the unit holds a cloud router key: a refused turn could fail over to it; run the leg on a unit without one"
        )
        cleared = [p for p, (fld, envname) in KEY_FIELDS.items() if p != keep and (provs.get(p) or {}).get("hasKey")]
        missing = [p for p in cleared if not self.env.get(KEY_FIELDS[p][1])]
        assert not missing, f"cannot restore {missing} afterwards: no key of that name in the bench .env"
        try:
            for p in cleared:  # inside the try: a clear that fails part way still restores all
                self.post_orch({"clr_" + KEY_FIELDS[p][0]: "1"})
            print(f"[storm] provider keys cleared for the leg: {cleared or 'none'}")
            yield
        finally:
            for p in cleared:
                self.post_orch({KEY_FIELDS[p][0]: self.env[KEY_FIELDS[p][1]]}, ok=(200, 507))
            print(f"[storm] provider keys restored: {cleared or 'none'}")

    @contextlib.contextmanager
    def head_model(self) -> Iterator[None]:
        """The head on Anthropic ``MODEL`` (the model-facing candidate legs)."""
        o = self.orch()
        prior = {"orchHost": o.get("orchHost") or "", "convReset": "1"}
        model = ((o.get("providers") or {}).get("anthropic") or {}).get("orchModel")
        if model:
            prior["orchM_anthropic"] = model
        self.post_orch({"orchHost": "anthropic", "orchM_anthropic": MODEL, "convReset": "1"})
        try:
            yield
        finally:
            self.post_orch(prior, ok=(200, 507))

    @contextlib.contextmanager
    def head_mock(self, scenario: str) -> Iterator[None]:
        """The head on the LAN mock (custom endpoint, model = the mock scenario). The
        custom head registers only at boot, so this restarts the unit before and after."""
        from mock_llm import find_lan_ip

        assert self.mock is not None, "the leg needs the mock_llm fixture"
        o = self.orch()
        if "cust" not in o:
            raise AssertionError("this image has no custom endpoint surface")
        prior_cust = dict(o.get("cust") or {})
        prior = {"orchHost": o.get("orchHost") or ""}
        host_ip = find_lan_ip(self.net.ip)
        assert host_ip, "no host LAN address the unit can reach"
        self.post_orch(
            {
                "custBase": self.mock.base_url(host_ip),
                "custConv": "openai",
                "custModel": scenario,
                "orchHost": "custom",
                "convReset": "1",
            }
        )
        self.restart()
        try:
            yield
        finally:
            self.post_orch(
                {
                    "custBase": prior_cust.get("base", ""),
                    "custConv": prior_cust.get("conv", "openai"),
                    "custModel": prior_cust.get("model", ""),
                    "orchHost": prior["orchHost"],
                    "convReset": "1",
                },
                ok=(200, 507),
            )
            self.restart()

    def set_mock_scenario(self, scenario: str) -> None:
        self.post_orch({"custModel": scenario})

    # ---- restarts ---------------------------------------------------------------------------
    def restart(self, timeout: float = 90.0) -> None:
        """Restart over HTTP and wait until the unit answers again with a fresh uptime;
        the console is reopened (the restart re-enumerates USB)."""
        r = self.net.post("/api/restart", {}, timeout=8.0)
        assert r.status_code in (200, 202), f"restart refused: HTTP {r.status_code}"
        self.wait_back(timeout)

    def wait_back(self, timeout: float = 120.0, fresh_ms: int = 120000) -> None:
        time.sleep(6.0)
        end = time.time() + timeout
        while time.time() < end:
            try:
                up = self.net.get_json("/api/state", timeout=4.0).get("uptimeMs", 10**9)
                if up < fresh_ms:
                    self.tap.restarted(up)
                    self.tap.poll()  # the boot's lines (a crash notice among them) before they scroll away
                    break
            except Exception:  # noqa: BLE001 - still booting
                pass
            time.sleep(2.0)
        else:
            raise AssertionError(f"the unit did not come back within {timeout:.0f} s")
        self.reopen_console()
        self.settle()

    def reopen_console(self, tries: int = 3) -> None:
        """Reattach the console after a restart re-enumerated USB. A reopen that finds a
        silent endpoint is followed by a USB bus reset of this unit only (by its MAC; the
        other unit is skipped), then retried; the console must answer PING. The control
        image is driven over HTTP alone, so there a console that stays away is noted and
        the leg goes on; on the candidate the leg stops here."""
        from device import Device

        last = None
        for _ in range(tries):
            try:
                self.device.reopen_after_reenumerate()
                if self.device.ping(timeout=4.0):
                    return
            except Exception as e:  # noqa: BLE001 - re-enumerating: try again
                last = e
            Device.bus_reset(target_serial=self.mac or None)
            time.sleep(5.0)
        if self.control:
            print(
                f"[storm] the console did not come back after the restart ({last}); the control leg goes on over HTTP"
            )
            return
        raise AssertionError(f"the console did not come back after the restart: {last}")

    def settle(self, uptime_ms: int = 30000, timeout: float = 90.0) -> None:
        end = time.time() + timeout
        while time.time() < end:
            try:
                if self.net.get_json("/api/state", timeout=6.0).get("uptimeMs", 0) >= uptime_ms:
                    return
            except Exception:  # noqa: BLE001 - busy
                pass
            time.sleep(3.0)

    # ---- inbound drives -------------------------------------------------------------------
    def inject(self, text: str, update_id: int, timeout: float = 300.0) -> List[str]:
        """Deliver one owner message: the raw page through TGINJECT on the candidate
        (returns the console lines until the page has run), the test inject route on the
        control image (returns [])."""
        if self.control:
            r = self._retry(
                lambda: self.net.post("/api/test/inject", {"chat": str(self.chat), "text": text}, timeout=8.0),
                "the test inject route",
            )
            assert r.status_code in (200, 202), f"inject refused: HTTP {r.status_code} {r.text[:80]}"
            return []
        page = text_page(update_id, self.chat, text, self.dates.setdefault(update_id, int(time.time())))
        line = ""
        for _ in range(60):
            line = self.cmd_paced("TGINJECT " + page, "TGINJECT", timeout=15.0)
            if "busy" not in line:
                break
            time.sleep(3.0)
        assert "queued" in line or "injected=1" in line, f"the page was not taken: {line}"
        if "queued" not in line:
            return [line]
        out, end = [], time.time() + timeout
        while True:
            got = self.device.expect_re(r".", timeout=max(1.0, end - time.time())).string
            out.append(got)
            if INJECT_DONE in got:
                return out

    def wait_turn_end(self, timeout: float = 120.0) -> bool:
        """Wait for the next 'turn end:' line in the device log."""
        end = time.time() + timeout
        while time.time() < end:
            if any(g.TURN_END_RE.search(ln) for ln in self.tap.poll()):
                return True
            time.sleep(2.0)
        return False

    def wait_quiet(self, quiet_s: float = 20.0, timeout: float = 600.0) -> None:
        """Wait until the device log has been quiet of turn activity for ``quiet_s``."""
        end = time.time() + timeout
        last = time.time()
        while time.time() < end:
            new = self.tap.poll()
            if any(("turn" in ln or "telegram: msg" in ln or "orchestrator:" in ln) for ln in new):
                last = time.time()
            if time.time() - last >= quiet_s:
                return
            time.sleep(3.0)

    def fresh_message(self, answer_re, text: str, update_id: int, timeout: float = 180.0) -> dict:
        """Send one fresh owner message after the storm and wait for the reply: returns
        {"usable", "usable_s", "reply"}. An honest refusal (any reply) counts as usable;
        silence does not."""
        before = self.chat_rows()
        sent_before = len(self.outbound() or [])
        t0 = time.time()
        self.inject(text, update_id, timeout=timeout)
        end = t0 + timeout
        while time.time() < end:
            rows = [r for r in g.new_rows(before, self.chat_rows()) if r.get("role") == "assistant"]
            # In bot mode a reply that went to Telegram counts too: a failed turn's honest
            # refusal is sent to the chat, never written to its history.
            rows += (self.outbound() or [])[sent_before:]
            if rows:
                return {"usable": True, "usable_s": time.time() - t0, "reply": rows[-1].get("text", "")}
            time.sleep(3.0)
        return {"usable": False, "usable_s": None, "reply": ""}

    # ---- the settings store -------------------------------------------------------------
    @contextlib.contextmanager
    def settings_full(self) -> Iterator[dict]:
        """The settings store full for the body of the block, freed after it. The
        candidate fills past the reserve with NVSFILL; the control image through the web
        API (settings_fill.py), restored from its snapshot."""
        if self.control:
            import settings_fill as sf

            api = sf.Api(self.net.ip, str(self.net.token()), log=print)
            snap = sf.take_snapshot(api)
            try:
                n = sf.fill(api, snap, target_free=0)  # records each filler in snap as it goes
                yield {"free": n.get("free"), "how": "web API fill"}
            finally:
                problems = sf.restore(api, snap)
                assert not problems, f"the settings store was not restored: {problems}"
            return
        out = ""
        try:
            for _ in range(40):
                out = self.verb("NVSFILL 400", "NVSFILL", timeout=20.0)
                if "full=1" in out:
                    break
            yield {"free": (self.state().get("nvs") or {}).get("free"), "how": "NVSFILL", "last": out}
        finally:
            self.verb("NVSFILL 0", "NVSFILL", timeout=20.0)

    # ---- evidence and the table ---------------------------------------------------------------
    def finish(self, leg: str, metrics: g.Metrics, bounds: g.Bounds) -> dict:
        """Write this run's evidence and print the leg's before/after table (the newest
        control record in the evidence directory is the 'before')."""
        if not metrics.blocked_by and at_ceiling_already(self.guard0, guard_counts(self) if self.guard0 else None):
            # No leg needs more than a dozen turns: a skip at the hourly ceiling means the
            # unit's hour was used up before the leg, not by its storm.
            metrics.blocked_by = CEILING_BLOCKED
        rec = g.evidence_record(
            g.RunId(leg, self.role, self.build, self.commit, self.unit, self.started, time.time()), metrics, bounds
        )
        path = g.write_evidence(EVIDENCE_DIR, rec)
        with open(path[: -len(".json")] + ".log", "w", encoding="utf-8") as f:
            f.write("\n".join(self.tap.lines) + "\n")  # the device log the leg was graded from
        records = g.load_evidence(EVIDENCE_DIR)
        control = g.metrics_of(g.latest(records, leg, g.CONTROL))
        candidate = g.metrics_of(g.latest(records, leg, g.CANDIDATE))
        print("\n" + g.table(leg, control, candidate, bounds))
        print(f"[storm] evidence: {path} grade={rec['grade']}")
        return rec


def assert_graded(rec: dict) -> None:
    """A candidate run fails the test when any property failed; a control run only
    records (its numbers are the 'before')."""
    if rec["role"] != g.CANDIDATE:
        return
    bad = {k: v for k, v in rec["verdicts"].items() if v["status"] != g.PASS}
    assert rec["grade"] == g.PASS, f"storm leg {rec['leg']} graded {rec['grade']}: {bad}"


# ---- the shared replay drive (legs a and b) -----------------------------------------------

ANSWER_RE = re.compile(r"pong|mock says hi|held reply", re.I)
REPLAY_NOTICE_RE = re.compile(r"storage|settings|duplicate|already|again|repeat|paused|could not be processed", re.I)
# Lines the drive itself causes on every delivery: not part of the device's behaviour.
DRIVE_LINES = ("injected page", "TGINJECT", "test inject")


def replay_bounds(copies: int) -> g.Bounds:
    """The replay legs' table: one paid turn, one turn's head requests, one notice. A
    re-served copy may leave one receipt line of its own (taken in, already in the
    history), so no line may repeat more often than the page was served: a loop
    multiplies lines beyond the deliveries."""
    return g.Bounds(
        max_paid_turns=1, max_provider_requests=8, max_identical=copies + 1, notices=(1, 1), usable_within_s=180.0
    )


def replay_storm(bench: StormBench, leg: str, copies: int, restart_at: Optional[int] = None) -> g.Metrics:
    """The same owner message delivered ``copies`` times (one Telegram page re-served),
    optionally with a restart before delivery ``restart_at``; returns the episode's
    metrics with the usable probe (a fresh message) graded after it."""
    uid = first_update_id()
    text = f"storm {leg} {uid}: reply with the single word pong"
    w = Window(bench).open()
    guard0 = guard_counts(bench)
    for k in range(copies):
        if restart_at is not None and k == restart_at:
            bench.restart()
        bench.inject(text, uid)
        if k == 0 and at_ceiling_already(guard0, guard_counts(bench)):
            return ceiling_blocked(guard0)
        if bench.control:
            # A re-served page comes back on the next poll, after the turn it caused:
            # one delivery per turn, never a burst the inbound batcher would merge.
            bench.wait_turn_end()
        bench.tap.poll()
    bench.wait_quiet(quiet_s=25.0, timeout=900.0)
    w.close()
    lines = w.lines
    replies = w.replies(REPLAY_NOTICE_RE)
    peak, peak_line = g.identical_peak(lines, ignore=DRIVE_LINES)
    m = g.Metrics(
        paid_turns=g.paid_turns(lines),
        provider_requests=w.requests(),
        parked_requests=0,
        notices=w.notices(REPLAY_NOTICE_RE),
        identical_peak=peak,
        identical_line=peak_line[:160],
    )
    m.extra.update(
        {
            "copies": copies,
            "restartAt": restart_at,
            "answers": len(replies["answers"]),
            "otherReplies": len(replies["other"]),
            "logNotices": g.log_notices(lines),
            "inboundLogged": len(g.inbound_texts(lines, bench.chat)),
            "telegramSent": [str(r.get("text") or "")[:120] for r in w.sent] if w.sent is not None else None,
        }
    )
    if not bench.control:
        m.extra["guard"] = guard_line(bench)
    probe = bench.fresh_message(ANSWER_RE, f"storm {leg} fresh {uid}: reply with the single word pong", uid + 7)
    m.usable, m.usable_s = probe["usable"], probe["usable_s"]
    m.extra["probeReply"] = probe["reply"][:200]
    return m


GUARD_RE = re.compile(r"TGGUARD dups=(\d+) ceiling=(\d+) poison=(\d+) tries=(\d+)")


def guard_counts(bench: StormBench) -> Optional[Dict[str, int]]:
    """The candidate's storm-guard counters (TGGUARD?); None on the control image."""
    if bench.control:
        return None
    m = GUARD_RE.search(guard_line(bench))
    return dict(zip(("dups", "ceiling", "poison", "tries"), map(int, m.groups()))) if m else None


def at_ceiling_already(before: Optional[Dict[str, int]], after: Optional[Dict[str, int]]) -> bool:
    """The leg's first, new owner message was skipped at the hourly ceiling: the device
    had used its hour before the leg began (earlier legs, other lanes), so the leg would
    grade that pause, not its storm."""
    return bool(before and after and after["ceiling"] > before["ceiling"])


CEILING_BLOCKED = "the unit's hourly turn ceiling was reached by earlier work; run the leg once its hour has room"


def ceiling_blocked(before: Dict[str, int]) -> g.Metrics:
    return g.Metrics(blocked_by=f"{CEILING_BLOCKED} (ceiling skips {before['ceiling']} before the leg)")


def guard_line(bench: StormBench) -> str:
    try:
        return bench.verb("TGGUARD?", "TGGUARD", timeout=6.0).strip()
    except Exception as e:  # noqa: BLE001 - informational
        return f"unreadable: {e}"


# ---- owner turns (legs c to h) ---------------------------------------------------------------


class Window:
    """What one stretch of a leg produced: device log lines, new owner-chat messages and
    LAN mock requests, from ``open()`` to ``close()``."""

    def __init__(self, bench: StormBench):
        self.bench = bench
        self.rows_before: Dict[str, dict] = {}
        self.mock_before = 0
        self.lines: List[g.LogLine] = []
        self.rows: List[dict] = []
        self.mock_requests = 0
        self.out_before = 0
        self.sent: Optional[List[dict]] = None  # outbound Telegram rows of the window (bot mode)

    def open(self) -> "Window":
        self.out_before = len(self.bench.outbound() or [])
        self.rows_before = self.bench.chat_rows()
        self.mock_before = len(self.bench.mock.completions()) if self.bench.mock else 0
        self.bench.tap.arm()
        return self

    def close(self) -> "Window":
        self.bench.tap.poll()
        self.lines = self.bench.tap.parsed()
        self.rows = g.new_rows(self.rows_before, self.bench.chat_rows())
        self.mock_requests = (len(self.bench.mock.completions()) - self.mock_before) if self.bench.mock else 0
        out = self.bench.outbound()
        self.sent = None if out is None else out[self.out_before :]
        return self

    def notices(self, notice_re) -> int:
        """Owner notices of the window, on the owner's channel: in bot mode the Telegram
        record (every outbound text with the leg's notice words: a failed turn's notice is
        sent to the chat and never written to its history), else the chat's history
        notices plus the notices the device logged as queued."""
        if self.sent is not None:
            return sum(1 for r in self.sent if notice_re.search(str(r.get("text") or "")))
        return len(self.replies(notice_re)["notices"]) + len(g.log_notices(self.lines))

    def replies(self, notice_re) -> dict:
        return g.classify_replies(self.rows, ANSWER_RE, notice_re)

    def requests(self, host: str = "api.anthropic.com") -> int:
        """Provider requests in the window: the mock's count on the control image, the
        PROVFAULT-answered and the real head exchanges on the candidate."""
        if self.bench.control:
            return self.mock_requests
        return g.override_requests(self.lines).get(host, 0) + g.billed_requests(self.lines)


def owner_turn(bench: StormBench, text: str, uid: int, timeout: float = 240.0) -> None:
    """One owner message, waited out: the candidate's TGINJECT returns once the page has
    run; on the control image the leg waits for the turn's end line."""
    bench.inject(text, uid, timeout=timeout)
    if bench.control:
        bench.wait_turn_end(timeout=timeout)
    bench.tap.poll()


def head_for(bench: StormBench, mock_scenario: str = "mock-echo"):
    """The leg's head: the LAN mock on the control image, Anthropic MODEL on the candidate."""
    return bench.head_mock(mock_scenario) if bench.control else bench.head_model()


def keep_for(bench: StormBench) -> Optional[str]:
    """The one provider key a leg keeps: none on the control image (it never spends)."""
    return None if bench.control else "anthropic"


def worst_episode(episodes: List[g.Metrics], bounds: g.Bounds) -> g.Metrics:
    """The episode a multi-episode leg is graded by: the first that fails, else the
    first that is blocked, else the last."""
    for status in (g.FAIL, g.BLOCKED):
        for e in episodes:
            if g.overall(g.grade(e, bounds)) == status:
                return e
    return episodes[-1]
