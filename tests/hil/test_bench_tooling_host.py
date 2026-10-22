"""Host tests for the bench tooling: the tg-bot lock is always given back, the
provisioning script reads the HIL suite's Wi-Fi names first, and the web-API settings
filler restores what it touched (over a fake unit)."""

from __future__ import annotations

import os
import sys
from types import SimpleNamespace

import pytest

import bench_lock
import prov_board

pytestmark = pytest.mark.host

sys.path.insert(1, os.path.join(os.path.dirname(os.path.abspath(__file__)), "storm"))


# ---- the bench locks ------------------------------------------------------------------


def test_the_lock_is_released_when_the_leg_fails(tmp_path, monkeypatch):
    monkeypatch.setenv(bench_lock.LOCK_ROOT_ENV, str(tmp_path))
    with pytest.raises(RuntimeError):
        with bench_lock.bench_lock("tg-bot", "unit test"):
            assert (tmp_path / "tg-bot" / "owner").read_text().startswith("unit test")
            raise RuntimeError("the console wedged")
    assert not (tmp_path / "tg-bot").exists()


def test_a_held_lock_skips_with_its_holder(tmp_path, monkeypatch):
    monkeypatch.setenv(bench_lock.LOCK_ROOT_ENV, str(tmp_path))
    (tmp_path / "tg-bot").mkdir()
    (tmp_path / "tg-bot" / "owner").write_text("another runner")
    with pytest.raises(pytest.skip.Exception, match="another runner"):
        with bench_lock.bench_lock("tg-bot", "unit test", wait_s=0.0, retry_s=0.0):
            pass
    assert (tmp_path / "tg-bot").exists(), "someone else's lock is never removed"


def test_no_lock_root_means_no_lock(monkeypatch):
    monkeypatch.delenv(bench_lock.LOCK_ROOT_ENV, raising=False)
    with bench_lock.bench_lock("tg-bot", "unit test") as path:
        assert path is None


def test_the_bot_token_is_cleared_and_the_lock_released_whatever_happens(tmp_path, monkeypatch):
    monkeypatch.setenv(bench_lock.LOCK_ROOT_ENV, str(tmp_path))
    posts = []

    def post(form):
        posts.append(form)
        return SimpleNamespace(status_code=200)

    with pytest.raises(AssertionError):
        with bench_lock.bot_on_unit(post, "tok", "1", "unit test"):
            assert False, "the leg failed"
    assert posts == [{"tgToken": "tok", "tgAllow": "1"}, {"clr_tgToken": "1"}]
    assert not (tmp_path / "tg-bot").exists()


def test_a_unit_that_will_not_clear_still_gives_the_lock_back(tmp_path, monkeypatch):
    monkeypatch.setenv(bench_lock.LOCK_ROOT_ENV, str(tmp_path))
    monkeypatch.setattr(bench_lock.time, "sleep", lambda s: None)
    calls = []

    def post(form):
        calls.append(form)
        if "clr_tgToken" in form:
            raise ConnectionError("the unit is gone")
        return SimpleNamespace(status_code=200)

    with bench_lock.bot_on_unit(post, "tok", "1", "unit test"):
        pass
    assert calls.count({"clr_tgToken": "1"}) == 3
    assert not (tmp_path / "tg-bot").exists()


# ---- provisioning: one set of Wi-Fi names -------------------------------------------------


def test_wifi_names_prefer_the_hil_suite_names():
    env = {"NIMBUS_TEST_STA_SSID": "hil", "NIMBUS_TEST_STA_PASS": "p1"}
    dotenv = {"TESTING_WIFI_SSID": "legacy", "TESTING_WIFI_PASS": "p2", "NIMBUS_TEST_STA_SSID": "dot"}
    assert prov_board.wifi_credentials(env, dotenv) == ("hil", "p1", "NIMBUS_TEST_STA_SSID")
    assert prov_board.wifi_credentials({}, {"NIMBUS_TEST_STA_SSID": "d", "NIMBUS_TEST_STA_PASS": "q"})[0] == "d"
    assert prov_board.wifi_credentials({}, dotenv) == ("legacy", "p2", "TESTING_WIFI_SSID")
    assert prov_board.wifi_credentials({}, {"NIMBUS_TEST_STA_SSID": "no password"}) is None


# ---- the settings filler restores what it touched ------------------------------------------


class FakeUnit:
    """The web API of a unit with a small settings store: names, directive, dials and
    the scratchpad cost entries; a string that no longer fits is a failed write."""

    def __init__(self, free=40):
        self.free, self.fails = free, 0
        self.orch = {"directive": "", "directiveDefault": "dflt", "loopMaxCnt": 5, "capProbeH": 12}
        self.names, self.scratch = {}, {"short": ["kept"], "mid": [], "long": []}

    def nvs(self):
        return {"free": self.free * 32, "fails": self.fails}

    def get_json(self, path):
        if path == "/api/orch":
            return dict(self.orch)
        if path == "/api/telegram":
            return {"allow": [{"id": k, "name": v} for k, v in self.names.items()]}
        if path == "/api/mem/scratchpad":
            return dict(self.scratch)
        raise AssertionError(path)

    def post(self, path, data):
        if path == "/api/telegram/rename":
            if data["name"]:
                if self.free < 8:
                    self.fails += 1
                    return 200, "{}"
                self.names[data["id"]], self.free = data["name"], self.free - 8
            else:
                self.names.pop(data["id"], None)
        elif path == "/api/mem/scratchpad":
            if data["action"] == "clear":
                self.scratch[data["tier"]] = []
            elif self.free >= 12:
                self.scratch[data["tier"]].append(data["text"])
                self.free -= 12
            else:
                self.fails += 1
        else:
            for k, v in data.items():
                self.orch["directive" if k == "sysPrompt" else k] = v
            self.free = max(0, self.free - 1)
        return 200, "{}"


def test_fill_and_restore_round_trip(monkeypatch):
    import settings_fill as sf

    monkeypatch.setattr(sf.time, "sleep", lambda s: None)
    unit = FakeUnit()
    api = sf.Api("192.0.2.1", "t" * 24, log=lambda s: None)
    api.get_json, api.post, api.nvs = unit.get_json, unit.post, unit.nvs
    snap = sf.take_snapshot(api)
    assert snap["emptyTiers"] == ["long", "mid"]
    n = sf.fill(api, snap, target_free=0)
    assert n["free"] == 0
    assert snap["padIds"] and unit.names
    assert sf.restore(api, snap) == []
    assert unit.names == {} and unit.scratch == {"short": ["kept"], "mid": [], "long": []}
    assert unit.orch["directive"] == "" and unit.orch["loopMaxCnt"] == 5 and unit.orch["capProbeH"] == 12
