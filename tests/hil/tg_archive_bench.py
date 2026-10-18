"""Bench helpers for the Telegram archive legs (test_l55_tg_archive.py).

Builds synthetic media on the host, uploads it to the bench chat through the bench
bot so Telegram holds real files the device can fetch, and turns the sent messages
into a raw getUpdates page for the console's TGINJECT verb (the page then takes the
device's normal receive path, which archives it before classification).

Pure helpers (fixtures, page building) import nothing outside the standard library
plus PIL, so ``pytest --collect-only`` stays clean on a host without a board; the
Bot API calls need ``requests`` and run only inside hardware-gated tests.

Environment (never printed):
  NIMBUS_TEST_TG_TOKEN   the bench bot token (the unit polls this bot)
  NIMBUS_TEST_TG_CHAT    the bench chat id (on the unit's allowlist)
  NIMBUS_TGINJECT_BASE   optional first synthetic update_id (default: time based)
"""

from __future__ import annotations

import io
import json
import os
import shutil
import subprocess
import tempfile
import time

TG_API = "https://api.telegram.org"


# ---- fixtures ----------------------------------------------------------------------


def _noise_image(width: int, height: int, seed: int, fmt: str, quality: int) -> bytes:
    from PIL import Image  # local import: only the hardware legs need PIL

    img = Image.frombytes("RGB", (width, height), _seeded(width * height * 3, seed))
    buf = io.BytesIO()
    img.save(buf, fmt, quality=quality)
    return buf.getvalue()


def noise_jpeg(width: int, height: int, seed: int, quality: int = 95) -> bytes:
    """A JPEG of random noise (incompressible, so its size is predictable)."""
    return _noise_image(width, height, seed, "JPEG", quality)


def _seeded(n: int, seed: int) -> bytes:
    import random

    return random.Random(seed).randbytes(n)


def jpeg_near(target_bytes: int, seed: int = 1) -> bytes:
    """A noise JPEG of at least target_bytes (grows the frame until it is)."""
    side = 600
    while True:
        data = noise_jpeg(side, side * 3 // 4, seed)
        if len(data) >= target_bytes:
            return data
        side = int(side * 1.25)


def sticker_webp(seed: int = 3) -> bytes:
    return _noise_image(512, 512, seed, "WEBP", 80)


def ffmpeg_audio(seconds: float, codec_args: list[str], suffix: str) -> bytes:
    """A sine tone encoded by ffmpeg (OGG/Opus for a voice note, MP3 for audio)."""
    ff = shutil.which("ffmpeg")
    if not ff:
        raise RuntimeError("ffmpeg not found (needed to build the voice and audio fixtures)")
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "a" + suffix)
        subprocess.run(
            [ff, "-loglevel", "error", "-f", "lavfi", "-i", f"sine=frequency=440:duration={seconds}", *codec_args, out],
            check=True,
        )
        with open(out, "rb") as f:
            return f.read()


def voice_ogg() -> bytes:
    return ffmpeg_audio(3, ["-c:a", "libopus", "-b:a", "32k"], ".ogg")


def audio_mp3_near(target_bytes: int) -> bytes:
    """An MP3 of about target_bytes at 320 kb/s (40 KB per second)."""
    return ffmpeg_audio(max(1, target_bytes // 40000 + 1), ["-c:a", "libmp3lame", "-b:a", "320k"], ".mp3")


# ---- raw pages ------------------------------------------------------------------------


def as_inbound(sent: dict, chat_id: int, update_id: int, sender: str = "Bench") -> dict:
    """A message the bot sent, recast as an update the owner sent (same files)."""
    msg = dict(sent)
    msg["from"] = {"id": chat_id, "is_bot": False, "first_name": sender}
    return {"update_id": update_id, "message": msg}


def text_update(update_id: int, chat_id: int, text: str, date: int | None = None) -> dict:
    msg = {
        "message_id": update_id % 1000000,
        "from": {"id": chat_id, "is_bot": False, "first_name": "Bench"},
        "chat": {"id": chat_id, "type": "private"},
        "date": date or int(time.time()),
        "text": text,
    }
    if text.startswith("/"):
        msg["entities"] = [{"type": "bot_command", "offset": 0, "length": len(text.split()[0])}]
    return {"update_id": update_id, "message": msg}


def page(updates: list[dict]) -> str:
    return json.dumps({"ok": True, "result": updates}, separators=(",", ":"))


def largest_photo(sent: dict) -> dict:
    """The rendition the device keeps: most pixels, then most bytes, the last on a tie."""
    best = None
    for p in sent["photo"]:
        key = (p.get("width", 0) * p.get("height", 0), p.get("file_size", 0))
        if best is None or key >= best[0]:
            best = (key, p)
    return best[1]


def first_update_id() -> int:
    env = os.environ.get("NIMBUS_TGINJECT_BASE")
    return int(env) if env else 900000000 + (int(time.time()) % 900000) * 100


# ---- TGINJECT with local files (no Telegram upload) ------------------------------------------


def local_photo_update(update_id: int, chat_id: int, path: str, size: int, group: str | None = None) -> dict:
    """A photo update whose largest rendition is the card file at `path` (file id
    "LOCAL_<path>", read by the test build instead of fetched from Telegram)."""
    msg = {
        "message_id": update_id % 1000000,
        "from": {"id": chat_id, "is_bot": False, "first_name": "Bench"},
        "chat": {"id": chat_id, "type": "private"},
        "date": int(time.time()),
        "photo": [
            {"file_id": "THUMB_not_fetched", "width": 90, "height": 60, "file_size": 1200},
            {"file_id": "LOCAL_" + path, "width": 2560, "height": 1920, "file_size": size},
        ],
    }
    if group:
        msg["media_group_id"] = group
    return {"update_id": update_id, "message": msg}


def local_file_update(update_id: int, chat_id: int, key: str, path: str, size: int, extra: dict | None = None) -> dict:
    """A voice / document / audio update whose file is the card file at `path`."""
    obj = {"file_id": "LOCAL_" + path, "file_size": size, **(extra or {})}
    msg = {
        "message_id": update_id % 1000000,
        "from": {"id": chat_id, "is_bot": False, "first_name": "Bench"},
        "chat": {"id": chat_id, "type": "private"},
        "date": int(time.time()),
        key: obj,
    }
    return {"update_id": update_id, "message": msg}


def fsput(device, data: bytes, path: str) -> None:
    """Put `data` at `path` on the card over the console's FSPUT verb (base64, ACK-paced)."""
    import base64
    import hashlib

    device.cmd(f"FSPUT {len(data)} {path}", "FSPUT READY", timeout=10.0)
    b64 = base64.b64encode(data).decode()
    acked = 0
    for i in range(0, len(b64), 1024):
        device.send(b64[i : i + 1024])
        sent = min(len(data), (i + 1024) // 4 * 3)
        while sent - acked > 8192 and acked < len(data):
            m = device.expect_re(r"FSACK (\d+)", timeout=15.0)
            acked = int(m.group(1))
    m = device.expect_re(r"FSPUT DONE bytes=(\d+) sha256=([0-9a-f]+)", timeout=60.0)
    if int(m.group(1)) != len(data) or m.group(2) != hashlib.sha256(data).hexdigest():
        raise RuntimeError(f"FSPUT to {path} did not verify")


# ---- the bench bot (hardware legs only) --------------------------------------------------


class BenchBot:
    def __init__(self, token: str, chat_id: int):
        import requests  # noqa: F401 - fail early when absent

        self.token, self.chat = token, chat_id

    def _call(self, method: str, data: dict, files: dict | None = None) -> dict:
        import requests

        try:
            r = requests.post(f"{TG_API}/bot{self.token}/{method}", data=data, files=files, timeout=120)
            body = r.json()
        except (requests.RequestException, ValueError) as e:
            # The URL carries the bot token: never let it reach a log or a report.
            raise RuntimeError(f"{method} failed: {str(e).replace(self.token, '<token>')}") from None
        if not body.get("ok"):
            raise RuntimeError(f"{method} failed: {body.get('description', r.status_code)}")
        return body["result"]

    def send_file(self, method: str, field: str, name: str, data: bytes, **extra) -> dict:
        return self._call(method, {"chat_id": self.chat, **extra}, {field: (name, data)})

    def send_album(self, photos: list[bytes], caption: str) -> list[dict]:
        media, files = [], {}
        for i, data in enumerate(photos):
            key = f"p{i}"
            item = {"type": "photo", "media": f"attach://{key}"}
            if i == 0:
                item["caption"] = caption
            media.append(item)
            files[key] = (f"{key}.jpg", data)
        return self._call("sendMediaGroup", {"chat_id": self.chat, "media": json.dumps(media)}, files)
