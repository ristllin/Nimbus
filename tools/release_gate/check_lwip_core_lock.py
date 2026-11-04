#!/usr/bin/env python3
"""Release gate: every lwIP call from a task outside the TCPIP thread holds the core lock.

Bench row A03 on the v5.0.0 candidate 28813029 panicked the unit during a Wi-Fi AP
drop and rejoin: task=relay, fp=7f7617df, "assert failed: udp_new_ip_type lwip udp.c:1278
(Required to lock TCPIP core functionality!)". Decoded against the candidate's exact ELF
(evidence/crash2/a03-decode-28813029.txt), the chain is relayTask ->
WiFiClientSecure::connect(host, ...) -> NetworkManager::hostByName -> dns_clear_cache
(NetworkManager.cpp:84, called WITHOUT the TCPIP core lock when an interface IP state
just flipped - exactly the rejoin case) -> dns_call_found -> sntp_dns_found ->
sntp_request -> dns_gethostbyname -> dns_alloc_random_port -> udp_new_ip_type ->
LWIP_ASSERT_CORE_LOCKED -> panic.

The scan reads src/ and lib/ and fails on:
  1. a raw lwIP core API call (dns_*, udp_*, tcp_*, pbuf_*, netbuf_*, netif_*, ip_addr_*,
     ...) outside a LOCK_TCPIP_CORE() ... UNLOCK_TCPIP_CORE() region. src/ never runs on
     the TCPIP thread, so every raw call must hold the lock; the socket API and the
     netconn API (lwip_getaddrinfo, netconn_gethostbyname, setsockopt, ...) take the lock
     internally and are not raw.
  2. a bare hostByName() call anywhere but the locked seam src/sys/net_util.h - the
     Arduino core's hostByName is the one that calls dns_clear_cache() unlocked.
  3. a host-based connect() that does not go through connectHost() in the seam: the
     first argument must be an IPAddress-typed expression (an IPAddress(...) literal or
     a name declared as IPAddress in the same file). Anything else is a hostname, and a
     hostname connect runs the core's unlocked hostByName inside.

Usage:
    python3 tools/release_gate/check_lwip_core_lock.py [paths...]
Exit 0 = clean; exit 1 = a bare lwIP call fails the gate.
"""

from __future__ import annotations

import os
import re
import sys
from typing import Iterator, List, NamedTuple

from check_critical_sections import _pair_end, blank_comments_and_strings

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_ROOTS = ("src", "lib")
EXTENSIONS = (".cpp", ".cc", ".c", ".h", ".hpp")

# The one file allowed to resolve by host / connect by host directly: it holds the
# TCPIP core lock around them (hostByNameLocked / connectHost).
SEAM = os.path.join("src", "sys", "net_util.h")

# Raw lwIP core APIs: each asserts LWIP_ASSERT_CORE_LOCKED() when the caller is not the
# lock holder. The socket API (lwip_socket, setsockopt, lwip_getaddrinfo, ...) and the
# netconn API take the core lock internally, so they are deliberately absent.
RAW_LWIP = re.compile(
    r"\b("
    r"dns_clear_cache|dns_gethostbyname\w*|dns_setserver\w*|dns_getserver\w*|dns_init|"
    r"udp_new\w*|udp_remove|udp_bind|udp_connect|udp_disconnect|udp_send\w*|udp_recv|"
    r"netbuf_\w+|pbuf_\w+|netif_\w+|ip_addr_\w+|ip4_addr\w*|ip6_addr\w*|"
    r"etharp_\w+|igmp_\w+|raw_\w+|"
    r"tcp_new\w*|tcp_abort|tcp_arg|tcp_recv|tcp_sent|tcp_err|tcp_poll|tcp_bind|tcp_listen|"
    r"tcp_accept|tcp_connect|tcp_write|tcp_output|tcp_close|tcp_shutdown"
    r")\s*\("
)
LOCK = re.compile(r"\bLOCK_TCPIP_CORE\s*\(\s*\)")
UNLOCK = re.compile(r"\bUNLOCK_TCPIP_CORE\s*\(\s*\)")
HOSTBYNAME = re.compile(r"\bhostByName\s*\(")
CONNECT = re.compile(r"\bconnect\s*\(\s*([^,()]+)")
IP_DECL = re.compile(r"\bIPAddress\s+([A-Za-z_]\w*)")


class Finding(NamedTuple):
    path: str
    line: int
    rule: str
    text: str


def lock_regions(text: str) -> Iterator["tuple[int, int]"]:
    """(start, end) of every LOCK_TCPIP_CORE() ... UNLOCK_TCPIP_CORE() region."""
    for m in LOCK.finditer(text):
        yield m.end(), _pair_end(text, m.end(), lambda i: UNLOCK.match(text, i))


def _ip_shaped(arg: str, ip_names: "set[str]") -> bool:
    arg = arg.strip()
    return (
        arg.startswith("IPAddress")
        or arg in ip_names
        or arg.startswith("127.")
        or arg == "0.0.0.0"
    )


def scan_source(path: str, src: str) -> List[Finding]:
    text = blank_comments_and_strings(src)
    lines = src.split("\n")
    rel = os.path.relpath(path, REPO) if os.path.isabs(path) else path
    seam = rel == SEAM
    regions = list(lock_regions(text))
    ip_names = set(IP_DECL.findall(text))

    def in_lock(pos: int) -> bool:
        return any(s <= pos < e for s, e in regions)

    def line_of(pos: int) -> int:
        return text.count("\n", 0, pos) + 1

    found: List[Finding] = []
    for m in RAW_LWIP.finditer(text):
        if not in_lock(m.start()):
            ln = line_of(m.start())
            found.append(Finding(rel, ln, "raw lwIP call outside LOCK_TCPIP_CORE", lines[ln - 1].strip()))
    if not seam:
        for m in HOSTBYNAME.finditer(text):
            ln = line_of(m.start())
            found.append(Finding(rel, ln, "bare hostByName outside the locked seam", lines[ln - 1].strip()))
        for m in CONNECT.finditer(text):
            if not _ip_shaped(m.group(1), ip_names):
                ln = line_of(m.start())
                found.append(Finding(rel, ln, "host-based connect not through connectHost", lines[ln - 1].strip()))
    # de-dup by line, keep order
    seen = set()
    out: List[Finding] = []
    for fd in sorted(found):
        if fd.line not in seen:
            seen.add(fd.line)
            out.append(fd)
    return out


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
    files = locks = 0
    for path in source_files(roots):
        files += 1
        with open(path, encoding="utf-8", errors="replace") as f:
            src = f.read()
        text = blank_comments_and_strings(src)
        locks += sum(1 for _ in lock_regions(text))
        found.extend(scan_source(path, src))
    for fd in found:
        print(f"FAIL {fd.path}:{fd.line}: {fd.rule}: {fd.text}")
    if files == 0:
        print("FAIL lwip core lock: no source read")
        return 1
    if found:
        print(f"FAIL lwip core lock: {len(found)} call(s) outside LOCK_TCPIP_CORE / the locked seam")
        return 1
    print(f"PASS lwip core lock: {files} files, {locks} lock regions, every raw lwIP call locked, every host connect through the seam")
    return 0


if __name__ == "__main__":
    sys.exit(main())
