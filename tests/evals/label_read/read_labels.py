#!/usr/bin/env python3
"""read_labels - PAID: one label read per photo, head and arm; results append to <evidence>/reads.jsonl.

  read_labels.py --head claude-sonnet-5-5 --arms P800,P1280,T800,T1280 --evidence <dir> --env-file <dotenv> --max-usd 3

Arms: P<px> sends the photo at a long edge of <px>. T<px> sends the same pixels plus an intake
transcription in the photo's label, as the device writes it ('(transcribed by <model>) <text>');
D<px> does the same with the transcription exactly as the device makes it today, cut at 900 output
tokens. Both transcriptions come from the device's caption default (claude-haiku-5-5, effort low,
the device's fallback caption prompt) on the 1,280 px rendition; T allows 4,000 output tokens so the
table is finished. Each is made once per photo and cached (--transcripts, --device-transcripts) so
every head sees the same text. Reruns skip stored reads. Keys come from the env file into this
process only and are never printed.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import io
import json
import os
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "behaviour"))
import providers  # noqa: E402  (PRICES, price, load_env from the RB-M0a eval)

TRANSCRIBER = "claude-haiku-5-5"
TRANSCRIBE_PX = 1280
# lib/harness/src/vision_caption.cpp kFallbackPrompt, verbatim.
CAPTION_PROMPT = (
    "You are reading a photo for an assistant that cannot see it. First copy every piece of "
    "text in the photo exactly as written (labels, numbers, units, prices, dates, names), "
    "one item per line. Then describe what the photo shows in one or two plain sentences. "
    "If the sender asked something, say what in the photo answers it. Do not guess at text "
    "you cannot read; say it is unreadable."
)
DEVICE_CAPTION_MAX_TOKENS = 900  # vision_caption.cpp outCap for the fallback kind (arm D)
FULL_CAPTION_MAX_TOKENS = 4000  # arm T: the same call with room to finish the table
MEDIA_TEXT_MAX = 2048  # lib/core/include/nimbus/orch/media_label.h kMediaTextMax
CUT_MARK = " [cut]"

SYSTEM = (
    "You keep a food log for the person you are talking with. When they send a nutrition label, "
    "you read its table. Report each figure exactly as printed on the label, as a bare number with "
    "any '<' sign kept and the unit left out (sodium in mg). Give one column per basis printed: "
    "per 100 g or 100 ml (basis_kind per100) and per serving or portion (basis_kind serving). A row "
    "the label does not print is null."
)
OWNER_TEXT = "What does this label say? Copy its nutrition table."

_FIG = {"anyOf": [{"type": "string"}, {"type": "null"}]}
FIELDS = (
    "energy_kj",
    "energy_kcal",
    "fat",
    "saturates",
    "carbohydrate",
    "sugars",
    "polyols",
    "fibre",
    "protein",
    "salt",
    "sodium_mg",
)
SCHEMA = {
    "type": "object",
    "additionalProperties": False,
    "required": ["columns"],
    "properties": {
        "columns": {
            "type": "array",
            "items": {
                "type": "object",
                "additionalProperties": False,
                "required": ["basis_kind", "basis_text", *FIELDS],
                "properties": {
                    "basis_kind": {"type": "string", "enum": ["per100", "serving"]},
                    "basis_text": {"type": "string"},
                    **{f: _FIG for f in FIELDS},
                },
            },
        }
    },
}


def rendition(path: Path, px: int) -> bytes:
    """The photo at a long edge of px (never upscaled), JPEG quality 85: what the head is sent."""
    from PIL import Image

    im = Image.open(path).convert("RGB")
    w, h = im.size
    s = min(1.0, px / max(w, h))
    if s < 1.0:
        im = im.resize((round(w * s), round(h * s)), Image.LANCZOS)
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=85)
    return buf.getvalue()


def ref8(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()[:8]


def capped(text: str) -> str:
    b = text.encode()
    if len(b) <= MEDIA_TEXT_MAX:
        return text
    return b[:MEDIA_TEXT_MAX].decode("utf-8", "ignore") + CUT_MARK


def photo_line(ref: str, transcript: str | None) -> str:
    """The engine-2 photo label (PRD section 5), with the device's transcribed suffix in arm T."""
    line = f"Photo 1 of 1 [p{ref}]"
    if transcript is None:
        return line + " (attached: the photo itself is in this message)"
    return line + f" (transcribed by {TRANSCRIBER}) " + capped(transcript)


def anthropic_client():
    import anthropic

    return anthropic.Anthropic(api_key=os.environ["ANTHROPIC_API_KEY"], max_retries=5, timeout=240)


def openai_client():
    import openai

    return openai.OpenAI(api_key=os.environ["OPENAI_API_KEY"], max_retries=5, timeout=240)


def usage_anthropic(u) -> dict:
    cached, written = u.cache_read_input_tokens or 0, u.cache_creation_input_tokens or 0
    return {
        "prompt": u.input_tokens + cached + written,
        "cached": cached,
        "written": written,
        "output": u.output_tokens,
    }


def transcribe(client, jpeg: bytes, max_tokens: int) -> tuple[str, dict, str]:
    r = client.messages.create(
        model=TRANSCRIBER,
        max_tokens=max_tokens,
        output_config={"effort": "low"},
        messages=[
            {
                "role": "user",
                "content": [
                    {"type": "image", "source": {"type": "base64", "media_type": "image/jpeg", "data": b64(jpeg)}},
                    {"type": "text", "text": CAPTION_PROMPT},
                ],
            }
        ],
    )
    text = "".join(b.text for b in r.content if b.type == "text").strip()
    return text, usage_anthropic(r.usage), r.stop_reason or ""


def b64(data: bytes) -> str:
    return base64.standard_b64encode(data).decode()


def read_anthropic(client, model: str, jpeg: bytes, text: str) -> tuple[dict | None, str, dict, str]:
    r = client.messages.create(
        model=model,
        max_tokens=16000,
        system=SYSTEM,
        output_config={"effort": "medium", "format": {"type": "json_schema", "schema": SCHEMA}},
        messages=[
            {
                "role": "user",
                "content": [
                    {"type": "image", "source": {"type": "base64", "media_type": "image/jpeg", "data": b64(jpeg)}},
                    {"type": "text", "text": text},
                ],
            }
        ],
    )
    raw = "".join(b.text for b in r.content if b.type == "text")
    return parse(raw), raw, usage_anthropic(r.usage), r.stop_reason or ""


def read_openai(client, model: str, jpeg: bytes, text: str) -> tuple[dict | None, str, dict, str]:
    r = client.responses.create(
        model=model,
        instructions=SYSTEM,
        store=False,
        input=[
            {
                "role": "user",
                "content": [
                    {"type": "input_image", "image_url": "data:image/jpeg;base64," + b64(jpeg), "detail": "high"},
                    {"type": "input_text", "text": text},
                ],
            }
        ],
        text={"format": {"type": "json_schema", "name": "label", "schema": SCHEMA, "strict": True}},
    )
    raw = r.output_text or ""
    u = r.usage
    cached = getattr(getattr(u, "input_tokens_details", None), "cached_tokens", 0) or 0
    usage = {"prompt": u.input_tokens, "cached": cached, "written": 0, "output": u.output_tokens}
    return parse(raw), raw, usage, getattr(r, "status", "") or ""


def parse(raw: str) -> dict | None:
    try:
        v = json.loads(raw)
        return v if isinstance(v, dict) else None
    except ValueError:
        return None


def load_jsonl(path: Path) -> list[dict]:
    if not path.exists():
        return []
    return [json.loads(x) for x in path.read_text().splitlines() if x.strip()]


def spent(evidence: Path, transcripts: list[Path]) -> float:
    """This head's reads plus every transcription in the shared caches."""
    return sum(r.get("usd", 0.0) for r in load_jsonl(evidence / "reads.jsonl")) + sum(
        r.get("usd", 0.0) for p in transcripts for r in load_jsonl(p)
    )


def transcript_for(ac, lab: dict, path: Path, cache: dict, max_tokens: int) -> str:
    """The cached intake transcription of a label, made once and appended to the shared file."""
    if lab["id"] not in cache:
        t0 = time.time()
        text, usage, stop = transcribe(ac, rendition(HERE / "photos" / lab["file"], TRANSCRIBE_PX), max_tokens)
        rec = {
            "id": lab["id"],
            "model": TRANSCRIBER,
            "max_tokens": max_tokens,
            "px": TRANSCRIBE_PX,
            "text": text,
            "stop": stop,
            "usage": usage,
            "usd": providers.price(TRANSCRIBER, usage),
            "seconds": round(time.time() - t0, 2),
        }
        with open(path, "a") as f:
            f.write(json.dumps(rec) + "\n")
        cache[lab["id"]] = rec
    return cache[lab["id"]]["text"]


def arm_px(arm: str) -> tuple[str, int]:
    """('P' pixels only | 'T' plus the full transcription | 'D' plus the device-capped one, long edge)."""
    if arm[:1] not in ("P", "T", "D") or not arm[1:].isdigit():
        raise SystemExit(f"bad arm {arm!r}: P<px>, T<px> or D<px>")
    return arm[0], int(arm[1:])


def parse_args():
    ap = argparse.ArgumentParser()
    ap.add_argument("--head", required=True)
    ap.add_argument("--arms", default="P800,P1280,T800,T1280")
    ap.add_argument("--evidence", required=True)
    ap.add_argument("--env-file", required=True)
    ap.add_argument(
        "--max-usd", type=float, required=True, help="stop when the evidence dir's total spend reaches this"
    )
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--manifest", default=str(HERE / "labels.json"))
    ap.add_argument("--transcripts", help="shared full transcription cache (default <evidence>/transcripts.jsonl)")
    ap.add_argument(
        "--device-transcripts", help="shared device-capped cache (default <evidence>/transcripts-device900.jsonl)"
    )
    ap.add_argument("--transcribe-only", action="store_true", help="fill both transcription caches and stop")
    return ap.parse_args()


class Run:
    """One head's pass over the labels: the transcription caches, the spend cap, the reads file."""

    def __init__(self, args):
        self.head, self.cap = args.head, args.max_usd
        self.ev = Path(args.evidence)
        self.ev.mkdir(parents=True, exist_ok=True)
        self.caches = {
            "T": (Path(args.transcripts or self.ev / "transcripts.jsonl"), FULL_CAPTION_MAX_TOKENS),
            "D": (Path(args.device_transcripts or self.ev / "transcripts-device900.jsonl"), DEVICE_CAPTION_MAX_TOKENS),
        }
        self.loaded = {k: {r["id"]: r for r in load_jsonl(p)} for k, (p, _) in self.caches.items()}
        self.done = {(r["id"], r["head"], r["arm"]) for r in load_jsonl(self.ev / "reads.jsonl") if r.get("ok")}
        self.ac = anthropic_client()
        self.is_openai = args.head.startswith("gpt-")
        self.hc = openai_client() if self.is_openai else self.ac

    def over_cap(self) -> bool:
        if spent(self.ev, [p for p, _ in self.caches.values()]) < self.cap:
            return False
        print(f"spend cap {self.cap} reached; stopping")
        return True

    def transcript(self, lab: dict, kind: str) -> str:
        path, max_tokens = self.caches[kind]
        return transcript_for(self.ac, lab, path, self.loaded[kind], max_tokens)

    def read(self, lab: dict, arm: str) -> dict:
        kind, px = arm_px(arm)
        tr = None if kind == "P" else self.transcript(lab, kind)
        jpeg = rendition(HERE / "photos" / lab["file"], px)
        text = photo_line(ref8(jpeg), tr) + "\n" + OWNER_TEXT
        t0 = time.time()
        try:
            fn = read_openai if self.is_openai else read_anthropic
            read, raw, usage, stop = fn(self.hc, self.head, jpeg, text)
            err = ""
        except Exception as e:  # noqa: BLE001 - a failed read is recorded, not fatal
            read, raw, usage, stop, err = None, "", {}, "", providers.redact(f"{type(e).__name__}: {str(e)[:300]}")
        rec = {
            "id": lab["id"],
            "head": self.head,
            "arm": arm,
            "px": px,
            "jpeg_bytes": len(jpeg),
            "ok": read is not None,
            "read": read,
            "raw": raw if read is None else "",
            "stop": stop,
            "error": err,
            "usage": usage,
            "usd": providers.price(self.head, usage) if usage else 0.0,
            "seconds": round(time.time() - t0, 2),
        }
        with open(self.ev / "reads.jsonl", "a") as f:
            f.write(json.dumps(rec) + "\n")
        print(f"{lab['id']} {self.head} {arm}: {'ok' if rec['ok'] else 'FAILED ' + err[:80]} {rec['usd']:.4f} USD")
        return rec


def plan(labels: list, arms: list, transcribe_only: bool) -> list:
    """(label, job) pairs: a transcription kind per label, or every arm per label."""
    jobs = ("T", "D") if transcribe_only else arms
    return [(lab, j) for lab in labels for j in jobs]


def main() -> int:
    args = parse_args()
    providers.load_env(args.env_file)
    labels = json.loads(Path(args.manifest).read_text())["labels"]
    labels = labels[: args.limit] if args.limit else labels
    arms = [a.strip() for a in args.arms.split(",") if a.strip()]
    for a in arms:
        arm_px(a)
    run = Run(args)
    step = run.transcript if args.transcribe_only else run.read
    for lab, job in plan(labels, arms, args.transcribe_only):
        if (lab["id"], run.head, job) in run.done:
            continue
        if run.over_cap():
            return 3
        step(lab, job)
    total = spent(run.ev, [p for p, _ in run.caches.values()])
    print(f"total spend in {run.ev} with the transcriptions: {total:.4f} USD")
    return 0


if __name__ == "__main__":
    sys.exit(main())
