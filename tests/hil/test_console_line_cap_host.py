"""Host checks for the test console's line cap and the fail-fast command reader.

The storm validation on unit A (21 Oct) found the L42 burst legs building a TGINJECT
line of about 820 bytes against a console that kept 512 bytes per line: the rest was
dropped, the device answered "TGINJECT parse FAILED ok=0 trunc=1", device.cmd() took
that reply as the TGINJECT it was waiting for, and the leg waited 120 s for a turn
that could not come. These pin the class with no device:

* every TGINJECT line the HIL legs build fits the console's cap (the payloads stay as
  the incident had them; the cap holds them);
* the host's CONSOLE_LINE_CAP is the firmware's kLineCap, and that cap lives only in
  the test console, whose whole body compiles only into test builds (the debug
  builds' mini-console keeps its own 512 and the production image has no console);
* device.cmd() never sends a line the console cannot take, and a reply that says
  FAILED or trunc=1 raises at once instead of passing for success.

The real-bot media page of test_l55_tg_archive (real Telegram file ids, sent by the
bench bot) cannot be built here; device.cmd() refuses it up front if it ever outgrows
the cap.
"""

from __future__ import annotations

import json
import re
import time
from pathlib import Path

import pytest

import tg_archive_bench as tb
from device import CONSOLE_LINE_CAP, CommandFailed, Device, command_line_problem, reply_failure
from o2_soak_load import photo_page
from test_l23_stack_health import PHOTO_PROMPT
from test_l42_replay_full_nvs import BURST, BURST2, injected_page
from test_l56_tg_inject import PAGES

pytestmark = pytest.mark.host

ROOT = Path(__file__).resolve().parents[2]
CHAT = 6098532481  # a ten-digit chat id, as real ones are
FILE_ID = "AgACAgQAAxkBAAI" + "x" * 76  # Telegram file ids run to about 90 characters


def _compact(page: dict) -> str:
    return json.dumps(page, separators=(",", ":"))


def _hil_tginject_lines() -> dict:
    """Every TGINJECT line a HIL leg or bench script builds on the host, by name."""
    lines = {
        "l42 burst": "TGINJECT " + injected_page(str(CHAT), BURST),
        "l42 burst2": "TGINJECT " + injected_page(str(CHAT), BURST2),
        "l42 injected leg": "TGINJECT "
        + injected_page(str(CHAT), ("inject one", "inject two", "/help", "inject four")),
        "o2 soak photo": "TGINJECT " + _compact(photo_page(FILE_ID, CHAT, 900000001)),
        "archive text": "TGINJECT "
        + tb.page([tb.text_update(900000001, CHAT, "Reply with exactly the word: archived")]),
    }
    l23 = photo_page(FILE_ID, CHAT, 900000002)  # the L23 photo leg's page has this shape
    l23["result"][0]["message"]["caption"] = PHOTO_PROMPT
    lines["l23 photo"] = "TGINJECT " + _compact(l23)
    for shape, body in PAGES.items():
        lines[f"l56 {shape}"] = "TGINJECT " + body
    # test_l55_tg_archive local inject: a photo, a 3-photo album, a voice note, a document.
    uid = 900000090
    ups = [tb.local_photo_update(uid, CHAT, "/mem/inject/p.jpg", 300000)]
    ups += [tb.local_photo_update(uid + 1 + i, CHAT, f"/mem/inject/a{i}.jpg", 120000, group="555") for i in range(3)]
    ups.append(tb.local_file_update(uid + 4, CHAT, "voice", "/mem/inject/v.ogg", 9000, {"mime_type": "audio/ogg"}))
    ups.append(tb.local_file_update(uid + 5, CHAT, "document", "/mem/inject/d.txt", 1100, {"file_name": "d.txt"}))
    lines["l55 archive local inject"] = "TGINJECT " + tb.page(ups)
    return lines


@pytest.mark.parametrize("name", sorted(_hil_tginject_lines()))
def test_every_hil_tginject_line_fits_the_console(name):
    line = _hil_tginject_lines()[name]
    assert command_line_problem(line) is None, f"{name}: {len(line.encode())} B > {CONSOLE_LINE_CAP} B"


def test_the_incident_line_did_not_fit_the_old_cap():
    """Counter-test: the L42 burst page is the one the 512 B console cut."""
    assert len(_hil_tginject_lines()["l42 burst"].encode()) > 512


def _firmware_cap() -> int:
    src = (ROOT / "src" / "test_console.cpp").read_text()
    m = re.search(r"constexpr size_t kLineCap = (\d+);", src)
    assert m, "src/test_console.cpp no longer defines kLineCap"
    return int(m.group(1))


def test_host_cap_matches_the_firmware_cap():
    assert CONSOLE_LINE_CAP == _firmware_cap()


def test_the_raised_cap_cannot_reach_a_production_image():
    """kLineCap is defined only in the test console, whose body is compiled only with
    NIMBUS_TEST; the debug builds' TURN/IP mini-console keeps its own 512."""
    hits = [p for p in (ROOT / "src").rglob("*.[ch]*") if "kLineCap" in p.read_text(errors="ignore")]
    assert [p.name for p in hits] == ["test_console.cpp"], hits
    src = (ROOT / "src" / "test_console.cpp").read_text()
    gate, cap = src.find("#if defined(NIMBUS_TEST)"), src.find("constexpr size_t kLineCap")
    assert 0 <= gate < cap < src.rfind("#endif  // NIMBUS_TEST"), "kLineCap is not inside the NIMBUS_TEST body"
    main = (ROOT / "src" / "main.cpp").read_text()
    assert "s_cmd.length() < 512" in main, "the debug mini-console's own cap moved"


# ---- device.cmd() fail-fast, on a fake console ------------------------------------------


class FakeConsole:
    """A serial stand-in: every line written is answered by ``respond(line)``."""

    def __init__(self, respond):
        self.respond = respond
        self.sent = []
        self.out = []

    def write(self, data: bytes) -> None:
        for line in data.decode("utf-8").splitlines():
            self.sent.append(line)
            self.out += [r.encode("utf-8") + b"\n" for r in self.respond(line)]

    def flush(self) -> None:
        pass

    def readline(self) -> bytes:
        if self.out:
            return self.out.pop(0)
        time.sleep(0.005)
        return b""


def _device(respond) -> Device:
    dev = Device(port="/dev/null-host-test")
    dev._ser = FakeConsole(respond)
    return dev


def _console_with_cap(cap: int):
    """The firmware console: keeps ``cap`` bytes of a line and answers TGINJECT."""

    def respond(line: str):
        kept = line.encode("utf-8")[:cap].decode("utf-8", "ignore")
        if not kept.startswith("TGINJECT "):
            return [f"{kept.split(' ', 1)[0]} ok"]
        if len(kept) < len(line):
            return ["TGINJECT parse FAILED ok=0 trunc=1"]
        return ["[agent] telegram: raw page staged", "TGINJECT queued parsed=4 text=4 media=0"]

    return respond


def test_a_cut_tginject_fails_at_once_not_after_the_turn_timeout():
    """The incident: a 512 B console cuts the burst page. cmd() raises on the reply
    instead of handing it back as the TGINJECT it waited for."""
    dev = _device(_console_with_cap(512))
    t0 = time.time()
    with pytest.raises(CommandFailed, match="trunc=1"):
        dev.cmd(_hil_tginject_lines()["l42 burst"], expect="TGINJECT", timeout=10.0)
    assert time.time() - t0 < 2.0


def test_the_same_page_on_the_test_console_cap_is_taken():
    dev = _device(_console_with_cap(CONSOLE_LINE_CAP))
    reply = dev.cmd(_hil_tginject_lines()["l42 burst"], expect="TGINJECT", timeout=5.0)
    assert reply.startswith("TGINJECT queued parsed=4")


def test_a_line_longer_than_the_cap_is_never_sent():
    dev = _device(_console_with_cap(CONSOLE_LINE_CAP))
    with pytest.raises(CommandFailed, match="the console takes"):
        dev.cmd("TGINJECT " + "x" * CONSOLE_LINE_CAP, expect="TGINJECT")
    with pytest.raises(CommandFailed, match="the console takes"):
        dev.cmd_re("TGINJECT " + "x" * CONSOLE_LINE_CAP, r"TGINJECT")
    assert dev._ser.sent == []


def test_cmd_re_fails_fast_too():
    dev = _device(lambda line: ["TGINJECT parse FAILED ok=0 trunc=0"])
    with pytest.raises(CommandFailed, match="parse FAILED"):
        dev.cmd_re("TGINJECT {}", r"TGINJECT (\S+)", timeout=5.0)


def test_a_leg_that_asks_for_the_failure_gets_it():
    """A negative-path leg names the failure in its expect: the reply is data."""
    dev = _device(lambda line: ["TGINJECT parse FAILED ok=0 trunc=1"])
    assert "trunc=1" in dev.cmd("TGINJECT {", expect="parse FAILED", timeout=5.0)
    assert reply_failure("trunc=1", "TGINJECT parse FAILED ok=0 trunc=1") is None


def test_ordinary_replies_pass_untouched():
    for reply in ("STATUS fw=v4.5.14 nvsfail=3", "FAULT cleared mask=0x00", "TGINJECT queued parsed=1 text=1 media=0"):
        assert reply_failure("STATUS", reply) is None
    assert reply_failure("TGINJECT", "TGINJECT parse FAILED ok=1 trunc=0")
    assert reply_failure("NVSFILL", "NVSFILL wrote=3 trunc=1")
