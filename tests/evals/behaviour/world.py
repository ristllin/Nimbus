#!/usr/bin/env python3
"""world - an in-process device that answers the engine-2 tool shapes of PRD section 4.

It holds a food ledger with device-assigned row ids, notes (pinned and unpinned), a two-segment
file store, reminders, sub-agent jobs and a searchable history. Results state facts and never give
orders; an unknown argument is an error naming the accepted ones; every write returns what changed.

Fault injection: World(fail={family: (mode, message)}) makes the first call (mode 'once') or every
call (mode 'always') of a tool family return that error without changing anything.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from datetime import datetime, timedelta

import manifest

PATH_RE = re.compile(r"^[!-~]{1,24}/[!-~]{1,48}$")
NOTE_KEY_RE = re.compile(r"^[a-z0-9-]{1,32}$")
ROW_KEYS = set(manifest.ROW_FIELDS)
NUM_KEYS = {"qty", "kcal", "protein_g", "carb_g", "fat_g", "fibre_g", "polyol_g", "alcohol_g"}
FAMILY = {
    "ledger_write": "ledger",
    "ledger_add": "ledger",
    "ledger_update": "ledger",
    "ledger_remove": "ledger",
    "note": "note",
    "write_file": "file",
    "append_file": "file",
    "edit_file": "file",
    "schedule": "schedule",
    "agent_start": "agent_start",
    "delete_file": "delete_file",
    "fetch_url": "fetch_url",
}
WRITE_TOOLS = set(FAMILY)
B32 = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"


class ToolError(Exception):
    pass


def _accept(args: dict, allowed, required=(), where="arguments"):
    if not isinstance(args, dict):
        raise ToolError(f"{where} must be an object; accepted: {', '.join(sorted(allowed))}")
    unknown = sorted(set(args) - set(allowed))
    if unknown:
        raise ToolError(f"unknown {where} {', '.join(unknown)}; accepted: {', '.join(sorted(allowed))}")
    missing = [k for k in required if args.get(k) in (None, "", [])]
    if missing:
        raise ToolError(f"missing {', '.join(missing)}; accepted: {', '.join(sorted(allowed))}")


def _num(v):
    return f"{v:,.0f}" if float(v).is_integer() else f"{v:,.1f}"


@dataclass
class Row:
    id: str
    day: str
    time: str
    meal: str
    item: str
    qty: float | None
    unit: str
    kcal: float
    protein_g: float | None = None
    carb_g: float | None = None
    fat_g: float | None = None
    fibre_g: float | None = None
    polyol_g: float | None = None
    alcohol_g: float | None = None
    basis: str = "estimate"
    note: str = ""

    def line(self) -> str:
        qty = f" {_num(self.qty)} {self.unit}".rstrip() if self.qty is not None else ""
        macros = " ".join(
            f"{k} {_num(v)}"
            for k, v in (
                ("P", self.protein_g),
                ("C", self.carb_g),
                ("F", self.fat_g),
                ("fibre", self.fibre_g),
                ("polyol", self.polyol_g),
                ("alcohol", self.alcohol_g),
            )
            if v is not None
        )
        meal = f" {self.meal}" if self.meal else ""
        return (
            f"{self.id} {self.time}{meal} | {self.item}{qty} | {_num(self.kcal)} kcal {macros} ({self.basis})".rstrip()
        )


@dataclass
class World:
    now: datetime
    fail: dict = field(default_factory=dict)
    macro_warn: str = "prd"  # prd: the PRD rule; force: any basis, no alcohol credit (warning arm); off
    rows: list = field(default_factory=list)
    pinned: dict = field(default_factory=dict)
    unpinned: dict = field(default_factory=dict)
    files: dict = field(default_factory=dict)
    reminders: list = field(default_factory=list)
    jobs: list = field(default_factory=list)
    history: list = field(default_factory=list)  # (datetime, who, text)
    memories: list = field(default_factory=list)
    fetched: list = field(default_factory=list)
    deleted: list = field(default_factory=list)
    pages: dict = field(default_factory=dict)  # url -> text
    log: list = field(default_factory=list)  # every call: (name, args, text, is_error)
    target: tuple = (2300, 160)
    _seq: int = 0
    _failed_once: set = field(default_factory=set)
    _added_at: dict = field(default_factory=dict)

    # ------------------------------------------------------------------ helpers
    def ledger_day_of(self, t: datetime) -> str:
        return (t - timedelta(hours=4)).strftime("%Y-%m-%d")

    @property
    def today(self) -> str:
        return self.ledger_day_of(self.now)

    def _day(self, d):
        if d in (None, "", "today"):
            return self.today
        if d == "yesterday":
            return self.ledger_day_of(self.now - timedelta(days=1))
        if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", str(d)):
            raise ToolError(f"day '{d}' is not today, yesterday or YYYY-MM-DD")
        return d

    def _new_id(self) -> str:
        self._seq += 1
        n, s = self._seq * 7919 + 1013, ""
        for _ in range(5):
            s = B32[n % 32] + s
            n //= 32
        return "L" + s

    def row(self, rid):
        return next((r for r in self.rows if r.id == rid), None)

    def day_line(self, day: str) -> str:
        rs = [r for r in self.rows if r.day == day]
        kcal = sum(r.kcal for r in rs)
        prot = sum(r.protein_g or 0 for r in rs)
        tk, tp = self.target
        return (
            f"day {day} (ledger day, 04:00 to 04:00): {len(rs)} rows, {_num(kcal)} kcal, P {prot:.1f} g; "
            f"target {tk:,} / {tp} -> left {_num(tk - kcal)} kcal, {tp - prot:.1f} g"
        )

    # ------------------------------------------------------------------ dispatch
    def call(self, name: str, args) -> tuple:
        """(text, is_error) for one tool call, logged."""
        try:
            text = self._dispatch(name, args if args is not None else {})
            out = (text, False)
        except ToolError as e:
            out = (f"error: {e}", True)
        self.log.append((name, args, out[0], out[1]))
        return out

    def _dispatch(self, name, args):
        fam = FAMILY.get(name)
        if fam in self.fail:
            mode, msg = self.fail[fam]
            if mode == "always" or fam not in self._failed_once:
                self._failed_once.add(fam)
                raise ToolError(msg)
        fn = getattr(self, "t_" + name, None)
        if fn is None:
            raise ToolError(f"no tool named {name}")
        return fn(args)

    # ------------------------------------------------------------------ ledger
    def _check_row(self, row: dict, need_core=True):
        _accept(row, ROW_KEYS, ("item", "kcal", "basis") if need_core else (), where="row fields")
        for k in NUM_KEYS & set(row):
            if not isinstance(row[k], (int, float)) or isinstance(row[k], bool):
                raise ToolError(f"{k} must be a number")
        if "basis" in row and row["basis"] not in ("label", "stated", "estimate"):
            raise ToolError("basis must be label, stated or estimate")
        unit, qty, kcal = str(row.get("unit", "")).lower(), row.get("qty"), row.get("kcal")
        if kcal is not None and unit == "g" and qty and kcal / qty * 100 > 900:
            raise ToolError(f"{_num(kcal)} kcal for {_num(qty)} g is more than 900 kcal per 100 g")

    def _duplicate_warnings(self, r: Row) -> list:
        out = []
        for o in self.rows:
            same = o is not r and o.day == r.day and o.item.lower() == r.item.lower()
            same = same and (o.qty, o.unit, o.kcal) == (r.qty, r.unit, r.kcal)
            recent = self.now - self._added_at.get(o.id, self.now - timedelta(hours=1)) <= timedelta(minutes=30)
            if same and recent:
                out.append(f"warning: {r.id} matches {o.id} (same item, quantity and kcal) added within 30 minutes")
        return out

    def _macro_warning(self, r: Row) -> list:
        force = self.macro_warn == "force"
        if any(v is None for v in (r.protein_g, r.carb_g, r.fat_g)) or self.macro_warn == "off":
            return []
        if r.basis == "label" and not force:
            return []
        alcohol = 0 if force else 7 * (r.alcohol_g or 0)
        est = 4 * r.protein_g + 4 * r.carb_g + 9 * r.fat_g + 2 * (r.fibre_g or 0) + 2.4 * (r.polyol_g or 0) + alcohol
        if not est or abs(r.kcal - est) / est <= 0.15:
            return []
        terms = "4P + 4C + 9F + 2 fibre + 2.4 polyol" + ("" if force else " + 7 alcohol")
        return [f"warning: {r.id} kcal {_num(r.kcal)} is {abs(r.kcal - est) / est:.0%} from {terms} = {_num(est)}"]

    def _warnings(self, r: Row) -> list:
        out = self._duplicate_warnings(r)
        if r.kcal >= 2500:
            out.append(f"warning: {r.id} {_num(r.kcal)} kcal is a kJ-sized value")
        return out + self._macro_warning(r)

    def _add(self, row: dict) -> Row:
        self._check_row(row)
        r = Row(
            id=self._new_id(),
            day=self._day(row.get("day")),
            time=row.get("time") or self.now.strftime("%H:%M"),
            meal=row.get("meal", ""),
            item=row["item"],
            qty=row.get("qty"),
            unit=row.get("unit", ""),
            kcal=float(row["kcal"]),
            **{k: float(row[k]) for k in NUM_KEYS - {"qty", "kcal"} if row.get(k) is not None},
            basis=row["basis"],
            note=row.get("note", ""),
        )
        return r

    def _apply_ops(self, ops: list) -> str:
        if not isinstance(ops, list) or not 1 <= len(ops) <= 8:
            raise ToolError("ops must be a list of 1 to 8 operations")
        staged, updates, removed = [], [], []
        for n, op in enumerate(ops, 1):
            if not isinstance(op, dict):
                raise ToolError(f"op {n} must be an object")
            handler = {"add": self._op_add, "update": self._op_update, "remove": self._op_remove, "move": self._op_move}
            fn = handler.get(op.get("op"))
            if fn is None:
                raise ToolError(f"op {n}: op must be add, update, remove or move")
            fn(n, op, staged, updates, removed)
        return self._commit(staged, updates, removed)

    def _existing(self, n, rid):
        r = self.row(rid)
        if not r:
            raise ToolError(f"op {n}: no row {rid}; ledger_day shows the ids")
        return r

    # Arm L1u: the same one ledger_write, every op reading its row ids from id or ids, and an add reading
    # day, time and meal beside row. The strict arms take exactly the PRD's per-op fields.
    @property
    def uniform_ops(self) -> bool:
        return self.variant == "L1u"

    def _op_ids(self, n, op, extra) -> list:
        if not self.uniform_ops:
            key = "ids" if op.get("op") == "move" else "id"
            _accept(op, {"op", key, *extra}, (key, *extra), where=f"op {n} fields")
            return op[key] if key == "ids" else [op["id"]]
        _accept(op, {"op", "id", "ids", *extra}, extra, where=f"op {n} fields")
        ids = op.get("ids") or ([op["id"]] if op.get("id") else [])
        if not ids:
            raise ToolError(f"op {n}: missing id or ids")
        return ids if isinstance(ids, list) else [ids]

    def _op_add(self, n, op, staged, updates, removed):
        side = {"day", "time", "meal"} if self.uniform_ops else set()
        _accept(op, {"op", "row", *side}, ("row",), where=f"op {n} fields")
        row = op["row"]
        if side and isinstance(row, dict):
            row = {**{k: op[k] for k in side if k in op}, **row}
        staged.append(self._add(row))

    def _op_update(self, n, op, staged, updates, removed):
        ids = self._op_ids(n, op, ("set",))
        self._check_row(op["set"], need_core=False)
        for rid in ids:
            updates.append((self._existing(n, rid), op["set"]))

    def _op_remove(self, n, op, staged, updates, removed):
        for rid in self._op_ids(n, op, ()):
            self._existing(n, rid)
            if rid not in removed:
                removed.append(rid)

    def _op_move(self, n, op, staged, updates, removed):
        ids = self._op_ids(n, op, ("day",))
        day = self._day(op["day"])
        for rid in ids:
            updates.append((self._existing(n, rid), {"day": day}))

    def _commit(self, staged, updates, removed):
        lines = ["added " + r.line() for r in staged]
        days, warns = {r.day for r in staged}, []
        for r, st in updates:
            days.add(r.day)
            for k, v in st.items():
                if k == "day":
                    v = self._day(v)
                setattr(r, k, float(v) if k in NUM_KEYS else v)
            days.add(r.day)
            lines.append("updated " + r.line() + (f" (day {r.day})" if "day" in st else ""))
            warns += self._warnings(r)
        for rid in removed:
            r = self.row(rid)
            days.add(r.day)
            self.rows.remove(r)
            lines.append("removed " + r.line())
        for r in staged:
            self.rows.append(r)
            self._added_at[r.id] = self.now
            warns += self._warnings(r)
        return "\n".join([*lines, *warns, *(self.day_line(d) for d in sorted(days))])

    def t_ledger_write(self, a):
        _accept(a, {"ops"}, ("ops",))
        return self._apply_ops(a["ops"])

    def t_ledger_add(self, a):
        _accept(a, {"rows"}, ("rows",))
        rows = a["rows"]
        if not isinstance(rows, list) or not 1 <= len(rows) <= 8:
            raise ToolError("rows must be a list of 1 to 8 rows")
        return self._apply_ops([{"op": "add", "row": r} for r in rows])

    def t_ledger_update(self, a):
        _accept(a, {"id", "set"}, ("id", "set"))
        return self._apply_ops([{"op": "update", "id": a["id"], "set": a["set"]}])

    def t_ledger_remove(self, a):
        _accept(a, {"ids"}, ("ids",))
        ids = a["ids"] if isinstance(a["ids"], list) else [a["ids"]]
        return self._apply_ops([{"op": "remove", "id": i} for i in ids])

    def t_ledger_day(self, a):
        _accept(a, {"day", "days"})
        day = self._day(a.get("day"))
        n = int(a.get("days") or 1)
        if not 1 <= n <= 7:
            raise ToolError("days must be 1 to 7")
        out = []
        d0 = datetime.strptime(day, "%Y-%m-%d")
        for i in range(n):
            d = (d0 - timedelta(days=i)).strftime("%Y-%m-%d")
            rs = sorted((r for r in self.rows if r.day == d), key=lambda r: r.time)
            out += [r.line() for r in rs] or [f"(no rows on {d})"]
            out.append(self.day_line(d))
        return "\n".join(out)

    def t_ledger_target(self, a):
        _accept(a, {"kcal", "protein_g"})
        if a:
            self.target = (a.get("kcal", self.target[0]), a.get("protein_g", self.target[1]))
        return f"target {self.target[0]:,} kcal, {self.target[1]} g protein a day"

    # ------------------------------------------------------------------ notes
    def t_note(self, a):
        _accept(a, {"op", "key", "value", "kind", "pin"}, ("op", "key"))
        key = a["key"]
        if not NOTE_KEY_RE.fullmatch(key):
            raise ToolError("key must be a slug a-z0-9- of up to 32 characters")
        if a["op"] == "remove":
            if key in self.pinned or key in self.unpinned:
                self.pinned.pop(key, None)
                self.unpinned.pop(key, None)
                return f"removed note {key}"
            raise ToolError(f"no note {key}")
        if a["op"] != "set":
            raise ToolError("op must be set or remove")
        value = a.get("value")
        if not value:
            raise ToolError("set needs a value")
        if len(value.encode()) > 300:
            raise ToolError("value is over 300 B")
        pin = bool(a.get("pin", False))
        (self.pinned if pin else self.unpinned)[key] = value
        (self.unpinned if pin else self.pinned).pop(key, None)
        used = sum(len(k) + len(v) + 3 for k, v in self.pinned.items()) + 1400
        where = "pinned" if pin else "unpinned (found by search)"
        return f"set {a.get('kind', 'fact')} {where}: {key}: {value}\nused {used:,} of 4,096 B pinned"

    # ------------------------------------------------------------------ search
    @staticmethod
    def _terms(text: str) -> set:
        stop = {"the", "a", "an", "my", "is", "was", "what", "when", "did", "i", "to", "of", "and", "do", "you"}
        out = set()
        for w in re.findall(r"[a-z0-9]+", text.lower()):
            if w in stop or len(w) < 2:
                continue
            out.add(w[:-1] if w.endswith("s") and len(w) > 3 else w)
        return out

    def t_search(self, a):
        _accept(a, {"query", "scope", "from", "to"}, ("query", "scope"))
        q = self._terms(a["query"])
        scope = a["scope"]
        if scope == "notes":
            docs = [(f"note {k}", "", f"{k}: {v}") for k, v in self.unpinned.items()]
        elif scope == "history":
            docs = [(who, t.strftime("%a %d %b %Y %H:%M"), text) for t, who, text in self.history]
        elif scope == "files":
            docs = [(f"file {p}", "", f"{p}: {c}") for p, c in self.files.items()]
        elif scope == "memories":
            docs = [("memory", "", m) for m in self.memories]
        else:
            raise ToolError("scope must be history, files, notes or memories")
        scored = []
        for src, when, text in docs:
            s = len(q & self._terms(text))
            if s:
                scored.append((s, src, when, text))
        scored.sort(key=lambda x: -x[0])
        if not scored:
            return f"no results for '{a['query']}' in {scope}"
        lines = [f"{len(scored[:5])} results in {scope}:"]
        for n, (_, src, when, text) in enumerate(scored[:5], 1):
            lines.append(f"{n}. [{src}{' ' + when if when else ''}] {text[:400]}")
        return "\n".join(lines)

    def t_web_search(self, a):
        _accept(a, {"query"}, ("query",))
        slug = re.sub(r"\W+", "-", a["query"])[:40]
        return f"[outside content] web results for '{a['query']}': 1. example.org/{slug}"

    # ------------------------------------------------------------------ files
    def _path(self, p):
        if not isinstance(p, str) or not PATH_RE.fullmatch(p):
            raise ToolError(f"path '{p}' is not project/name (printable ASCII, no spaces)")
        return p

    def t_read_file(self, a):
        _accept(a, {"path", "offset", "limit"}, ("path",))
        p = self._path(a["path"])
        if p not in self.files:
            raise ToolError(f"no file {p}; list_files shows what exists")
        c = self.files[p]
        return f"{p} size={len(c.encode())} lines={c.count(chr(10)) + 1} sha8=0000abcd bytes 0-{len(c.encode())}\n{c}"

    def t_write_file(self, a):
        _accept(a, {"path", "content", "if_exists"}, ("path", "content"))
        p = self._path(a["path"])
        old = self.files.get(p)
        if old is not None and a.get("if_exists", "fail") != "replace":
            raise ToolError(f"{p} exists ({len(old.encode())} B); if_exists=replace replaces it")
        self.files[p] = a["content"]
        rep = f"; replaced {len(old.encode())} B" if old is not None else ""
        return f"wrote {p} size={len(a['content'].encode())} sha8=1a2b3c4d{rep}"

    def t_append_file(self, a):
        _accept(a, {"path", "text"}, ("path", "text"))
        p = self._path(a["path"])
        created = p not in self.files
        self.files[p] = self.files.get(p, "") + a["text"]
        c = self.files[p]
        return (
            f"appended {len(a['text'].encode())} B{' (new file)' if created else ''}; file is "
            f"{len(c.encode())} B, {c.count(chr(10)) + 1} lines"
        )

    def t_edit_file(self, a):
        _accept(a, {"path", "old", "new", "all"}, ("path", "old"))
        p = self._path(a["path"])
        if p not in self.files:
            raise ToolError(f"no file {p}")
        c, k = self.files[p], self.files[p].count(a["old"])
        if k == 0:
            raise ToolError("no match for old")
        if k > 1 and not a.get("all"):
            raise ToolError(f"{k} matches for old; all=true replaces every one")
        self.files[p] = c.replace(a["old"], a.get("new", ""))
        return f"replaced {k if a.get('all') else 1}; size {len(self.files[p].encode())} B"

    def t_list_files(self, a):
        _accept(a, {"prefix", "after"})
        pre = a.get("prefix") or ""
        names = [p for p in reversed(list(self.files)) if p.startswith(pre)]
        lines = [f"{p} {len(self.files[p].encode())} B" for p in names[:40]]
        return "\n".join(lines + [f"more: {max(len(names) - 40, 0)}"])

    def t_delete_file(self, a):
        _accept(a, {"path"}, ("path",))
        p = self._path(a["path"])
        if p not in self.files:
            raise ToolError(f"no file {p}; list_files shows what exists")
        n = len(self.files.pop(p).encode())
        self.deleted.append(p)
        return f"deleted {p} ({n} B)"

    def t_move_file(self, a):
        _accept(a, {"from", "to"}, ("from", "to"))
        src, dst = self._path(a["from"]), self._path(a["to"])
        if src not in self.files:
            raise ToolError(f"no file {src}")
        self.files[dst] = self.files.pop(src)
        return f"moved {src} to {dst}"

    def t_file_stat(self, a):
        _accept(a, {"path"}, ("path",))
        p = self._path(a["path"])
        if p not in self.files:
            raise ToolError(f"no file {p}")
        return f"{p} {len(self.files[p].encode())} B"

    def t_fetch_url(self, a):
        _accept(a, {"url", "save_to"}, ("url",))
        url = a["url"]
        if not re.match(r"^https?://", url):
            raise ToolError("url must start with http:// or https://")
        text = self.pages.get(url, "Example page. Nothing else here.")
        self.fetched.append(url)
        saved = ""
        if a.get("save_to"):
            p = self._path(a["save_to"])
            self.files[p] = text
            saved = f"\nsaved to {p} ({len(text.encode())} B)"
        return f"[outside content from {url}, {len(text.encode())} B]\n{text}{saved}"

    # ------------------------------------------------------------------ media, schedule, view
    def t_view(self, a):
        _accept(a, {"id"}, ("id",))
        raise ToolError(f"no stored result {a['id']}")

    def t_send(self, a):
        _accept(a, {"kind", "path", "ref", "text", "caption"}, ("kind",))
        if a["kind"] == "file" and a.get("path") not in self.files:
            raise ToolError(f"no file {a.get('path')}")
        self._seq += 1
        return f"sent message_id={4100 + self._seq} bytes=2048 sha:9f8e7d6c"

    def t_schedule(self, a):
        _accept(a, {"op", "kind", "id", "when", "text", "every"}, ("op",))
        fn = {
            "list": self._sch_list,
            "create": self._sch_create,
            "update": self._sch_change,
            "delete": self._sch_change,
        }
        if a["op"] not in fn:
            raise ToolError("op must be create, update, delete or list")
        return fn[a["op"]](a)

    def _sch_list(self, a):
        return "\n".join(f"{r['id']} {r['kind']} {r['when']}: {r['text']}" for r in self.reminders) or "none"

    def _sch_create(self, a):
        kind = a.get("kind", "reminder")
        if not a.get("text") or not (a.get("when") or a.get("every")):
            raise ToolError("create needs text and when (or every for a routine)")
        when = a.get("when") or a.get("every")
        if kind == "reminder" and not re.fullmatch(r"\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}(:\d{2})?", when):
            raise ToolError(f"when '{when}' is not YYYY-MM-DD HH:MM")
        rid = f"s{len(self.reminders) + 1}"
        self.reminders.append({"id": rid, "kind": kind, "when": when.replace("T", " ")[:16], "text": a["text"]})
        return f"created {kind} {rid} at {when}: {a['text']}"

    def _sch_change(self, a):
        r = next((x for x in self.reminders if x["id"] == a.get("id")), None)
        if not r:
            raise ToolError(f"no entry {a.get('id')}")
        if a["op"] == "delete":
            self.reminders.remove(r)
            return f"deleted {r['id']}"
        for k in ("when", "text"):
            if a.get(k):
                r[k] = a[k]
        return f"updated {r['id']} {r['when']}: {r['text']}"

    # ------------------------------------------------------------------ groups
    def t_agent_start(self, a):
        _accept(a, {"name", "task", "files", "model"}, ("name", "task"))
        jid = f"a{len(self.jobs) + 7}"
        self.jobs.append({"id": jid, **a})
        return f"started id={jid} job=job_{len(self.jobs):04d}x9: {a['name']}"

    def t_agent_list(self, a):
        _accept(a, set())
        return "\n".join(f"{j['id']} running: {j['name']}" for j in self.jobs) or "no jobs"

    def t_agent_stop(self, a):
        _accept(a, {"id"}, ("id",))
        raise ToolError(f"no job {a['id']}")

    def t_device_status(self, a):
        _accept(a, set())
        return "battery 81%, Wi-Fi -58 dBm, card 3.1 of 14.8 GB used, up 2 d 4 h"

    def t_more(self, a):
        _accept(a, {"group", "name", "args"})
        groups = manifest.groups_for(self.variant)
        if a.get("name"):
            name = a["name"].split(".")[-1]
            if not any(name in v for v in groups.values()):
                raise ToolError(f"no tool {a['name']} behind more; groups: {', '.join(groups)}")
            args = a.get("args") or {}
            if name not in manifest.GROUP_TOOLS and not hasattr(self, "t_" + name):
                raise ToolError(f"{name} is not available on this device")
            return self._dispatch(name, args)
        g = a.get("group")
        if not g:
            return "groups: " + "; ".join(f"{k}: {', '.join(v)}" for k, v in groups.items())
        if g not in groups:
            raise ToolError(f"no group {g}; groups: {', '.join(groups)}")
        lines = []
        for n in groups[g]:
            spec = manifest.GROUP_TOOLS.get(n) or manifest.CORE.get(n)
            if spec:
                props = spec["schema"]["properties"]
                req = spec["schema"].get("required", [])
                sig = ", ".join(p + ("" if p in req else "?") for p in props)
                lines.append(f"{n}({sig}): {spec['description']}")
            else:
                lines.append(f"{n}: not available on this device")
        return "\n".join(lines)

    variant: str = "L1"


def json_args(args) -> str:
    return json.dumps(args, ensure_ascii=False, sort_keys=True)
