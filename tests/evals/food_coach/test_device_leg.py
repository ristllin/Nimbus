"""Host tests for the device leg: a fake unit stands in for the board; no network, no hardware."""

import json
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import device_leg as dl  # noqa: E402
import fcworld  # noqa: E402

CFG = fcworld.load_config()


class Resp:
    def __init__(self, status=200, body=None, text=""):
        self.status_code, self._body, self.text = status, body, text or json.dumps(body or {})

    def json(self):
        return self._body


class FakeUnit:
    """The routes the leg uses, over a mutable fake state; records every call."""

    def __init__(self):
        self.headers = {}
        self.calls = []
        self.orch = {
            "orchHost": "mistral",
            "directive": "OLD DIRECTIVE",
            "providers": {"anthropic": {"hasKey": True, "orchModel": "old-model"}},
            "usage": {"ceiling": {"limitCents": 2000, "spentCents": 4444, "frozen": True, "active": True}},
        }
        self.usage = {"models": [{"prov": "anthropic", "m": "m1", "pIn": 100, "pOut": 10, "pCr": 0, "pCw": 0}]}
        self.turns = {}
        self.replies = ["The first answer.", "The second answer."]
        self.parked = False
        self.tool_text = 'files_read({"project":"nutrition"}) -> today: 1400'
        self.epi = []
        self.memories = [{"id": "v1", "content": "an unrelated memory of the bench"}]

    def get(self, url, timeout=0):
        path = url.split(self.ip_marker)[1] if self.ip_marker in url else url
        self.calls.append(("GET", path))
        if path == "/api/state":
            return Resp(body={"mode": 1, "fw": "v-test"})
        if path == "/api/orch":
            return Resp(body=self.orch)
        if path.startswith("/api/chat?turn="):
            return Resp(body={"done": True, "reply": self.turns[path.split("=")[1]]})
        if path.startswith("/api/mem/episodic"):
            return Resp(body={"messages": list(self.epi)})
        if path == "/api/usage/models":
            return Resp(body=self.usage)
        if path.startswith("/api/mem/vector"):
            return Resp(body={"entries": list(self.memories) if "offset=0" in path else []})
        return Resp(404)

    ip_marker = "http://10.0.0.9"

    ORCH_FIELDS = {"sysPrompt": "directive", "orchHost": "orchHost", "provPrio": "provPrio"}

    def post(self, url, data=None, files=None, timeout=0):
        path = url.split(self.ip_marker)[1]
        self.calls.append(("POST", path, dict(data or {})))
        handler = {
            "/api/orch": self._orch,
            "/api/test/astool": self._astool,
            "/api/mem/vector": self._vector,
            "/api/chat": self._chat,
        }.get(path)
        return handler(data or {}) if handler else Resp(200, {})

    def _orch(self, data):
        for k, v in data.items():
            if k in self.ORCH_FIELDS:
                self.orch[self.ORCH_FIELDS[k]] = v
            elif k == "orchM_anthropic":
                self.orch["providers"]["anthropic"]["orchModel"] = v
            elif k == "spendCeil":
                self.orch["usage"]["ceiling"].update(limitCents=int(v), frozen=False)
        return Resp(200, {})

    def _astool(self, data):
        args = json.loads(data["body"])["params"]["arguments"]
        self.memories.append({"id": f"v{len(self.memories) + 1}", "content": args["content"]})
        return Resp(200, {})

    def _vector(self, data):
        self.memories = [m for m in self.memories if m["id"] != data["id"]]
        return Resp(200, {"ok": True})

    def _chat(self, data):
        tid = f"w{len(self.turns):08x}"
        row_id = f"m{len(self.epi) + 1:08x}"
        if self.parked:
            err = '{"ok":false,"err":"anthropic: parked: anthropic is out of credit (HTTP 400); no request sent"}'
            self.epi.append({"id": row_id, "kind": "log", "text": err})
            self.turns[tid] = ""
        else:
            self.epi.append({"id": row_id, "text": self.tool_text})
            self.turns[tid] = self.replies[len(self.turns) % len(self.replies)]
        return Resp(200, {"ok": True, "turn": tid})


@pytest.fixture(autouse=True)
def fast(monkeypatch):
    monkeypatch.setattr(dl.time, "sleep", lambda s: None)


def make_unit():
    fake = FakeUnit()
    return dl.Unit("10.0.0.9", "tok-secret", session=fake), fake


def test_only_the_bench_units_and_never_lumi(tmp_path):
    (tmp_path / "unitA_ip.txt").write_text("10.0.0.9\n")
    (tmp_path / "unitA_webtok.txt").write_text("tok\n")
    assert dl.read_unit("A", tmp_path) == ("10.0.0.9", "tok")
    with pytest.raises(dl.UnitRefused):
        dl.read_unit("lumi", tmp_path)
    (tmp_path / "unitL_ip.txt").write_text("192.168.50.76\n")
    (tmp_path / "unitL_webtok.txt").write_text("tok\n")
    with pytest.raises(dl.UnitRefused, match="Lumi"):
        dl.read_unit("L", tmp_path)
    with pytest.raises(dl.UnitRefused):
        dl.Unit("192.168.50.76", "tok", session=FakeUnit())


def test_the_token_rides_a_header_and_never_a_url():
    unit, fake = make_unit()
    assert fake.headers["X-Nimbus-Token"] == "tok-secret"
    unit.get("/api/state")
    assert all("tok-secret" not in str(c) for c in fake.calls)


def test_the_bench_lock_is_exclusive_and_always_released(tmp_path):
    with dl.bench_lock("A", "QW7", tmp_path) as path:
        assert (path / "owner").read_text().startswith("QW7")
        with pytest.raises(dl.UnitRefused, match="held by QW7"):
            with dl.bench_lock("A", "OTHER thing", tmp_path):
                pass
    assert not (tmp_path / "device-unitA").exists()
    with pytest.raises(RuntimeError):
        with dl.bench_lock("A", "QW7", tmp_path):
            raise RuntimeError("leg failed")
    assert not (tmp_path / "device-unitA").exists()


def test_settings_are_set_for_the_leg_and_put_back():
    unit, fake = make_unit()
    saved = dl.snapshot(unit)
    dl.apply_leg_settings(unit, "head-model", "NEW DIRECTIVE")
    assert fake.orch["orchHost"] == "anthropic" and fake.orch["directive"] == "NEW DIRECTIVE"
    assert fake.orch["providers"]["anthropic"]["orchModel"] == "head-model"
    assert fake.orch["usage"]["ceiling"]["limitCents"] == 4444 + 5000, "room under a reached ceiling"
    dl.restore(unit, saved)
    assert fake.orch["orchHost"] == "mistral" and fake.orch["directive"] == "OLD DIRECTIVE"
    assert fake.orch["providers"]["anthropic"]["orchModel"] == "old-model"
    assert fake.orch["usage"]["ceiling"]["limitCents"] == 2000


def test_a_unit_without_an_anthropic_key_is_refused():
    unit, fake = make_unit()
    fake.orch["providers"]["anthropic"]["hasKey"] = False
    with pytest.raises(dl.UnitRefused, match="Anthropic key"):
        dl.snapshot(unit)


def test_the_ceiling_is_left_alone_when_there_is_room():
    unit, fake = make_unit()
    fake.orch["usage"]["ceiling"].update(spentCents=100, frozen=False)
    assert dl.room_under_ceiling(unit) == ""


def test_trace_rows_become_calls_and_results_oldest_first():
    rows = [{"id": "m00000003", "text": "b({}) -> two"}, {"id": "m00000002", "text": "a({})\n\u2192 one\nmore"}]
    assert dl.split_trace(rows) == (["a({})", "b({})"], ["one\nmore", "two"])
    assert dl.split_trace([{"id": "m1", "text": "odd row"}]) == (["odd row"], [""])


def test_a_scenario_replays_earlier_owner_turns_live_and_grades_the_last():
    unit, fake = make_unit()
    scn = fcworld.load_scenarios(["plan-earlier"])[0]
    payload = fcworld.run_payload(scn, CFG, dl.dt.date(2026, 10, 26))
    got = dl.run_scenario(unit, payload)
    owner_turns = [c[2]["text"] for c in fake.calls if c[0:2] == ("POST", "/api/chat")]
    assert owner_turns[:-1] == [h["text"] for h in scn["world"]["history"] if h["role"] == "user"]
    assert owner_turns[-1] == scn["turns"][0]
    assert [h["role"] for h in got["history"]] == ["user", "assistant"] * 3, (
        "the unit's own replies, not the seeded ones"
    )
    turn = got["turns"][0]
    assert (
        turn["owner"] == scn["turns"][0]
        and turn["ok"]
        and turn["tool_calls"] == ["files_read({\"project\":\"nutrition\"})"]
    )
    posted = {c[1] for c in fake.calls if c[0] == "POST"}
    assert {"/api/files/rmproject", "/api/skills/save", "/api/skills/approve", "/api/test/astool"} <= posted


def test_a_turn_that_never_finishes_is_reported_not_hung(monkeypatch):
    unit, fake = make_unit()
    monkeypatch.setattr(fake, "get", lambda url, timeout=0: Resp(body={"done": False, "pending": True}))
    monkeypatch.setattr(dl, "TURN_TIMEOUT_S", 0)
    got = dl.send_turn(unit, "hello", timeout_s=0)
    assert got["ok"] is False and "timed out" in got["error"]


def test_cost_is_the_growth_of_the_units_own_usage_record():
    before = {"models": [{"prov": "anthropic", "m": "claude-sonnet-5-5", "pIn": 1000, "pOut": 100, "pCr": 0, "pCw": 0}]}
    after = {
        "models": [{"prov": "anthropic", "m": "claude-sonnet-5-5", "pIn": 2000, "pOut": 200, "pCr": 10000, "pCw": 0}]
    }
    cost = dl.priced_delta(before, after)
    assert cost["total"] == pytest.approx((1000 * 2.0 + 100 * 10.0 + 10000 * 0.2) / 1e6, abs=1e-6)
    assert dl.priced_delta(after, after)["total"] == 0.0


def test_a_lock_the_caller_already_holds_is_checked_and_left_in_place(tmp_path):
    with pytest.raises(dl.UnitRefused, match="not held"):
        with dl.bench_lock("A", "QW7 food coach", tmp_path, held=True):
            pass
    lock = tmp_path / "device-unitA"
    lock.mkdir()
    (lock / "owner").write_text("QW7 flash 2026-10-26T15:00:00Z\n")
    with dl.bench_lock("A", "QW7 food coach", tmp_path, held=True):
        pass
    assert (lock / "owner").exists(), "the caller releases a lock it took"
    (lock / "owner").write_text("OTHER 2026-10-26T15:00:00Z\n")
    with pytest.raises(dl.UnitRefused, match="not held"):
        with dl.bench_lock("A", "QW7 food coach", tmp_path, held=True):
            pass


def test_a_unit_with_a_cloud_router_key_is_refused_and_one_provider_is_set():
    unit, fake = make_unit()
    fake.orch["provPrio"] = "openai,anthropic,mistral"
    saved = dl.snapshot(unit)
    dl.apply_leg_settings(unit, "head-model", "D")
    assert fake.orch.get("provPrio") == "anthropic"
    dl.restore(unit, saved)
    assert fake.orch.get("provPrio") == "openai,anthropic,mistral"
    fake.orch["providers"]["cumulo"] = {"hasKey": True}
    with pytest.raises(dl.UnitRefused, match="router key"):
        dl.snapshot(unit)


def test_bench_paths_follow_the_environment(monkeypatch, tmp_path):
    monkeypatch.setenv("NIMBUS_DEVICE_SECRETS", str(tmp_path / "s"))
    monkeypatch.setenv("NIMBUS_BENCH_LOCKS", str(tmp_path / "l"))
    assert dl.bench_paths() == (tmp_path / "s", tmp_path / "l")
    monkeypatch.delenv("NIMBUS_DEVICE_SECRETS")
    monkeypatch.delenv("NIMBUS_BENCH_LOCKS")
    secrets, locks = dl.bench_paths()
    assert secrets.name == "devices" and secrets.parent.name == "secrets" and locks.name == "locks"


def test_a_run_starts_from_a_conversation_boundary_and_leaves_no_memory_behind():
    unit, fake = make_unit()
    scn = fcworld.load_scenarios(["qty-vague"])[0]
    payload = fcworld.run_payload(scn, CFG, dl.dt.date(2026, 10, 26))
    dl.run_scenario(unit, payload)
    first_orch = next(c for c in fake.calls if c[:2] == ("POST", "/api/orch"))
    assert first_orch[2].get("clearConv") == "1", "the web chat's own /clear, not a typed message"
    assert any(dl.MEMORY_MARK in m["content"] for m in fake.memories), "the targets were seeded"
    dl.cleanup(unit)
    assert [m["id"] for m in fake.memories] == ["v1"], "only the leg's own memory is removed"
    removed = [c for c in fake.calls if c[:2] == ("POST", "/api/files/rmproject")]
    assert removed and removed[-1][2]["confirm"] == "DELETE"


def test_a_parked_provider_ends_the_leg_with_the_way_out():
    unit, fake = make_unit()
    fake.parked = True
    scn = fcworld.load_scenarios(["plan-stated"])[0]
    payload = fcworld.run_payload(scn, CFG, dl.dt.date(2026, 10, 26))
    with pytest.raises(dl.UnitRefused, match="PARK clear"):
        dl.run_scenario(unit, payload)


def test_a_turn_with_no_reply_carries_the_units_own_reason():
    unit, fake = make_unit()
    fake.parked = True
    got = dl.send_turn(unit, "hello")
    assert got["ok"] is False
    assert dl.turn_error(unit, 0).startswith("anthropic: parked")
