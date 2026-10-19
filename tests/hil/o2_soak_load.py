#!/usr/bin/env python3
"""Load driver for the O2 24 h soak (lane O2, v4.5.14). Not a pytest test.

Runs beside ``sram_sampler.py --hours 24 --interval 60`` (which records /api/state)
and drives the load the soak's pass criteria are judged under:

  * a turn every ``--turn-every`` seconds (default 10 min) through the web chat, with a
    prompt that takes two tool rounds; the head must already be set to the model under
    test (claude-haiku-5-5 for the program);
  * the web app page every ``--page-every`` seconds (default 30 min) over the LAN; the
    cloud page load is the orchestrator's (lane X's cloud-page-probe in the owner's
    browser tab, one ``cumuloCloudPageProbeStart({rounds: 1})`` per half hour);
  * ``--photos`` photo turns (default 6) spread over the run, injected as raw
    Telegram updates on the test console (``--port``; the photo must be one the bench
    bot can download: ``NIMBUS_TEST_TG_PHOTO_FILE_ID`` and ``NIMBUS_TEST_TG_CHAT_ID``).

Every event is appended to ``--out`` as one JSON line. Afterwards:
  sram_sampler.py --soak-verdict <sampler.jsonl>

Usage:
  o2_soak_load.py --ip <unit A> --token "$NIMBUS_TEST_TOKEN" --hours 24 \\
      --port /dev/cu.usbmodemXXXX --out o2_soak_load.jsonl
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time

TURN_PROMPT = "Use memory_search for 'tea', then files_list once, then answer in one short sentence."
PHOTO_CAPTION = "What is in this photo? Then check memory for anything related, and answer briefly."


def _emit(out, row):
    row["t"] = round(time.time(), 3)
    line = json.dumps(row)
    print(line, flush=True)
    if out:
        with open(out, "a", encoding="utf-8") as f:
            f.write(line + "\n")


def _session(token):
    import requests

    s = requests.Session()
    s.headers["X-Nimbus-Token"] = token
    return s


def run_turn(s, ip, text, timeout_s=240):
    r = s.post(f"http://{ip}/api/chat", data={"text": text}, timeout=15)
    if r.status_code != 200:
        return {"ok": False, "status": r.status_code}
    turn = r.json().get("turn", "")
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        time.sleep(3)
        j = s.get(f"http://{ip}/api/chat", params={"turn": turn} if turn else None, timeout=15).json()
        if not j.get("pending", True):
            return {"ok": not j.get("error"), "reply": str(j.get("reply", ""))[:120], "error": j.get("error")}
    return {"ok": False, "error": "timeout"}


def photo_page(file_id, chat_id, update_id):
    return {
        "ok": True,
        "result": [
            {
                "update_id": update_id,
                "message": {
                    "message_id": update_id % 100000,
                    "from": {"id": int(chat_id), "first_name": "Bench"},
                    "chat": {"id": int(chat_id), "type": "private"},
                    "date": int(time.time()),
                    "photo": [{"file_id": file_id, "file_unique_id": "bench", "width": 1280, "height": 960}],
                    "caption": PHOTO_CAPTION,
                },
            }
        ],
    }


def _parse(argv):
    p = argparse.ArgumentParser(description="O2 24 h soak load driver")
    p.add_argument("--ip", required=True)
    p.add_argument("--token", default=os.environ.get("NIMBUS_TEST_TOKEN", ""))
    p.add_argument("--hours", type=float, default=24.0)
    p.add_argument("--turn-every", type=float, default=600.0)
    p.add_argument("--page-every", type=float, default=1800.0)
    p.add_argument("--photos", type=int, default=6)
    p.add_argument("--port", help="test console serial port (photo injects)")
    p.add_argument("--out", default="o2_soak_load.jsonl")
    args = p.parse_args(argv)
    if not args.token:
        p.error("--token (or NIMBUS_TEST_TOKEN) is required")
    return args


def _open_console(args):
    """The test console for photo injects, or None (logged) when it cannot be used."""
    if not args.photos:
        return None
    if args.port and os.environ.get("NIMBUS_TEST_TG_PHOTO_FILE_ID") and os.environ.get("NIMBUS_TEST_TG_CHAT_ID"):
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from device import Device

        return Device(port=args.port).open()
    why = "needs --port, NIMBUS_TEST_TG_PHOTO_FILE_ID, NIMBUS_TEST_TG_CHAT_ID"
    _emit(args.out, {"event": "photos-skipped", "why": why})
    return None


def _load_page(s, args):
    try:
        r = s.get(f"http://{args.ip}/", timeout=60)
        _emit(args.out, {"event": "page", "status": r.status_code, "bytes": len(r.content)})
    except Exception as e:  # noqa: BLE001 - a failed load is data
        _emit(args.out, {"event": "page", "error": str(e)})


def _inject_photo(dev, args, n):
    page = photo_page(
        os.environ["NIMBUS_TEST_TG_PHOTO_FILE_ID"], os.environ["NIMBUS_TEST_TG_CHAT_ID"], int(time.time())
    )
    dev.send("TGINJECT " + json.dumps(page, separators=(",", ":")))
    _emit(args.out, {"event": "photo", "n": n})


def main(argv=None):
    args = _parse(argv)
    s = _session(args.token)
    dev = _open_console(args)
    start = time.time()
    end = start + args.hours * 3600
    next_turn, next_page = start, start + 60
    photos = [start + (i + 0.5) * (end - start) / max(1, args.photos) for i in range(args.photos)] if dev else []
    n = 0
    while time.time() < end:
        now = time.time()
        if photos and now >= photos[0]:
            photos.pop(0)
            _inject_photo(dev, args, args.photos - len(photos))
        if now >= next_turn:
            n += 1
            _emit(args.out, {"event": "turn", "n": n, **run_turn(s, args.ip, f"[soak {n}] {TURN_PROMPT}")})
            next_turn = now + args.turn_every
        if now >= next_page:
            _load_page(s, args)
            next_page = now + args.page_every
        time.sleep(5)
    if dev:
        dev.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
