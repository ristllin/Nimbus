#!/usr/bin/env python3
"""Host-only self test of the persona rig: no docker, no network beyond loopback, no keys.

A small in-process server speaks the nimbusd routes the rig uses, so a whole persona runs
end to end here, and the graders are proven able to fail. Runs under `make -C nimbusd test`
and the CI python gates (pytest collects it; `python3 test_persona_rig.py` also works).
"""

import json
import os
import sys
import tempfile
import threading
import unittest
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import grade  # noqa: E402
import persona_rig as pr  # noqa: E402

# Split so no secret-shaped literal sits in the repo (GH013 and gitleaks).
FAKE_KEY = "sk" + "-" + "persona-rig-self-test-key-0123456789"


class FakeNimbusd:
    """The routes the rig drives, with just enough behavior to tell a pass from a fail."""

    def __init__(self):
        self.ring, self.seq, self.log, self.files = [], 0, [], {}
        self.config_posts, self.vectors, self.turn_counter = [], 0, 0
        self.spawn_turns_unresolved = set()
        self.lock = threading.Lock()

    def push(self, role, text):
        self.seq += 1
        self.ring.append({"seq": self.seq, "role": role, "text": text})
        return self.seq

    def run_turn(self, text):
        """Canned behavior keyed on the prompt. The reply is ready as soon as it is polled."""
        turn = self.push("user", text)
        self.log.append("turn spend: ok owner | fake/fake-model in=100 out=10 cR=0 cW=0 n=1 id=fake-model")
        self.turn_counter += 1
        if "sub-agent" in text:
            self.spawn_turns_unresolved.add(turn)
            self.log.append("orchestrator: spawned job0000 -> fake")
            self.push("assistant", "On it.")
            self.push("assistant", "The answer is 55.")
        elif "remember" in text:
            self.vectors += 1
            self.push("assistant", "Saved.")
        elif "broken" in text:
            self.push("assistant", "That didn't finish - try again later.")
        else:
            self.push("assistant", f"You said: {text[:40]}")
        return turn

    def reply_for(self, turn):
        if turn in self.spawn_turns_unresolved:
            return {"reply": "", "pending": True}
        for row in self.ring:
            if row["seq"] > turn and row["role"] == "assistant":
                return {"reply": row["text"], "pending": False}
        return {"reply": "", "pending": True}

    def state(self):
        return {"spend": {"spentCents": self.turn_counter}}

    def orch(self):
        return {
            "providers": {"mistral": {"verify": 1, "vts": 1}},
            "usage": {"sessIn": 100 * self.turn_counter, "sessOut": 10 * self.turn_counter, "turns": self.turn_counter},
        }


def make_handler(fake: FakeNimbusd, token: str):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_a):
            pass

        def _send(self, status, body, ctype="application/json"):
            data = body if isinstance(body, bytes) else (body if isinstance(body, str) else json.dumps(body)).encode()
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def _authorized(self):
            return self.headers.get("X-Nimbus-Token") == token

        def do_GET(self):
            url = urllib.parse.urlparse(self.path)
            q = urllib.parse.parse_qs(url.query)
            if not self._authorized():
                return self._send(401, {"error": "unauthorized"})
            with fake.lock:
                routes = {
                    "/readyz": lambda: "ready",
                    "/api/replies": lambda: self._replies(q),
                    "/api/chat": lambda: fake.reply_for(int(q["turn"][0])),
                    "/api/mem/episodic": lambda: {"messages": []},
                    "/api/log": lambda: "\n".join(fake.log),
                    "/api/lastturn": lambda: f"turn {fake.turn_counter}\n===== 4. TOOL LOOP =====\n"
                    '[tool] files_read {"a":1}\n[result] files_read ok\n',
                        "===== 4. TOOL LOOP =====\n[tool] files_read {\"a\":1}\n[result] files_read ok\n"
                    ),
                    "/api/orch": fake.orch,
                    "/api/state": fake.state,
                    "/api/mem/stats": lambda: {"vectors": fake.vectors},
                }
                if url.path == "/api/files/dl":
                    data = fake.files.get((q["project"][0], q["name"][0]))
                    return self._send(200, data) if data is not None else self._send(404, {"error": "not found"})
                if url.path in routes:
                    return self._send(200, routes[url.path]())
            return self._send(404, {"error": "not found"})

        def _replies(self, q):
            after = int(q.get("after", ["0"])[0])
            return {
                "replies": [r for r in fake.ring if r["seq"] > after],
                "lastSeq": fake.seq,
                "turnInFlight": False,
            }

        def do_POST(self):
            size = int(self.headers.get("Content-Length", 0))
            raw = self.rfile.read(size)
            if not self._authorized():
                return self._send(401, {"error": "unauthorized"})
            url = urllib.parse.urlparse(self.path)
            with fake.lock:
                if url.path == "/api/chat":
                    text = urllib.parse.parse_qs(raw.decode())["text"][0]
                    return self._send(200, {"pending": True, "turn": fake.run_turn(text)})
                if url.path == "/api/message":
                    fake.run_turn(json.loads(raw)["text"])
                    return self._send(202, {"queued": True})
                if url.path == "/api/files/upload":
                    q = urllib.parse.parse_qs(url.query)
                    fake.files[(q["project"][0], q["name"][0])] = raw
                    return self._send(200, {"ok": True})
                if url.path == "/api/loops":
                    return self._send(400, {"ok": False, "error": "Not available on a hosted Nimbus yet: routines"})
                fake.config_posts.append((url.path, raw.decode()))
                refused = "refuse-me" in raw.decode()
                return self._send(400 if refused else 200, {"ok": not refused})

    return Handler


class FakeBackend:
    """Stands in for DockerBackend: a loopback server per persona instead of a container."""

    def __init__(self, fake: FakeNimbusd):
        self.fake, self.started, self.stopped, self.token = fake, [], [], None
        self.server = None

    def start(self, name, env):
        self.started.append((name, env))
        self.token = env["NIMBUSD_WEB_TOKEN"]
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(self.fake, self.token))
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        return self.server.server_address[1]

    def restart(self, _name):
        return self.server.server_address[1]

    def logs(self, _name):
        return f"[nimbusd] boot with key {FAKE_KEY} and token {self.token}\n"

    def stop(self, name):
        self.stopped.append(name)
        if self.server:
            self.server.shutdown()
            self.server.server_close()


def persona(**over):
    base = {
        "name": "unit",
        "keys": {"mistral": "FAKE_MISTRAL"},
        "models": {"head": {"mistral": "fake-model"}, "sub": {"mistral": "fake-sub"}},
        "host": "mistral",
        "timezone": "Europe/London",
        "display_name": "Unit",
        "members": [{"chat": "1001", "name": "Ann", "role": "user"}],
        "monthly_cap_cents": 500,
        "scenarios": [
            {
                "id": "basic",
                "steps": [
                    {
                        "say": "hello there",
                        "settle_s": 0,
                        "expect": [{"turn_resolved": True}, {"reply_contains": "hello"}],
                    },
                ],
            }
        ],
    }
    base.update(over)
    return pr.validate_persona(base)


class Loading(unittest.TestCase):
    def test_refuses_what_would_test_nothing(self):
        bad = [
            ({"name": "x", "scenarios": []}, "no scenarios"),
            ({"name": "bad name!", "scenarios": [{"id": "a", "steps": [{"sleep": 0}]}]}, "bad persona name"),
            ({"name": "x", "scenarios": [{"id": "a", "steps": []}]}, "no steps"),
            (
                {
                    "name": "x",
                    "scenarios": [{"id": "a", "steps": [{"sleep": 0}]}, {"id": "a", "steps": [{"sleep": 0}]}],
                },
                "repeated",
            ),
            ({"name": "x", "scenarios": [{"id": "a", "steps": [{"sleep": 0, "say": "hi"}]}]}, "exactly one"),
            (
                {
                    "name": "x",
                    "scenarios": [{"id": "a", "steps": [{"say": "hi", "expect": [{"reply_contians": "typo"}]}]}],
                },
                "unknown assertion",
            ),
        ]
        for p, why in bad:
            with self.assertRaises(pr.PersonaError, msg=why) as ctx:
                pr.validate_persona(json.loads(json.dumps(p)))
            self.assertIn(why, str(ctx.exception))

    def test_matrix_expands_vars_in_keys_and_values_and_keeps_repeat_counter(self):
        doc = {
            "vars": {"fact": "F"},
            "matrix": [{"m": "one", "host": "mistral"}, {"m": "two", "host": "zai"}],
            "persona": {
                "name": "p-${m}",
                "keys": {"${host}": "K"},
                "scenarios": [{"id": "s", "steps": [{"say": "${fact} note $i", "repeat": 2, "pad": 100, "expect": [{"reply_nonempty": True}]}]}],
            },
        }
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "m.json"
            path.write_text(json.dumps(doc))
            ps = pr.load_personas(str(path))
        self.assertEqual([p["name"] for p in ps], ["p-one", "p-two"])
        self.assertEqual(ps[1]["keys"], {"zai": "K"})
        steps = ps[0]["scenarios"][0]["steps"]
        self.assertEqual(len(steps), 2)
        self.assertTrue(steps[0]["say"].startswith("F note 1"))
        self.assertTrue(steps[1]["say"].startswith("F note 2"))
        self.assertGreater(len(steps[0]["say"]), 100)
        self.assertNotEqual(steps[0]["say"][10:], steps[1]["say"][10:], "filler must differ between turns")

    def test_missing_var_is_refused(self):
        doc = {
            "matrix": [{"a": "1"}],
            "persona": {"name": "p", "note": "${nope}", "scenarios": [{"id": "s", "steps": [{"sleep": 0}]}]},
        }
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "m.json"
            path.write_text(json.dumps(doc))
            with self.assertRaises(pr.PersonaError):
                pr.load_personas(str(path))

    def test_shipped_persona_files_load(self):
        files = sorted((HERE / "personas").glob("*.json"))
        self.assertTrue(files, "the rig ships at least one persona")
        for f in files:
            self.assertTrue(pr.load_personas(str(f)), f.name)


class LoadingGuards(unittest.TestCase):
    def test_a_bare_dollar_is_text_and_only_the_counter_expands(self):
        doc = {
            "matrix": [{"m": "a"}],
            "persona": {
                "name": "p-${m}",
                "scenarios": [
                    {"id": "s", "steps": [{"say": "costs $5, note $i", "repeat": 1, "expect": [{"reply_regex": "x$"}]}]}
                ],
            },
        }
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "m.json"
            path.write_text(json.dumps(doc))
            step = pr.load_personas(str(path))[0]["scenarios"][0]["steps"][0]
        self.assertEqual(step["say"], "costs $5, note 1")
        self.assertEqual(step["expect"], [{"reply_regex": "x$"}])

    def test_a_turn_without_expect_and_a_path_shaped_id_are_refused(self):
        bad = [
            {"name": "x", "scenarios": [{"id": "s", "steps": [{"say": "hi"}]}]},
            {"name": "x", "scenarios": [{"id": "../../x", "steps": [{"sleep": 0}]}]},
        ]
        for p in bad:
            with self.assertRaises(pr.PersonaError):
                pr.validate_persona(p)

    def test_a_run_that_would_prove_less_than_it_says_is_refused(self):
        p = persona()
        for personas, only in (([], None), ([p, p], None), ([p], {"tpyo"})):
            with self.assertRaises(pr.PersonaError):
                pr.run_all(personas, FakeBackend(FakeNimbusd()), {}, Path("."), {"only": only})


class Ctx:
    """A grade context with fixed answers."""

    def __init__(self, files=None, api=None):
        self.files, self.api = files or {}, api or {}

    def file_bytes(self, project, name):
        if (project, name) in self.files:
            return 200, self.files[(project, name)]
        return 404, "not found"

    def get(self, path):
        return (200, json.dumps(self.api[path])) if path in self.api else (404, "not found")


# One passing and one failing case for EVERY assertion. A new assertion with no row here
# fails test_every_assertion_has_cases: a check that cannot fail is a defect.
REC = {
    "reply": "The answer is 51.",
    "replies": ["The answer is 51."],
    "tools": [{"name": "files_read"}],
    "log": "fold(owner): ok on mistral",
    "http": {"status": 200, "body": '{"ok":true}'},
    "usage_delta": {"in": 100, "out": 10},
    "turn_resolved": True,
}
CTX = Ctx(files={("p", "n"): "hello"}, api={"/s": {"a": {"b": 3}, "h": "x"}})
CASES = {
    "reply_nonempty": (True, {**REC, "reply": "", "replies": []}, True),
    "no_error_reply": (True, {**REC, "reply": "That didn't finish.", "replies": []}, True),
    "reply_contains": ("answer", REC, "banana"),
    "reply_not_contains": ("banana", REC, "answer"),
    "reply_regex": (r"\b51\b", REC, r"\b52\b"),
    "reply_not_regex": (r"\b52\b", REC, r"\b51\b"),
    "tool_called": ("files_read", REC, "memory_write"),
    "tool_not_called": ("memory_write", REC, "files_read"),
    "no_fabricated_action": (True, {**REC, "reply": "I've emailed it.", "replies": []}, True),
    "log_regex": (r"fold\(owner\): ok", REC, "no such line"),
    "log_not_regex": ("no such line", REC, "fold"),
    "http_status": (200, REC, 400),
    "body_contains": ('"ok":true', REC, "nope"),
    "body_not_contains": ("nope", REC, '"ok":true'),
    "body_regex": ("ok.:true", REC, "nope"),
    "turn_resolved": (True, REC, False),
    "tokens_max": (1000, REC, 5),
    "file_contains": ({"project": "p", "name": "n", "text": "hello"}, REC, {"project": "p", "name": "n", "text": "x"}),
    "file_absent": ({"project": "p", "name": "other"}, REC, {"project": "p", "name": "n"}),
    "api_min": ({"path": "/s", "field": "a.b", "min": 3}, REC, {"path": "/s", "field": "a.b", "min": 4}),
    "api_equals": ({"path": "/s", "field": "h", "value": "x"}, REC, {"path": "/s", "field": "h", "value": "y"}),
}


class Grading(unittest.TestCase):
    def test_every_assertion_has_cases(self):
        self.assertEqual(set(CASES), set(grade.ASSERTIONS))

    def test_every_assertion_passes_and_fails(self):
        for name, (good_arg, bad_rec_or_rec, bad_arg) in CASES.items():
            pass_rec = REC
            ok = grade.grade_step(pass_rec, [{name: good_arg}], CTX)[0]
            self.assertTrue(ok["pass"], f"{name} should pass: {ok}")
            bad = grade.grade_step(bad_rec_or_rec, [{name: bad_arg}], CTX)[0]
            self.assertFalse(bad["pass"], f"{name} should fail: {bad}")

    def test_engine_filler_and_cut_answers_are_error_replies(self):
        for text in ("Done.", "placeholder", "That answer came back in a form that could not be read, so nothing."):
            self.assertFalse(grade.no_error_reply({"reply": text}, True, None)[0], text)
        self.assertTrue(grade.no_error_reply({"reply": "Done. I saved vnc/notes.txt."}, True, None)[0])

    def test_a_backed_claim_is_not_a_fabrication(self):
        rec = {"reply": "I've emailed it.", "tools": [{"name": "gmail_send"}]}
        self.assertTrue(grade.no_fabricated_action(rec, True, None)[0])

    def test_no_trace_and_no_usage_fail_instead_of_passing(self):
        no_trace = {**REC, "tools": None}
        for item in ({"tool_not_called": "x"}, {"tool_called": "x"}, {"no_fabricated_action": True}):
            self.assertFalse(grade.grade_step(no_trace, [item], CTX)[0]["pass"], item)
        self.assertFalse(grade.grade_step({}, [{"tokens_max": 5}], CTX)[0]["pass"])

    def test_file_absent_needs_a_real_404(self):
        class Down:
            def file_bytes(self, *_a):
                return 0, "connection refused"

        self.assertFalse(grade.file_absent({}, {"name": "n"}, Down())[0])

    def test_a_check_that_raises_is_a_failure(self):
        out = grade.grade_step(REC, [{"file_contains": {"no": "name"}}], CTX)[0]
        self.assertFalse(out["pass"])
        self.assertIn("check raised", out["seen"])


class Helpers(unittest.TestCase):
    def test_overlap_tail_keeps_repeated_lines(self):
        old = ["a", "b", "b"]
        self.assertEqual(pr.overlap_tail(old, ["a", "b", "b", "b", "c"]), ["b", "c"])
        self.assertEqual(pr.overlap_tail(old, ["b", "b", "x"]), ["x"], "a rotated log still overlaps")
        self.assertEqual(pr.overlap_tail(old, ["z"]), ["z"], "no overlap keeps everything")
        self.assertEqual(pr.overlap_tail(old, old), [])

    def test_tools_come_from_the_anatomy_section_only(self):
        text = "[tool] not_this {}\n===== 4. TOOL LOOP =====\n[tool] files_read {\"a\":1}\n[result] files_read hi\n"
        self.assertEqual(pr.tools_from_anatomy(text), [{"name": "files_read", "args": '{"a":1}', "result": "hi"}])
        self.assertEqual(pr.tools_from_anatomy("no section"), [])

    def test_spend_lines_parse(self):
        line = "turn spend: ok owner | mistral/mistral-small in=7973 out=77 cR=5 cW=0 n=1 id=mistral-small-latest"
        self.assertEqual(
            pr.spend_lines(line), [{"provider": "mistral", "in": 7973, "out": 77, "cache_read": 5, "cache_write": 0}]
        )

    def test_scrubber_removes_known_and_key_shaped_values(self):
        s = pr.Scrubber(["abcdef123456"])
        self.assertEqual(s("x abcdef123456 y"), "x [REDACTED] y")
        self.assertNotIn(FAKE_KEY, s(f"key={FAKE_KEY}"))

    def test_solid_png_is_a_png(self):
        png = pr.solid_png((1, 2, 3), size=4)
        self.assertTrue(png.startswith(b"\x89PNG\r\n\x1a\n"))
        self.assertTrue(png.endswith(b"IEND\xaeB`\x82"))

    def test_env_file_reader(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "e"
            path.write_text("# c\nexport A=1\nB='two'\n\nnot a line\n")
            self.assertEqual(pr.read_env_file(str(path)), {"A": "1", "B": "two"})


class Docker(unittest.TestCase):
    def test_secrets_never_reach_argv_and_port_is_loopback(self):
        calls = []

        class R:
            returncode, stderr = 0, ""
            stdout = "127.0.0.1:49999\n"

        def runner(argv, **kw):
            calls.append((argv, kw.get("env", {})))
            return R()

        b = pr.DockerBackend("nimbusd:x", docker="docker", runner=runner)
        port = b.start("nimbusrig-a", {"NIMBUSD_WEB_TOKEN": "tok-value-1", "OPENAI_API_KEY": FAKE_KEY})
        self.assertEqual(port, 49999)
        run_argv, run_env = calls[0]
        self.assertNotIn(FAKE_KEY, " ".join(run_argv))
        self.assertNotIn("tok-value-1", " ".join(run_argv))
        self.assertEqual(run_env["OPENAI_API_KEY"], FAKE_KEY)
        self.assertIn("127.0.0.1:0:8787", run_argv)
        self.assertIn("nimbusrig-a:/data", run_argv, "each persona gets its own volume")

    def test_failed_docker_run_raises(self):
        class R:
            returncode, stderr, stdout = 1, "no such image", ""

        b = pr.DockerBackend("nimbusd:x", docker="docker", runner=lambda *a, **k: R())
        with self.assertRaises(RuntimeError):
            b.start("n", {})


class EndToEnd(unittest.TestCase):
    def run_persona(self, p, env=None):
        fake = FakeNimbusd()
        backend = FakeBackend(fake)
        with tempfile.TemporaryDirectory() as d:
            ev = Path(d)
            results = pr.run_all([p], backend, {"FAKE_MISTRAL": FAKE_KEY} if env is None else env, ev)
            texts = {str(f.relative_to(ev)): f.read_text() for f in ev.rglob("*") if f.is_file()}
        return results[0], fake, backend, texts

    def test_a_persona_runs_and_configures_like_the_web_app(self):
        summary, fake, backend, texts = self.run_persona(persona())
        self.assertTrue(summary["pass"], summary)
        posts = [f"{path} {urllib.parse.unquote(body)}" for path, body in fake.config_posts]
        for want in (
            "/api/orch orchM_mistral=fake-model",
            "/api/orch subM_mistral=fake-sub",
            "/api/orch orchHost=mistral",
            "/api/orch spendCeil=500",
            "/api/config devTz=Europe/London",
            "/api/config devName=Unit",
            "/api/telegram/add id=1001&name=Ann",
            "/api/tenant id=1001&role=user",
        ):
            self.assertIn(want, posts)
        self.assertTrue(any(p.startswith("/api/orch mistKey=") for p in posts))
        self.assertEqual(len(backend.stopped), 1, "the container is removed at the end")
        rec = json.loads(texts["unit/basic/step-00.json"])
        self.assertEqual(rec["reply"], "You said: hello there")
        self.assertEqual(rec["tools"][0]["name"], "files_read")
        self.assertEqual(rec["spend_lines"][0]["provider"], "fake")

    def test_no_secret_reaches_any_evidence_file(self):
        fake = FakeNimbusd()
        backend = FakeBackend(fake)
        with tempfile.TemporaryDirectory() as d:
            ev = Path(d)
            rc = pr.run_and_write([persona()], backend, {"FAKE_MISTRAL": FAKE_KEY}, ev)
            self.assertEqual(rc, 0)
            self.assertTrue((ev / "run-summary.json").is_file())
            texts = {str(f.relative_to(ev)): f.read_text() for f in ev.rglob("*") if f.is_file()}
        token = backend.started[0][1]["NIMBUSD_WEB_TOKEN"]
        self.assertGreater(len(texts), 3)
        self.assertTrue(any(FAKE_KEY in body for _path, body in fake.config_posts), "the key really was posted")
        for name, body in texts.items():
            self.assertNotIn(FAKE_KEY, body, name)
            self.assertNotIn(token, body, name)

    def test_a_missing_key_name_stops_before_any_container(self):
        summary, _f, backend, _t = self.run_persona(persona(), env={})
        self.assertFalse(summary["pass"])
        self.assertIn("FAKE_MISTRAL", summary["error"])
        self.assertEqual(backend.started, [])

    def test_a_rejected_key_stops_the_persona(self):
        class Rejecting(FakeNimbusd):
            def orch(self):
                return {**super().orch(), "providers": {"mistral": {"verify": 0, "vts": 1}}}

        fake = Rejecting()
        with tempfile.TemporaryDirectory() as d:
            results = pr.run_all([persona()], FakeBackend(fake), {"FAKE_MISTRAL": FAKE_KEY}, Path(d))
        self.assertFalse(results[0]["pass"])
        self.assertFalse(any("hello there" in row["text"] for row in fake.ring))

    def test_a_refused_turn_fails_even_if_its_checks_would_not(self):
        rec = {"send": [500, "boom"], "timed_out": True}
        self.assertEqual([c["check"] for c in pr._implicit_checks(rec)], ["turn_sent", "turn_finished"])
        self.assertEqual(pr._implicit_checks({"send": [200, ""], "timed_out": False}), [])

    def test_a_persona_where_nothing_ran_does_not_pass(self):
        with tempfile.TemporaryDirectory() as d:
            ev = Path(d)
            p = persona(scenarios=[{"id": "a", "steps": [{"sleep": 0}]}, {"id": "b", "steps": [{"sleep": 0}]}])
            q = persona(name="other", scenarios=[{"id": "c", "steps": [{"sleep": 0}]}])
            rs = pr.run_all([p, q], FakeBackend(FakeNimbusd()), {"FAKE_MISTRAL": FAKE_KEY}, ev, {"only": {"c"}})
        self.assertFalse(next(r for r in rs if r["persona"] == "unit")["pass"], "no scenario ran for it")

    def test_failing_logs_still_remove_the_container(self):
        class Broken(FakeBackend):
            def logs(self, _name):
                raise OSError("docker gone")

        backend = Broken(FakeNimbusd())
        with tempfile.TemporaryDirectory() as d:
            results = pr.run_all([persona()], backend, {"FAKE_MISTRAL": FAKE_KEY}, Path(d))
        self.assertFalse(results[0]["pass"])
        self.assertEqual(len(backend.stopped), 1)

    def test_a_failed_assertion_fails_the_persona(self):
        p = persona(
            scenarios=[{"id": "s", "steps": [{"say": "hi", "settle_s": 0, "expect": [{"reply_contains": "banana"}]}]}]
        )
        summary, _f, _b, _t = self.run_persona(p)
        self.assertFalse(summary["pass"])
        self.assertEqual(summary["scenarios"][0]["failed"][0]["check"], "reply_contains")

    def test_an_error_reply_is_caught(self):
        p = persona(
            scenarios=[
                {"id": "s", "steps": [{"say": "broken turn", "settle_s": 0, "expect": [{"no_error_reply": True}]}]}
            ]
        )
        self.assertFalse(self.run_persona(p)[0]["pass"])

    def test_a_refused_setting_stops_the_persona(self):
        p = persona(directive="refuse-me")
        summary, fake, _b, _t = self.run_persona(p)
        self.assertFalse(summary["pass"])
        self.assertFalse(summary["configured"])
        self.assertFalse(any("hello there" in row["text"] for row in fake.ring), "nothing was spent after a refusal")

    def test_a_turn_that_never_resolves_is_recorded_and_graded_from_the_ring(self):
        p = persona(
            scenarios=[
                {
                    "id": "s",
                    "steps": [
                        {
                            "say": "start a sub-agent",
                            "await_reply_regex": r"\b55\b",
                            "settle_s": 0,
                            "timeout_s": 10,
                            "expect": [
                                {"turn_resolved": False},
                                {"reply_regex": r"\b55\b"},
                                {"log_regex": "spawned job"},
                            ],
                        }
                    ],
                }
            ]
        )
        summary, _f, _b, texts = self.run_persona(p)
        self.assertTrue(summary["pass"], summary)
        rec = json.loads(texts["unit/s/step-00.json"])
        self.assertFalse(rec["turn_resolved"])
        self.assertFalse(rec["timed_out"], "the wait ended on the ring, not the deadline")

    def test_member_turn_upload_http_and_restart_steps(self):
        p = persona(
            scenarios=[
                {
                    "id": "s",
                    "steps": [
                        {
                            "say": "please remember this",
                            "as": "1001",
                            "settle_s": 0,
                            "expect": [
                                {"reply_contains": "Saved"},
                                {"api_min": {"path": "/api/mem/stats", "field": "vectors", "min": 1}},
                            ],
                        },
                        {
                            "upload": {"project": "chat", "name": "a.png", "png_rgb": [1, 2, 3]},
                            "expect": [{"http_status": 200}],
                        },
                        {
                            "upload": {"project": "chat", "name": "b.txt", "text": "plain words"},
                            "expect": [
                                {"http_status": 200},
                                {"file_contains": {"project": "chat", "name": "b.txt", "text": "plain words"}},
                            ],
                        },
                        {
                            "http": {"method": "POST", "path": "/api/loops", "form": {"action": "create"}},
                            "expect": [{"http_status": 400}, {"body_contains": "routines"}],
                        },
                        {"restart": True},
                        {"say": "still there", "settle_s": 0, "expect": [{"reply_contains": "still there"}]},
                    ],
                }
            ]
        )
        summary, _f, _b, _t = self.run_persona(p)
        self.assertTrue(summary["pass"], summary)


if __name__ == "__main__":
    os.environ.setdefault("PYTHONDONTWRITEBYTECODE", "1")
    unittest.main(verbosity=1)
