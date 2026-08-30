"""§L31 - contiguous internal-SRAM headroom guardrail (CUM-222).

Two internal-SRAM axes matter on this ESP32-S3: total free (heapMin, the "low SRAM"
floor) and the largest CONTIGUOUS block (intLargest, what a TLS handshake / AsyncTCP
accept needs). CUM-185 measured intLargest collapsing to ~7 KB on the Freenove, which
starved the web server. CUM-222 moved minimp3's ~15 KB per-frame decode scratch off the
audio task stacks into PSRAM (mp3dec_decode_frame_ex), dropping the sfx stack 20 KB ->
12 KB and making the 8 KB music task safe. Bench-measured on the Solide S3 (nimbus-6):
that returned ~9 KB to total free (heapMin 49 KB -> 58 KB); intLargest was already
healthy there (31.7 KB) and is layout-bound, so it did not move on that board.

Two layers of guard, both fail a battery instead of reaching the owner:

  * Deterministic SOURCE guards (no hardware) - the reduction and the invariant it
    depends on cannot be silently reverted.
  * A live largest-free-block floor under load (net-marked; skips LOUD if no board),
    plus the audio task high-water surfaced for CUM-222 in /api/state.

Pairs with tests/hil/test_l23 (task-stack floors) and test/test_mp3_scratch (host
proof the two decode paths are byte-identical).
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]

# The sfx stack after the scratch moved to PSRAM. A ceiling, not the exact value: a
# deliberate *raise* (if a real crash ever needs more) goes red and forces a conscious
# decision that also re-justifies giving back the CUM-222 internal-SRAM win.
SFX_STACK_CEILING = 12288
# Live largest-contiguous-internal floor under load. A board-agnostic catastrophic
# guard: above the ~7 KB CUM-185 Freenove starvation floor, below every healthy
# reading. Bench-measured on the Solide S3 (nimbus-6) at 31.7 KB steady and unchanged
# under load (TFT blit + web + a real turn); left at 12288 so the guard does not
# false-fail a lower-headroom board while still catching a return to starvation.
INTLARGEST_FLOOR = 12288
# Audio task worst-ever free stack danger floor (bytes). The sfx task runs a TLS tick
# + MP3 reply decode; under ~2 KB free is the "about to overflow" zone.
RUNTIME_MIN_FREE = 2048


# ---- deterministic source guards (no device) -------------------------------
def test_sfx_stack_stays_reduced():
    """src/sfx/sound_fx.cpp kSfxStackBytes must stay <= 12288. It was 20480 only to
    hold minimp3's ~15 KB on-stack scratch; that scratch now lives in PSRAM, so a
    revert to 20480 would give back the CUM-222 contiguous-SRAM headroom for nothing."""
    src = (_REPO / "src" / "sfx" / "sound_fx.cpp").read_text()
    m = re.search(r"kSfxStackBytes\s*=\s*(\d+)", src)
    assert m, "kSfxStackBytes not found in sound_fx.cpp"
    size = int(m.group(1))
    assert size <= SFX_STACK_CEILING, (
        f"sfx stack {size} > {SFX_STACK_CEILING}: minimp3's ~15 KB decode scratch is in "
        f"PSRAM now (mp3dec_decode_frame_ex), so the sfx task does not need the old 20 KB. "
        f"Raising this reclaims internal SRAM the owner asked us to protect (CUM-222)."
    )


def test_music_decode_routes_scratch_off_stack():
    """src/sfx/music.cpp MUST decode through mp3dec_decode_frame_ex (external PSRAM
    scratch). The music task stack is only 8 KB; the stock mp3dec_decode_frame puts a
    ~15 KB scratch on the stack and would overflow it (the latent bug CUM-222 fixed)."""
    src = (_REPO / "src" / "sfx" / "music.cpp").read_text()
    assert "mp3dec_decode_frame_ex(" in src, (
        "music.cpp no longer calls mp3dec_decode_frame_ex - the ~15 KB minimp3 scratch is "
        "back on the 8 KB music task stack and will overflow on a real MP3 track (CUM-222)."
    )
    # And the stock on-stack entry point must NOT be what the decode loop calls.
    assert re.search(r"\bmp3dec_decode_frame\s*\(", src) is None, (
        "music.cpp calls the on-stack mp3dec_decode_frame(); the decode loop must use the "
        "external-scratch mp3dec_decode_frame_ex() so the scratch stays in PSRAM."
    )


def test_minimp3_scratch_api_intact():
    """lib/minimp3/minimp3.h must keep the external-scratch API and the byte-identical
    wrapper (the stock mp3dec_decode_frame still uses an on-stack scratch, so any other
    caller is unchanged). Guards against a vendored-decoder re-drop that loses the seam."""
    hdr = (_REPO / "lib" / "minimp3" / "minimp3.h").read_text()
    assert "int mp3dec_scratch_size(void)" in hdr, "mp3dec_scratch_size() lost from minimp3.h"
    assert "mp3dec_decode_frame_ex(" in hdr, "mp3dec_decode_frame_ex() lost from minimp3.h"
    # The stock wrapper still declares the scratch on-stack (behavior preserved).
    assert re.search(r"mp3dec_scratch_t\s+scratch;", hdr), (
        "the on-stack scratch wrapper is gone from minimp3.h - mp3dec_decode_frame() must "
        "stay byte-identical for any non-Nimbus caller."
    )


# ---- live largest-free-block floor under load (net; skip+warn) -------------
@pytest.mark.net
def test_contiguous_sram_floor_under_load(device, net, secrets, require_secret):
    """Drive the web + inline-embed path, then confirm the largest contiguous internal
    block (mem.intLargest) still clears the floor. Skips LOUD if the board is
    unreachable - never a silent pass."""
    from test_l4_network import lan_ip_or_skip

    ip = lan_ip_or_skip(device, net, secrets, require_secret)

    # Deepen the internal-SRAM pressure: the inline embed runs on async_tcp, and a few
    # web reads keep the server working. Tolerate per-call errors (empty VDB, no
    # embedder) - the point is to load the box, then read the floor.
    for _ in range(4):
        try:
            net.get_json("/api/mem/vector", params={"query": "sram floor probe", "limit": "3"}, ip=ip, timeout=15.0)
        except Exception:
            pass
    try:
        net.get_json("/api/log", ip=ip, timeout=8.0)
    except Exception:
        pass

    st = net.get_json("/api/state", ip=ip, timeout=8.0)
    mem = st.get("mem", {})
    if "intLargest" not in mem:
        pytest.skip("device build does not surface mem.intLargest")
    largest = int(mem["intLargest"])
    assert largest >= INTLARGEST_FLOOR, (
        f"largest contiguous internal block {largest} B < {INTLARGEST_FLOOR} B floor under "
        f"load - the CUM-185 starvation is back. Find the new internal-SRAM holder; do NOT "
        f"lower this floor to make it pass (fix the pressure, not the guard)."
    )

    # CUM-222 audio task high-water: if the tasks have run, their worst free stack must
    # stay out of the danger zone (the reduced sfx stack must still be safe).
    for name in ("sfxStackMin", "musicStackMin"):
        if name in mem:
            free = int(mem[name])
            assert free > RUNTIME_MIN_FREE, (
                f"{name} worst free stack {free} B <= {RUNTIME_MIN_FREE} B danger floor - the "
                f"audio stack sizing (CUM-222) is too tight on this board."
            )
