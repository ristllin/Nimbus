"""Static internal RAM host guard: the long-lived buffers moved to PSRAM stay there.

v4.5.14 started with about 4.5 KB more static internal RAM than v4.5.13, which came
straight off the internal heap at every point of a turn. These buffers were moved
to PSRAM with ``nimbus::psramStatic`` (src/sys/psram_static.h) to win it back: each
is declared as a reference to PSRAM storage, so only 4 bytes stay in internal .bss.
Objects that need their constructor (the job and turn engines) move the same way with
``nimbus::psramObject``.
Moving one back inline (``static char g_memEcho[1201];``) silently costs that RAM
again, so the source leg fails on it. The ELF leg checks the same thing in a built
image when one is present. No board needed.
"""

from __future__ import annotations

import re
import subprocess
from glob import glob
from pathlib import Path

import pytest

pytestmark = pytest.mark.host

ROOT = Path(__file__).resolve().parents[2]

# file -> the buffers it keeps in PSRAM. A new buffer moved with psramStatic belongs here.
MOVED = {
    "src/agent/orchestrator.cpp": (
        "g_memEcho",
        "g_foldEcho",
        "s_sysTurnPrompt",
        "engine",
        "turnEngine",
        "journalStore",
    ),
    "src/agent/connectors.cpp": ("s_inRedirect", "s_inCode", "s_snapVerify", "s_snapErr", "s_snapAuthUrl"),
    "src/agent/telegram.cpp": ("g_media", "s_pending"),
    "src/sys/ota_update.cpp": ("g_manifest",),
    "src/sfx/sound_fx.cpp": ("g_reply", "g_jobs"),
}


def declared_in_psram(src: str, name: str) -> bool:
    """True iff `name` is declared as a reference bound to psramStatic or psramObject storage."""
    ref = re.compile(
        r"(?:\(\s*&\s*" + name + r"\s*\)\s*\[[^\]]*\]|&\s*" + name + r")\s*=\s*(?:::)?nimbus::psram(?:Static|Object)<"
    )
    return bool(ref.search(src))


def declared_inline(src: str, name: str) -> bool:
    """True iff `name` is (also) declared as an inline array or object in internal RAM."""
    decl = re.compile(
        r"^\s*(?:static\s+)?(?:const\s+)?[A-Za-z_][\w:]*\s+"  # the type
        r"(?:\w+\s*(?:\[[^\]]*\])?\s*,\s*)*"  # earlier names in the same declaration
        + name
        + r"\s*(?:\[[^\]]*\])?\s*[;,(]"  # "(": an object built with arguments
    )
    for line in src.splitlines():
        if decl.search(re.sub(r"=\s*\{[^}]*\}", "", line)):
            return True
    return False


@pytest.mark.parametrize("path,name", [(p, n) for p, names in MOVED.items() for n in names])
def test_buffer_stays_in_psram(path, name):
    src = (ROOT / path).read_text()
    assert declared_in_psram(src, name), f"{path}: {name} is no longer declared with nimbus::psramStatic"
    assert not declared_inline(src, name), f"{path}: {name} is back inline in internal RAM"


def test_helper_keeps_the_internal_fallback_and_zero_start():
    src = (ROOT / "src/sys/psram_static.h").read_text()
    assert "MALLOC_CAP_SPIRAM" in src and "MALLOC_CAP_INTERNAL" in src, "PSRAM first, internal RAM fallback"
    assert "heap_caps_calloc" in src, "a moved buffer must start zeroed, as a static does"


def test_the_guard_sees_the_shapes_it_bans():
    assert declared_inline("static char g_memEcho[1201] = {};", "g_memEcho")
    assert declared_inline("char s_inName[64] = {0}, s_inCode[1024] = {0};", "s_inCode")
    assert declared_inline("static PendingVoice g_voice = {};", "g_voice")
    assert declared_inline("static Pending s_pending[5];", "s_pending")
    assert not declared_inline("char (&s_inCode)[1024] = ::nimbus::psramStatic<char[1024]>();", "s_inCode")
    assert not declared_inline("  strncpy(g_voice.fileId, x, sizeof(g_voice.fileId) - 1);", "g_voice")
    assert not declared_inline("  for (int i = 0; i < n; i++) snap[i] = s_pending[i];", "s_pending")
    assert not declared_inline("static Pending (&s_pending)[5] = ::nimbus::psramStatic<Pending[5]>();", "s_pending")
    assert declared_in_psram(
        "static char (&g_memEcho)[N + 1] =\n    ::nimbus::psramStatic<char[N + 1]>();", "g_memEcho"
    )
    assert declared_in_psram("static PendingVoice& g_voice = ::nimbus::psramStatic<PendingVoice>();", "g_voice")
    assert not declared_in_psram("static char g_memEcho[1201] = {};", "g_memEcho")
    assert declared_inline("  static TurnEngine turnEngine(buildTurnDeps(),", "turnEngine")
    assert not declared_inline(
        "  static TurnEngine& turnEngine = ::nimbus::psramObject<TurnEngine>(deps,", "turnEngine"
    )
    assert not declared_inline("  g_engine = &turnEngine;", "turnEngine")
    assert declared_in_psram("  static JobEngine& engine = ::nimbus::psramObject<JobEngine>(buildJobDeps(),", "engine")


def _built_elfs():
    return [p for e in ("esp32s3", "esp32s3-cyd") for p in glob(str(ROOT / ".pio/build" / e / "firmware.elf"))]


@pytest.mark.skipif(not _built_elfs(), reason="no shipped firmware ELF built in this tree")
def test_built_image_holds_only_references():
    nm = glob(str(Path.home() / ".platformio/packages/toolchain-*/bin/xtensa-esp32s3-elf-nm"))
    if not nm:
        pytest.skip("no xtensa nm")
    names = {n for ns in MOVED.values() for n in ns}
    for elf in _built_elfs():
        out = subprocess.run([nm[0], "-S", "-C", elf], capture_output=True, text=True).stdout
        seen = {}
        for line in out.splitlines():
            parts = line.split()
            if len(parts) < 4 or parts[2] not in "bBdD":
                continue
            name = " ".join(parts[3:])  # demangled names carry "(anonymous namespace)"
            leaf = name.rsplit("::", 1)[-1]
            if leaf in names and name.startswith(
                (
                    "agent::orchestrator",
                    "agent::telegram",
                    "agent::connectors",
                    "otaupd::",
                    "sfx::(anonymous namespace)",
                )
            ):
                seen[leaf] = max(seen.get(leaf, 0), int(parts[1], 16))
        assert set(seen) == names, f"{elf}: missing {sorted(names - set(seen))}"
        big = {k: v for k, v in seen.items() if v > 8}
        assert not big, f"{elf}: back in internal RAM: {big}"
