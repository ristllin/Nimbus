#!/usr/bin/env python3
"""Nimbus tone synth - the generated replacement sound pack (no game audio).

Every clip is SYNTHESIZED here from a per-event segment recipe: short sine/
triangle notes on a C-major pentatonic around C5, soft envelopes, no samples,
no downloads. Everything is SEEDED (mirror tools/logo/gen_logo.py's doctrine):
re-running on the SAME machine regenerates the same WAV bytes, so the committed
embedded header is a build product of this file - edit here, re-run, never
hand-edit outputs. (Byte-identity is same-platform only: the tone math goes
through libm transcendentals that aren't bit-specified across platforms, so a
different OS/CPU may differ by a quantization ULP. Regenerate dist/ and
sfx_basic_data.h from one designated environment; --selfcheck proves only
same-process determinism.)

The spec asked for `random.Random(hash((MASTER_SEED, slug, n)))`; Python string
hashing is per-process randomized (PYTHONHASHSEED), which would break byte
reproducibility, so the seed is a sha256 of the same tuple rendered as text.

Sound language (design-approved): pentatonic blips/arpeggios for lifecycle
events, a low quiet beating dyad for errors (calm, not alarming), sweeps for
mode/reply motion, clicks+ticks for SD, unresolved-vs-resolving note pairs for
voice listen/stop.

render(slug, n, rate, pool) -> list[int16] mono samples; write_wav() emits a
canonical 44-byte-header PCM s16le WAV via the stdlib wave module.
"""

from __future__ import annotations

import hashlib
import math
import random
import struct
import wave

MASTER_SEED = 11  # bump to re-roll every variant of the whole pack

# Peak target: -6 dBFS.
PEAK = 10 ** (-6 / 20)

# Note table (equal temperament, A4=440).
N = {
    "CS4": 277.18,
    "E4": 329.63,
    "G4": 392.00,
    "C5": 523.25,
    "D5": 587.33,
    "E5": 659.25,
    "G5": 783.99,
    "A5": 880.00,
    "C6": 1046.50,
}

# Segment keys: t start (s), dur (s), f Hz | note | (f1, f2) dyad, f2 sweep-end,
# wave "sine"|"tri", amp 0..1, harm2 adds a 2nd harmonic at -12 dB.
# A "click" is just a very short high triangle blip.


def _click(t, low=False):
    return {"t": t, "dur": 0.02, "f": 900.0 if low else 2400.0, "wave": "tri", "amp": 0.7}


RECIPES = {
    # boot: rising G4 -> C5 -> E5, warm, ~0.9 s
    "boot": [
        {"t": 0.00, "dur": 0.30, "f": "G4", "harm2": True},
        {"t": 0.28, "dur": 0.30, "f": "C5", "harm2": True},
        {"t": 0.56, "dur": 0.34, "f": "E5", "harm2": True},
    ],
    "wifi_up": [
        {"t": 0.00, "dur": 0.12, "f": "E5"},
        {"t": 0.17, "dur": 0.16, "f": "A5"},
    ],
    "wifi_down": [
        {"t": 0.00, "dur": 0.12, "f": "A5"},
        {"t": 0.17, "dur": 0.16, "f": "E5"},
    ],
    # soft blip + quieter echo
    "ble_up": [
        {"t": 0.00, "dur": 0.10, "f": "C6", "amp": 0.8},
        {"t": 0.22, "dur": 0.14, "f": "C6", "amp": 0.35},
    ],
    "ble_down": [
        {"t": 0.00, "dur": 0.10, "f": "C6", "amp": 0.8},
        {"t": 0.22, "dur": 0.14, "f": "G5", "amp": 0.35},
    ],
    # two blips converging to a unison dyad
    "ble_bond": [
        {"t": 0.00, "dur": 0.10, "f": "G5"},
        {"t": 0.13, "dur": 0.10, "f": "A5"},
        {"t": 0.28, "dur": 0.22, "f": (N["A5"], N["A5"] * 1.003), "harm2": True},
    ],
    "agent_spawn": [
        {"t": 0.00, "dur": 0.07, "f": "C5"},
        {"t": 0.08, "dur": 0.07, "f": "E5"},
        {"t": 0.16, "dur": 0.09, "f": "G5"},
    ],
    "agent_done": [
        {"t": 0.00, "dur": 0.15, "f": "G5", "harm2": True},
        {"t": 0.17, "dur": 0.28, "f": "C5", "harm2": True},
    ],
    # LOW soft beating dyad - calm signage, not an alarm
    "error": [{"t": 0.00, "dur": 0.50, "f": (220.0, 233.08), "amp": 0.5}],
    "needs_you": [
        {"t": 0.00, "dur": 0.14, "f": "A5", "amp": 0.7},
        {"t": 0.35, "dur": 0.20, "f": "A5", "amp": 0.7},
    ],
    "low_battery": [
        {"t": 0.00, "dur": 0.20, "f": "E4", "harm2": True},
        {"t": 0.22, "dur": 0.30, "f": "CS4", "harm2": True},
    ],
    "battery_ok": [
        {"t": 0.00, "dur": 0.15, "f": "C5"},
        {"t": 0.17, "dur": 0.25, "f": "E5"},
    ],
    "mode_switch": [{"t": 0.00, "dur": 0.50, "f": 400.0, "f2": 900.0}],
    "sd_mounted": [_click(0.0), {"t": 0.06, "dur": 0.10, "f": "C6", "amp": 0.8}],
    "sd_lost": [_click(0.0, low=True), {"t": 0.06, "dur": 0.14, "f": "E4", "amp": 0.8}],
    "turn_start": [{"t": 0.00, "dur": 0.12, "f": "E5"}],
    # short up-swish
    "reply_sent": [{"t": 0.00, "dur": 0.18, "f": 600.0, "f2": 1400.0, "amp": 0.8}],
    # two close rising notes, deliberately UNRESOLVED
    "voice_listen": [
        {"t": 0.00, "dur": 0.12, "f": "G5"},
        {"t": 0.16, "dur": 0.16, "f": "A5"},
    ],
    # falling, resolving mirror
    "voice_stop": [
        {"t": 0.00, "dur": 0.12, "f": "A5"},
        {"t": 0.16, "dur": 0.10, "f": "G5"},
        {"t": 0.28, "dur": 0.16, "f": "E5"},
    ],
    "mem_saved": [_click(0.0), {"t": 0.05, "dur": 0.13, "f": "C6", "amp": 0.7}],
    "net_degraded": [{"t": 0.00, "dur": 0.30, "f": N["E5"], "f2": N["E5"] * 0.944}],
    "net_ok": [{"t": 0.00, "dur": 0.30, "f": N["E5"] * 0.944, "f2": N["E5"]}],
    "ask_cleared": [{"t": 0.00, "dur": 0.22, "f": "C5", "amp": 0.6}],
    # 3-note falling-then-resolving chime
    "sync_done": [
        {"t": 0.00, "dur": 0.18, "f": "A5", "harm2": True},
        {"t": 0.22, "dur": 0.18, "f": "E5", "harm2": True},
        {"t": 0.44, "dur": 0.32, "f": "G5", "harm2": True},
    ],
}


def _seed(pool: str, slug: str, n: int) -> int:
    """Deterministic across processes (unlike hash() on strings)."""
    digest = hashlib.sha256(f"{MASTER_SEED}|{pool}|{slug}|{n}".encode()).digest()
    return int.from_bytes(digest[:8], "big")


def _freqs(f):
    if isinstance(f, str):
        return (N[f],)
    if isinstance(f, tuple):
        return f
    return (float(f),)


def _env(x: float, dur: float) -> float:
    """8 ms raised-cosine attack, exp decay (tau=dur/3), 30 ms cosine fade-out."""
    att = min(0.008, dur / 4)
    fade = min(0.030, dur / 3)
    e = math.exp(-x / (dur / 3.0))
    if x < att:
        e *= 0.5 * (1 - math.cos(math.pi * x / att))
    if x > dur - fade:
        e *= 0.5 * (1 + math.cos(math.pi * (x - (dur - fade)) / fade))
    return e


def _osc(phase: float, wav: str) -> float:
    if wav == "tri":
        frac = phase / (2 * math.pi) % 1.0
        return 4.0 * abs(frac - 0.5) - 1.0
    return math.sin(phase)


def _render_segment(seg, rate: int, out: list, detune: float, jitter: float):
    t0 = max(0.0, seg["t"] + jitter)
    dur = seg["dur"]
    amp = seg.get("amp", 1.0)
    wav = seg.get("wave", "sine")
    harm2 = seg.get("harm2", False)
    f2 = seg.get("f2")
    base = _freqs(seg["f"])
    start = int(t0 * rate)
    n_samp = int(dur * rate)
    if start + n_samp > len(out):
        out.extend([0.0] * (start + n_samp - len(out)))
    dt = 1.0 / rate
    phases = [0.0] * len(base)
    for i in range(n_samp):
        x = i * dt
        e = _env(x, dur) * amp / len(base)
        acc = 0.0
        for j, f in enumerate(base):
            fx = f * detune
            if f2 is not None:
                fx = (f + (f2 - f) * (x / dur)) * detune
            phases[j] += 2 * math.pi * fx * dt
            v = _osc(phases[j], wav)
            if harm2:
                v += 0.25 * _osc(2 * phases[j], wav)  # 2nd harmonic at -12 dB
            acc += v * e
        out[start + i] += acc


def render(slug: str, n: int, rate: int, pool: str = "general") -> list[int]:
    """Synthesize one variant -> int16 mono samples at `rate`."""
    segs = RECIPES[slug]
    rng = random.Random(_seed(pool, slug, n))
    out: list[float] = []
    for seg in segs:
        cents = rng.uniform(-6.0, 6.0)  # +/-6-cent detune per segment
        jitter = rng.uniform(-0.008, 0.008)  # +/-8 ms timing jitter
        _render_segment(seg, rate, out, 2.0 ** (cents / 1200.0), jitter)
    out.extend([0.0] * int(0.01 * rate))  # tiny silent tail
    peak = max((abs(v) for v in out), default=1.0)
    scale = (PEAK * 32767.0 / peak) if peak > 0 else 0.0
    return [max(-32768, min(32767, int(round(v * scale)))) for v in out]


def write_wav(path, samples: list[int], rate: int) -> None:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack(f"<{len(samples)}h", *samples))


if __name__ == "__main__":
    import sys

    slug = sys.argv[1] if len(sys.argv) > 1 else "boot"
    s = render(slug, 0, 22050)
    write_wav(f"/tmp/{slug}.wav", s, 22050)
    print(f"wrote /tmp/{slug}.wav ({len(s)} samples)")
