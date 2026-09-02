"""Host tests for the Telegram PSRAM-staging gate (N7 win #2, CUM-24).

Each assertion goes RED on a pre-fix / regressed ELF and GREEN on the shipped
one, so the guard itself is proven - the same discipline as test_release_gate.py.

Run: python3 -m pytest tools/release_gate
"""

import os
import subprocess

import check_sram_staging as st

# nm -S -C rows as (size, type, name). A shipped build: every staging buffer is
# a 4 B PSRAM pointer handle; the other telegram statics are legitimately inline.
GOOD_ROWS = [
    (0x4, "b", "agent::telegram::(anonymous namespace)::g_inboundStage"),
    (0x4, "b", "agent::telegram::(anonymous namespace)::g_apiResp"),
    (0x4, "b", "agent::telegram::(anonymous namespace)::g_pollBody"),
    (0x280, "b", "agent::telegram::(anonymous namespace)::s_pending"),  # legit inline
    (0x25C, "b", "agent::telegram::(anonymous namespace)::g_attach"),  # legit inline
]


def test_shipped_build_passes():
    ok, msgs = st.judge(GOOD_ROWS)
    assert ok, msgs
    assert all("win intact" in m for m in msgs)


def test_inbound_reverted_inline_fails():
    # `static InboundMsg im;` back at file scope = the full struct in internal bss.
    rows = [r for r in GOOD_ROWS if st._leaf(r[2]) != "g_inboundStage"]
    rows.append((0x1040, "b", "agent::telegram::(anonymous namespace)::g_inboundStage"))
    ok, msgs = st.judge(rows)
    assert not ok
    assert any("g_inboundStage" in m and "moved back inline" in m for m in msgs)


def test_small_response_revert_still_caught():
    # Even the smallest realistic revert (`static char resp[512]`) must trip it -
    # a ceiling set above the legit inline statics (s_pending=640) would miss this.
    rows = [r for r in GOOD_ROWS if st._leaf(r[2]) != "g_apiResp"]
    rows.append((0x200, "b", "agent::telegram::(anonymous namespace)::g_apiResp"))
    ok, _ = st.judge(rows)
    assert not ok


def test_missing_handle_fails_not_silently_passes():
    # An empty/half-built ELF (or renamed code) must FAIL, never pass by absence.
    rows = [r for r in GOOD_ROWS if st._leaf(r[2]) != "g_pollBody"]
    ok, msgs = st.judge(rows)
    assert not ok
    assert any("missing staging handle g_pollBody" in m for m in msgs)


def test_legit_large_telegram_statics_do_not_false_positive():
    # s_pending / g_attach are internal by design and not part of this win; the
    # guard names only the staging handles, so their size never trips it.
    ok, _ = st.judge(GOOD_ROWS)
    assert ok


def test_rodata_symbol_is_not_counted_as_internal_sram():
    # A same-named symbol in rodata (flash, type r) is not internal SRAM; if only
    # a rodata copy existed the handle counts as missing, not as a huge inline.
    rows = [r for r in GOOD_ROWS if st._leaf(r[2]) != "g_apiResp"]
    rows.append((0x400, "r", "agent::telegram::(anonymous namespace)::g_apiResp"))
    ok, msgs = st.judge(rows)
    assert not ok
    assert any("missing staging handle g_apiResp" in m for m in msgs)


def test_parse_nm_skips_unsized_and_short_rows():
    text = "\n".join(
        [
            "3fca9f30 00000004 b agent::telegram::(anonymous namespace)::g_inboundStage",
            "         U some_undefined_symbol",  # undefined, no address/size
            "420f17d4 t agent::telegram::ensureApiResp()",  # unsized (no size field)
            "",
        ]
    )
    rows = st.parse_nm(text)
    assert (0x4, "b", "agent::telegram::(anonymous namespace)::g_inboundStage") in rows
    assert all(len(r) == 3 for r in rows)
    # the unsized `t` row must not be misparsed into a bogus sized row
    assert not any(st._leaf(name) == "ensureApiResp()" for _s, _t, name in rows)


# --- integration leg: run against the real ELF when it has been built ---------
def test_real_elf_when_built():
    if not (os.path.exists(st.DEFAULT_ELF) and st.find_nm()):
        return  # ELF/toolchain not present in this environment - logic tests cover it
    rc = subprocess.run(
        ["python3", os.path.join(st.REPO, "tools", "release_gate", "check_sram_staging.py")],
        cwd=st.REPO,
    ).returncode
    assert rc == 0, "the shipped esp32s3 ELF must pass the staging gate"
