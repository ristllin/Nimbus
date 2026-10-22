"""L56 - full inbound Telegram path with no phone (program-2026-10-18 amendment).

A wave gate must run the whole inbound Telegram path - text, command, photo with
the real PhotoSize array, album with media_group_id, voice, document, video,
sticker, location, edited message, a reply from another chat - so the human gate
is a confirmation, not the only coverage. Lane N lands the ``TGINJECT <json>``
console verb that feeds a complete raw getUpdates page into the normal receive
path; lane R (the Telegram archive, at the receive boundary) wires the full-fidelity
hook. This leg drives TGINJECT with one page per update shape and asserts the
device took the right action.

Lane split: lane N lands this skeleton + the TGINJECT verb. Until lane R wires the
raw-update hook, TGINJECT runs text and command updates through the public inject
queue and reports the rest, so the text/command legs pass now and the media/album
legs SKIP with a reason (never a silent pass). Lane D's album batching turns the
album leg green.

⚠ ``hil`` (needs the board). The pages carry no media download, so no provider
spend; a leg that drives a real turn is marked ``agent`` and gated on a key.
"""

from __future__ import annotations

import time

import pytest

from device import Device, ExpectTimeout

pytestmark = pytest.mark.hil

CHAT = 424242

# One raw getUpdates page per inbound shape. The device must take the named action.
PAGES = {
    "text": (
        '{"ok":true,"result":[{"update_id":80001,"message":{"message_id":1,"date":1792300000,'
        '"chat":{"id":%d,"type":"private"},"from":{"id":%d,"first_name":"Bench"},'
        '"text":"hello from the bench"}}]}' % (CHAT, CHAT)
    ),
    "command": (
        '{"ok":true,"result":[{"update_id":80002,"message":{"message_id":2,"date":1792300001,'
        '"chat":{"id":%d,"type":"private"},"from":{"id":%d,"first_name":"Bench"},'
        '"text":"/skills","entities":[{"type":"bot_command","offset":0,"length":7}]}}]}' % (CHAT, CHAT)
    ),
    "photo": (
        '{"ok":true,"result":[{"update_id":80003,"message":{"message_id":3,"date":1792300002,'
        '"chat":{"id":%d,"type":"private"},"from":{"id":%d,"first_name":"Bench"},'
        '"caption":"what is this","photo":[{"file_id":"ph_s","width":90,"height":90,"file_size":1200},'
        '{"file_id":"ph_l","width":1280,"height":1280,"file_size":180000}]}}]}' % (CHAT, CHAT)
    ),
    "album": (
        '{"ok":true,"result":['
        '{"update_id":80004,"message":{"message_id":4,"date":1792300003,"media_group_id":"G1",'
        '"chat":{"id":%d,"type":"private"},"from":{"id":%d,"first_name":"Bench"},'
        '"photo":[{"file_id":"a1_l","width":1280,"height":1280,"file_size":170000}]}},'
        '{"update_id":80005,"message":{"message_id":5,"date":1792300003,"media_group_id":"G1",'
        '"chat":{"id":%d,"type":"private"},"from":{"id":%d,"first_name":"Bench"},'
        '"photo":[{"file_id":"a2_l","width":1280,"height":1280,"file_size":175000}]}}]}' % (CHAT, CHAT, CHAT, CHAT)
    ),
    "voice": (
        '{"ok":true,"result":[{"update_id":80006,"message":{"message_id":6,"date":1792300004,'
        '"chat":{"id":%d,"type":"private"},"from":{"id":%d,"first_name":"Bench"},'
        '"voice":{"file_id":"vo_1","duration":3,"mime_type":"audio/ogg","file_size":9000}}}]}' % (CHAT, CHAT)
    ),
    "document": (
        '{"ok":true,"result":[{"update_id":80007,"message":{"message_id":7,"date":1792300005,'
        '"chat":{"id":%d,"type":"private"},"from":{"id":%d,"first_name":"Bench"},'
        '"document":{"file_id":"do_1","file_name":"notes.pdf","mime_type":"application/pdf","file_size":40000}}}]}'
        % (CHAT, CHAT)
    ),
    "edited": (
        '{"ok":true,"result":[{"update_id":80008,"edited_message":{"message_id":1,"date":1792300006,'
        '"edit_date":1792300007,"chat":{"id":%d,"type":"private"},"from":{"id":%d,"first_name":"Bench"},'
        '"text":"hello from the bench (edited)"}}]}' % (CHAT, CHAT)
    ),
}

# Which legs run text through now (A-independent) vs need the full receive hook.
# An edited message is not one of them: by design it carries ids and the edit time
# only and is handled, never run as a new message (tg_updates.h, Update::edited).
TEXT_LIKE = {"text", "command"}


def _raw_hook_present(dev: Device) -> bool:
    """True once lane R wires the full raw-update hook: TGINJECT then reports the
    media/album path instead of 'need the telegram receive hook'."""
    try:
        line = dev.cmd("TGINJECT " + PAGES["photo"], "TGINJECT", timeout=10.0)
        return "receive hook" not in line
    except ExpectTimeout:
        return False


def _inject(dev: Device, page: str, timeout_s: float = 90.0) -> str:
    """TGINJECT one page, waiting out a page that is still running.

    The poll task runs one injected page per poll cycle (an album one photo per
    cycle) and refuses the next page with ``busy`` until then; legs sent back to back
    in the v4.5.14 release run hit that refusal, and the media legs read it as a
    pass. Every leg is judged on its own page, and a page that never finishes fails."""
    deadline = time.time() + timeout_s
    while True:
        line = dev.cmd("TGINJECT " + page, "TGINJECT", timeout=15.0)
        if "busy" not in line:
            return line
        if time.time() > deadline:
            pytest.fail(f"the previous injected page never finished running ({timeout_s:.0f} s): {line}")
        time.sleep(2.0)


@pytest.mark.parametrize("shape", sorted(PAGES))
def test_inbound_shape_takes_the_right_action(device: Device, shape):
    dev = device
    line = _inject(dev, PAGES[shape])
    if shape in TEXT_LIKE:
        # Text and commands run through the public inject queue even before lane R.
        assert "injected=1" in line or "text=1" in line, f"{shape}: not injected: {line}"
        return
    if shape == "edited":
        assert "queued" in line and "text=0" in line, f"an edit must be handled, not run as a new message: {line}"
        return
    if "receive hook" in line:
        pytest.skip(f"{shape}: full raw-update hook not wired yet (lane R)")
    # Lane A/D present: the media/album path acknowledged the shape.
    assert "parse FAILED" not in line, f"{shape}: parse failed: {line}"
    assert "queued" in line, f"{shape}: the page was not queued: {line}"
