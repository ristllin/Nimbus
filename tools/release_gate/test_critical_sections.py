"""Host tests for the spinlock scan (check_critical_sections.py).

Run: python3 -m pytest tools/release_gate
"""

import os
import re

import check_critical_sections as cs

HERE = os.path.dirname(os.path.abspath(__file__))

# The shape of the 29 Oct 2026 crash (confirmed by decoding its frames against the v4.6.1 build), as v4.6.1 shipped it.
V461_SINK = """
static void orchDeviceSink(const nimbus::orch::ValidatedAction& a) {
  using nimbus::orch::ActionKind;
  if (a.kind == ActionKind::Reboot) { g_rebootPending = true; return; }
  net::ConfigLockGuard lk;
  if (a.kind == ActionKind::Lights) {
    agent::alogf("devsink: lights -> %s", a.lightsOff ? "off" : "full");
    g_lightsOff = a.lightsOff;
  }
}
"""


def lines(src):
    return [f.line for f in cs.scan_source("x.cpp", src)]


def test_the_crash_shape_fails_on_its_line():
    assert lines(V461_SINK) == [7]


def test_the_fix_passes():
    assert (
        lines(
            """
void sink(const A& a) {
  if (a.kind == 1) agent::alogf("devsink");
  net::ConfigLockGuard lk;
  g_lightsOff = a.off;
}
"""
        )
        == []
    )


def test_the_tree_is_clean():
    assert cs.main([]) == 0


def test_every_forbidden_call_fails_in_every_section_shape():
    calls = [
        'alogf("x");',
        'agent::alog("x");',
        'alogif("x");',
        'alogw("x");',
        'agent::alogc(kNvs, "x");',
        'agent::alogcf(kNvs, "x %d", 1);',
        "agent::alogLevelf(lvl, fmt, 1);",
        "agent::logPersist(red, nullptr, lvl);",
        "logSerial(red);",
        'Serial.print("x");',
        'log_e("x");',
        'ESP_LOGW(TAG, "x");',
        "fwrite(b, 1, n, f);",
        'SD.open("/x");',
        'LittleFS.remove("/x");',
        "store::setVerify(a, b);",
        "memory::captureEvent(a, b);",
        'captureEvent("dream", row);',
        "errlog::append(a, b);",
        'hlog::log("x");',
        'agent::hlog::logf("x %d", 1);',
        'nimbus::agent::hlog::logf("x %d", 1);',
        'hlog :: logf("x");',
        "xSemaphoreTake(m, 10);",
        "vTaskDelay(1);",
        "delay(2);",
        'nvs_set_str(h, "k", "v");',
    ]
    shapes = [
        "void f() {{ portENTER_CRITICAL(&m); {0} portEXIT_CRITICAL(&m); }}",
        "void f() {{ portENTER_CRITICAL_ISR(&m); {0} portEXIT_CRITICAL_ISR(&m); }}",
        "void f() {{ portENTER_CRITICAL_SAFE(&m); {0} portEXIT_CRITICAL_SAFE(&m); }}",
        "void f() {{ lockConfig(); {0} unlockConfig(); }}",
        "void f() {{ net::ConfigLockGuard lk; {0} }}",
    ]
    for call in calls:
        for shape in shapes:
            assert lines(shape.format(call)) == [1], shape.format(call)


def test_every_log_entry_point_in_agent_log_is_forbidden():
    """A new entry point in agent_log.h must fail the scan the day it lands, not when it first crashes."""
    with open(os.path.join(cs.REPO, "src/sys/agent_log.h"), encoding="utf-8") as f:
        src = f.read()
    sinks = src[src.index("inline void logSerial") :]
    names = re.findall(r"^\s*inline\s+\w+\s+(\w+)\s*\(", sinks, re.M)
    assert {"alog", "alogf", "alogcf", "alogLevelf", "logPersist", "logSerial"} <= set(names)
    for name in names:
        assert lines(f"void f() {{ net::ConfigLockGuard lk; agent::{name}(a); }}") == [1], name


def test_the_harness_log_seam_is_forbidden():
    """hlog::log and hlog::logf reach the same durable write as alogf (the device's sink calls alog),
    so one under a spinlock aborts the same way. Without the hlog alternative neither is flagged."""
    with open(os.path.join(cs.REPO, "lib/harness/include/nimbus/harness/log.h"), encoding="utf-8") as f:
        src = f.read()
    names = re.findall(r"^inline\s+void\s+(log\w*)\s*\(", src, re.M)
    assert {"log", "logf"} <= set(names)
    for name in names:
        assert lines(f'void f() {{ net::ConfigLockGuard lk; agent::hlog::{name}("x"); }}') == [1], name
        assert lines(f'void f() {{ portENTER_CRITICAL(&m); hlog::{name}("x"); portEXIT_CRITICAL(&m); }}') == [1], name
    # Outside a section it is the harness's ordinary logging.
    assert lines('void f() { agent::hlog::logf("x"); net::ConfigLockGuard lk; g = 1; }') == []


def test_a_guard_ends_with_its_scope():
    assert lines('void f() {\n  { net::ConfigLockGuard lk; x = 1; }\n  alogf("after");\n}\n') == []
    assert lines('void f() {\n  { net::ConfigLockGuard lk; { alogf("inner"); } }\n}\n') == [2]


def test_a_section_ends_at_its_own_exit():
    assert (
        lines('void f() {\n  portENTER_CRITICAL(&m);\n  x = 1;\n  portEXIT_CRITICAL(&m);\n  alogf("after");\n}\n') == []
    )
    # Another mux's exit does not close it.
    assert lines(
        'void f() {\n  portENTER_CRITICAL(&m);\n  portEXIT_CRITICAL(&n);\n  alogf("in");\n  portEXIT_CRITICAL(&m);\n}\n'
    ) == [4]


def test_an_early_exit_in_a_block_does_not_end_the_section():
    src = """
void f() {
  portENTER_CRITICAL(&m);
  if (busy) { portEXIT_CRITICAL(&m); return; }
  alogf("still inside");
  portEXIT_CRITICAL(&m);
}
"""
    assert lines(src) == [5]


def test_comments_and_strings_are_not_calls():
    src = """
void f() {
  net::ConfigLockGuard lk;
  // alogf("a comment");
  /* Serial.print("a block comment"); */
  const char* s = "alogf(not a call)";
  g = s;
}
"""
    assert lines(src) == []


def test_a_name_that_only_ends_like_one_is_not_a_call():
    assert lines("void f() { net::ConfigLockGuard lk; my_delay(1); dialog(2); xalogf(3); xhlog::log(4); }") == []


def test_the_gate_runs_it():
    with open(os.path.join(HERE, "run_gate.py"), encoding="utf-8") as f:
        assert "check_critical_sections.main" in f.read()
