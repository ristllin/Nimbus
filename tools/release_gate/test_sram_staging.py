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


# --- relay / cloud-sync staging (CUM-387, source leg) -------------------------
# Minimal shipped-shape sources: the relay staging containers are the PSRAM alias
# and each header keeps its PSRAM static_assert. Each mutation below reverts one
# relay buffer to the internal heap (or drops a guard) and must turn the leg RED,
# so the guard is proven non-tautological - the same discipline as the ELF leg.
GOOD_HTTP = """
using BodyBuf = nimbus::cloud::PsVector<uint8_t>;
#if defined(NIMBUS_RELAY_PSRAM_BODY)
static_assert(std::is_same<BodyBuf::allocator_type, nimbus::cloud::PsramAlloc<uint8_t>>::value,
              "relay response body/staging must stay PSRAM-backed (CUM-387)");
#endif
  BodyBuf buf_;
  BodyBuf body_;
"""

GOOD_WS = """
struct Message {
  nimbus::cloud::PsVector<uint8_t> payload;
};
  nimbus::cloud::PsVector<uint8_t> buf_;
  nimbus::cloud::PsVector<uint8_t> frag_;
  std::vector<Message> ready_;   // legit: a vector of messages, not a byte buffer
#if defined(NIMBUS_RELAY_PSRAM_BODY)
static_assert(std::is_same<decltype(Message::payload)::allocator_type,
                           nimbus::cloud::PsramAlloc<uint8_t>>::value,
              "relay inbound WS message payload must stay PSRAM-backed (CUM-387)");
#endif
"""

HTTP_REL = os.path.join("lib", "core", "include", "nimbus", "cloud", "http_replay.h")
WS_REL = os.path.join("lib", "core", "include", "nimbus", "cloud", "relay_ws.h")


def _good_sources():
    return {HTTP_REL: GOOD_HTTP, WS_REL: GOOD_WS}


def test_relay_shipped_shape_passes():
    ok, msgs = st.judge_relay(_good_sources())
    assert ok, msgs
    assert any("BodyBuf is the PSRAM alias" in m for m in msgs)
    assert all("win intact" in m or "guard intact" in m for m in msgs)


def test_relay_bodybuf_reverted_to_internal_fails():
    # `using BodyBuf = std::vector<uint8_t>;` puts the ~278 KB response body back on
    # the scarce internal heap - exactly the CUM-387 regression the gate must catch.
    src = _good_sources()
    src[HTTP_REL] = GOOD_HTTP.replace(
        "using BodyBuf = nimbus::cloud::PsVector<uint8_t>;",
        "using BodyBuf = std::vector<uint8_t>;",
    )
    ok, msgs = st.judge_relay(src)
    assert not ok
    assert any("BodyBuf reverted to std::vector<uint8_t>" in m for m in msgs)


def test_relay_ws_member_reverted_to_internal_fails():
    # Revert just the WS unparsed-frame staging (buf_) to the internal heap.
    src = _good_sources()
    src[WS_REL] = GOOD_WS.replace(
        "nimbus::cloud::PsVector<uint8_t> buf_;",
        "std::vector<uint8_t> buf_;",
    )
    ok, msgs = st.judge_relay(src)
    assert not ok
    assert any("buf_ reverted to std::vector<uint8_t>" in m for m in msgs)


def test_relay_static_assert_removal_fails():
    # Dropping the compile-time guard is itself a regression (it is the first line of
    # defense); the gate refuses a header that no longer carries it. Strip the whole
    # `static_assert(...);` statement, as a real deletion would.
    import re

    src = _good_sources()
    src[WS_REL] = re.sub(r"static_assert\s*\([^;]*;", "", GOOD_WS, flags=re.S)
    ok, msgs = st.judge_relay(src)
    assert not ok
    assert any("missing inbound WS payload PSRAM static_assert" in m for m in msgs)


def test_relay_missing_header_fails_not_silently_passes():
    # A renamed/removed header must FAIL, never pass by absence (deliberate stop).
    src = _good_sources()
    del src[HTTP_REL]
    ok, msgs = st.judge_relay(src)
    assert not ok
    assert any("missing relay staging header" in m and "http_replay.h" in m for m in msgs)


def test_relay_ready_vector_of_messages_does_not_false_positive():
    # `std::vector<Message> ready_;` is a legit internal vector of messages (not a
    # uint8_t byte buffer); the gate names only the uint8_t staging members, so it
    # never trips on it.
    ok, _ = st.judge_relay(_good_sources())
    assert ok


def test_relay_real_headers_pass():
    # Integration leg: the actual shipped headers must pass with no board and no build.
    sources = st.read_relay_sources()
    assert HTTP_REL in sources and WS_REL in sources, "relay staging headers not found"
    ok, msgs = st.judge_relay(sources)
    assert ok, msgs
