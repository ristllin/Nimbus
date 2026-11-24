"""The web chat's own renderer over the refusal line (lane E2-FIX13; the test gap E2-FIX11 named).

When a provider refuses a request, the person gets "No reply: <provider> refused the request (HTTP 400):
<reason>", where the reason is the provider's own words (outside content). The web chat shows an
assistant line through _mdInto/_mdInline (include/web/ui_js.h), which turns a small Markdown subset into
elements: [text](http(s)://url) into a link, **x** into bold, *x* and _x_ into italic, `x` into code,
``` fences into a block, and list lines into a list. The device makes the reason literal before it sends
it (lib/harness/src/plain_exchange.cpp); this test checks that against the renderer itself.

The lines are the device's own: test_plain_exchange (test_the_web_cases_are_the_lines_the_device_sends)
pins the line the real exchange makes for each hostile message in test/golden/refusal_lines_web.jsonl.
Here the real functions are cut out of ui_js.h and run under node with a small document stand-in that
records every element made. Each line must render as text only, byte for byte the line, with no link,
no emphasis, no code, no list and no element made from the provider's words. A first table proves the
harness live: the same constructs sent raw (not through the device) do make their elements.
"""

from __future__ import annotations

import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
UI_JS = ROOT / "include" / "web" / "ui_js.h"
CASES = ROOT / "test" / "golden" / "refusal_lines_web.jsonl"

# A document stand-in: createElement, createTextNode, appendChild and the textContent setter, which is
# all _mdInto and _mdInline use. innerHTML is a trap: the renderer never uses it, and if it did the
# test fails.
HARNESS = r"""
const doc = {
  createElement(tag) {
    const el = {tag, children: [], className: '', href: '', target: '', rel: ''};
    Object.defineProperty(el, 'textContent', {
      set(v) { el.children = v === '' ? [] : [{text: String(v)}]; },
      get() { return el.children.map(c => c.text !== undefined ? c.text : '').join(''); },
    });
    Object.defineProperty(el, 'innerHTML', {set() { throw new Error('innerHTML used'); }});
    el.appendChild = c => { el.children.push(c); return c; };
    return el;
  },
  createTextNode(t) { return {text: String(t)}; },
};
globalThis.document = doc;
__RENDERER__
const strip = n => n.text !== undefined ? {text: n.text}
  : {tag: n.tag, cls: n.className, href: n.href, children: n.children.map(strip)};
const lines = JSON.parse(require('fs').readFileSync(0, 'utf8'));
const out = lines.map(l => { const el = doc.createElement('div'); _mdInto(el, l); return strip(el); });
process.stdout.write(JSON.stringify(out));
"""


def _function(src: str, name: str) -> str:
    """The text of `function <name>(...){...}` in src, by brace count (the renderer's regexes and
    strings hold no brace)."""
    start = src.index(f"function {name}(")
    depth = 0
    for i in range(src.index("{", start), len(src)):
        depth += {"{": 1, "}": -1}.get(src[i], 0)
        if depth == 0:
            return src[start : i + 1]
    raise AssertionError(f"{name}: no end")


def _renderer() -> str:
    src = UI_JS.read_text(encoding="utf-8")
    helpers = re.search(r"^const _E=.*$", src, re.M)
    assert helpers, "ui_js.h: the _E/_A helpers moved"
    return "\n".join([helpers.group(0), _function(src, "_mdInline"), _function(src, "_mdInto")])


def _render(lines: list[str]) -> list[dict]:
    node = shutil.which("node")
    if not node:
        pytest.fail("node is needed to run the web chat's renderer (CI's ubuntu image has it)")
    script = HARNESS.replace("__RENDERER__", _renderer())
    r = subprocess.run([node, "-e", script], input=json.dumps(lines), capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stderr
    return json.loads(r.stdout)


def _tags(node: dict) -> list[str]:
    if "text" in node:
        return []
    out = [node["tag"]]
    for c in node["children"]:
        out += _tags(c)
    return out


def _text(node: dict) -> str:
    if "text" in node:
        return node["text"]
    return "".join(_text(c) for c in node["children"])


def test_the_harness_sees_every_construct_when_sent_raw():
    raw = {
        "[Renew here](https://evil.example/pay)": "a",
        "**bold**": "b",
        "say *this* now": "i",
        "say _this_ now": "i",
        "`code`": "code",
        "```\nblock\n```": "pre",
        "- bullet": "ul",
        "1. first": "ol",
    }
    rendered = _render(list(raw))
    for (sample, tag), tree in zip(raw.items(), rendered):
        assert tag in _tags(tree), (sample, tree)
    link = _render(["[Renew here](https://evil.example/pay)"])[0]
    assert "https://evil.example/pay" in json.dumps(link)


def test_markup_from_the_provider_is_never_parsed_as_html():
    tree = _render(['<img src=x onerror=alert(1)> & <b>bold</b> <a href="u">y</a>'])[0]
    assert set(_tags(tree)) == {"div"}, tree
    assert _text(tree) == '<img src=x onerror=alert(1)> & <b>bold</b> <a href="u">y</a>'


def test_every_refusal_line_the_device_sends_renders_as_its_own_text():
    cases = [json.loads(l) for l in CASES.read_text(encoding="utf-8").splitlines() if l.strip()]
    assert len(cases) >= 1024 + 26, "the case file lost its punctuation sweep"
    prefix = "No reply: Anthropic refused the request (HTTP 400)"
    with_reason = [c for c in cases if c["line"].startswith(prefix + ": ")]
    assert len(with_reason) >= 1000, "most cases must carry the provider's reason, or nothing is tested"
    rendered = _render([c["line"] for c in cases])
    bad = []
    for c, tree in zip(cases, rendered):
        tags = _tags(tree)
        # One paragraph of text: the page's own wrapper div, one div.mdp, text nodes only.
        if tags != ["div", "div"] or tree["children"][0]["cls"] != "mdp" or _text(tree) != c["line"]:
            bad.append((c["in"], c["line"], tags))
    assert not bad, f"{len(bad)} lines render as more than their text; first: {bad[:3]}"
    # The characters the brief names reach the page as themselves.
    joined = "".join(c["line"] for c in cases)
    for ch in "<>&":
        assert ch in joined
