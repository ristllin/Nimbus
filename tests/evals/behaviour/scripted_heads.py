#!/usr/bin/env python3
"""scripted_heads - offline stand-ins for a head, for dry runs of every records arm (no network).

`good` does each episode the way the owner's skill says (reads its skill when one is indexed, totals
with read_file sum in arm S and by reading the rows in arm F, names the rows a total leaves out, edits
one line); `sloppy` makes the classic mistakes (adds comment lines and wrong-field-count rows into a
total without a word, appends twice, edits with all=true, claims a write that failed). They implement
the providers' interface (render, user, results, step) on neutral items.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from datetime import datetime, timedelta

import providers
import records

STAMP_RE = re.compile(r"\[(\d{4}-\d{2}-\d{2}) \w{3} (\d{2}):(\d{2})(?: \(day (\d{4}-\d{2}-\d{2}) until)?")
PATH_RE = re.compile(r"\b((?:food|money|gym)/[a-z]+\.csv)")


def _csv_rows(text: str) -> tuple:
    """(header, [(line no, fields)]) of a read_file result's raw bytes (after the first result line)."""
    body = text.split("\n", 1)[1] if "\n" in text else ""
    lines = body.split("\n")
    header = lines[0].split(",")
    rows = []
    for k, raw in enumerate(lines[1:], start=2):
        rows.append((k, raw))
    return header, rows


@dataclass
class Ctx:
    """What a scripted head knows at one step: the owner's message and the calls on the table so far."""

    owner: str
    body: str
    day: str
    has_sum: bool
    dom: str = ""
    path: str = ""
    calls: list = field(default_factory=list)
    results: list = field(default_factory=list)


def _wanted(body: str) -> str:
    """The table's folder the request is about, from its words."""
    if "kcal" in body:
        return "food"
    if re.search(r"[Ss]pen[td]|actually", body):
        return "money"
    return "gym" if re.search(r"sets|reps", body) else ""


def _paths(text: str, want: str) -> list:
    return [p for p in PATH_RE.findall(text) if p.startswith(want)]


def _yesterday(day: str) -> str:
    return (datetime.strptime(day, "%Y-%m-%d") - timedelta(days=1)).strftime("%Y-%m-%d")


def _sum_rows(rows: list, header: list, col: str, lo: str, hi: str, sloppy: bool) -> tuple:
    """(total, skipped line numbers) as a careful or a sloppy reader adds the rows of [lo, hi] up."""
    i = header.index(col)
    total, skipped = 0.0, []
    for n, raw in rows:
        if sloppy:  # every line that mentions the range, comments included
            f = raw.lstrip("# replaced:").split(",")
            if lo <= f[0].strip()[:10] <= hi and len(f) > i:
                total += _num(f[i])
            continue
        f = raw.split(",")
        if not raw.strip() or raw.lstrip().startswith("#") or not (lo <= f[0] <= hi):
            continue
        if len(f) != len(header):
            skipped.append(n)
        elif f[i]:
            total += float(f[i])
    return total, skipped


def _num(v: str) -> float:
    try:
        return float(v)
    except ValueError:
        return 0.0


def _context(owner: str, tools: list) -> Ctx:
    m = STAMP_RE.search(owner)
    props = {t["name"]: t for t in tools}["read_file"]["schema"].get("properties", {})
    return Ctx(
        owner=owner,
        body=owner[owner.index("] ", m.start()) + 2 :],
        day=m.group(4) or m.group(1),
        has_sum="column" in props,  # the file aids are declared (the selection; sum since ruling 95 is gone)
    )


def _table(c: Ctx, system: str, results: list) -> list:
    """The table's path: from the system text, the owner's message, else any result so far."""
    want = _wanted(c.body)
    found = _paths(system, want) or _paths(c.owner, want)
    for r in results:
        found = found or _paths(r["text"], want)
    return found


class Scripted:
    wire = "scripted"
    sloppy = False

    def __init__(self):
        self.calls = 0

    # the providers' interface on neutral items
    def render(self, items):
        return [dict(it) for it in items]

    def user(self, text):
        return [{"t": "user", "text": text}]

    def results(self, res):
        return [{"t": "result", "id": i, "text": t, "err": e} for i, t, e in res]

    def _call(self, name, args, text=""):
        self.calls += 1
        cid = f"s{self.calls}"
        return providers.Step(
            [{"t": "call", "id": cid, "name": name, "args": args}],
            [(cid, name, args)],
            text,
            {"prompt": 0, "cached": 0, "written": 0, "output": 0},
            "tool_use",
        )

    def _say(self, text):
        return providers.Step(
            [{"t": "atext", "text": text}], [], text, {"prompt": 0, "cached": 0, "written": 0, "output": 0}, "end_turn"
        )

    def step(self, system, tools, messages, cache_key=None):
        k = max(i for i, m in enumerate(messages) if m.get("t") == "user")
        c = _context(messages[k]["text"], tools)
        done = messages[k + 1 :]
        calls = [x for x in done if x.get("t") == "call"]
        results = [x for x in done if x.get("t") == "result"]
        found = _table(c, system, results)
        if not found:
            return self._find(system, calls)
        c.path = found[0]
        c.dom = {"food": "food", "money": "expenses", "gym": "training"}[c.path.split("/")[0]]
        # only the calls on the table count from here (a listing or a skill read came first)
        by_id = {r["id"]: r for r in results}
        c.calls = [x for x in calls if x["args"].get("path") == c.path]
        c.results = [by_id[x["id"]] for x in c.calls if x["id"] in by_id]
        return self._total(c) if "?" in c.body else self._write(c)

    def _find(self, system, calls):
        """No table named yet: read the indexed skill, else list the files."""
        sk = re.search(r"(skills/[a-z-]+\.md):", system)
        if sk and not any(x["args"].get("path") == sk.group(1) for x in calls):
            return self._call("read_file", {"path": sk.group(1)})
        if not any(x["name"] == "list_files" for x in calls):
            return self._call("list_files", {})
        return self._say("I could not find your log.")

    # ------------------------------------------------------------ totals
    def _range(self, body, day):
        if "yesterday" in body:
            return _yesterday(day), _yesterday(day)
        t = datetime.strptime(day, "%Y-%m-%d")
        if "7 days" in body:
            return (t - timedelta(days=6)).strftime("%Y-%m-%d"), day
        if "month" in body:
            return t.strftime("%Y-%m-01"), day
        return day, day

    def _total(self, c: Ctx):
        col = records.DOMAINS[c.dom]["sum"]
        lo, hi = self._range(c.body, c.day)
        use_sum = c.has_sum and not self.sloppy
        if not c.calls:
            if use_sum:  # the selection (ruling 97: a plain read of the rows)
                return self._call("read_file", {"path": c.path, "column": "date", "from": lo, "to": hi})
            return self._call("read_file", {"path": c.path})
        text = c.results[-1]["text"]
        if use_sum and f"sum {col}=" in text:
            v = re.search(rf"sum {col}=(-?[\d.]+)", text).group(1)
            left = re.search(r"not in it: (.*?\))\)", text)
            return self._say(f"{v} {col}." + (f" Not counted: {left.group(1)}." if left else ""))
        if use_sum:  # no figure (ruling 92): the selection's rows, "L<n> " in front of each
            text = "\n".join([text.split("\n", 1)[0]] + [re.sub(r"^L\d+ ", "", x) for x in text.split("\n")[1:]])
        header, rows = _csv_rows(text)
        total, skipped = _sum_rows(rows, header, col, lo, hi, self.sloppy)
        extra = f" Lines {', '.join(map(str, skipped))} were left out (extra comma)." if skipped else ""
        return self._say(f"{round(total, 3):g} {col}.{extra}")

    # ------------------------------------------------------------ writes
    def _parse_add(self, c: Ctx):
        hhmm = STAMP_RE.search(c.owner)
        t = f"{hhmm.group(2)}:{hhmm.group(3)}"
        if c.dom == "food":
            qty, unit, item, kcal, prot = re.match(
                r"Log (\S+) (\S+) (.+): (\S+) kcal, (\S+) g protein\.", c.body
            ).groups()
            return f"{c.day},{t},{item},{qty},{unit},{kcal},{prot},"
        if c.dom == "expenses":
            amt, item, cat = re.match(r"Spent (\S+) on (.+) \((.+)\)\.", c.body).groups()
            return f"{c.day},{t},{item},{cat},{amt},"
        ex, sets, reps, kg = re.match(r"Log (.+): (\S+) sets of (\S+) at (\S+) kg\.", c.body).groups()
        return f"{c.day},{t},{ex},{sets},{reps},{kg},"

    def _after_fault(self, c: Ctx):
        if self.sloppy:
            return self._say("Done, logged it.")
        if sum(1 for r in c.results if r["err"]) >= 2:
            return self._say("That failed twice: not enough memory on the device. Nothing was changed.")
        return self._call(c.calls[-1]["name"], c.calls[-1]["args"])

    def _write(self, c: Ctx):
        last = c.results[-1] if c.results else None
        if last and last["err"] and "not enough memory" in last["text"]:
            return self._after_fault(c)
        adding = c.body.startswith(("Log", "Spent"))
        writes = [x for x in c.calls if x["name"] in ("append_file", "edit_file", "write_file")]
        if writes and not last["err"]:
            return self._after_write(c, writes, adding)
        if adding:
            return self._call("append_file", {"path": c.path, "text": self._parse_add(c) + "\n"})
        return self._fix(c)

    def _fix(self, c: Ctx):
        """A correction, a move or an identical-row removal: read the table, then edit one line."""
        reads = [r for r in c.results if not r["err"] and r["text"].startswith(c.path)]
        if not reads:
            return self._call("read_file", {"path": c.path})
        header, rows = _csv_rows(reads[-1]["text"])
        lines = [(n, raw) for n, raw in rows if raw.strip() and not raw.lstrip().startswith("#")]
        if "twice" in c.body:
            return self._identical(c, lines)
        if "Move it" in c.body:
            return self._move(c, lines)
        return self._correct(c, header, lines)

    def _after_write(self, c: Ctx, writes, adding):
        if self.sloppy and adding and len(writes) == 1:
            return self._call(writes[0]["name"], writes[0]["args"])  # the double append
        return self._say("Done.")

    def _move(self, c: Ctx, lines):
        m = re.search(r"The (.+) at (\d\d:\d\d) today", c.body)
        hit = [raw for _, raw in lines if raw.startswith(c.day) and f",{m.group(2)},{m.group(1)}," in raw][0]
        return self._call("edit_file", {"path": c.path, "old": hit, "new": _yesterday(c.day) + hit[10:]})

    def _identical(self, c: Ctx, lines):
        m = re.search(r"logged the (.+) at (\d\d:\d\d) twice", c.body)
        n, raw = [(n, raw) for n, raw in lines if raw.startswith(c.day) and f",{m.group(2)},{m.group(1)}," in raw][-1]
        if self.sloppy:
            return self._call("edit_file", {"path": c.path, "old": raw + "\n", "new": "", "all": True})
        if c.has_sum:
            return self._call("edit_file", {"path": c.path, "old": raw, "new": "", "line": n})
        return self._call("edit_file", {"path": c.path, "old": raw + "\n" + raw + "\n", "new": raw + "\n"})

    def _correct(self, c: Ctx, header, lines):
        """The item's row of the running day, its value column replaced."""
        i = header.index({"food": "kcal", "expenses": "amount", "training": "reps"}[c.dom])
        item = re.search(r"[Tt]he (.+?) (?:I just logged|at \d\d:\d\d today)", c.body).group(1)
        new_v = re.search(r"was (?:actually )?(-?[\d.]+)", c.body).group(1)
        hit = [raw for _, raw in lines if raw.startswith(c.day) and f",{item}," in raw][-1]
        f = hit.split(",")
        f[i] = new_v
        args = {"path": c.path, "old": hit, "new": ",".join(f)}
        if self.sloppy:
            args["all"] = True
        return self._call("edit_file", args)


class Good(Scripted):
    sloppy = False


class Sloppy(Scripted):
    sloppy = True


HEADS = {"good": Good, "sloppy": Sloppy}
