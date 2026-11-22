#!/usr/bin/env python3
"""Extract every model-facing string of the built engines into a catalog golden.

The catalog is the input of the no-prompt-patch gate (check_no_prompt_patch.py,
PRD section 11): growth in any class of model-facing text fails the build unless
equal bytes are removed and the parity rates before and after are logged in
coverage/PROMPT-CHANGES.md.

Two catalogs:
- e1, the old engine (test/golden/model_text_catalog_e1.txt): the string literals of
  the source files listed in E1_SOURCES, by class. A literal counts when a model could
  read it as words (letters and a space) and it is not written by a log call. Files
  that build a tool result or register a tool must be listed (the coverage check fails
  on one that is not), so a new tool cannot hide its text outside the catalog.
- e2, the plain engine (test/golden/model_text_catalog.txt): every literal of
  lib/harness/include/nimbus/harness/plain/texts.h, classed by the namespace that holds
  it (core, device_notes, stubs, tool_results, probe, summarizer, tool_descriptions),
  plus the sources RB-E2 lists in E2_EXTRA_SOURCES. The catalog is live once texts.h
  exists; from then on a prose literal in a plain engine file (lib/harness/src/plain_*,
  providers/plain_wire_*) outside texts.h is an error.

Usage (from the repo root):
  python3 tools/release_gate/gen_model_text_catalog.py            # check the goldens are fresh
  python3 tools/release_gate/gen_model_text_catalog.py --write    # regenerate them
  python3 tools/release_gate/gen_model_text_catalog.py --summary  # bytes per class
Exit 0: every golden matches the sources (last line MODEL TEXT CATALOG GREEN); 1: drift
or a source error (MODEL TEXT CATALOG RED).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
from dataclasses import dataclass

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# ---------------------------------------------------------------------------
# Engine 1: the source files whose prose literals reach a model, by class.
# ---------------------------------------------------------------------------

E1_CLASSES = ("system", "device_notes", "tool_descriptions", "tool_results", "side_prompts", "skills")

# Every prose literal of these files is in the class, except in any file: the description
# and schema of a registered tool go to tool_descriptions, and the text of a ToolResult::ok
# or fail call goes to tool_results.
E1_SOURCES = {
    "system": (
        "lib/core/include/nimbus/orch/orch_schema.h",
        "lib/core/src/orch_connectors_wire.cpp",
        "lib/core/src/orch_day_start.cpp",
        "lib/core/src/orch_world.cpp",
        "lib/harness/include/nimbus/harness/board_identity.h",
        "lib/harness/include/nimbus/harness/prompt_layout.h",
        "lib/harness/src/compose.cpp",
        "lib/harness/src/day_context.cpp",
        "lib/harness/src/prompt_layout.cpp",
        "lib/harness/src/skill_attach.cpp",
        "lib/harness/src/skill_md.cpp",
    ),
    "device_notes": (
        "lib/core/include/nimbus/orch/billing_refusal.h",
        "lib/core/src/orch_degraded.cpp",
        "lib/core/src/orch_gradient.cpp",
        "lib/core/src/orch_head_loop.cpp",
        "lib/core/src/orch_provider_caps.cpp",
        "lib/core/src/orch_provider_park.cpp",
        "lib/core/src/orch_spend_ceiling.cpp",
        "lib/core/src/orch_transcript.cpp",
        "lib/core/src/orch_truth.cpp",
        "lib/core/src/orch_turn.cpp",
        "lib/harness/include/nimbus/harness/jobs.h",
        "lib/harness/include/nimbus/harness/router_refusal.h",
        "lib/harness/src/apply.cpp",
        "lib/harness/src/conversation.cpp",
        "lib/harness/src/engine.cpp",
        "lib/harness/src/engine_fold.cpp",
        "lib/harness/src/jobs.cpp",
        "lib/harness/src/providers/anthropic.cpp",
        "lib/harness/src/providers/custom.cpp",
        "lib/harness/src/providers/loop_common.cpp",
        "lib/harness/src/providers/mistral.cpp",
        "lib/harness/src/providers/openai.cpp",
        "lib/harness/src/turn_contract.cpp",
        "lib/harness/src/turn_memory.cpp",
        "nimbusd/src/rig.h",
        "src/agent/orchestrator_common.cpp",  # RB-SEAM1b: the engine-neutral half of the device seam
        "src/agent/orchestrator_e1.cpp",
        "src/main.cpp",
    ),
    "tool_results": (
        "lib/core/include/nimbus/orch/result_envelope.h",
        "lib/core/include/nimbus/orch/tool_gates.h",
        "lib/core/src/orch_calc.cpp",  # RB-REC8: calc's result and error lines
        "lib/core/src/orch_calc_tool.cpp",  # RB-REC8: calc's registration
        "lib/core/src/orch_file_append.cpp",
        "lib/core/src/orch_file_edit.cpp",
        "lib/core/src/orch_file_aids_e1.cpp",  # RB-REC4: files.read and files.edit on the shared aids
        "lib/core/src/orch_loops.cpp",
        "lib/core/src/orch_memory_tools.cpp",
        "lib/core/src/orch_models_tool.cpp",
        "lib/core/src/orch_openq_tools.cpp",
        "lib/core/src/orch_prefs_tools.cpp",
        "lib/core/src/orch_result_store.cpp",
        "lib/core/src/orch_session_tools.cpp",
        "lib/core/src/orch_tool_groups.cpp",
        "lib/core/src/orch_tool_registry.cpp",
        "lib/core/src/orch_unattended_policy.cpp",
        "lib/harness/src/skill_store.cpp",
        "lib/harness/src/websearch.cpp",
        "nimbusd/src/hosted_file_tools.h",  # E2-HOST: the reserved-name refusal of the hosted file tools (ruling 112)
        "nimbusd/src/hosted_loops.h",
        "nimbusd/src/hosted_photos.h",
        "nimbusd/src/hosted_skills.h",
        "nimbusd/src/posix_files.h",
        "src/agent/connectors.cpp",
        "src/agent/device_log_tool.cpp",
        "src/agent/files_subsystem.cpp",
        "src/agent/loops_subsystem.cpp",
        "src/agent/memory_subsystem.cpp",
        "src/agent/pdf_tool.cpp",
        "src/sfx/music.cpp",
    ),
    "side_prompts": (
        "lib/core/include/nimbus/orch/truth.h",
        "lib/core/src/orch_compact.cpp",
        "lib/harness/src/dream.cpp",
        "lib/harness/src/fold_driver.cpp",
        "lib/harness/src/vision_caption.cpp",
        "src/agent/dream_subsystem.cpp",
    ),
    "skills": ("lib/harness/src/builtin_skills.cpp",),
}

# Files a new file's prose may sit in without being model text, each with its reason. A file
# added under SCAN_DIRS with prose literals outside a log call must be in a catalog or here
# (check_no_prompt_patch.py, with a git base).
NON_MODEL_FILES: dict = {
    "nimbusd/src/engine2_host.h": "E2-HOST: the startup switch's refusal line (the daemon log) and two owner-facing "
    "lines of the hosted engine-2 chat (a non-Claude model picked, a chat that could not start), sent to the owner's "
    "chat, never to a model",
    "lib/core/include/nimbus/orch/day_start.h": "RB-REC5: the day start setting's error sentence for the web page and "
    "/api/config, never sent to a model (the stamp's own words are in lib/core/src/orch_day_start.cpp, catalogued)",
    "lib/core/src/ota_channel.cpp": "RB-OTA2: the owner's /ota answers and the /api/ota/channel JSON lines, sent to the "
    "owner's chat or a web client as device copy, never to a model",
    "src/net/ota_channel_routes.cpp": "RB-OTA2: the route's 401 body for a web client (the same literal every owner "
    "route sends), never sent to a model",
    # RB-Q1c: the send seam. Its one literal is the owner-facing overflow line of a reply over
    # kTelegramMaxParts parts (moved byte for byte from src/agent/telegram.cpp send()); it goes
    # to the chat, never into a request (history keeps the engine's reply, not the parts).
    "lib/core/src/orch_deliver_text.cpp": "owner-facing send seam: the overflow line only, never sent to a model",
    # RB-BG: the history profile's governor and the /budget profile store. Their literals are the
    # owner's /usage and /budget replies, the governor's one-line notice (sent to the owner's chat
    # and kept in the web chat's history like the spend ceiling's notices) and the Usage page's
    # change record; none is system text, a tool description or a tool result.
    "lib/core/src/orch_budget_governor.cpp": "RB-BG: owner-facing /usage and /budget profile replies and the "
    "governor's owner notice, never system text, a tool or a tool result",
    "src/agent/budget_profile_store.cpp": "RB-BG: the /budget profile reply when the file does not store, owner "
    "copy, never sent to a model",
    # E2-DEV: the engine-2 device seam. Every model-facing byte is the engine's (plain/texts.h); the seam's
    # own literals are owner-facing device copy delivered with system provenance before any model turn
    # (a head that is not Claude, no key, the monthly limit, a chat that could not start), its fold status
    # line, a routine's refusal reason and log lines.
    "lib/harness/include/nimbus/harness/plain/chat_binding.h": "E2-DEV: the device chat binding's log lines (through "
    "its log callback), never sent to a model or an owner",
    "src/agent/orchestrator_plain.cpp": "E2-DEV: the engine-2 seam's owner-facing refusal lines (not Claude, no key, "
    "monthly limit, could not start), its fold status line and its no-routines reason, device copy; the model's text "
    "is the engine's. E2-CHAT: the too-large skill line and the /budget view (coverage/PROMPT-CHANGES.md PC-0177, "
    "PC-0178), owner-facing device copy",
    # E2-CHAT: the owner's slash commands on engine 2 answer with device copy before any model turn.
    "src/agent/commands_plain.cpp": "E2-CHAT: the owner's command replies on engine 2 (engine 1's own replies reused, "
    "the not-yet line and /clear's question and answer: PC-0177), delivered with system provenance, never sent to a "
    "model",
}

# The engine-1 baseline: each class's bytes and sha8 when the gate was introduced. It is the
# baseline table of coverage/PROMPT-CHANGES.md too (check_no_prompt_patch.py fails when they
# differ), and the byte ceilings of engine 1: it only shrinks, a ceiling is never raised.
E1_BASELINE = {
    "system": (47989, "faa8207c"),
    "device_notes": (54203, "c415e340"),
    "tool_descriptions": (23131, "542ad7ca"),
    "tool_results": (41962, "63debd41"),
    "side_prompts": (9795, "fada0e91"),
    "skills": (23264, "cef6121d"),
}
E1_CEILINGS = {cls: size for cls, (size, _sha) in E1_BASELINE.items()}

# ---------------------------------------------------------------------------
# Engine 2: texts.h by namespace (PRD section 4 ceilings).
# ---------------------------------------------------------------------------

E2_TEXTS = "lib/harness/include/nimbus/harness/plain/texts.h"
E2_CLASSES = ("core", "device_notes", "stubs", "tool_results", "probe", "summarizer", "tool_descriptions")
E2_CEILINGS = {
    "core": 3072,
    "device_notes": 2048,
    "stubs": 512,
    "tool_results": 6144,
    "probe": 1536,  # ruling 33: the qualified probe prompt (1,296 B) is used byte for byte
    "summarizer": 1024,
    "tool_descriptions": 10240,
}
# RB-E2: a manifest file outside texts.h whose literals are model text, as
# (path, class); every literal of it counts (not only prose), like texts.h.
E2_EXTRA_SOURCES: tuple = (
    # RB-N2a: the plain file tools keep their texts in one block until texts.h exists; RB-E2 moves them.
    ("lib/core/src/orch_file_tools.cpp", "tool_results"),
    # RB-C1: the device-written pieces of the digest block and the recent-files list.
    ("lib/core/include/nimbus/orch/tx/digest.h", "core"),
    # RB-REC8: the calc tool's result and error lines (phase 2 moves them into texts.h).
    ("lib/core/src/orch_calc.cpp", "tool_results"),
    # RB-REC4: the general file aids (CSV totals, the line pick, the append facts); their
    # results and errors stay in these files until phase 2 binds them to the plain tools.
    ("lib/core/include/nimbus/orch/file_aids.h", "tool_results"),
    ("lib/core/src/orch_csv_sum.cpp", "tool_results"),
    ("lib/core/src/orch_file_aids.cpp", "tool_results"),
    # RB-MG1a: the device notes a migration and a re-entry store (the anchor and the import note).
    ("lib/core/include/nimbus/orch/tx/migrate.h", "device_notes"),
    # RB-G1: the truth probe engine 2 sends (RB-G0's measured prompt and its model), shared with engine 1.
    ("lib/core/include/nimbus/orch/truth.h", "probe"),
    # RB-N1: the note tool's results and errors (lib/core, beside its rules, as the file tools').
    ("lib/core/src/orch_notes.cpp", "tool_results"),
    # RB-TG1: what a stored PDF's line in the owner message says after its inbox path.
    ("lib/core/include/nimbus/net/tg_inbox.h", "stubs"),
    # E2-TOOLS: the engine-2 device registrations (web_search over the old web.search: its names and argument).
    ("src/agent/tools_plain.cpp", "tool_results"),
)
# The engine-2 files that may hold no model text of their own once texts.h exists.
E2_ENGINE_GLOBS = (r"lib/harness/src/plain_[^/]*\.(cpp|h)$", r"lib/harness/src/providers/plain_wire_[^/]*\.(cpp|h)$")
# Calls in engine-2 files whose literals are not model text (stub markers).
E2_NON_MODEL_CALLS = frozenset({"pending"})

# phase "build": the engine is being written to its spec, and a PROMPT-CHANGES entry of
# kind build may grow a class inside its ceiling. phase "frozen": growth always fails.
# e2 flips to frozen at the engine-2 candidate gate (PRD stage S2).
CATALOGS = {
    "e1": {"golden": "test/golden/model_text_catalog_e1.txt", "phase": "frozen", "classes": E1_CLASSES},
    "e2": {"golden": "test/golden/model_text_catalog.txt", "phase": "build", "classes": E2_CLASSES},
}

LOG_CALLS = frozenset(
    {
        "logf",
        "log",
        "logLine",
        "alog",
        "alogf",
        "beat",
        "printf",
        "fprintf",
        "log_e",
        "log_w",
        "log_i",
        "log_d",
        "ESP_LOGE",
        "ESP_LOGW",
        "ESP_LOGI",
        "ESP_LOGD",
        "ESP_LOGV",
        "Serial.printf",
        "Serial.println",
        "Serial.print",
        "static_assert",
    }
)
RESULT_CALLS = frozenset({"ok", "fail"})  # ToolResult::ok / ToolResult::fail
ADD_CALLS = frozenset({"add", "addOwnerOnly"})  # tool registration: (reg,) name, description, handler, schema
TOOL_FILE = re.compile(
    r'\bToolResult::(?:ok|fail)\s*\(|(?<!struct )\bToolResult\s*\{|registry\(\)\s*\.add\s*\(|'
    r'\.add\(\s*"[a-z][a-z0-9_]*\.[a-z0-9_.]+"'
)
SCAN_DIRS = ("lib", "src", "nimbusd/src")
SCAN_SKIP = ("lib/core/include/nimbus/orch/tool_registry.h",)


@dataclass
class Literal:
    text: str
    line: int
    calls: tuple = ()  # enclosing call names, outermost first
    arg_index: int = -1  # argument position in the innermost call (-1: not in a call)
    namespaces: tuple = ()  # enclosing namespace names, outermost first
    names: tuple = ()  # identifiers of the statement before the literal (kAppendDesc, ...)
    in_add: bool = False  # a direct argument of a tool registration call, outside a lambda body

    @property
    def logged(self) -> bool:
        return any(c in LOG_CALLS for c in self.calls)


_ESC = {
    "n": "\n",
    "t": "\t",
    "r": "\r",
    "\\": "\\",
    "'": "'",
    '"': '"',
    "?": "?",
    "a": "\a",
    "b": "\b",
    "f": "\f",
    "v": "\v",
}


def _unescape(body: str) -> str:
    """C escapes to text. Byte escapes (\\xNN, octal) are gathered as bytes and decoded as UTF-8."""
    out = bytearray()
    i = 0
    while i < len(body):
        c = body[i]
        if c != "\\":
            out += c.encode("utf-8")
            i += 1
            continue
        n = body[i + 1] if i + 1 < len(body) else ""
        if n in "\r\n" and n:  # a backslash-newline inside a literal: the compiler drops both
            i += 2 + (body[i + 1 : i + 3] == "\r\n")
        elif n == "x":
            m = re.match(r"[0-9A-Fa-f]{1,2}", body[i + 2 :])
            out.append(int(m.group(0), 16) if m else ord("x"))
            i += 2 + (len(m.group(0)) if m else 0)
        elif n in "01234567" and n:
            m = re.match(r"[0-7]{1,3}", body[i + 1 :])
            out.append(int(m.group(0), 8) & 0xFF)
            i += 1 + len(m.group(0))
        elif n in ("u", "U"):
            k = 4 if n == "u" else 8
            out += chr(int(body[i + 2 : i + 2 + k], 16)).encode("utf-8")
            i += 2 + k
        else:
            out += _ESC.get(n, n).encode("utf-8")
            i += 2
    return out.decode("utf-8", errors="replace")


_STR_START = re.compile(r'(?:u8|u|U|L)?(R)?"')
_DIRECTIVE_SKIP = re.compile(r"#\s*(include|pragma|error|warning)\b")


class _Lexer:
    """One pass over a C/C++ source: string literals with the calls, namespaces and
    statement names around them. Adjacent literals are joined as the compiler joins them."""

    def __init__(self, src: str):
        self.src, self.n = src, len(src)
        self.i, self.line = 0, 1
        self.stack: list = []  # [kind, name, arg_index], kind: call | paren | brace | ns
        self.stmt: list = []  # identifiers since the last ; { or }
        self.out: list = []
        self.pending_ns = None  # "" after the keyword namespace, then its name
        self.last_ident = ""
        self.cur = None  # the literal being joined: [text, first line]

    def run(self) -> list:
        while self.i < self.n:
            if self._skip() or self._string():
                continue
            self._flush()
            if self._char_literal() or self._ident():
                continue
            self._punct(self.src[self.i])
            self.i += 1
        self._flush()
        return self.out

    # -- what does not end a literal run --------------------------------------------------
    def _skip(self) -> bool:
        src, i = self.src, self.i
        c = src[i]
        if c in " \t\r\f\v\n":
            self.line += c == "\n"
            self.i += 1
        elif c == "\\" and src.startswith("\n", i + 1 + (src[i + 1 : i + 2] == "\r")):  # macro line continuation
            self.i += 1
        elif src.startswith("//", i):
            self.i = self._eol(i)
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = self.n if j < 0 else j + 2
            self.line += src.count("\n", i, j)
            self.i = j
        elif c == "#" and src[src.rfind("\n", 0, i) + 1 : i].strip() == "" and _DIRECTIVE_SKIP.match(src, i):
            self.i = self._eol(i)
        else:
            return False
        return True

    def _eol(self, i: int) -> int:
        j = self.src.find("\n", i)
        return self.n if j < 0 else j

    def _string(self) -> bool:
        src, i = self.src, self.i
        m = _STR_START.match(src, i) if src[i] in 'uULR"' else None
        if not m or (i > 0 and (src[i - 1].isalnum() or src[i - 1] == "_")):
            return False
        if m.group(1):  # raw string R"delim( ... )delim"
            k = src.index("(", m.end())
            delim = src[m.end() : k]
            end = src.index(")" + delim + '"', k)
            body, text = src[k + 1 : end], src[k + 1 : end]
            self.i = end + len(delim) + 2
        else:
            j = m.end()
            while src[j] != '"':
                j += 2 if src[j] == "\\" else 1
            body = src[m.end() : j]
            text = _unescape(body)
            self.i = j + 1
        if self.cur is None:
            self.cur = ["", self.line]
        self.cur[0] += text
        self.line += body.count("\n")
        return True

    def _flush(self) -> None:
        if self.cur is None:
            return
        stack = self.stack
        inner = next((e for e in reversed(stack) if e[0] in ("call", "paren")), None)
        in_add = False
        for e in reversed(stack):
            if e[0] in ("brace", "ns"):
                break
            if e[0] == "call" and e[1] in ADD_CALLS:
                in_add = True
                break
        self.out.append(
            Literal(
                text=self.cur[0],
                line=self.cur[1],
                calls=tuple(e[1] for e in stack if e[0] == "call"),
                arg_index=inner[2] if inner is not None and inner[0] == "call" else -1,
                namespaces=tuple(e[1] for e in stack if e[0] == "ns"),
                names=tuple(self.stmt),
                in_add=in_add,
            )
        )
        self.cur = None

    # -- tokens between literals ----------------------------------------------------------
    def _char_literal(self) -> bool:
        if self.src[self.i] != "'":
            return False
        k = self.i
        while k > 0 and (self.src[k - 1].isalnum() or self.src[k - 1] == "_"):
            k -= 1
        if k < self.i and self.src[k].isdigit():  # a digit separator (1'000, 0xFF'FF), not a char literal
            self.i += 1
            return True
        j = self.i + 1
        while j < self.n and self.src[j] != "'":
            j += 2 if self.src[j] == "\\" else 1
        self.i = j + 1
        return True

    def _ident(self) -> bool:
        src, i = self.src, self.i
        if not (src[i].isalpha() or src[i] == "_"):
            return False
        j = i
        while j < self.n and (src[j].isalnum() or src[j] == "_"):
            j += 1
        word = src[i:j]
        if i > 0 and src[i - 1] == "." and self.last_ident:  # Serial.printf: keep the owner for log detection
            k = i - 2
            while k >= 0 and (src[k].isalnum() or src[k] == "_"):
                k -= 1
            if src[k + 1 : i - 1]:
                word = src[k + 1 : i - 1] + "." + word
        if word == "namespace":
            self.pending_ns = ""
        elif self.pending_ns == "":
            self.pending_ns = word
        self.last_ident = word
        self.stmt.append(word)
        self.i = j
        return True

    def _punct(self, c: str) -> None:
        if c in ":<>":
            return  # keep the name across nimbus::hlog::logf and template arguments
        if c == "(":
            self.stack.append(["call", self.last_ident, 0] if self.last_ident else ["paren", "", 0])
        elif c == "{":
            self.stmt.clear()
            self.stack.append(
                ["ns", self.pending_ns or "<anon>", 0] if self.pending_ns is not None else ["brace", "", 0]
            )
            self.pending_ns = None
        elif c in ")}":
            self._close(("brace", "ns") if c == "}" else ("call", "paren"))
            if c == "}":
                self.stmt.clear()
        elif c == "," and self.stack and self.stack[-1][0] == "call":
            self.stack[-1][2] += 1
        elif c == ";":
            self.stmt.clear()
            self.pending_ns = None
        self.last_ident = ""

    def _close(self, kinds: tuple) -> None:
        while self.stack:
            if self.stack.pop()[0] in kinds:
                return


def extract_literals(src: str) -> list:
    """Every string literal of a C/C++ source, adjacent literals joined, with the calls,
    namespaces and statement names that enclose it. Comments, char literals and
    #include / #pragma lines are skipped."""
    return _Lexer(src).run()


_PROSE = re.compile(r"[A-Za-z]{2,}")
_SPACE = re.compile(r"\s")


def is_prose(text: str) -> bool:
    """A literal a model could read as words: letters and any whitespace (a newline or NBSP counts)."""
    return _SPACE.search(text) is not None and _PROSE.search(text) is not None


# ---------------------------------------------------------------------------
# Catalog
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Entry:
    cls: str
    path: str
    text: str

    @property
    def size(self) -> int:
        return len(self.text.encode("utf-8"))

    @property
    def sha8(self) -> str:
        return sha8(self.text)


def sha8(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()[:8]


def class_sha8(texts) -> str:
    """The identity of a class: its texts as a multiset, so a move between files is neutral."""
    return hashlib.sha256("\0".join(sorted(texts)).encode("utf-8")).hexdigest()[:8]


class CatalogError(Exception):
    pass


def _read(root: str, rel: str) -> "str | None":
    path = os.path.join(root, rel)
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


# A declared name that says it holds a description: kAppendDesc, desc, kListSchema, description.
_DESC_NAME = re.compile(r"^(k\w*(Desc|Description|Schema)\w*|\w*_?(desc|description|schema))$", re.I)


def _is_tool_description(lit: Literal) -> bool:
    """The description or schema of a tool: an add() argument, a JSON schema, or a named description."""
    return lit.in_add or _is_schema(lit.text) or any(_DESC_NAME.match(n) for n in lit.names)


def _is_schema(text: str) -> bool:
    t = text.lstrip()
    return t.startswith("{") and '"type"' in t


def literal_class(lit: Literal, file_cls: str) -> str:
    """A tool result's text is a tool result, a registration's text a description, the rest the file's class."""
    if RESULT_CALLS.intersection(lit.calls):
        return "tool_results"
    if _is_tool_description(lit):
        return "tool_descriptions"
    return file_cls


def model_literals(rel: str, src: str) -> list:
    """The prose literals of a source that are not written by a log call."""
    try:
        lits = extract_literals(src)
    except (ValueError, IndexError) as e:  # an unterminated literal: fail with the gate's marker, not a trace
        raise CatalogError(f"{rel}: its string literals cannot be read ({e})") from e
    return [lit for lit in lits if not lit.logged and is_prose(lit.text)]


def _e1_file_entries(cls: str, rel: str, src: str) -> list:
    return [Entry(literal_class(lit, cls), rel, lit.text) for lit in model_literals(rel, src)]


def e1_sources() -> list:
    """(class, path) for every engine-1 source."""
    return [(cls, p) for cls, paths in E1_SOURCES.items() for p in paths]


def catalogued_paths() -> set:
    return {p for _, p in e1_sources()} | {p for p, _ in E2_EXTRA_SOURCES} | {E2_TEXTS}


def _scan_files(root: str):
    for d in SCAN_DIRS:
        for dirpath, dirnames, files in os.walk(os.path.join(root, d)):
            dirnames.sort()
            for f in sorted(files):
                if f.endswith((".cpp", ".h", ".hpp", ".cc")):
                    yield os.path.relpath(os.path.join(dirpath, f), root).replace(os.sep, "/")


def _is_e2_engine_file(rel: str) -> bool:
    return any(re.match(g, rel) for g in E2_ENGINE_GLOBS)


def uncatalogued_tool_files(root: str) -> list:
    """Files that build a tool result or register a tool but are in no catalog."""
    listed = catalogued_paths()
    out = []
    for rel in _scan_files(root):
        if rel in listed or rel in SCAN_SKIP or _is_e2_engine_file(rel):
            continue
        src = _read(root, rel) or ""
        if TOOL_FILE.search(src):
            out.append(rel)
    return out


def build_e1(root: str) -> list:
    entries, missing = [], []
    for cls, rel in e1_sources():
        src = _read(root, rel)
        if src is None:
            missing.append(rel)
            continue
        entries += _e1_file_entries(cls, rel, src)
    if missing:
        # A deleted or moved engine-1 file leaves the list in the same change, so a rename cannot
        # drop its text from the catalog unseen.
        raise CatalogError(
            "E1_SOURCES names a file that does not exist (remove or rename it there): " + ", ".join(missing)
        )
    stray = uncatalogued_tool_files(root)
    if stray:
        raise CatalogError(
            "a file builds a tool result or registers a tool but is in no catalog class "
            "(add it to E1_SOURCES): " + ", ".join(stray)
        )
    return entries


def _e2_literal_entries(rel: str, src: str, default_cls: "str | None") -> list:
    out, bad = [], []
    try:
        lits = extract_literals(src)
    except (ValueError, IndexError) as e:
        raise CatalogError(f"{rel}: its string literals cannot be read ({e})") from e
    for lit in lits:
        cls = next((n for n in reversed(lit.namespaces) if n in E2_CLASSES), default_cls)
        if cls is None:
            bad.append(f"{rel}:{lit.line}")
            continue
        out.append(Entry(cls, rel, lit.text))
    if bad:
        raise CatalogError(
            "a literal in texts.h sits in no class namespace (" + ", ".join(E2_CLASSES) + "): " + ", ".join(bad)
        )
    return out


def e2_stray_literals(root: str) -> list:
    """Prose literals in plain engine files: model text belongs in texts.h."""
    out = []
    for rel in _scan_files(root):
        if not _is_e2_engine_file(rel):
            continue
        for lit in model_literals(rel, _read(root, rel) or ""):
            if E2_NON_MODEL_CALLS.intersection(lit.calls):
                continue
            out.append(f"{rel}:{lit.line} {lit.text[:60]!r}")
    return out


def build_e2(root: str) -> "list | None":
    """None while texts.h does not exist (RB-E2 creates it)."""
    src = _read(root, E2_TEXTS)
    if src is None:
        return None
    entries = _e2_literal_entries(E2_TEXTS, src, None)
    for rel, cls in E2_EXTRA_SOURCES:
        extra = _read(root, rel)
        if extra is None:
            raise CatalogError(f"E2_EXTRA_SOURCES names a missing file: {rel}")
        entries += _e2_literal_entries(rel, extra, cls)
    stray = e2_stray_literals(root)
    if stray:
        raise CatalogError("model text outside texts.h in a plain engine file: " + "; ".join(stray))
    return entries


BUILDERS = {"e1": build_e1, "e2": build_e2}


def ceilings(name: str) -> dict:
    return dict(E1_CEILINGS if name == "e1" else E2_CEILINGS)


def class_stats(name: str, entries: list) -> dict:
    """{class: {bytes, entries, sha8}} in the catalog's class order."""
    out = {}
    for cls in CATALOGS[name]["classes"]:
        texts = [e.text for e in entries if e.cls == cls]
        out[cls] = {
            "bytes": sum(len(t.encode("utf-8")) for t in texts),
            "entries": len(texts),
            "sha8": class_sha8(texts),
        }
    return out


def render(name: str, entries: list) -> str:
    stats = class_stats(name, entries)
    ceil = ceilings(name)
    lines = [
        f"# Model text catalog {name}: generated by tools/release_gate/gen_model_text_catalog.py; do not edit.",
        "# Regenerate with --write. Gate: tools/release_gate/check_no_prompt_patch.py (coverage/PROMPT-CHANGES.md).",
        "# One entry per line: class, source, sha8, bytes, the text as a JSON string.",
    ]
    for cls, st in stats.items():
        lines.append(
            f"# class {cls} bytes={st['bytes']} ceiling={ceil.get(cls, 0)} entries={st['entries']} sha8={st['sha8']}"
        )
    order = {c: i for i, c in enumerate(CATALOGS[name]["classes"])}
    for e in sorted(entries, key=lambda e: (order[e.cls], e.path)):  # stable: keeps source order in a file
        lines.append(f"{e.cls}\t{e.path}\t{e.sha8}\t{e.size}\t{json.dumps(e.text, ensure_ascii=True)}")
    return "\n".join(lines) + "\n"


_CLASS_LINE = re.compile(r"^# class (\w+) bytes=(\d+) ceiling=(\d+) entries=(\d+) sha8=([0-9a-f]{8})$")


def parse(text: str) -> dict:
    """A rendered catalog back to {"classes": {cls: {bytes, ceiling, entries, sha8}}, "entries": [Entry]}."""
    classes, entries = {}, []
    for line in text.splitlines():
        m = _CLASS_LINE.match(line)
        if m:
            classes[m.group(1)] = {
                "bytes": int(m.group(2)),
                "ceiling": int(m.group(3)),
                "entries": int(m.group(4)),
                "sha8": m.group(5),
            }
            continue
        if not line or line.startswith("#"):
            continue
        cls, path, _sha, _size, js = line.split("\t", 4)
        entries.append(Entry(cls, path, json.loads(js)))
    return {"classes": classes, "entries": entries}


def build_all(root: str) -> dict:
    """{catalog name: entries or None (not live yet)}."""
    return {name: BUILDERS[name](root) for name in CATALOGS}


# ---------------------------------------------------------------------------
# Re-baseline (the orchestrator's step when a release line is taken into the integration branch)
# ---------------------------------------------------------------------------

CHANGES = "coverage/PROMPT-CHANGES.md"
SELF = "tools/release_gate/gen_model_text_catalog.py"
_REBASE_HEAD = re.compile(r"^### (RBL-(\d{4}))\b", re.M)


def _git_short(root: str) -> str:
    import subprocess

    r = subprocess.run(["git", "-C", root, "rev-parse", "--short=8", "HEAD"], capture_output=True, text=True)
    return r.stdout.strip() or "unknown"


def _last_entry_id(changes: str) -> str:
    ids = re.findall(r"^### (PC-\d{4})\b", changes.split("\n## Changes", 1)[-1], re.M)
    return ids[-1] if ids else "none"


def rebaseline(root: str, why: str) -> str:
    """Re-measure engine 1 on this tree and record it as the new baseline.

    Rewrites the golden, E1_BASELINE in this file (the engine-1 ceilings) and the ledger's
    baseline table, and appends a re-baseline record (old -> new per class, the commit, the entries
    it supersedes, why). check_no_prompt_patch.py accepts a changed baseline only with such a
    record. Returns the record."""
    entries = build_e1(root)
    stats = class_stats("e1", entries)
    E1_BASELINE.clear()
    E1_BASELINE.update({c: (v["bytes"], v["sha8"]) for c, v in stats.items()})
    E1_CEILINGS.clear()
    E1_CEILINGS.update({c: v["bytes"] for c, v in stats.items()})
    with open(os.path.join(root, CATALOGS["e1"]["golden"]), "w", encoding="utf-8") as f:
        f.write(render("e1", entries))  # its class lines carry the new ceilings
    src = _read(root, SELF) or ""
    m = re.search(r"^E1_BASELINE = \{\n(?:    .*\n)*\}", src, re.M)
    if m is None:
        raise CatalogError(f"{SELF}: no E1_BASELINE block")
    old = {c: (int(b), sh) for c, b, sh in re.findall(r'"(\w+)": \((\d+), "([0-9a-f]{8})"\)', m.group(0))}
    block = (
        "E1_BASELINE = {\n" + "".join(f'    "{c}": ({v["bytes"]}, "{v["sha8"]}"),\n' for c, v in stats.items()) + "}"
    )
    with open(os.path.join(root, SELF), "w", encoding="utf-8") as f:
        f.write(src[: m.start()] + block + src[m.end() :])
    changes = _read(root, CHANGES) or ""
    head = _git_short(root)
    table = "".join(f"| e1 | {c} | {v['bytes']} | {v['sha8']} |\n" for c, v in stats.items())
    changes = re.sub(
        r"(\| Catalog \| Class \| Bytes \| sha8 \|\n\|---\|---\|---\|---\|\n)(?:\| e1 .*\n)*",
        lambda mm: mm.group(1) + table,
        changes,
        count=1,
    )
    changes = re.sub(
        r"^Measured on .*?\. The engine-1 ceilings in$",
        f"Measured on rb-main {head} (the latest re-baseline below). The engine-1 ceilings in",
        changes,
        count=1,
        flags=re.M,
    )
    n = max([int(x) for _, x in _REBASE_HEAD.findall(changes)] + [0]) + 1
    lines = [f"### RBL-{n:04d}: re-measured on {head}", f"- base: {head}", f"- supersedes: {_last_entry_id(changes)}"]
    for c, v in stats.items():
        b0, s0 = old.get(c, (0, "00000000"))
        lines.append(f"- e1 {c}: {b0} {s0} -> {v['bytes']} {v['sha8']} ({v['bytes'] - b0:+d} B)")
    lines.append(f"- why: {why}")
    record = "\n".join(lines) + "\n"
    if "\n## Re-baselines\n" not in changes:
        changes = changes.replace("\n## Changes\n", "\n## Re-baselines\n\n## Changes\n", 1)
    changes = changes.replace("\n## Changes\n", "\n" + record + "\n## Changes\n", 1)
    with open(os.path.join(root, CHANGES), "w", encoding="utf-8") as f:
        f.write(changes)
    return record


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=REPO)
    ap.add_argument("--write", action="store_true", help="regenerate the goldens")
    ap.add_argument("--summary", action="store_true", help="print bytes per class")
    ap.add_argument(
        "--rebaseline",
        metavar="WHY",
        help="orchestrator only: re-measure engine 1 here as the new baseline, with the reason",
    )
    args = ap.parse_args(argv)
    if args.rebaseline:
        try:
            print(rebaseline(args.root, args.rebaseline), end="")
        except CatalogError as e:
            print(f"model text catalog: {e}")
            print("MODEL TEXT CATALOG RED")
            return 1
        print("REBASELINE WRITTEN: review the diff, then run check_no_prompt_patch.py")
        return 0
    try:
        built = build_all(args.root)
    except CatalogError as e:
        print(f"model text catalog: {e}")
        print("MODEL TEXT CATALOG RED")
        return 1
    ok = True
    for name, entries in built.items():
        golden = os.path.join(args.root, CATALOGS[name]["golden"])
        if entries is None:
            if os.path.exists(golden):
                print(f"{name}: {CATALOGS[name]['golden']} exists but its sources do not")
                ok = False
            continue
        text = render(name, entries)
        if args.summary:
            for cls, st in class_stats(name, entries).items():
                print(
                    f"{name} {cls:18} {st['bytes']:7d} B  {st['entries']:4d} entries  ceiling "
                    f"{ceilings(name).get(cls, 0)}"
                )
        if args.write:
            with open(golden, "w", encoding="utf-8") as f:
                f.write(text)
            print(f"wrote {CATALOGS[name]['golden']} ({len(entries)} entries)")
            continue
        cur = _read(args.root, CATALOGS[name]["golden"])
        if cur != text:
            print(
                f"{name}: {CATALOGS[name]['golden']} is stale: model-facing text changed. Regenerate with "
                "--write, then log the change in coverage/PROMPT-CHANGES.md"
            )
            ok = False
    print("MODEL TEXT CATALOG GREEN" if ok else "MODEL TEXT CATALOG RED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
