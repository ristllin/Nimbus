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
check), then hold internal memory with ``FAULT sram_ballast`` as far down as the console
allows (4 KB free, up to 16 blocks) and put the network under load (LOAD_THREADS parallel
``GET /`` from the LAN, the Wi-Fi receive buffers and TCP/IP allocations a turn's traffic
makes). For LOAD_S seconds, alternate the card probe (``SDCHECK``: create, write, read back,
delete) and a read of the file (``FSTAT``). Stop the load, release, read the file again,
probe twice. The ballast alone leaves about 1 KB of DMA-capable memory, which a 512 B
allocation still finds; the network load takes the rest away from moment to moment, as the
storm leg's turn did.

Pass: every probe answers ``probe=1 sdlost=0``, every read of the file is byte-identical
(under the load and after it), and the console shows no ``not enough mem`` from the card
driver and no demotion. On the image before the fix (17f0f67c, unit A, 23 Oct) the same
drive logged ``sdmmc_read_sectors: not enough mem`` (26 lines in one run, with a failed
read of the file, two failed probes and the card demoted). A restart during the leg is
recorded and fails it (a load that exhausts internal memory completely restarts any image
from outside the card path: newlib's file lock, the Wi-Fi PHY timer; LOAD_THREADS and
LOAD_S keep the load below that). The console transcript of each run is saved beside its
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
import requests

_REPO = Path(__file__).resolve().parents[2]

BALLAST_RE = re.compile(r"FAULT sram_ballast=(\d+) held=(\d+) intFree=(\d+) intLargest=(\d+) chunks=(\d+)")
HEAPINFO_DMA_RE = re.compile(r"HEAPINFO dma free=(\d+) alloc=\d+ largest=(\d+)")
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
BALLAST_ALL = 1_000_000  # more than the unit has: the ballast holds all it can above its 4 KB floor


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
        dev.send("A" * (len(b64) + 64))
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
    want = hashlib.sha256(data).hexdigest()
    put = _fsput(dev, data, sink)
    record["fsput"] = put.group(0)
    before = _cmd(dev, f"FSTAT {PATH}", FSTAT_RE, sink, timeout=30.0)
    record["fstatBefore"] = before.group(0)
    reads = record.setdefault("readsUnder", [])
    ballast = _cmd(dev, f"FAULT sram_ballast {BALLAST_ALL}", BALLAST_RE, sink)
    try:
        record["ballast"] = ballast.group(0)
        dma = _cmd(dev, "HEAPINFO", HEAPINFO_DMA_RE, sink)
        record["dmaFreeUnder"], record["dmaLargestUnder"] = int(dma.group(1)), int(dma.group(2))
        with _LanLoad(ip) as load:
            end = time.time() + LOAD_S
            while time.time() < end:
                record["probes"].append(_cmd(dev, "SDCHECK", SDCHECK_RE, sink, timeout=20.0).group(0))
                reads.append(
                    _cmd(
                        dev,
                        f"FSTAT {PATH}",
                        re.compile(r"FSTAT (ERR \w+|\S+ bytes=\d+ sha256=\w+)"),
                        sink,
                        timeout=30.0,
                    ).group(1)
                )
        record["lanLoad"] = {"answered": load.ok, "failed": load.err}
    finally:
        record["release"] = _cmd(dev, "FAULT sram_ballast 0", BALLAST_RE, sink).group(0)
    after = _cmd(dev, f"FSTAT {PATH}", FSTAT_RE, sink, timeout=30.0)
    record["fstatAfter"] = after.group(0)
    record["probesAfter"] = [_cmd(dev, "SDCHECK", SDCHECK_RE, sink).group(0) for _ in range(2)]
    failures = [ln for ln in sink if CARD_FAIL_RE.search(ln)]
    started = next(i for i, ln in enumerate(sink) if SDCHECK_RE.search(ln))
    restarts = [ln for ln in sink[started:] if RESTART_RE.search(ln)]
    record["restartLines"] = restarts[:5]
    record["cardFailLines"] = len(failures)
    record["cardFailSample"] = failures[:5]
    bad_probes = [p for p in record["probes"] + record["probesAfter"] if p != "SDCHECK probe=1 sdlost=0"]
    bad_reads = [r for r in reads if not r.endswith(f"bytes={PAYLOAD_BYTES} sha256={want}")]
    why = []
    if put.group(2) != want or before.group(3) != want:
        why.append("the file did not land on the card byte-identical")
    if bad_reads:
        why.append(f"{len(bad_reads)} of {len(reads)} reads under the load failed or differed: {bad_reads[:2]}")
    if after.group(3) != want or int(after.group(2)) != PAYLOAD_BYTES:
        why.append(f"the card read back different bytes after the ballast: {after.group(0)}")
    if bad_probes:
        why.append(f"the card probe failed {len(bad_probes)} time(s): {bad_probes[:3]}")
    if failures:
        why.append(f"the card driver ran out of memory or the card was demoted: {failures[:3]}")
    if restarts:
        why.append(f"the device restarted during the leg: {restarts[:3]}")
    if len(record["probes"]) < 5:
        why.append(f"only {len(record['probes'])} probes ran under the load (the leg measured too little)")
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
