"""Host tier for L18's write check (no board).

On 28 Oct the leg went RED on its positive control: a memory.write that answered "queued
memory <id>, not stored yet ... stored automatically ..." passed the leg's check because
that sentence holds the word "stored". The leg's exact-sentence search then found nothing
(fixed in the firmware: memory.search now finds queued facts), and the log's unrelated
"You do not have access" line got the blame: an RBAC regression that never existed.
"""

from __future__ import annotations

import pytest

import test_l18_multitenant as l18

pytestmark = pytest.mark.host


@pytest.mark.parametrize(
    "result,landed",
    [
        ('{"text": "stored memory m0001 (pinned)"}', True),
        (
            '{"text": "queued memory m0001, not stored yet: tls busy. It is kept on the device and stored '
            'automatically when embeddings work again; keyword search finds it meanwhile."}',
            True,
        ),
        # A full queue says "not stored" too, and must not pass as a write.
        ('{"text": "embedding failed (memory not stored: 16 memories are already waiting)"}', False),
        ('{"text": "this conversation is not approved to use memory"}', False),
        ('{"text": "duplicate of an existing memory, importance raised"}', False),
    ],
)
def test_write_landed(result, landed):
    assert l18.write_landed(result) is landed
