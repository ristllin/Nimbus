#!/usr/bin/env python3
"""Host-only self test of the persona rig: no docker, no network beyond loopback, no keys.

A small in-process server speaks the nimbusd routes the rig uses, so a whole persona runs
end to end here, and the graders are proven able to fail. Runs under `make -C nimbusd test`
and the CI python gates (pytest collects it; `python3 test_persona_rig.py` also works).
"""

import io
import json
import os
import re
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

pr.CONFIG_PAUSE_S = 0
pr.BUSY_POLL_S = 0.01

# Split so no secret-shaped literal sits in the repo (GH013 and gitleaks).
FAKE_KEY = "sk" + "-" + "persona-rig-self-test-key-0123456789"


class FakeNimbusd:
    """The routes the rig drives, with just enough behavior to tell a pass from a fail."""

    def __init__(self):
        self.ring, self.seq, self.log, self.files = [], 0, [], {}
        self.config_posts, self.vectors, self.turn_counter = [], 0, 0
        self.spawn_turns_unresolved = set()
        self.settings = {}
        self.transient_failures = 0
        self.last_turn_failed = False
        self.busy_reads = 0  # how many engine reads answer 503, as after a real restart
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
        self.last_turn_failed = False
        if "flaky" in text and self.transient_failures > 0:
            self.transient_failures -= 1
            self.last_turn_failed = True
            self.log.append("orchTurn(mistral-loop): HTTP 429 rate limited")
            self.push("assistant", "That didn't finish - try again later.")
        elif "sub-agent" in text:
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
        slugs = ("mistral", "cumulo", "zai", "anthropic", "openai")
        rows = {
            s: {
                "verify": 1,
                "vts": 1,
                "orchModel": self.settings.get(f"orchM_{s}", ""),
                "subModel": self.settings.get(f"subM_{s}", ""),
            }
            for s in slugs
        }
        return {
            "providers": rows,
            "orchHost": self.settings.get("orchHost", ""),
            "directive": self.settings.get("sysPrompt", ""),
            "usage": {
                "sessIn": 100 * self.turn_counter,
                "sessOut": 10 * self.turn_counter,
                "turns": self.turn_counter,
                "ceiling": {"limitCents": int(self.settings.get("spendCeil", 2000))},
            },
        }

    def config(self):
        return {"devTz": self.settings.get("devTz", ""), "devName": self.settings.get("devName", "")}

    def telegram(self):
        return {"allow": [{"id": self.settings["id"]}] if "id" in self.settings else []}

    def tenant(self):
        return {"tenants": [{"id": self.settings.get("id"), "role": self.settings.get("role")}]}


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
                    "/api/lastturn": lambda: (
                        f"turn {fake.turn_counter}\n===== 4. TOOL LOOP =====\n"
                        + ('[tool] files_read {"a":1}\n[result] files_read ok\n' if not fake.last_turn_failed else "")
                    ),
                    "/api/orch": fake.orch,
                    "/api/state": fake.state,
                    "/api/config": fake.config,
                    "/api/telegram": fake.telegram,
                    "/api/tenant": fake.tenant,
                    "/api/mem/stats": lambda: {"vectors": fake.vectors},
                }
                if url.path == "/api/mem/episodic" and fake.busy_reads > 0:
                    fake.busy_reads -= 1
                    return self._send(503, {"error": "instance busy - try again"})
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
                fake.settings.update({k: v[0] for k, v in urllib.parse.parse_qs(raw.decode()).items()})
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
        self.fake.log.clear()  # the daemon's log is an in-memory tail: a restart empties it
        self.fake.busy_reads = 3  # and its engine answers 503 while it loads its stores
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
                "scenarios": [
                    {
                        "id": "s",
                        "steps": [
                            {"say": "${fact} note $i", "repeat": 2, "pad": 100, "expect": [{"reply_nonempty": True}]}
                        ],
                    }
                ],
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
    "tools": [{"name": "files_read", "args": '{"name":"notes.txt"}', "result": "hello-file"}],
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
    "tool_args_regex": ({"tool": "files_read", "regex": "notes"}, REC, {"tool": "files_read", "regex": "other"}),
    "tool_result_regex": ({"tool": "files_read", "regex": "hello"}, REC, {"tool": "files_write", "regex": "hello"}),
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

        rows = grade.grade_step({}, [{"file_absent": {"name": "n"}}], Down())
        self.assertFalse(rows[0]["pass"])
        self.assertTrue(rows[0]["broke"])

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


def sc(*steps, **over):
    return {"id": over.pop("id", "s"), "steps": list(steps), **over}


def say(text="hello there", *expect, **over):
    return {"say": text, "settle_s": 0, "expect": list(expect) or [{"reply_nonempty": True}], **over}


class Schema(unittest.TestCase):
    """A typo must stop the run. On defaults it would spend real money and could still pass."""

    def refused(self, why, **over):
        with self.assertRaises(pr.PersonaError) as ctx:
            persona(**over)
        self.assertIn(why, str(ctx.exception))

    def test_a_mistyped_persona_field_is_refused_with_the_likely_spelling(self):
        for bad, right in (("monthly_cap", "monthly_cap_cents"), ("timezon", "timezone"), ("membrs", "members")):
            with self.assertRaises(pr.PersonaError) as ctx:
                pr.validate_persona({"name": "x", bad: 1, "scenarios": [sc({"sleep": 0})]})
            self.assertIn(f"unknown field {bad!r}", str(ctx.exception))
            self.assertIn(right, str(ctx.exception))

    def test_a_mistyped_step_or_scenario_field_is_refused(self):
        for bad in ("timout_s", "settel_s"):
            self.refused("unknown field", scenarios=[sc(say(**{bad: 5}))])
        self.refused("unknown field", scenarios=[sc(say(), knwon_defect="no-fold")])

    def test_repeat_and_pad_are_checked_before_they_expand(self):
        self.refused("at least 1", scenarios=[sc(say(repeat=0))])
        self.refused("at least 1", scenarios=[sc(say(repeat=-2))])
        self.refused("only works with repeat", scenarios=[sc(say(pad=100))])
        self.refused("repeat needs a say step", scenarios=[sc({"sleep": 0, "repeat": 2})])

    def test_a_check_that_cannot_fail_is_refused(self):
        for check in ({"reply_contains": ""}, {"reply_regex": ""}, {"tool_called": ""}, {"body_contains": ""}):
            self.refused("cannot fail", scenarios=[sc(say("x", check))])
        self.refused("cannot fail", scenarios=[sc(say("x", {"file_contains": {"name": "n", "text": ""}}))])
        self.refused("missing", scenarios=[sc(say("x", {"file_contains": {"name": "n"}}))])
        self.refused("cannot fail", scenarios=[sc(say("x", {"tool_args_regex": {"tool": "a", "regex": ""}}))])

    def test_nested_values_and_markers_are_typed(self):
        self.refused("must be text", keys={"mistral": 5})
        self.refused("must be text", env={"A": 1})
        self.refused("must be text", models={"head": {"mistral": 5}})
        self.refused("instance_env_keys", instance_env_keys=[5])
        self.refused("true or false", scenarios=[sc(say("x", {"reply_contains": "x", "known_defect": "no-fold", "may_pass": "no"}))])
        self.refused("is text", scenarios=[sc(say("x", {"reply_contains": "x", "known_defect": ["no-fold"]}))])

    def test_wrong_types_are_refused_not_raised_later(self):
        self.refused("monthly_cap_cents", monthly_cap_cents="500")
        self.refused("timeout_s", scenarios=[sc(say(timeout_s="60"))])
        self.refused("repeat", scenarios=[sc(say(repeat="3"))])
        self.refused("tokens_max", scenarios=[sc(say("x", {"tokens_max": "9"}))])
        self.refused("api_min", scenarios=[sc(say("x", {"api_min": {"path": "/a", "field": "b", "min": "1"}}))])
        self.refused("api_min", scenarios=[sc(say("x", {"api_min": {"path": "/a"}}))])
        self.refused("http_status", scenarios=[sc({"http": {"path": "/a"}, "expect": [{"http_status": "200"}]})])
        self.refused("bad regex", scenarios=[sc(say("x", {"reply_regex": "("}))])
        self.refused("below 100", monthly_cap_cents=50)
        self.refused("provider", keys={"mistrall": "K"})
        self.refused("host", host="mistrall")

    def test_a_say_field_on_another_action_is_refused(self):
        self.refused("only a say step", scenarios=[sc({"http": {"path": "/a"}, "timeout_s": 5})])

    def test_turn_resolved_cannot_be_asked_of_a_member_turn(self):
        self.refused("no turn id", scenarios=[sc(say("x", {"turn_resolved": True}, **{"as": "1001"}))])

    def test_needs_must_name_an_earlier_scenario(self):
        self.refused("no earlier scenario", scenarios=[sc({"sleep": 0}, needs=["later"])])

    def test_a_marker_must_name_a_registered_defect(self):
        self.refused("known_defects.json", scenarios=[sc(say("x", {"reply_contains": "x", "known_defect": "nope"}))])
        self.refused("needs a known_defect", scenarios=[sc(say("x", {"reply_contains": "x", "may_pass": True}))])

    def test_the_report_defects_are_registered(self):
        rows = pr.known_defects()
        self.assertTrue({"no-fold", "glm-no-tools", "no-routines", "binary-read-kills-turn"} <= set(rows))
        self.assertTrue(all(r["title"] and r["report"] for r in rows.values()))

    def _load_matrix(self, doc):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "m.json"
            path.write_text(json.dumps(doc))
            return pr.load_personas(str(path))

    def test_a_matrix_row_can_drive_numbers_and_whole_scenarios(self):
        doc = {
            "vars": {"cap": 300},
            "matrix": [
                {"n": "a", "turns": 2, "wait": 5, "floor": 7, "extra": [sc({"sleep": 0}, id="only-a")]},
                {"n": "b", "turns": 3, "wait": 9, "floor": 1, "extra": [sc({"sleep": 1}, id="only-b")]},
            ],
            "persona": {
                "name": "p-${n}",
                "monthly_cap_cents": "${cap}",
                "scenarios": "${extra}",
            },
        }
        a, b = self._load_matrix(doc)
        self.assertEqual(a["monthly_cap_cents"], 300)
        self.assertEqual(a["scenarios"][0]["id"], "only-a")
        self.assertEqual(b["scenarios"][0]["id"], "only-b")
        doc["matrix"] = [{"n": "a", "turns": 2, "wait": 5, "floor": 7}]
        doc["persona"]["scenarios"] = [
            sc(
                say(
                    "note $i",
                    {"tokens_max": "${floor}"},
                    {"api_min": {"path": "/a", "field": "b", "min": "${floor}"}},
                    repeat="${turns}",
                    timeout_s="${wait}",
                )
            )
        ]
        steps = self._load_matrix(doc)[0]["scenarios"][0]["steps"]
        self.assertEqual(len(steps), 2)
        self.assertEqual(steps[0]["timeout_s"], 5)
        self.assertEqual(steps[0]["expect"][0]["tokens_max"], 7)
        self.assertEqual(steps[0]["expect"][1]["api_min"]["min"], 7)

    def test_a_number_inside_text_stays_text_and_a_text_number_is_refused(self):
        doc = {
            "matrix": [{"n": 7}],
            "persona": {"name": "p-${n}", "scenarios": [sc(say("turn ${n}", repeat="${n}x"))]},
        }
        with self.assertRaises(pr.PersonaError):
            self._load_matrix(doc)
        doc["persona"]["scenarios"] = [sc(say("turn ${n}"))]
        self.assertEqual(self._load_matrix(doc)[0]["scenarios"][0]["steps"][0]["say"], "turn 7")


class Truth(unittest.TestCase):
    def setUp(self):
        class Page(BaseHTTPRequestHandler):
            def log_message(self, *_a):
                pass

            def do_GET(self):
                body = b"<a>Download Python 3.99.1</a>"
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Page)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}/"

    def doc(self, regex):
        return {
            "truth": {"latest": {"url": self.url, "regex": regex}},
            "matrix": [{"n": "a"}],
            "persona": {"name": "p-${n}", "scenarios": [sc(say("x", {"reply_contains": "${latest}"}))]},
        }

    def test_the_value_and_its_raw_source_are_kept(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "m.json"
            path.write_text(json.dumps(self.doc(r"Download Python (3\.\d+\.\d+)")))
            ps = pr.load_personas(str(path), Path(d) / "truth")
            record = json.loads((Path(d) / "truth" / "latest.json").read_text())
            body = (Path(d) / "truth" / "latest.body").read_bytes()
            dry = pr.load_personas(str(path))
        self.assertEqual(ps[0]["scenarios"][0]["steps"][0]["expect"][0]["reply_contains"], "3.99.1")
        self.assertEqual(record["value"], "3.99.1")
        self.assertIn(b"3.99.1", body)
        self.assertIn("UNFETCHED", dry[0]["scenarios"][0]["steps"][0]["expect"][0]["reply_contains"])

    def test_a_source_must_be_a_public_page_and_the_regex_must_capture(self):
        cases = [
            {"url": "file:///etc/passwd", "regex": "(x)"},
            {"url": self.url + "?key=abc", "regex": "(x)"},
            {"url": self.url, "regex": "no group"},
            {"url": self.url, "regex": "(x)", "extra": 1},
        ]
        for spec in cases:
            with tempfile.TemporaryDirectory() as d, self.assertRaises(pr.PersonaError, msg=str(spec)):
                path = Path(d) / "m.json"
                path.write_text(json.dumps({**self.doc("(x)"), "truth": {"latest": spec}}))
                pr.load_personas(str(path), Path(d) / "truth")

    def test_a_source_that_does_not_answer_the_regex_stops_the_run(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "m.json"
            path.write_text(json.dumps(self.doc("no such words (x)")))
            with self.assertRaises(pr.PersonaError):
                pr.load_personas(str(path), Path(d) / "truth")


class Contract(unittest.TestCase):
    """The rig against the daemon's own source, so a rename fails here and not on every live persona."""

    REPO = HERE.parents[1]

    def read(self, path):
        return (self.REPO / path).read_text(errors="replace")

    def test_everything_the_rig_relies_on_is_in_the_daemon_source(self):
        self.assertEqual(pr.contract_missing(self.read), [])

    def test_a_rename_of_any_entry_is_caught_in_its_own_file(self):
        for path, tokens in pr.CONTRACT.items():
            for token in tokens:
                broken = lambda p, _t=token, _f=path: self.read(p).replace(_t, "renamed") if p == _f else self.read(p)
                self.assertEqual(pr.contract_missing(broken), [f"{path}: {token}"])

    def test_a_field_only_the_served_page_names_does_not_count(self):
        self.assertFalse(any("webui_page" in path or "include/web" in path for path in pr.CONTRACT))

    def _contract_text(self):
        return " ".join(t for tokens in pr.CONTRACT.values() for t in tokens)

    def test_every_form_the_rig_posts_is_in_the_contract(self):
        p = persona(directive="d", env={"A": "b"})
        runner = pr.Runner(p, FakeBackend(FakeNimbusd()), {"FAKE_MISTRAL": FAKE_KEY}, Path(tempfile.mkdtemp()))
        text = self._contract_text()
        for _label, path, form in runner._config_forms():
            self.assertIn(f'"{path.rsplit("/add", 1)[0]}', text)
            for field in form:
                prefix = field.split("_")[0] + "_"
                self.assertTrue(f'"{field}"' in text or f'"{prefix}"' in text, field)

    def test_every_route_the_rig_reads_is_in_the_contract(self):
        text = self._contract_text()
        source = (HERE / "persona_rig.py").read_text().split("def contract_missing")[1]
        for route in set(re.findall(r'"(/(?:api/[a-z/_]+[a-z]|readyz))[?"]', source)):
            self.assertIn(f'"{route.removesuffix("add")}"', text, route)

    def test_every_fake_route_is_in_the_contract(self):
        import inspect

        text = self._contract_text()
        for route in set(re.findall(r'"(/(?:api/[a-z/]+|readyz))"', inspect.getsource(make_handler))) - {"/api/loops"}:
            self.assertIn(f'"{route}"', text, route)


class DeadInstance:
    def file_bytes(self, _project, _name):
        return 0, "ConnectionError"

    def get(self, _path):
        return 503, "busy"


class Markers(unittest.TestCase):
    def run_one(self, p, **options):
        with tempfile.TemporaryDirectory() as d:
            return pr.run_all([p], FakeBackend(FakeNimbusd()), {"FAKE_MISTRAL": FAKE_KEY}, Path(d), options)[0]

    def test_a_marked_failure_does_not_fail_the_persona_and_is_listed(self):
        check = {"reply_contains": "banana", "known_defect": "no-fold"}
        summary = self.run_one(persona(scenarios=[sc(say("hi", check))]))
        self.assertTrue(summary["pass"], summary)
        row = summary["scenarios"][0]
        self.assertEqual(row["verdict"], "known_defect")
        self.assertEqual(row["known_failures"][0]["known_defect"], "no-fold")
        self.assertEqual(summary["known_defects"], ["no-fold"])

    def test_a_marked_check_that_passes_fails_until_the_marker_goes(self):
        check = {"reply_contains": "hi", "known_defect": "no-fold"}
        summary = self.run_one(persona(scenarios=[sc(say("hi", check))]))
        self.assertFalse(summary["pass"])
        self.assertEqual(summary["scenarios"][0]["xpass"][0]["check"], "reply_contains")

    def test_may_pass_is_for_a_defect_that_comes_and_goes(self):
        check = {"reply_contains": "hi", "known_defect": "no-fold", "may_pass": True}
        self.assertTrue(self.run_one(persona(scenarios=[sc(say("hi", check))]))["pass"])

    def test_an_unmarked_failure_beside_a_marked_one_still_fails(self):
        checks = [{"reply_contains": "banana", "known_defect": "no-fold"}, {"reply_contains": "kiwi"}]
        self.assertFalse(self.run_one(persona(scenarios=[sc(say("hi", *checks))]))["pass"])

    def test_a_marker_on_a_scenario_is_refused(self):
        with self.assertRaises(pr.PersonaError):
            persona(scenarios=[sc(say("hi"), known_defect="no-routines")])

    def test_a_marker_never_excuses_a_refused_turn_or_a_check_that_broke(self):
        broke = grade.grade_step({"reply": "x"}, [{"tool_called": "y", "known_defect": "no-routines"}], None)
        self.assertTrue(broke[0]["broke"])
        self.assertNotIn("known_defect", broke[0])
        dead = grade.grade_step({}, [{"file_absent": {"name": "n"}, "known_defect": "no-routines"}], DeadInstance())
        self.assertTrue(dead[0]["broke"])
        rec = {"checks": [*broke, *dead, {"check": "turn_finished", "arg": True, "pass": False, "seen": "out of time"}]}
        out = {"failed": [], "known_failures": [], "xpass": []}
        runner = pr.Runner(persona(), FakeBackend(FakeNimbusd()), {"FAKE_MISTRAL": FAKE_KEY}, Path(tempfile.mkdtemp()))
        runner._classify(0, rec, out)
        self.assertEqual(len(out["failed"]), 3)
        self.assertEqual(out["known_failures"], [])

    def test_an_empty_marker_from_a_matrix_row_marks_nothing(self):
        check = {"reply_contains": "banana", "known_defect": "", "may_pass": True}
        self.assertFalse(self.run_one(persona(scenarios=[sc(say("hi", check))]))["pass"])


class SpendLimits(unittest.TestCase):
    def test_a_persona_with_no_cap_gets_the_rig_default_not_the_daemons(self):
        p = persona()
        del p["monthly_cap_cents"]
        fake = FakeNimbusd()
        with tempfile.TemporaryDirectory() as d:
            pr.run_all([p], FakeBackend(fake), {"FAKE_MISTRAL": FAKE_KEY}, Path(d))
        self.assertEqual(int(fake.settings["spendCeil"]), pr.DEFAULT_CAP_CENTS)
        self.assertLess(pr.DEFAULT_CAP_CENTS, 500)

    def test_the_shipped_paid_personas_name_a_cap(self):
        for path in (HERE / "personas").glob("*.json"):
            for p in pr.load_personas(str(path)):
                if p.get("keys") and not p["name"].startswith("first-run"):
                    self.assertIn("monthly_cap_cents", p, p["name"])

    def test_the_run_ceiling_stops_the_next_turn(self):
        fake = FakeNimbusd()
        limits = pr.RunLimits(max_cents=1)
        p = persona(scenarios=[sc(say("one"), say("two"), say("three"))])
        with tempfile.TemporaryDirectory() as d:
            ev = Path(d)
            rc = pr.run_and_write([p], FakeBackend(fake), {"FAKE_MISTRAL": FAKE_KEY}, ev, {"limits": limits})
            totals = json.loads((ev / "run-totals.json").read_text())
        self.assertEqual(rc, 1)
        self.assertEqual(fake.turn_counter, 1, "the fake bills one cent per turn, so one turn ran")
        self.assertEqual(totals["turns_cents"], 1)

    def test_a_ceiling_already_reached_starts_no_container(self):
        backend = FakeBackend(FakeNimbusd())
        limits = pr.RunLimits(max_cents=0)
        with tempfile.TemporaryDirectory() as d:
            results = pr.run_all([persona()], backend, {"FAKE_MISTRAL": FAKE_KEY}, Path(d), {"limits": limits})
        self.assertFalse(results[0]["pass"])
        self.assertEqual(backend.started, [])

    def test_judge_spend_counts_toward_the_ceiling(self):
        usage = {"input_tokens": 1000000, "output_tokens": 0}
        verdict = {"satisfied": True, "honest": True, "score": 1, "judge_provider": "openai", "judge_model": "gpt-5.5"}
        limits = pr.RunLimits(max_cents=1000)
        options = {"limits": limits, "judge_factory": lambda _a: lambda *_x: {**verdict, "judge_usage": usage}}
        with tempfile.TemporaryDirectory() as d:
            pr.run_all([persona()], FakeBackend(FakeNimbusd()), {"FAKE_MISTRAL": FAKE_KEY}, Path(d), options)
        self.assertEqual(limits.snapshot()["judge_cents"], 500)

    def test_the_judge_is_not_called_past_the_ceiling(self):
        calls = []
        limits = pr.RunLimits(max_cents=0.5)
        limits.add(1)
        runner = pr.Runner(
            persona(),
            FakeBackend(FakeNimbusd()),
            {"FAKE_MISTRAL": FAKE_KEY},
            Path(tempfile.mkdtemp()),
            {"limits": limits, "judge_factory": lambda _a: lambda *x: calls.append(x)},
        )
        rec = {"checks": [], "reply": "x"}
        runner._judge_step({"say": "x"}, rec)
        self.assertEqual(calls, [])
        self.assertIn("skipped", rec["judge"]["rationale"])

    def test_a_judge_call_with_no_usage_counts_a_high_figure_and_a_failed_call_counts_nothing(self):
        self.assertEqual(pr.judge_cents({"satisfied": True, "score": 1, "judge_model": "gpt-5.5"}), 2)
        self.assertEqual(pr.judge_cents(None), 2, "billed, but its answer could not be parsed")
        self.assertEqual(pr.judge_cents({"satisfied": None, "score": None, "judge_model": "gpt-5.5"}), 0)
        self.assertEqual(pr.judge_cents({"rationale": "judge unavailable"}), 0)


class Judge(unittest.TestCase):
    def run_with(self, host, provider):
        seen = []

        def factory(avoid):
            seen.append(avoid)
            return lambda *_a: {
                "satisfied": False,
                "honest": True,
                "score": 0.4,
                "judge_provider": provider,
                "judge_model": "gpt-5.5",
            }

        p = persona(keys={host: "FAKE_MISTRAL"}, models={"head": {host: "m"}}, host=host)
        with tempfile.TemporaryDirectory() as d:
            ev = Path(d)
            summary = pr.run_all([p], FakeBackend(FakeNimbusd()), {"FAKE_MISTRAL": FAKE_KEY}, ev, {"judge_factory": factory})[0]
            on_disk = json.loads((ev / "unit" / "summary.json").read_text())
        return seen, summary, on_disk

    def test_the_judge_avoids_the_personas_own_provider_and_summary_says_who_judged(self):
        seen, summary, on_disk = self.run_with("anthropic", "openai")
        self.assertEqual(seen, ["anthropic"])
        self.assertEqual(summary["judge"]["provider"], ["openai"])
        self.assertTrue(summary["judge"]["independent"])
        self.assertEqual(summary["judge"]["unsatisfied"], 1)
        self.assertEqual(on_disk["judge"]["min_score"], 0.4)
        self.assertEqual(on_disk["scenarios"][0]["judge"]["unsatisfied_steps"], [0])

    def test_a_judge_on_the_same_provider_reads_as_not_independent(self):
        self.assertFalse(self.run_with("anthropic", "anthropic")[1]["judge"]["independent"])

    def test_the_real_factory_passes_the_provider_to_avoid(self):
        calls = []
        real = pr.make_judge_from_env
        pr.make_judge_from_env = lambda env, avoid="": calls.append(avoid) or (lambda *_a: None)
        self.addCleanup(setattr, pr, "make_judge_from_env", real)
        pr._judge_factory({})("anthropic")
        self.assertEqual(calls, ["", "anthropic"])


class ToolTrace(unittest.TestCase):
    def test_results_pair_with_their_own_call_by_name(self):
        text = (
            "===== 4. TOOL LOOP\n[tool] a {1}\n[tool] b {2}\n[result] b two\n[result] a one\n"
        )
        self.assertEqual(
            pr.tools_from_anatomy(text),
            [{"name": "a", "args": "{1}", "result": "one"}, {"name": "b", "args": "{2}", "result": "two"}],
        )

    def test_a_turn_later_overwritten_in_the_slot_keeps_its_tools(self):
        class Slot:
            def __init__(self):
                self.text = "old"

            def get(self, path):
                if path == "/api/lastturn":
                    return 200, self.text
                if path.startswith("/api/replies"):
                    return 200, json.dumps({"lastSeq": 0, "replies": []})
                if path == "/api/orch":
                    return 200, json.dumps({"usage": {"sessIn": 0, "sessOut": 0, "turns": 0}})
                if path == "/api/state":
                    return 200, json.dumps({"spend": {"spentCents": 0}})
                if path == "/api/log":
                    return 200, ""
                return 200, json.dumps({"messages": []})

            def get_json(self, path):
                return json.loads(self.get(path)[1])

            get_json_strict = get_json

            def text_strict(self, path, also_ok=()):
                return self.get(path)[1]

        slot = Slot()
        cap = pr.Capture(slot)
        slot.text = "===== 4. TOOL LOOP\n[tool] head_tool {}\n"
        cap.note_turn()
        slot.text = "===== 4. TOOL LOOP\n[tool] synth_tool {}\n"
        rec = {}
        cap.fill(rec)
        self.assertEqual([t["name"] for t in rec["tools"]], ["head_tool", "synth_tool"])
        self.assertEqual(len(rec["lastturn_earlier"]), 1)


class RunOptions(unittest.TestCase):
    def run_it(self, personas, fake=None, ev=None, **options):
        backend = FakeBackend(fake or FakeNimbusd())
        ev = ev or Path(tempfile.mkdtemp())
        return pr.run_all(personas, backend, {"FAKE_MISTRAL": FAKE_KEY}, ev, options), backend, ev

    def test_a_provider_429_retries_and_keeps_the_failed_try(self):
        fake = FakeNimbusd()
        fake.transient_failures = 1
        p = persona(scenarios=[sc(say("flaky turn", {"no_error_reply": True}))])
        results, _b, ev = self.run_it([p], fake, backoff_s=0)
        self.assertTrue(results[0]["pass"], results[0])
        rec = json.loads((ev / "unit" / "s" / "step-00.json").read_text())
        self.assertEqual(len(rec["earlier_tries"]), 1)
        self.assertIn("HTTP 429", rec["earlier_tries"][0]["log"])

    def test_a_429_that_never_clears_still_fails_after_its_tries(self):
        fake = FakeNimbusd()
        fake.transient_failures = 9
        p = persona(scenarios=[sc(say("flaky turn", {"no_error_reply": True}))])
        results, _b, _e = self.run_it([p], fake, backoff_s=0, retries=2)
        self.assertFalse(results[0]["pass"])
        self.assertEqual(fake.turn_counter, 3)

    def test_a_turn_that_ran_a_tool_is_not_retried(self):
        rec = {"reply": "That didn't finish", "log": "HTTP 429", "tools": [{"name": "files_write"}]}
        self.assertFalse(pr._transient_failure(rec), "another try would repeat the write")
        self.assertTrue(pr._transient_failure({**rec, "tools": []}))
        self.assertTrue(pr._transient_failure({**rec, "tools": None}))

    def test_a_failure_with_no_provider_error_is_not_retried(self):
        fake = FakeNimbusd()
        p = persona(scenarios=[sc(say("broken turn", {"reply_nonempty": True}))])
        self.run_it([p], fake, backoff_s=0)
        self.assertEqual(fake.turn_counter, 1)

    def test_turns_are_gated_per_provider(self):
        limits = pr.RunLimits(provider_limit=1)
        mistral = limits.turn_slot("mistral")
        self.assertIs(mistral, limits.turn_slot("mistral"))
        self.assertTrue(mistral.acquire(blocking=False))
        self.assertFalse(limits.turn_slot("mistral").acquire(blocking=False), "one turn at a time")
        self.assertTrue(limits.turn_slot("zai").acquire(blocking=False), "another provider is not held up")
        mistral.release()

    def test_resume_skips_a_finished_persona_and_reruns_one_that_stopped(self):
        ev = Path(tempfile.mkdtemp())
        p = persona()
        _r, first, _e = self.run_it([p], ev=ev)
        self.assertEqual(len(first.started), 1)
        results, second, _e = self.run_it([persona()], ev=ev, resume=True)
        self.assertEqual(second.started, [], "the finished persona did not run again")
        self.assertTrue(results[0]["pass"])
        summary_path = ev / "unit" / "summary.json"
        summary = json.loads(summary_path.read_text())
        summary["error"] = "stopped part way"
        summary_path.write_text(json.dumps(summary))
        _r, third, _e = self.run_it([persona()], ev=ev, resume=True)
        self.assertEqual(len(third.started), 1, "a persona that stopped runs again")
        self.assertTrue(list(ev.glob("unit.prev-*")), "the older run is kept, not mixed in")

    def test_resume_runs_again_after_the_persona_changed_or_on_a_corrupt_summary(self):
        ev = Path(tempfile.mkdtemp())
        self.run_it([persona()], ev=ev)
        _r, edited, _e = self.run_it([persona(timezone="UTC")], ev=ev, resume=True)
        self.assertEqual(len(edited.started), 1)
        (ev / "unit" / "summary.json").write_text("{truncated")
        _r, corrupt, _e = self.run_it([persona(timezone="UTC")], ev=ev, resume=True)
        self.assertEqual(len(corrupt.started), 1)

    def test_a_persona_with_none_of_the_only_ids_starts_no_container(self):
        p = persona(scenarios=[sc({"sleep": 0}, id="a")])
        q = persona(name="other", scenarios=[sc({"sleep": 0}, id="c")])
        results, backend, _e = self.run_it([p, q], only={"c"})
        self.assertEqual(len(backend.started), 1)
        self.assertFalse(next(r for r in results if r["persona"] == "unit")["pass"])

    def test_only_brings_the_scenarios_it_needs(self):
        p = persona(
            scenarios=[
                sc(say("remember this", {"reply_nonempty": True}), id="memory"),
                sc({"restart": True}, say("still there"), id="restart", needs=["memory"]),
                sc(say("other"), id="other"),
            ]
        )
        results, _b, _e = self.run_it([p], only={"restart"})
        self.assertEqual([s["id"] for s in results[0]["scenarios"]], ["memory", "restart"])

    def test_the_turn_log_survives_a_restart_and_says_so(self):
        p = persona(scenarios=[sc(say("before"), {"restart": True}, say("after"))])
        results, _b, ev = self.run_it([p])
        self.assertTrue(results[0]["pass"], results[0])
        log = (ev / "unit" / "turn-log.txt").read_text()
        self.assertEqual(log.count("turn spend:"), 2, "both turns, though the daemon's own log lost the first")
        self.assertIn("container restarted", log)
        self.assertEqual((ev / "unit" / "api-log.txt").read_text().count("turn spend:"), 1)
        self.assertEqual(results[0]["provider_usage"]["fake"]["in"], 200)

    def test_a_read_that_stays_busy_stops_the_persona_instead_of_reading_as_zero(self):
        fake = FakeNimbusd()
        fake.busy_reads = 10**6
        original = pr.BUSY_WAIT_S
        pr.BUSY_WAIT_S = 0.05
        self.addCleanup(setattr, pr, "BUSY_WAIT_S", original)
        results, _b, _e = self.run_it([persona()], fake)
        self.assertFalse(results[0]["pass"])
        self.assertIn("503", results[0]["error"])

    def test_scenarios_start_on_a_cleared_conversation(self):
        fake = FakeNimbusd()
        p = persona(
            scenarios=[
                sc(say("one"), id="a"),
                sc(say("two"), id="b"),
                sc(say("three"), id="c", continue_conversation=True),
            ]
        )
        results, _b, ev = self.run_it([p], fake)
        clears = [b for _path, b in fake.config_posts if "clearConv" in b]
        self.assertEqual(len(clears), 1, "before b only: a starts clean and c asked to continue")
        self.assertEqual([s["isolated"] for s in results[0]["scenarios"]], [False, True, False])
        self.assertEqual(json.loads((ev / "unit" / "b" / "reset.json").read_text())["status"], 200)

    def test_a_failed_clear_fails_the_scenario_and_runs_none_of_it(self):
        fake = FakeNimbusd()
        p = persona(scenarios=[sc(say("one"), id="a"), sc(say("refuse-me-not two"), id="b")])
        original = pr.Instance.call

        def refuse_clear(inst, method, path, form=None, data=None, ctype=None):
            if form and "clearConv" in form:
                return 503, "busy"
            return original(inst, method, path, form=form, data=data, ctype=ctype)

        pr.Instance.call = refuse_clear
        self.addCleanup(setattr, pr.Instance, "call", original)
        results, _b, _e = self.run_it([p], fake)
        row = results[0]["scenarios"][1]
        self.assertFalse(row["pass"])
        self.assertEqual(row["failed"][0]["check"], "conversation_reset")
        self.assertEqual(fake.turn_counter, 1, "the second scenario's turn was not paid for")

    def test_a_setting_that_did_not_take_stops_the_persona_before_any_turn(self):
        class Coercing(FakeNimbusd):
            def orch(self):
                d = super().orch()
                d["providers"]["mistral"]["orchModel"] = "some-other-model"
                return d

        fake = Coercing()
        results, _b, _e = self.run_it([persona()], fake)
        self.assertFalse(results[0]["pass"])
        self.assertIn("some-other-model", results[0]["error"])
        self.assertEqual(fake.turn_counter, 0)

    def test_every_setting_is_read_back(self):
        results, _b, _e = self.run_it([persona(directive="be brief")])
        got = {r["setting"]: r["got"] for r in results[0]["config_readback"]}
        self.assertEqual(got["head model mistral"], "fake-model")
        self.assertEqual(got["sub model mistral"], "fake-sub")
        self.assertEqual(got["time zone"], "Europe/London")
        self.assertEqual(got["monthly ceiling"], 500)
        self.assertEqual(got["directive"], "be brief")
        self.assertEqual(got["member 1001"], "1001")
        self.assertEqual(got["member role 1001"], "user")

    def _with_verdict(self, verify, reason=""):
        class Verdict(FakeNimbusd):
            def orch(self):
                d = super().orch()
                d["providers"]["cumulo"].update(verify=verify, vfyReason=reason)
                return d

        return Verdict()

    def test_a_persona_can_expect_a_rejected_or_no_credit_key(self):
        for verify, reason, want in ((0, "", "rejected"), (-1, "nocredits", "nocredits")):
            p = persona(keys={"cumulo": "FAKE_MISTRAL"}, key_expect={"cumulo": want}, models={}, host="cumulo")
            results, _b, _e = self.run_it([p], self._with_verdict(verify, reason))
            self.assertTrue(results[0]["pass"], results[0])

    def test_a_key_that_works_fails_a_persona_expecting_a_refusal(self):
        p = persona(keys={"cumulo": "FAKE_MISTRAL"}, key_expect={"cumulo": "rejected"}, models={}, host="cumulo")
        results, _b, _e = self.run_it([p])
        self.assertFalse(results[0]["pass"])

    def test_a_made_up_key_needs_no_env_entry_and_never_reaches_the_evidence(self):
        fake = self._with_verdict(0)
        p = persona(keys={"cumulo": "RIG_FAKE_CUMULO"}, key_expect={"cumulo": "rejected"}, models={}, host="cumulo")
        ev = Path(tempfile.mkdtemp())
        results = pr.run_all([p], FakeBackend(fake), {}, ev)
        self.assertTrue(results[0]["pass"], results[0])
        posted = fake.settings["cumuloKey"]
        self.assertGreaterEqual(len(posted), 32)
        for f in ev.rglob("*"):
            if f.is_file():
                self.assertNotIn(posted, f.read_text(), f)

    def test_a_cumulo_persona_file_loads_and_runs_on_the_fake(self):
        ps = pr.load_personas(str(HERE / "personas" / "cumulo_router.json"))
        self.assertEqual({p["key_expect"]["cumulo"] for p in ps}, {"verified", "nocredits"})
        funded = next(p for p in ps if p["key_expect"]["cumulo"] == "verified")
        env = {"CUMULO_TEST_KEY": FAKE_KEY}
        fake = FakeNimbusd()
        results = pr.run_all([funded], FakeBackend(fake), env, Path(tempfile.mkdtemp()))
        self.assertEqual(fake.settings["cumuloKey"], FAKE_KEY)
        self.assertFalse(results[0]["pass"], "the fake does not do arithmetic, so the check can fail")


class Regrade(unittest.TestCase):
    def write(self, d, checks):
        rec = {"reply": "Done.", "replies": ["Done."], "checks": checks}
        path = Path(d) / "p" / "s"
        path.mkdir(parents=True)
        (path / "step-00.json").write_text(json.dumps(rec))

    def test_a_stored_verdict_todays_graders_would_change_is_reported(self):
        with tempfile.TemporaryDirectory() as d:
            self.write(d, [{"check": "no_error_reply", "arg": True, "pass": True, "seen": ""}])
            out = io.StringIO()
            self.assertEqual(pr.regrade(Path(d), out), 1)
        self.assertIn("no_error_reply: stored True, now False", out.getvalue())

    def test_no_steps_a_corrupt_step_and_superseded_runs(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(pr.regrade(Path(d), io.StringIO()), 1, "nothing checked is not a clean result")
            self.write(d, [{"check": "no_error_reply", "arg": True, "pass": False, "seen": ""}])
            old = Path(d) / "p.prev-1" / "s"
            old.mkdir(parents=True)
            (old / "step-00.json").write_text(
                json.dumps({"reply": "Done.", "checks": [{"check": "no_error_reply", "arg": True, "pass": True}]})
            )
            self.assertEqual(pr.regrade(Path(d), io.StringIO()), 0, "a moved-aside run is not this run")
            (Path(d) / "p" / "s" / "step-01.json").write_text("{cut")
            self.assertEqual(pr.regrade(Path(d), io.StringIO()), 1)

    def test_agreement_and_live_checks_read_clean(self):
        with tempfile.TemporaryDirectory() as d:
            self.write(
                d,
                [
                    {"check": "no_error_reply", "arg": True, "pass": False, "seen": ""},
                    {"check": "api_min", "arg": {"path": "/a", "field": "b", "min": 1}, "pass": True, "seen": ""},
                    {"check": "turn_finished", "arg": True, "pass": False, "seen": ""},
                ],
            )
            self.assertEqual(pr.regrade(Path(d), io.StringIO()), 0)


if __name__ == "__main__":
    os.environ.setdefault("PYTHONDONTWRITEBYTECODE", "1")
    unittest.main(verbosity=1)
