"""L41 (source guard) - no unarbitrated TLS handshake or bulk transfer.

The device runs ONE outbound work-TLS exchange at a time through the work slot
(``src/sys/tls_arbiter``, AGENTS.md "No on-device concurrency"). The cloud relay
broke that rule for years: it dialed (``relay_client.cpp`` ``g_ws->connect``) and
streamed tunneled pages (``emitResFrame`` -> ``wsWriteAll``) with no slot, and on
14 Oct a 26 s page upload overlapped two head-turn body writes and both failed.

This guard encodes the class, deterministically and with no board:

  * every function in ``src/`` that opens a ``WiFiClientSecure`` connection takes
    the slot itself before its first connect, with the scope guard
    (``arbiter::WorkSlot slot(ms)``, a ``nimbus::HeldSlot`` over
    ``acquireWork``/``releaseWork``, which gives the slot back on every path out) or
    by hand (``acquireWork``, the non-turn sites ``test_held_slot`` lists), or is
    allowlisted below with the function that holds the slot for it (and that holder
    is checked to take the slot and to call it);
  * the Telegram poll task (``telegram.cpp``) is the one exception by design: its
    own session is serialized with turns by task (a turn runs on that task) and it
    closes its socket before any other TLS. The Telegram archive's downloads
    (``tg_archive.cpp``) share that exception only because they run on that task,
    after the poll socket is closed, which is checked below;
  * the relay's big frames go through the slot: ``emitResFrame`` routes an answer
    whose body is over ``kRelayBulkBytes`` to ``writeBigFrame``, which takes the slot
    per chunk (decided on the body: base64 makes a 7 KB poll a 9.5 KB frame, and a
    poll must not wait for a turn); ``wsWriteAll`` is used only for small frames
    (heartbeat, pong, hello, acks, small answers);
  * the transport takes the slot with the guard before its traced exchange and gives
    it back in exactly one place (the trace finish), never by hand;
  * the transport's retained-document guard measures what the parse kept
    (``measureJson``), never ``memoryUsage()``, which always returns 0 in
    ArduinoJson 7 and left the 512 KB guard unable to fire.

It fails on the v4.5.13 tree (the relay dial at ``relay_client.cpp:1189`` and the
``wsWriteAll`` write at ``:394``) and passes once both go through the slot.
"""

from __future__ import annotations

import re
from pathlib import Path

_REPO = Path(__file__).resolve().parents[2]
_SRC = _REPO / "src"

# A function definition starts at column 0 (the codebase defines functions flush
# left, including inside namespaces); its header may span lines. Control statements
# and type blocks are not functions.
_HEAD_START = re.compile(
    r"^(?!namespace\b|struct\b|class\b|enum\b|union\b|if\b|for\b|while\b|switch\b|return\b|"
    r"using\b|typedef\b|template\b|#|//|\s|\})[A-Za-z_][^;{}]*\("
)
_NAME = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
_LITERALS = re.compile(r'"(?:[^"\\]|\\.)*"|\'(?:[^\'\\]|\\.)*\'|//.*$')


def _code(line: str) -> str:
    """The line without string/char literals and // comments (they can hold braces)."""
    return _LITERALS.sub("", line)


_TLS_CONNECT = re.compile(r"(?:\.|->)connect\s*\(")
# Taking the work slot: the scope guard (arbiter::WorkSlot, or a nimbus::HeldSlot built
# on acquireWork) or the hand-held call. A declaration only: `WorkSlot& s` or a
# parameter does not take it.
_TAKES_SLOT = re.compile(
    r"\bacquireWork\s*\(|\bWorkSlot\s+\w+\s*[({]|\bHeldSlot\s+\w+\s*[({]\s*&?\s*(?:\w+::)*acquireWork\b"
)


def _strip(body: str) -> str:
    """A body without string/char literals and // comments (a comment naming the guard
    is not the guard)."""
    return "\n".join(_code(line) for line in body.splitlines())


def _takes_slot(body: str) -> bool:
    return bool(_TAKES_SLOT.search(_strip(body)))


def _slot_before_connect(body: str) -> bool:
    """The slot is taken before the function's first TLS connect."""
    code = _strip(body)
    take, conn = _TAKES_SLOT.search(code), _TLS_CONNECT.search(code)
    return bool(take) and (conn is None or take.start() < conn.start())


# Functions that open a TLS connection while a CALLER holds the slot:
# (file, helper) -> the call chain up to the function that takes the slot. The test
# below checks each link calls the next and the last one takes the slot.
_HELD_BY_CALLER = {
    ("agent/provider_verify.cpp", "probeModel"): ["probeSelectedModels", "runOne"],
    ("agent/provider_verify.cpp", "zaiHostAnswers"): ["zaiPickHost", "runOne"],
    ("agent/provider_verify.cpp", "mistralConnectorsIo"): ["syncMistralConnectors", "runOne"],
    ("agent/provider_verify.cpp", "syncCumuloFallbacks"): ["runOne"],
    ("agent/provider_verify.cpp", "fetchAnthropicPage"): ["fetchModelList", "runOne"],
    ("agent/adapters/provider_file_fetch.cpp", "httpsGetToSink"): ["captureProviderFile"],
    ("agent/adapters/audio_tts.cpp", "connectTts"): ["synthesizeToFile"],
    ("sys/ota_update.cpp", "httpsGetOnce"): ["httpsGetUrl", "fetchManifest", "checkRun"],
}

# Files whose TLS is outside the work slot by design.
_TASK_SERIALIZED = {
    "agent/telegram.cpp": "tg_poll: turns run on this task, and it closes its session before any other TLS",
    "agent/tg_archive.cpp": "tg_poll: the archive downloads run inside the poll task, after the poll socket closes",
}


def _functions(text: str):
    """Yield (name, first_line, body) for each column-0 function definition."""
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        if not _HEAD_START.match(lines[i]):
            i += 1
            continue
        # Accumulate the header until its opening brace (a ';' first: a declaration).
        head, j, paren = [], i, 0
        while j < len(lines) and j - i < 8:
            head.append(lines[j])
            code = _code(lines[j])
            paren += code.count("(") - code.count(")")
            if paren <= 0 and code.rstrip().endswith("{"):
                break
            if paren <= 0 and ";" in code:
                head = []
                break
            j += 1
        if not head or not _code(head[-1]).rstrip().endswith("{"):
            i = j + 1 if head == [] else i + 1
            continue
        name_m = _NAME.search(" ".join(head))
        depth, k, body = 0, i, []
        while k < len(lines):
            body.append(lines[k])
            code = _code(lines[k])
            depth += code.count("{") - code.count("}")
            if depth <= 0 and k >= j:
                break
            k += 1
        yield name_m.group(1), i + 1, "\n".join(body)
        i = k + 1


def _fn_map(rel: str) -> dict:
    return {name: (line, body) for name, line, body in _functions((_SRC / rel).read_text())}


def _tls_offenders(rel: str, text: str) -> list:
    """The functions in one source file that open TLS without taking the slot first."""
    offenders = []
    for name, line, body in _functions(text):
        tls = "WiFiClientSecure" in body or "g_ws" in body
        if not tls or not _TLS_CONNECT.search(_strip(body)):
            continue
        if _slot_before_connect(body) or (rel, name) in _HELD_BY_CALLER:
            continue
        offenders.append(f"src/{rel}:{line} {name}() opens TLS without the work slot")
    return offenders


def test_every_tls_connect_takes_the_work_slot():
    offenders = []
    for path in sorted(_SRC.rglob("*.cpp")):
        rel = path.relative_to(_SRC).as_posix()
        if rel not in _TASK_SERIALIZED:
            offenders += _tls_offenders(rel, path.read_text())
    assert not offenders, (
        "TLS connections outside the work slot (AGENTS.md 'No on-device concurrency'):\n  " + "\n  ".join(offenders)
    )


_RAW = "bool fetchIt() {\n  WiFiClientSecure c;\n  return c.connect(\"h\", 443);\n}\n"
_GUARDED = (
    "bool fetchIt() {\n  arbiter::WorkSlot slot(10000);\n  if (!slot) return false;\n"
    "  WiFiClientSecure c;\n  return c.connect(\"h\", 443);\n}\n"
)


def test_guard_flags_a_raw_connect_and_accepts_the_slot_forms():
    """Counter-test: the guard still fails a connect with no slot, a slot named only in
    a comment, and a slot taken after the connect; it accepts the scope guard, a
    HeldSlot over acquireWork and the hand-held call."""
    flagged = ["src/agent/x.cpp:1 fetchIt() opens TLS without the work slot"]
    assert _tls_offenders("agent/x.cpp", _RAW) == flagged
    commented = _RAW.replace("  WiFiClientSecure c;", "  // arbiter::WorkSlot slot(10000);\n  WiFiClientSecure c;")
    assert _tls_offenders("agent/x.cpp", commented) == flagged
    late = _RAW.replace(
        "  return c.connect(\"h\", 443);",
        "  bool ok = c.connect(\"h\", 443);\n  arbiter::WorkSlot slot(1);\n  return ok;",
    )
    assert _tls_offenders("agent/x.cpp", late) == flagged
    held = _GUARDED.replace(
        "arbiter::WorkSlot slot(10000);", "nimbus::HeldSlot slot(&arbiter::acquireWork, &arbiter::releaseWork, 1);"
    )
    by_hand = _GUARDED.replace(
        "arbiter::WorkSlot slot(10000);\n  if (!slot) return false;", "if (!arbiter::acquireWork(1)) return false;"
    )
    for ok in (_GUARDED, held, by_hand):
        assert _tls_offenders("agent/x.cpp", ok) == [], ok


def test_archive_tls_runs_only_on_the_poll_task():
    """The tg_archive.cpp exception holds only while its TLS (TgFileSource, used by
    runDownloads, reached from tick) runs inside telegram.cpp's pollTask and closes the
    poll socket first. A second caller, or a download before the close, fails here."""
    arc = (_SRC / "agent" / "tg_archive.cpp").read_text()
    users = [n for n, _, b in _functions(arc) if re.search(r"\bTgFileSource\s+\w+\s*\(", b)]
    assert users == ["runDownloads"], f"TgFileSource opened outside runDownloads: {users}"
    run = _fn_map("agent/tg_archive.cpp")["runDownloads"][1]
    assert 0 <= run.find("beforeTls()") < run.find("TgFileSource "), "the poll socket must close before the download"
    callers = [
        f"{p.relative_to(_SRC).as_posix()}:{n}"
        for p in sorted(_SRC.rglob("*.cpp"))
        for n, _, b in _functions(p.read_text())
        if re.search(r"\b(tgarchive::tick|runDownloads)\s*\(", b) and n not in ("tick", "runDownloads")
    ]
    assert callers == ["agent/telegram.cpp:pollTask"], f"archive downloads reached from: {callers}"
    poll = _fn_map("agent/telegram.cpp")["pollTask"][1]
    assert re.search(r"tgarchive::tick\([^;]*&closePollSocket\)", poll), "tick must close the poll socket first"


def test_allowlisted_helpers_are_called_by_a_slot_holder():
    for (rel, helper), chain in _HELD_BY_CALLER.items():
        fns = _fn_map(rel)
        assert helper in fns, f"allowlisted {helper}() is gone from src/{rel}: drop it from the list"
        callee = helper
        for caller in chain:
            assert caller in fns, f"{caller}() is gone from src/{rel}: update the chain for {helper}()"
            assert re.search(rf"\b{callee}\s*\(", fns[caller][1]), f"{caller}() no longer calls {callee}()"
            callee = caller
        assert _takes_slot(fns[chain[-1]][1]), f"{chain[-1]}() (holding the slot for {helper}) no longer takes it"


def test_relay_dial_and_big_frames_take_the_slot():
    fns = _fn_map("net/relay_client.cpp")
    dialers = [n for n, (_, b) in fns.items() if re.search(r"g_ws->connect\s*\(", b)]
    assert dialers, "the relay dial (g_ws->connect) moved: update this guard"
    for n in dialers:
        assert _slot_before_connect(fns[n][1]), f"relay {n}() dials the cloud without the TLS work slot"

    assert "emitResFrame" in fns, "emitResFrame moved: update this guard"
    emit = fns["emitResFrame"][1]
    sender = fns.get("sendFrame", (0, ""))[1]
    assert re.search(r"\bsendFrame\s*\(", emit) and "frameNeedsSlot(" in sender and "writeBigFrame(" in sender, (
        "emitResFrame must send a body over kRelayBulkBytes through writeBigFrame (the slot)"
    )
    assert re.search(r"frameNeedsSlot\(\s*bodyLen\s*\)", sender), (
        "sendFrame must decide the slot on the body length, never the base64 frame's"
    )
    big = fns.get("writeBigFrame", (0, ""))[1]
    assert "acquireWork(" in big and "writeBulk(" in big and "releaseWork(" in big, (
        "writeBigFrame must take the slot per chunk and give it back between chunks"
    )
    # wsWriteAll is for small frames only: the small-frame sender and sendFrame's small
    # branch (guarded by frameNeedsSlot above; its other wsWriteAll is the bench
    # counter-leg behind FAULT relay_noslot).
    callers = sorted(n for n, (_, b) in fns.items() if n != "wsWriteAll" and re.search(r"\bwsWriteAll\s*\(", b))
    assert callers == ["sendFrame", "wsSendSmall"], f"wsWriteAll has new callers: {callers}"
    assert "relayNoSlot()" in sender, "the unslotted big-frame write must stay behind the bench fault"


def test_relay_never_holds_the_slot_across_the_loopback():
    fns = _fn_map("net/relay_client.cpp")
    for name in ("doLoopback", "replayReq", "handleReq"):
        assert name in fns, f"{name}() moved: update this guard"
        assert not _takes_slot(fns[name][1]), (
            f"{name}() takes the TLS slot: a tunneled handler can run an embed call that "
            "takes the slot itself, so the replay must never hold it"
        )


def test_transport_takes_the_slot_before_the_traced_exchange():
    text = (_SRC / "agent" / "transport_tls.cpp").read_text()
    fn = re.search(r"int execCore\([^)]*\)\s*\{(?P<b>.*?)\n  \}\n", text, re.S)
    assert fn, "transport execCore moved: update this guard"
    b = _strip(fn.group("b"))
    take = re.search(r"\barbiter::WorkSlot\s+slot\s*\(", b)
    t = b.find("tracedExchange(")
    assert take and 0 <= take.start() < t, (
        "the transport must take the work slot (the guard) before its traced exchange"
    )
    assert b.count("slot.release()") == 1, "the slot is released in exactly one place (the trace finish)"
    assert b.find("slot.release()") > t, "the slot is released in the trace finish, after the exchange starts"
    assert "acquireWork(" not in b and "releaseWork(" not in b, (
        "the transport holds the slot by hand again: an exception between the two would keep it (L24)"
    )


def test_retained_guard_measures_what_was_kept():
    text = (_SRC / "agent" / "transport_tls.cpp").read_text()
    fn = re.search(r"bool readBody\(\)\s*\{(?P<b>.*?)\n  \}\n", text, re.S)
    assert fn, "transport readBody moved: update this guard"
    b = fn.group("b")
    assert re.search(r"kept\s*=\s*measureJson\(\*doc\)", b), "the guard must measure the kept document"
    assert "retainedTooLarge(kept)" in b, "the guard must use the host-tested rule (nimbus/exchange_trace.h)"
    # The class: memoryUsage() is a dead measure in ArduinoJson 7, anywhere it is called.
    callers = [
        str(p.relative_to(_REPO))
        for root in (_SRC, _REPO / "lib")
        for p in sorted(root.rglob("*"))
        if p.suffix in (".cpp", ".h") and re.search(r"(\.|->)memoryUsage\s*\(", p.read_text(errors="ignore"))
    ]
    assert callers == [], f"memoryUsage() always returns 0 in ArduinoJson 7: {callers}"


def test_guard_parser_sees_the_relay_functions():
    """Counter-test for the guard itself: the column-0 parser must find the
    functions it reasons about, so a parser regression cannot pass vacuously."""
    fns = _fn_map("net/relay_client.cpp")
    for name in ("doLoopback", "wsWriteAll", "wsSendSmall", "emitResFrame", "handleReq", "runSession"):
        assert name in fns, f"guard parser no longer finds relay {name}()"
    assert len(fns) > 40
