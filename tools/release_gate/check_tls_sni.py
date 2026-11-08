#!/usr/bin/env python3
"""Release gate: every outbound TLS dial names its host (SNI and the certificate name check).

Lane SNI. From lane CRASH2 (e697fe87) the seam src/sys/net_util.h resolved the name under the
lwIP core lock and then dialed WiFiClientSecure::connect(ip, port[, timeout]). In arduino-esp32
3.3.9 those overloads pass host = NULL (NetworkClientSecure.cpp:110-136), so start_ssl_client set
the TLS name to the address string (ssl_client.cpp:309): the ClientHello carried no SNI, the
certificate was checked against "160.79.104.10", and the CDN front ends refused every handshake
(-30592). The K01 soak on c168797b recorded every provider exchange as fail=connect, and bench B03
on bf40a3ed saw the same unit connect on v4.6.4 and be refused on every handshake after the update.

The one dial for a secure client is nimbus::net::connectSecure (lib/core/include/nimbus/net/
tls_connect.h, host-tested in test/test_tls_connect), reached only through connectHost in
src/sys/net_util.h. The scan reads src/ and lib/ and fails on:
  1. a dial (X.connect(a, b, ...) or X->connect(...), two or more arguments) outside the two seam
     files whose receiver is not declared as a plain WiFiClient. A secure client dialed
     directly, a NetworkClient (a WiFiClientSecure can sit in one), a Client, an auto, or a
     receiver the scan cannot find all fail: only the seam dials a secure client.
  2. a connectHost(arg, ...) call outside the seam whose arg is not a name (or *name of a pointer)
     declared as WiFiClientSecure or WiFiClient. A Client&, NetworkClient&, NetworkClientSecure&
     or Client* reaches whatever overload takes the base class, and a base class cannot carry a
     name to the TLS layer.
  3. the seam net_util.h: a live Client& overload, a missing deleted Client& overload, any
     NetworkClient/NetworkClientSecure declaration (the Network base names - WiFiClient and
     WiFiClientSecure are typedefs of them, and a NetworkClient& can hold a secure client),
     a missing live overload for WiFiClientSecure or WiFiClient, a secure overload that does
     not dial through nimbus::net::connectSecure, a plain overload that does not dial through
     nimbus::net::connectPlain, a missing can_carry_tls<> specialization for Client or the
     secure class, or a dial of its own.
  4. the template tls_connect.h: connectSecure has no dial, or a dial that is not
     connect(<address>, <port>, host, nullptr, nullptr, nullptr) - the only framework overload that
     carries the name (the three null pointers keep the bundle or insecure mode the client was
     set up with: start_ssl_client reads those from members, ssl_client.cpp:203-227) - or
     connectPlain without the static_assert refusing a client that can carry TLS
     (can_carry_tls, specialized for the device's types in the seam).
  5. a direct instantiation of connectSecure or connectPlain outside the two seam files: the
     templates are the seam's own, instantiated only by connectHost.

The receiver's type is the nearest declaration of that name above the call in the same file, which
is what the compiler sees for a local, a parameter or a member declared above its use.

Usage:
    python3 tools/release_gate/check_tls_sni.py [paths...]
Exit 0 = clean; exit 1 = a dial that can reach the TLS layer without the host's name.
"""

from __future__ import annotations

import os
import re
import sys
from typing import Iterator, List, NamedTuple, Optional, Tuple

from check_critical_sections import blank_comments_and_strings

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_ROOTS = ("src", "lib")
EXTENSIONS = (".cpp", ".cc", ".c", ".h", ".hpp")

SEAM = os.path.join("src", "sys", "net_util.h")
TEMPLATE = os.path.join("lib", "core", "include", "nimbus", "net", "tls_connect.h")

# Only the two WiFi client type names may be dialed. arduino-esp32 3.3.9 has two client
# classes, NetworkClient and NetworkClientSecure (derived from it); WiFiClient and
# WiFiClientSecure are typedefs of them (WiFiClient.h:3, WiFiClientSecure.h:3), so the WiFi
# names are the convention, not distinct types. The Network base names can hold a secure
# client and cannot carry its name to the TLS layer: the Client& overloads must be deleted,
# and a NetworkClient/NetworkClientSecure declaration in the seam is a failure (a
# NetworkClient& parameter is exactly the by-address TLS hole).
SECURE_TYPES = ("WiFiClientSecure",)
PLAIN_TYPES = ("WiFiClient",)
CLIENT_BASE = "Client"
NETWORK_BASE = ("NetworkClient", "NetworkClientSecure")
BASE_TYPES = (CLIENT_BASE,) + NETWORK_BASE
CLIENT_DECL_TYPES = SECURE_TYPES + PLAIN_TYPES + BASE_TYPES + ("auto",)

DIAL = re.compile(r"(?:\.|->)\s*connect\s*\(|(?<![\w.>:])connect\s*\(")
CONNECT_HOST = re.compile(r"\bconnectHost\s*\(")
TEMPLATE_CALL = re.compile(r"\bconnect(?:Secure|Plain)\s*<")
OVERLOAD = re.compile(
    r"\bint\s+connectHost\s*\(\s*(" + "|".join(CLIENT_DECL_TYPES[:-1]) + r")\s*&[^)]*\)\s*(=\s*delete\s*;|\{)"
)
SECURE_TEMPLATE = re.compile(r"\bint\s+connectSecure\s*\([^)]*\)\s*\{")
PLAIN_TEMPLATE = re.compile(r"\bint\s+connectPlain\s*\([^)]*\)\s*\{")
PLAIN_GUARD = re.compile(r"static_assert\s*\(\s*!\s*can_carry_tls\s*<\s*Plain\s*>")
TRAIT_SPEC = "struct\\s+can_carry_tls\\s*<\\s*{kind}\\s*>\\s*:\\s*std::true_type"

RULE_DIAL = "a dial outside the seam on a client that is not a declared plain WiFiClient"
RULE_ARG = "connectHost on a client that is not a declared WiFiClientSecure or WiFiClient"
RULE_SEAM = "the seam can dial a secure client without its host's name"
RULE_TEMPLATE = "connectSecure does not dial with connect(ip, port, host, nullptr, nullptr, nullptr)"
RULE_TPL = "the dial templates are instantiated only by the seam"


class Finding(NamedTuple):
    path: str
    line: int
    rule: str
    text: str


def _rel(path: str) -> str:
    return os.path.relpath(path, REPO) if os.path.isabs(path) else path


def call_args(text: str, open_paren: int) -> Tuple[List[str], int]:
    """The top-level arguments of the call whose "(" is at open_paren, and the index past its ")"."""
    depth, start, args = 0, open_paren + 1, []
    i = open_paren
    while i < len(text):
        ch = text[i]
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
            if depth == 0:
                last = text[start:i].strip()
                if last or args:
                    args.append(last)
                return args, i + 1
        elif ch == "," and depth == 1:
            args.append(text[start:i].strip())
            start = i + 1
        i += 1
    return args, len(text)


def body_at(text: str, open_brace: int) -> str:
    """The text between the brace at open_brace and its match."""
    depth = 0
    for i in range(open_brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace + 1 : i]
    return text[open_brace + 1 :]


def declared_type(text: str, name: str, before: int) -> Optional[Tuple[str, str]]:
    """(type, "&" | "*" | "") of the nearest declaration of `name` above `before`, among the client
    types and auto; None when there is none."""
    decl = re.compile(
        r"\b("
        + "|".join(CLIENT_DECL_TYPES)
        + r")\b\s*(?:const\s*)?([&*]?)\s*(?:const\s+)?\b"
        + re.escape(name)
        + r"\b(?!\s*\()"
    )
    found = None
    for m in decl.finditer(text, 0, before):
        found = (m.group(1), m.group(2))
    return found


def _receiver(text: str, at: int) -> Optional[str]:
    """The name in front of the `.connect(` or `->connect(` at `at`, or None (no receiver, or not a name)."""
    if not text.startswith((".", "-"), at):
        return None
    i = at
    while i > 0 and text[i - 1].isspace():
        i -= 1
    m = re.search(r"([A-Za-z_]\w*)$", text[:i])
    if not m:
        return None
    # g_ws->connect is a name; foo().connect and a[i].connect are not.
    return m.group(1)


def classify_arg(text: str, arg: str, at: int) -> Tuple[Optional[str], str]:
    """("secure" | "plain" | None, why) for the client argument of a connectHost call at `at`."""
    m = re.fullmatch(r"(\*?)\s*([A-Za-z_]\w*)", arg.strip())
    if not m:
        return None, f"the client argument {arg.strip()!r} is not a name"
    deref, name = m.group(1) == "*", m.group(2)
    decl = declared_type(text, name, at)
    if not decl:
        return None, f"{name} has no client declaration above the call"
    kind, mark = decl
    if deref != (mark == "*"):
        return None, f"{'*' if deref else ''}{name} does not match its declaration {kind}{mark} {name}"
    if kind in SECURE_TYPES:
        return "secure", f"{name} is a {kind}"
    if kind in PLAIN_TYPES:
        return "plain", f"{name} is a {kind}"
    return None, f"{name} is declared {kind}{mark}, which can hold a secure client and cannot carry its name"


def _finding(rel: str, text: str, lines: List[str], pos: int, rule: str, why: str = "") -> Finding:
    ln = text.count("\n", 0, pos) + 1
    src_line = lines[ln - 1].strip() if ln - 1 < len(lines) else ""
    return Finding(rel, ln, rule, f"{src_line}  [{why}]" if why else src_line)


def _scan_dials(rel, text, lines) -> List[Finding]:
    out = []
    for m in DIAL.finditer(text):
        args, _ = call_args(text, m.end() - 1)
        if len(args) < 2:
            continue  # connect() with no address is not a dial (a member named connect)
        name = _receiver(text, m.start())
        if name is None:
            out.append(_finding(rel, text, lines, m.start(), RULE_DIAL, "no named receiver"))
            continue
        decl = declared_type(text, name, m.start())
        if not decl or decl[0] not in PLAIN_TYPES or decl[1] == "*" and not text.startswith("-", m.start()):
            why = f"{name} is declared {decl[0]}{decl[1]}" if decl else f"{name} has no client declaration"
            out.append(_finding(rel, text, lines, m.start(), RULE_DIAL, why))
    return out


def _scan_connect_host_calls(rel, text, lines) -> List[Finding]:
    out = []
    for m in CONNECT_HOST.finditer(text):
        if re.search(r"\bint\s+$", text[: m.start()]):
            continue  # a definition, not a call
        args, _ = call_args(text, m.end() - 1)
        if not args:
            continue
        kind, why = classify_arg(text, args[0], m.start())
        if kind is None:
            out.append(_finding(rel, text, lines, m.start(), RULE_ARG, why))
    return out


def _seam_overloads(text: str) -> "Tuple[dict[str, List[str]], set]":
    """(live kind -> overload bodies, deleted kinds) of the seam's connectHost overloads."""
    live: "dict[str, List[str]]" = {}
    deleted = set()
    for m in OVERLOAD.finditer(text):
        kind, tail = m.group(1), m.group(2)
        if tail.startswith("="):
            deleted.add(kind)
        else:
            live.setdefault(kind, []).append(body_at(text, m.end() - 1))
    return live, deleted


def _seam_overload_findings(rel: str, live: "dict[str, List[str]]", deleted: set) -> List[Finding]:
    out: List[Finding] = []
    if CLIENT_BASE in live:
        out.append(
            Finding(
                rel,
                1,
                RULE_SEAM,
                "a Client& overload that dials: a secure client bound to it reaches a by-address dial",
            )
        )
    elif CLIENT_BASE not in deleted:
        out.append(Finding(rel, 1, RULE_SEAM, "no deleted connectHost(Client&, ...) overload poisons the base class"))
    for kind in NETWORK_BASE:
        if kind in live or kind in deleted:
            out.append(
                Finding(
                    rel,
                    1,
                    RULE_SEAM,
                    f"the seam declares connectHost({kind}&, ...): {kind} is the "
                    "Network base name (WiFiClient/WiFiClientSecure are typedefs of "
                    "it) and a reference to it can hold a secure client; declare the "
                    "WiFi type name",
                )
            )
    for kind in SECURE_TYPES + PLAIN_TYPES:
        if kind in deleted:
            out.append(Finding(rel, 1, RULE_SEAM, f"the {kind}& overload is deleted; only the Client& base is"))
        elif kind not in live:
            out.append(Finding(rel, 1, RULE_SEAM, f"no live connectHost({kind}&, ...) overload"))
    for kind, bodies in live.items():
        want = "connectSecure" if kind in SECURE_TYPES else "connectPlain"
        for body in bodies:
            if not re.search(r"\bnimbus\s*::\s*net\s*::\s*" + want + r"\s*<", body):
                out.append(Finding(rel, 1, RULE_SEAM, f"the {kind}& overload does not call {want}"))
    return out


def _seam_trait_findings(rel: str, text: str) -> List[Finding]:
    out: List[Finding] = []
    specs = [
        (CLIENT_BASE, TRAIT_SPEC.format(kind=CLIENT_BASE)),
        ("WiFiClientSecure", TRAIT_SPEC.format(kind="WiFiClientSecure|NetworkClientSecure")),
    ]
    for name, pattern in specs:
        if not re.search(pattern, text):
            out.append(
                Finding(
                    rel, 1, RULE_SEAM, f"no can_carry_tls<{name}> specialization: connectPlain would accept a {name}"
                )
            )
    return out


def scan_seam(rel: str, src: str) -> List[Finding]:
    text = blank_comments_and_strings(src)
    lines = src.split("\n")
    live, deleted = _seam_overloads(text)
    out = _seam_overload_findings(rel, live, deleted) + _seam_trait_findings(rel, text)
    for m in DIAL.finditer(text):
        args, _ = call_args(text, m.end() - 1)
        if len(args) >= 2:
            out.append(_finding(rel, text, lines, m.start(), RULE_SEAM, "the seam dials itself"))
    return out


def scan_template(rel: str, src: str) -> List[Finding]:
    text = blank_comments_and_strings(src)
    lines = src.split("\n")
    m = SECURE_TEMPLATE.search(text)
    if not m:
        return [Finding(rel, 1, RULE_TEMPLATE, "no connectSecure definition")]
    body = body_at(text, m.end() - 1)
    base = m.end()
    dials = [d for d in DIAL.finditer(body) if len(call_args(body, d.end() - 1)[0]) >= 2]
    if not dials:
        return [_finding(rel, text, lines, base, RULE_TEMPLATE, "connectSecure never dials")]
    out = []
    for d in dials:
        args, _ = call_args(body, d.end() - 1)
        if len(args) != 6 or args[2] != "host" or any(a != "nullptr" for a in args[3:]):
            out.append(_finding(rel, text, lines, base + d.start(), RULE_TEMPLATE, f"dials with ({', '.join(args)})"))
    p = PLAIN_TEMPLATE.search(text)
    if not p:
        out.append(Finding(rel, 1, RULE_TEMPLATE, "no connectPlain definition"))
    else:
        pbody = body_at(text, p.end() - 1)
        if not PLAIN_GUARD.search(pbody):
            out.append(
                Finding(
                    rel,
                    1,
                    RULE_TEMPLATE,
                    "connectPlain has no static_assert refusing "
                    "can_carry_tls<Plain>: a plain dial on a secure client "
                    "would go out by address with no name",
                )
            )
    return out


def scan_source(path: str, src: str) -> List[Finding]:
    rel = _rel(path)
    if rel == SEAM:
        return scan_seam(rel, src)
    if rel == TEMPLATE:
        return scan_template(rel, src)
    text = blank_comments_and_strings(src)
    lines = src.split("\n")
    found = _scan_dials(rel, text, lines) + _scan_connect_host_calls(rel, text, lines)
    for m in TEMPLATE_CALL.finditer(text):
        found.append(_finding(rel, text, lines, m.start(), RULE_TPL, "instantiated outside the seam"))
    return sorted(set(found))


def definition_brace(text: str, head: str) -> Optional[int]:
    """The opening brace of the function defined as `head(...)` (head ends with "(", spaces are
    free), skipping declarations; None when the file defines no such function."""
    pattern = r"\s*".join(re.escape(ch) for ch in re.sub(r"\s+", "", head))
    for m in re.finditer(pattern, text):
        _args, past = call_args(text, m.end() - 1)
        tail = re.match(r"(?:\s|const\b|override\b|noexcept\b)*\{", text[past:])
        if tail:
            return past + tail.end() - 1
    return None


def connect_host_routes(path: str, src: str, head: str) -> List[Tuple[str, Optional[str], str]]:
    """Every connectHost call inside the function defined as `head(...)`: (client argument,
    "secure" | "plain" | None, why)."""
    text = blank_comments_and_strings(src)
    brace = definition_brace(text, head)
    if brace is None:
        raise LookupError(f"{_rel(path)}: no function {head!r}")
    body = body_at(text, brace)
    out = []
    for c in CONNECT_HOST.finditer(body):
        args, _ = call_args(body, c.end() - 1)
        kind, why = classify_arg(text, args[0], brace + 1 + c.start())
        out.append((args[0], kind, why))
    return out


def seam_findings(repo: str = REPO) -> List[Finding]:
    """Rules 3 and 4 over the tree's own seam files: empty when the secure overloads name the host."""
    found: List[Finding] = []
    for rel in (SEAM, TEMPLATE):
        with open(os.path.join(repo, rel), encoding="utf-8") as f:
            found.extend(scan_source(rel, f.read()))
    return found


def source_files(roots: "tuple[str, ...]") -> Iterator[str]:
    for root in roots:
        path = root if os.path.isabs(root) else os.path.join(REPO, root)
        if os.path.isfile(path):
            yield path
            continue
        for dirpath, _dirs, names in os.walk(path):
            for name in sorted(names):
                if name.endswith(EXTENSIONS):
                    yield os.path.join(dirpath, name)


def main(argv: "list[str] | None" = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    roots = tuple(args) if args else DEFAULT_ROOTS
    found: List[Finding] = []
    files = 0
    seen = set()
    for path in source_files(roots):
        files += 1
        seen.add(_rel(path))
        with open(path, encoding="utf-8", errors="replace") as f:
            found.extend(scan_source(path, f.read()))
    if not args:
        for rel in (SEAM, TEMPLATE):
            if rel not in seen:
                found.append(Finding(rel, 0, RULE_SEAM, "the seam file is missing"))
    for fd in found:
        print(f"FAIL {fd.path}:{fd.line}: {fd.rule}: {fd.text}")
    if files == 0:
        print("FAIL tls sni: no source read")
        return 1
    if found:
        print(f"FAIL tls sni: {len(found)} dial(s) can reach TLS without the host's name")
        return 1
    print(
        f"PASS tls sni: {files} files, every secure dial goes through connectSecure with the host, "
        "the base-class overloads are deleted, connectPlain refuses TLS-capable clients, "
        "the templates are instantiated only by the seam, every other dial is a declared plain WiFiClient"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
