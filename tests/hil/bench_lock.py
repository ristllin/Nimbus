"""Bench locks for the HIL legs that share hardware or the Telegram test bot.

Telegram's getUpdates has one owner per bot token (a second poller gets HTTP 409), so a
leg that puts the dedicated test bot's token on a unit first takes the bench's
``tg-bot`` lock, and gives it back in a ``finally`` however the leg ends: a failed
assertion, a wedged console, a lost unit. On 22 Oct 2026 a runner kept the lock for
3 h 48 min after a console wedge, and every Telegram leg on the other unit waited.

The locks are directories (``mkdir`` is atomic) under ``NIMBUS_BENCH_LOCKS``; each holds
an ``owner`` file naming the holder. Without ``NIMBUS_BENCH_LOCKS`` (a single-unit
bench) no lock is taken. A held lock is retried every ``retry_s`` seconds until
``wait_s`` runs out, then the leg skips with the holder's name: never a silent pass.
"""

from __future__ import annotations

import contextlib
import os
import socket
import time
from typing import Iterator, Optional

import pytest

LOCK_ROOT_ENV = "NIMBUS_BENCH_LOCKS"
TG_BOT = "tg-bot"
RETRY_S = 60.0
WAIT_S = float(os.environ.get("NIMBUS_BENCH_LOCK_WAIT_S", "1800"))


def lock_root() -> Optional[str]:
    return os.environ.get(LOCK_ROOT_ENV) or None


def holder(path: str) -> str:
    try:
        with open(os.path.join(path, "owner"), encoding="utf-8") as f:
            return f.read().strip() or "unknown"
    except OSError:
        return "unknown"


@contextlib.contextmanager
def bench_lock(name: str, who: str, wait_s: float = WAIT_S, retry_s: float = RETRY_S) -> Iterator[Optional[str]]:
    """Hold the bench lock ``name`` for the block; released in a finally."""
    root = lock_root()
    if not root:
        yield None
        return
    path = os.path.join(root, name)
    end = time.time() + wait_s
    while True:
        try:
            os.mkdir(path)
            break
        except FileExistsError:
            if time.time() >= end:
                pytest.skip(f"bench lock {name} still held by {holder(path)} after {wait_s:.0f} s")
            time.sleep(retry_s)
    with open(os.path.join(path, "owner"), "w", encoding="utf-8") as f:
        f.write(
            f"{who} pid={os.getpid()} host={socket.gethostname()} {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}\n"
        )
    try:
        yield path
    finally:
        with contextlib.suppress(OSError):
            os.remove(os.path.join(path, "owner"))
        with contextlib.suppress(OSError):
            os.rmdir(path)


def clear_bot_token(post, tries: int = 3, gap_s: float = 10.0) -> bool:
    """Take the bot token off the unit, retrying a dropped request. ``post`` sends the
    form to /api/orch and returns the response. Never raises: the caller releases the
    lock either way, and says so when the token may still be on the unit."""
    for t in range(tries):
        try:
            if post({"clr_tgToken": "1"}).status_code == 200:
                return True
        except Exception:  # noqa: BLE001 - a busy or restarting unit: try again
            pass
        if t + 1 < tries:
            time.sleep(gap_s)
    return False


@contextlib.contextmanager
def bot_on_unit(post, token, chat, who: str, before=None) -> Iterator[None]:
    """The test bot's token (and its chat as the owner) on the unit for the block:
    ``tg-bot`` taken first, the token cleared and the lock released in a finally.
    ``before`` runs once the lock is held and before the token goes on (no poller runs)."""
    with bench_lock(TG_BOT, who):
        try:
            if before is not None:
                before()
            r = post({"tgToken": token, "tgAllow": chat})
            assert r.status_code == 200, f"setting the test bot token failed: HTTP {r.status_code}"
            yield
        finally:
            if not clear_bot_token(post):
                print(f"\n[bench_lock] {who}: the bot token could not be cleared; the unit may still poll the bot")
