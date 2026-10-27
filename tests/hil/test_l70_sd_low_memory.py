"""L70 - the SD card keeps working when internal memory is short (storm leg e, no turn).

The storm suite's low-memory leg (22 and 23 Oct) saw the card drop out under the ballast:
``sdmmc_read_sectors: not enough mem, err=0x101``, then "SD lost mid-run - demoted to
no-card tier". Cause: FATFS keeps its buffers in PSRAM on this build, the S3 SDMMC host can
only DMA internal memory, so the IDF driver allocated a fresh 512 B internal DMA buffer for
every card transfer, and that allocation fails when internal memory is short. The fix
reserves one DMA buffer at boot and routes every such transfer through it
(src/sys/sd_dma_bounce.cpp, link-time wrappers; host tests in test/test_sd_bounce).

Source guards (no board): every full-firmware env carries the wrapper link flags, the
wrappers exist, and the buffer is reserved before the first card mount.

Bench leg (``hil``, an SDMMC unit with a card, test build, ``NIMBUS_TEST_IP``; no model
turn, nothing spent): write an 8 KB file (``FSPUT``, 16 sectors; every card transfer goes
through the reserved buffer whatever the memory level, so this is also the write-integrity
check), then hold internal memory with ``FAULT sram_ballast`` down to the 19 Oct storm
shape (about 13.5 KB free, largest block about 7 KB: the depth the card actually dropped
out at on the owner's device, and the depth the storm leg e ballasts to) and put the
network under load (LOAD_THREADS parallel ``GET /`` from the LAN, the Wi-Fi receive
buffers and TCP/IP allocations a turn's traffic makes). For LOAD_S seconds, alternate the
card probe (``SDCHECK``: create, write, read back, delete) and a read of the file
(``FSTAT``). Stop the load, release, read the file again, probe twice. The shape leaves
about 1 KB of DMA-capable memory, which a 512 B allocation only sometimes finds; the
network load takes the rest away from moment to moment, as the storm leg's turn did.

Pass: every probe answers ``probe=1 sdlost=0``, every read of the file is byte-identical
(under the load and after it), and the console shows no ``not enough mem`` from the card
driver and no demotion. On the image before the fix (17f0f67c, unit A, 23 Oct) the same
drive logged ``sdmmc_read_sectors: not enough mem`` (26 lines in one run, with a failed
read of the file, two failed probes and the card demoted). A restart during the leg is
recorded and fails it. The leg once ballasted to the console's 4 KB floor instead, and
that depth tips the device into the total-exhaustion aborts from outside the card path
(newlib's file lock, the Wi-Fi PHY timer; CUM-757, lane O3) - a different defect, below
the storm's shape; the ballast now targets the storm shape so this leg grades the card,
not the exhaustion abort. The console transcript of each run is saved beside its
evidence record.

Second bench leg (``hil``, the same unit, nothing spent): a card that stops answering loses
nothing and is told once. ``FAULT sd_io on`` makes every card write fail (the probe and the
history appends; the card itself is fine), two probes demote the tier, three history rows
are captured (``HISTADD``, a unique token) and held. Then, from the console log: exactly one
owner notice after the 20 s grace, a remount after a minute (internal memory is fine), and,
after ``FAULT sd_io off``, the promote and the three held rows written. A restart and a
search for the token (``EPIQ``) prove the rows are on the card.

Run: NIMBUS_TEST_IP=<unit ip> python3 -m pytest tests/hil/test_l70_sd_low_memory.py -m hil --allow-hardware -s
Evidence: NIMBUS_SD_EVIDENCE=<dir> writes l70-<image>-<utc>.json and .log there.
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import re
import threading
import time
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]

BALLAST_RE = re.compile(r"FAULT sram_ballast=(\d+) held=(\d+) intFree=(\d+) intLargest=(\d+) chunks=(\d+)")
HEAPINFO_DMA_RE = re.compile(r"HEAPINFO dma free=(\d+) alloc=\d+ largest=(\d+)")
HEAPINFO_INTERNAL_RE = re.compile(r"HEAPINFO internal free=(\d+) alloc=\d+ largest=(\d+)")
SDCHECK_RE = re.compile(r"SDCHECK probe=(\d) sdlost=(\d)")
FSTAT_RE = re.compile(r"FSTAT (\S+) bytes=(\d+) sha256=([0-9a-f]{64})")
CARD_FAIL_RE = re.compile(r"sdmmc_(read|write)_sectors: not enough mem|SD lost mid-run|SD lost -> degraded")
RESTART_RE = re.compile(r"abort\(\) was called|Guru Meditation|Rebooting\.\.\.|^rst:0x|^READY mode=")
PATH = "/sdlow_l70.bin"
PAYLOAD_BYTES = 8 * 1024
# The network load: enough to take the last DMA-capable bytes away from moment to moment
# (the image before the fix fails at once), short of exhausting internal memory outright.
LOAD_THREADS = int(os.environ.get("NIMBUS_L70_LOAD_THREADS", "3"))
LOAD_S = float(os.environ.get("NIMBUS_L70_LOAD_S", "30"))
# The 19 Oct storm shape the leg ballasts to: about 13.5 KB internal free (the storm leg's
# own ballast lands at 13,752 free / 7,156 largest). The verb keeps 4 KB as its floor, so a
# target below that still bottoms out at the floor.
BALLAST_FREE = int(os.environ.get("NIMBUS_L70_BALLAST_FREE", "13500"))


# ---- source guards (no board) ------------------------------------------------------


def _envs(ini: str) -> dict:
    out = {}
    for m in re.finditer(r"\n\[env:([\w-]+)\]\n(.*?)(?=\n\[|\Z)", "\n" + ini, re.DOTALL):
        body = "\n".join(ln for ln in m.group(2).splitlines() if not ln.lstrip().startswith(";"))
        out[m.group(1)] = body
    return out


def _carries_wrap(name: str, envs: dict, seen=()) -> bool:
    body = envs[name]
    if "${s3.sd_wrap_flags}" in body:
        return True
    parent = re.search(r"\$\{env:([\w-]+)\.build_flags\}", body)
    return bool(parent and parent.group(1) not in seen and _carries_wrap(parent.group(1), envs, seen + (name,)))


WRAPPED = ("sdmmc_read_sectors", "sdmmc_write_sectors")


def test_every_full_firmware_env_wraps_the_card_transfers():
    ini = (_REPO / "platformio.ini").read_text()
    flags = re.search(r"^sd_wrap_flags\s*=(.*)$", ini, re.M)
    assert flags, "[s3] sd_wrap_flags is gone"
    for sym in WRAPPED:
        assert f"-Wl,--wrap={sym}" in flags.group(1).split(), f"sd_wrap_flags no longer wraps {sym}"
    envs = _envs(ini)
    full = [n for n, b in envs.items() if re.search(r"build_src_filter\s*=\s*\+<\*>", b)]
    full += [
        n
        for n, b in envs.items()
        if n not in full
        and re.search(r"^extends\s*=\s*env:", b, re.M)
        and any(re.search(r"extends\s*=\s*env:" + re.escape(f) + r"\s*$", b, re.M) for f in full)
    ]
    missing = [n for n in full if not _carries_wrap(n, envs)]
    assert not missing, f"full-firmware envs without ${{s3.sd_wrap_flags}}: {missing}"
    # Counter-test: the parser saw the envs the release and the bench build.
    assert {"esp32s3", "esp32s3-cyd", "test", "test-cyd"} <= set(full)


def test_wrappers_defined_and_buffer_reserved_before_the_mount():
    glue = (_REPO / "src" / "sys" / "sd_dma_bounce.cpp").read_text()
    for sym in WRAPPED:
        assert re.search(r'extern "C" (?:\w+ )+__wrap_' + sym + r"\(", glue), f"__wrap_{sym} is not defined"
    main = (_REPO / "src" / "main.cpp").read_text()
    reserve = main.find("nimbus::sdbounce::reserve();")
    mounts = [m.start() for m in re.finditer(r"= solide::begin\(|if \(solide::storage::begin\(\)\)", main)]
    assert reserve >= 0 and len(mounts) == 2 and all(reserve < m for m in mounts), (
        "the card's DMA buffer must be reserved before the first card mount (solide::begin() mounts it)"
    )


# ---- bench leg -------------------------------------------------------------------------


def _read_until(dev, pattern: re.Pattern, timeout: float, sink: list):
    deadline = time.time() + timeout
    while True:
        line = dev._readline(deadline)
        if line is None:
            raise AssertionError(f"no {pattern.pattern!r} within {timeout:.0f} s; last lines: {sink[-8:]}")
        sink.append(line)
        m = pattern.search(line)
        if m:
            return m


def _cmd(dev, line: str, pattern: re.Pattern, sink: list, timeout: float = 10.0):
    while (got := dev._readline(time.time() + 0.2)) is not None:  # keep what drain would drop
        sink.append(got)
    dev.send(line)
    return _read_until(dev, pattern, timeout, sink)


def _fsput_once(dev, data: bytes, sink: list) -> "re.Match | None":
    _cmd(dev, f"FSPUT {len(data)} {PATH}", re.compile(r"FSPUT (READY|ERR)"), sink)
    if "ERR" in sink[-1]:
        raise AssertionError(f"FSPUT refused: {sink[-1]}")
    b64 = base64.b64encode(data).decode()
    ack_re = re.compile(r"FSACK (\d+)")
    acked = 0
    try:
        for i in range(0, len(b64), 256):
            dev.send(b64[i : i + 256])
            time.sleep(0.1)
            while min(len(data), (i + 256) // 4 * 3) - acked > 8192 and acked < len(data):
                acked = int(_read_until(dev, ack_re, 15.0, sink).group(1))
        return _read_until(dev, re.compile(r"FSPUT DONE bytes=(\d+) sha256=([0-9a-f]{64})"), 30.0, sink)
    except AssertionError:
        # Bytes the console dropped leave it short of the length: fill it up so it leaves
        # its data mode (the excess reads as one rejected console line), then let it settle.
        # The filler ends with a newline: when the console never ENTERED data mode (the
        # FSPUT line itself was dropped), the filler would otherwise swallow the retry's
        # command into one rejected line (bench, unit A, 23 Oct: "ERR unknown 'AAA...FSPUT'").
        dev.send("A" * (len(b64) + 64) + "\n")
        while dev._readline(time.time() + 2.0) is not None:
            pass
        return None


def _fsput(dev, data: bytes, sink: list) -> re.Match:
    """FSPUT in 256 B lines 100 ms apart (about 2.5 KB/s, so the console's 1.5 KB receive buffer
    rides out a main-loop pause of half a second; a burst it cannot drain in time is dropped:
    bench, unit A, 23 Oct, unpaced no ACK ever came, and at 5 to 10 KB/s two runs lost bytes in
    the last 4 KB). A transfer that stalls is completed with filler and tried once more."""
    for _ in range(2):
        done = _fsput_once(dev, data, sink)
        if done:
            return done
    raise AssertionError(f"FSPUT stalled twice; last lines: {sink[-6:]}")


def _cmd_patient(dev, line: str, pattern: re.Pattern, sink: list, tries: int = 3, timeout: float = 30.0):
    """A command sent right after the ballast release. The device is busy then re-establishing
    its cloud relay (the TLS handshake alone takes seconds) and the main loop pauses for its
    steady-memory snapshot, so the console can drop or starve a line the device is perfectly
    able to answer (bench, unit A, 23 Oct: the post-release FSTAT timed out at 30 s over live
    'relay: online=1' lines, on a card that had answered every probe). The read is idempotent,
    so a timed-out try is sent again instead of grading a healthy device a failure."""
    for _ in range(tries - 1):
        try:
            return _cmd(dev, line, pattern, sink, timeout=timeout)
        except AssertionError:
            pass
    return _cmd(dev, line, pattern, sink, timeout=timeout)


def _save_transcript(record: dict, sink: list) -> None:
    out = os.environ.get("NIMBUS_SD_EVIDENCE")
    if out:
        Path(out).mkdir(parents=True, exist_ok=True)
        (Path(out) / f"l70-{record['image']}-{record['started']}.log").write_text("\n".join(sink) + "\n")


def _evidence(record: dict) -> None:
    out = os.environ.get("NIMBUS_SD_EVIDENCE")
    if not out:
        return
    Path(out).mkdir(parents=True, exist_ok=True)
    (Path(out) / f"l70-{record['image']}-{record['started']}.json").write_text(json.dumps(record, indent=2))


@pytest.mark.hil
def test_card_keeps_working_under_the_ballast(device):
    sink: list = []
    record = {
        "leg": "l70_sd_low_memory",
        "image": os.environ.get("NIMBUS_SD_IMAGE", "unknown"),
        "started": time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()),
        "payloadBytes": PAYLOAD_BYTES,
        "probes": [],
        "verdict": "UNFINISHED",
    }
    try:
        _run(device, record, sink)
    finally:
        _save_transcript(record, sink)
        _evidence(record)
        print(f"\n[l70] {json.dumps(record, indent=2)}")
    assert record["verdict"] == "PASS", record.get("why", record)


class _LanLoad:
    """LOAD_THREADS threads fetching the device's page until stopped (answers or not)."""

    def __init__(self, ip: str):
        self.ip, self.stop, self.ok, self.err = ip, threading.Event(), 0, 0
        self.threads = [threading.Thread(target=self._loop, daemon=True) for _ in range(LOAD_THREADS)]

    def _loop(self) -> None:
        import requests  # noqa: PLC0415 - the bench's LAN client; collection runs without it

        s = requests.Session()
        while not self.stop.is_set():
            try:
                s.get(f"http://{self.ip}/", timeout=2)
                self.ok += 1
            except requests.RequestException:
                self.err += 1

    def __enter__(self):
        for t in self.threads:
            t.start()
        return self

    def __exit__(self, *exc):
        self.stop.set()
        for t in self.threads:
            t.join(timeout=5)


def _run(dev, record: dict, sink: list) -> None:
    ip = os.environ.get("NIMBUS_TEST_IP")
    if not ip:
        pytest.skip("NIMBUS_TEST_IP is not set (the leg loads the unit's network)")
    first = _cmd(dev, "SDCHECK", SDCHECK_RE, sink)
    if first.group(1) != "1":
        pytest.skip(f"no healthy card on this unit ({first.group(0)})")
    data = os.urandom(PAYLOAD_BYTES)
    record["want"] = hashlib.sha256(data).hexdigest()
    record["fsput"] = _fsput(dev, data, sink).group(0)
    record["fstatBefore"] = _cmd(dev, f"FSTAT {PATH}", FSTAT_RE, sink, timeout=30.0).group(0)
    _under_ballast(dev, record, sink, ip)
    record["fstatAfter"] = _cmd_patient(dev, f"FSTAT {PATH}", FSTAT_RE, sink).group(0)
    record["probesAfter"] = [_cmd_patient(dev, "SDCHECK", SDCHECK_RE, sink, timeout=20.0).group(0) for _ in range(2)]
    _grade(record, sink)


def _under_ballast(dev, record: dict, sink: list, ip: str) -> None:
    # Hold what leaves the storm shape free: the depth the card dropped out at on the
    # owner's device (the deeper, console-floor ballast trips the exhaustion aborts,
    # which are not this leg's defect).
    free = int(_cmd(dev, "HEAPINFO", HEAPINFO_INTERNAL_RE, sink).group(1))
    hold = max(0, free - BALLAST_FREE)
    ballast = _cmd(dev, f"FAULT sram_ballast {hold}", BALLAST_RE, sink)
    try:
        record["ballast"] = ballast.group(0)
        record["ballastFreeTarget"] = BALLAST_FREE
        dma = _cmd(dev, "HEAPINFO", HEAPINFO_DMA_RE, sink)
        record["dmaFreeUnder"], record["dmaLargestUnder"] = int(dma.group(1)), int(dma.group(2))
        with _LanLoad(ip) as load:
            _probe_and_read(dev, record, sink)
        record["lanLoad"] = {"answered": load.ok, "failed": load.err}
    finally:
        record["release"] = _cmd(dev, "FAULT sram_ballast 0", BALLAST_RE, sink).group(0)


def _probe_and_read(dev, record: dict, sink: list) -> None:
    read_re = re.compile(r"FSTAT (ERR \w+|\S+ bytes=\d+ sha256=\w+)")
    reads = record.setdefault("readsUnder", [])
    end = time.time() + LOAD_S
    while time.time() < end:
        record["probes"].append(_cmd(dev, "SDCHECK", SDCHECK_RE, sink, timeout=20.0).group(0))
        reads.append(_cmd(dev, f"FSTAT {PATH}", read_re, sink, timeout=30.0).group(1))


def _grade(record: dict, sink: list) -> None:
    good = f"bytes={PAYLOAD_BYTES} sha256={record['want']}"
    started = next(i for i, ln in enumerate(sink) if SDCHECK_RE.search(ln))
    failures = [ln for ln in sink if CARD_FAIL_RE.search(ln)]
    restarts = [ln for ln in sink[started:] if RESTART_RE.search(ln)]
    reads = record.get("readsUnder", [])
    bad_reads = [r for r in reads if not r.endswith(good)]
    bad_probes = [p for p in record["probes"] + record["probesAfter"] if p != "SDCHECK probe=1 sdlost=0"]
    record.update(restartLines=restarts[:5], cardFailLines=len(failures), cardFailSample=failures[:5])
    checks = [
        (record["fsput"].endswith(f"sha256={record['want']}"), "the file did not land on the card byte-identical"),
        (record["fstatBefore"].endswith(good), f"the first read differed: {record['fstatBefore']}"),
        (not bad_reads, f"{len(bad_reads)} of {len(reads)} reads under the load failed or differed: {bad_reads[:2]}"),
        (record["fstatAfter"].endswith(good), f"the read after the ballast differed: {record['fstatAfter']}"),
        (not bad_probes, f"the card probe failed {len(bad_probes)} time(s): {bad_probes[:3]}"),
        (not failures, f"the card driver ran out of memory or the card was demoted: {failures[:3]}"),
        (not restarts, f"the device restarted during the leg: {restarts[:3]}"),
        (len(record["probes"]) >= 5, f"only {len(record['probes'])} probes ran under the load"),
    ]
    why = [msg for ok, msg in checks if not ok]
    record["why"] = "; ".join(why)
    record["verdict"] = "FAIL" if why else "PASS"


# ---- second bench leg: an outage loses nothing and is told once -------------------------

NOTICE_LINE = "owner told the card is not answering"
REMOUNT_LINE = "lost card remounted"
FLUSH_RE = re.compile(r"(\d+) history row\(s\) held during the card outage written \(")


@pytest.mark.hil
def test_an_outage_loses_nothing_and_is_told_once(device):
    sink: list = []
    record = {
        "leg": "l70_outage",
        "image": os.environ.get("NIMBUS_SD_IMAGE", "unknown"),
        "started": time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()),
        "verdict": "UNFINISHED",
    }
    try:
        _run_outage(device, record, sink)
    finally:
        _cmd(device, "FAULT sd_io off", re.compile(r"FAULT sd_io=0"), sink)
        record["image"] += "-outage"
        _save_transcript(record, sink)
        _evidence(record)
        print(f"\n[l70] {json.dumps(record, indent=2)}")
    assert record["verdict"] == "PASS", record.get("why", record)


def _run_outage(dev, record: dict, sink: list) -> None:
    first = _cmd(dev, "SDCHECK", SDCHECK_RE, sink)
    if first.group(1) != "1":
        pytest.skip(f"no healthy card on this unit ({first.group(0)})")
    token = f"sdlow-held-{os.urandom(4).hex()}"
    record["token"] = token
    _cmd(dev, "FAULT sd_io on", re.compile(r"FAULT sd_io=1"), sink)
    record["probesOut"] = [_cmd(dev, "SDCHECK", SDCHECK_RE, sink).group(0) for _ in range(2)]
    lost_at = time.time()
    record["histadd"] = _cmd(
        dev, f"HISTADD hiltest-sdlow 3 {token}", re.compile(r"HISTADD (chat=.*|busy.*)"), sink
    ).group(0)
    _read_until(dev, re.compile(re.escape(NOTICE_LINE)), 60.0, sink)
    record["noticeAfterS"] = round(time.time() - lost_at, 1)
    record["remount"] = _read_until(dev, re.compile(re.escape(REMOUNT_LINE) + r".*"), 120.0, sink).group(0)
    record["remountAfterS"] = round(time.time() - lost_at, 1)
    _cmd(dev, "FAULT sd_io off", re.compile(r"FAULT sd_io=0"), sink)
    record["promote"] = _read_until(dev, re.compile(r"SD recovered.*"), 30.0, sink).group(0)
    # New rows move held ones themselves, so the bulk step may find little left: its line is
    # recorded when it comes; the decisive check is the rows on the card after the restart.
    try:
        record["flush"] = _read_until(dev, re.compile(FLUSH_RE.pattern + r"0 left"), 60.0, sink).group(0)
    except AssertionError:
        record["flush"] = "no bulk flush line (held rows moved by new rows)"
        time.sleep(15.0)
    record["notices"] = sum(NOTICE_LINE in ln for ln in sink)
    dev.reset()  # a restart (the console's own reset path: it reopens the port and waits for READY)
    time.sleep(5.0)
    found = _cmd(dev, f"EPIQ {token}", re.compile(r"EPIQ rows=(\d+)"), sink, timeout=20.0)
    record["epiqAfterRestart"] = found.group(0)
    why = []
    if record["probesOut"][-1] != "SDCHECK probe=0 sdlost=1":
        why.append(f"the fault did not demote the tier: {record['probesOut']}")
    if not record["histadd"].startswith("HISTADD chat="):
        why.append(f"the rows were not captured: {record['histadd']}")
    if record["notices"] != 1:
        why.append(f"{record['notices']} owner notices for one outage (want exactly 1)")
    if int(found.group(1)) < 3:
        why.append(f"the held rows are not on the card after a restart: {found.group(0)}")
    record["why"] = "; ".join(why)
    record["verdict"] = "FAIL" if why else "PASS"
