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
        return providers.Step([{"t": "call", "id": cid, "name": name, "args": args}], [(cid, name, args)], text,
                              {"prompt": 0, "cached": 0, "written": 0, "output": 0}, "tool_use")

    def _say(self, text):
        return providers.Step([{"t": "atext", "text": text}], [], text, {"prompt": 0, "cached": 0, "written": 0,
                                                                         "output": 0}, "end_turn")

    def step(self, system, tools, messages, cache_key=None):
        k = max(i for i, m in enumerate(messages) if m.get("t") == "user")
        owner = messages[k]["text"]
        done = [m for m in messages[k + 1:] if m.get("t") in ("call", "result")]
        calls = [m for m in done if m["t"] == "call"]
        results = [m for m in done if m["t"] == "result"]
        names = {t["name"]: t for t in tools}
        has_sum = "sum" in names["read_file"]["schema"].get("properties", {})
        m = STAMP_RE.search(owner)
        day = m.group(4) or m.group(1)
        body = owner[owner.index("] ", m.start()) + 2:]
        want = "food" if "kcal" in body else "money" if re.search(r"[Ss]pen[td]|actually", body) else (
            "gym" if re.search(r"sets|reps", body) else "")
        path = None
        for src in (system, owner):
            found = [p for p in PATH_RE.findall(src) if p.startswith(want)]
            if found:
                path = found[0]
                break
        if path is None:
            # read the indexed skill first, else list files
            sk = re.search(r"(skills/[a-z-]+\.md):", system)
            if sk and not any(c["args"].get("path") == sk.group(1) for c in calls):
                return self._call("read_file", {"path": sk.group(1)})
            for r in results:
                found = [p for p in PATH_RE.findall(r["text"]) if p.startswith(want)]
                if found:
                    path = found[0]
            if path is None:
                if not any(c["name"] == "list_files" for c in calls):
                    return self._call("list_files", {})
                return self._say("I could not find your log.")
        dom = {"food": "food", "money": "expenses", "gym": "training"}[path.split("/")[0]]
        # only the calls on the table count from here (a listing or a skill read came first)
        by_id = {r["id"]: r for r in results}
        mine = [c for c in calls if c["args"].get("path") == path]
        calls, results = mine, [by_id[c["id"]] for c in mine if c["id"] in by_id]
        if "?" in body:
            return self._total(dom, path, day, body, calls, results, has_sum)
        return self._write(dom, path, day, body, owner, calls, results, has_sum)

    # ------------------------------------------------------------ totals
    def _range(self, body, day):
        t = datetime.strptime(day, "%Y-%m-%d")
        if "yesterday" in body:
            y = (t - timedelta(days=1)).strftime("%Y-%m-%d")
            return y, y
        if "7 days" in body:
            return (t - timedelta(days=6)).strftime("%Y-%m-%d"), day
        if "month" in body:
            return t.strftime("%Y-%m-01"), day
        return day, day

    def _total(self, dom, path, day, body, calls, results, has_sum):
        col = records.DOMAINS[dom]["sum"]
        lo, hi = self._range(body, day)
        if not calls:
            if has_sum and not self.sloppy:
                return self._call("read_file", {"path": path, "column": "date", "from": lo, "to": hi, "sum": [col]})
            return self._call("read_file", {"path": path})
        text = results[-1]["text"]
        if has_sum and not self.sloppy:
            v = re.search(rf"sum {col}=(-?[\d.]+)", text).group(1)
            sk = re.search(r"skipped \d+ \(([^)]*)\)", text)
            extra = f" Not counted: {sk.group(1)}." if sk else ""
            return self._say(f"{v} {col}.{extra}")
        header, rows = _csv_rows(text)
        i = header.index(col)
        total, skipped = 0.0, []
        for n, raw in rows:
            if self.sloppy:  # every line that mentions the range, comments included
                f = raw.lstrip("# replaced:").split(",")
                if f and lo <= f[0].strip()[:10] <= hi and len(f) > i:
                    try:
                        total += float(f[i])
                    except ValueError:
                        pass
                continue
            if not raw.strip() or raw.lstrip().startswith("#"):
                continue
            f = raw.split(",")
            if not (lo <= f[0] <= hi):
                continue
            if len(f) != len(header):
                skipped.append(n)
                continue
            if f[i]:
                total += float(f[i])
        extra = f" Lines {', '.join(map(str, skipped))} were left out (extra comma)." if skipped else ""
        return self._say(f"{round(total, 3):g} {col}.{extra}")

    # ------------------------------------------------------------ writes
    def _parse_add(self, dom, body, day, owner):
        hhmm = STAMP_RE.search(owner)
        t = f"{hhmm.group(2)}:{hhmm.group(3)}"
        if dom == "food":
            m = re.match(r"Log (\S+) (\S+) (.+): (\S+) kcal, (\S+) g protein\.", body)
            qty, unit, item, kcal, prot = m.groups()
            return f"{day},{t},{item},{qty},{unit},{kcal},{prot},"
        if dom == "expenses":
            m = re.match(r"Spent (\S+) on (.+) \((.+)\)\.", body)
            amt, item, cat = m.groups()
            return f"{day},{t},{item},{cat},{amt},"
        m = re.match(r"Log (.+): (\S+) sets of (\S+) at (\S+) kg\.", body)
        ex, sets, reps, kg = m.groups()
        return f"{day},{t},{ex},{sets},{reps},{kg},"

    def _write(self, dom, path, day, body, owner, calls, results, has_sum):
        if results and results[-1]["err"] and "not enough memory" in results[-1]["text"]:
            if self.sloppy:
                return self._say("Done, logged it.")
            if sum(1 for r in results if r["err"]) >= 2:
                return self._say("That failed twice: not enough memory on the device. Nothing was changed.")
            c = calls[-1]
            return self._call(c["name"], c["args"])
        writes = [c for c in calls if c["name"] in ("append_file", "edit_file", "write_file")]
        if writes and not results[-1]["err"]:
            if self.sloppy and body.startswith(("Log", "Spent")) and len(writes) == 1:
                return self._call(writes[0]["name"], writes[0]["args"])  # the double append
            return self._say("Done.")
        if body.startswith(("Log", "Spent")):
            return self._call("append_file", {"path": path, "text": self._parse_add(dom, body, day, owner) + "\n"})
        reads = [r for r in results if not r["err"] and r["text"].startswith(path)]
        if not reads:
            return self._call("read_file", {"path": path})
        header, rows = _csv_rows(reads[-1]["text"])
        lines = [(n, raw) for n, raw in rows if raw.strip() and not raw.lstrip().startswith("#")]
        if "twice" in body:
            m = re.search(r"logged the (.+) at (\d\d:\d\d) twice", body)
            hits = [(n, raw) for n, raw in lines if raw.startswith(day) and f",{m.group(2)},{m.group(1)}," in raw]
            raw = hits[-1][1]
            if self.sloppy:
                return self._call("edit_file", {"path": path, "old": raw + "\n", "new": "", "all": True})
            if has_sum:
                return self._call("edit_file", {"path": path, "old": raw, "new": "", "line": hits[-1][0]})
            return self._call("edit_file", {"path": path, "old": raw + "\n" + raw + "\n", "new": raw + "\n"})
        if "Move it" in body:
            m = re.search(r"The (.+) at (\d\d:\d\d) today", body)
            hit = [(n, raw) for n, raw in lines if raw.startswith(day) and f",{m.group(2)},{m.group(1)}," in raw][0]
            y = (datetime.strptime(day, "%Y-%m-%d") - timedelta(days=1)).strftime("%Y-%m-%d")
            return self._call("edit_file", {"path": path, "old": hit[1], "new": y + hit[1][10:]})
        # a correction: the item's row of the running day, the value column replaced
        col = {"food": "kcal", "expenses": "amount", "training": "reps"}[dom]
        i = header.index(col)
        item = re.search(r"[Tt]he (.+?) (?:I just logged|at \d\d:\d\d today)", body).group(1)
        new_v = re.search(r"was (?:actually )?(-?[\d.]+)", body).group(1)
        hit = [(n, raw) for n, raw in lines if raw.startswith(day) and f",{item}," in raw][-1]
        f = hit[1].split(",")
        f[i] = new_v
        args = {"path": path, "old": hit[1], "new": ",".join(f)}
        if self.sloppy:
            args["all"] = True
        return self._call("edit_file", args)


class Good(Scripted):
    sloppy = False


class Sloppy(Scripted):
    sloppy = True


HEADS = {"good": Good, "sloppy": Sloppy}
