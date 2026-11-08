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
  2. a bare hostByName() call anywhere, the seam src/sys/net_util.h included - the
     Arduino core's hostByName is the one that calls dns_clear_cache() unlocked
     (NetworkManager.cpp:84), the A03 crash path.
  3. a hostByName() call INSIDE a LOCK_TCPIP_CORE region: Network.hostByName resolves
     through the netconn API, which takes the core lock itself (netdb.c ->
     netconn_gethostbyname_addrtype -> tcpip_send_msg_wait_sem -> LOCK_TCPIP_CORE,
     tcpip.c:446), and the core lock is a non-recursive mutex taken with portMAX_DELAY
     (port/freertos/sys_arch.c:44,63) - holding it across hostByName self-deadlocks the
     calling task on a mutex it already holds. Resolve with the RAW dns_gethostbyname
     under the lock instead (it asserts the lock held but never takes it), then
     connect by IPAddress.
  4. a host-based connect() that does not go through connectHost() in the seam: the
     first argument must be an IPAddress-typed expression (an IPAddress(...) literal or
     a name declared as IPAddress in the same file). Anything else - a string, a
     member, a cast, any parenthesized expression - is a hostname, and a hostname
     connect runs the core's unlocked hostByName inside (NetworkClient.cpp:312,
     NetworkClientSecure.cpp:140,192).
  5. a LOCK_TCPIP_CORE() with no matching UNLOCK_TCPIP_CORE() - an unclosed lock must
     fail the gate, not silently exempt the rest of the file from rule 1.

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

# The seam file: the one place allowed to resolve hostnames and to connect, and only
# through resolveHostLocked() (raw dns_gethostbyname under the lock) + connectHost()
# (connect by IPAddress). The rules below apply to it like to every other file.
SEAM = os.path.join("src", "sys", "net_util.h")
# The dial itself (lane SNI): connectHost runs nimbus::net::connectSecure / connectPlain, templates
# whose address type is a parameter the device instantiates with IPAddress. In that one file a name
# declared `Addr` is the resolved address.
DIAL_TEMPLATE = os.path.join("lib", "core", "include", "nimbus", "net", "tls_connect.h")
ADDR_DECL = re.compile(r"\bAddr\s+([A-Za-z_]\w*)")

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
# The first argument, allowing one level of nested parens: a cast or any
# parenthesized expression (e.g. c.connect((const char*)host, 443)) must be captured
# and flagged, not skipped; a zero-argument connect() (a member named connect, not a
# dial) does not match at all.
CONNECT = re.compile(r"\bconnect\s*\(\s*((?:[^,()]|\([^()]*\))+)")
IP_DECL = re.compile(r"\bIPAddress\s+([A-Za-z_]\w*)")

RULE_RAW = "raw lwIP call outside LOCK_TCPIP_CORE"
RULE_HOSTBYNAME = "bare hostByName anywhere (unlocked dns_clear_cache path)"
RULE_HOSTBYNAME_LOCKED = (
    "hostByName under LOCK_TCPIP_CORE self-deadlocks (netconn re-takes the non-recursive core lock)"
)
RULE_CONNECT = "host-based connect not through connectHost"
RULE_UNCLOSED = "LOCK_TCPIP_CORE without matching UNLOCK_TCPIP_CORE"


class Finding(NamedTuple):
    path: str
    line: int
    rule: str
    text: str


def lock_regions(text: str) -> Iterator["tuple[int, int, int, bool]"]:
    """(lock_start, start, end, unclosed) of every LOCK_TCPIP_CORE() region.

    An unclosed LOCK (no UNLOCK_TCPIP_CORE at its brace depth) extends to end of text
    and is reported unclosed, so it fails the gate instead of exempting the rest of
    the file from the raw-lwIP rule.
    """
    for m in LOCK.finditer(text):
        end = _pair_end(text, m.end(), lambda i: UNLOCK.match(text, i))
        yield m.start(), m.end(), end, end == len(text)


def _ip_shaped(arg: str, ip_names: "set[str]") -> bool:
    arg = arg.strip()
    return arg.startswith("IPAddress") or arg in ip_names or arg.startswith("127.") or arg == "0.0.0.0"


def scan_source(path: str, src: str) -> List[Finding]:
    text = blank_comments_and_strings(src)
    lines = src.split("\n")
    rel = os.path.relpath(path, REPO) if os.path.isabs(path) else path
    regions = list(lock_regions(text))
    ip_names = set(IP_DECL.findall(text))
    if rel == DIAL_TEMPLATE:
        ip_names |= set(ADDR_DECL.findall(text))

    def in_lock(pos: int) -> bool:
        return any(s <= pos < e for _ls, s, e, _u in regions)

    def line_of(pos: int) -> int:
        return text.count("\n", 0, pos) + 1

    found: List[Finding] = []
    for m in RAW_LWIP.finditer(text):
        if not in_lock(m.start()):
            ln = line_of(m.start())
            found.append(Finding(rel, ln, RULE_RAW, lines[ln - 1].strip()))
    for m in HOSTBYNAME.finditer(text):
        ln = line_of(m.start())
        rule = RULE_HOSTBYNAME_LOCKED if in_lock(m.start()) else RULE_HOSTBYNAME
        found.append(Finding(rel, ln, rule, lines[ln - 1].strip()))
    for m in CONNECT.finditer(text):
        if not _ip_shaped(m.group(1), ip_names):
            ln = line_of(m.start())
            found.append(Finding(rel, ln, RULE_CONNECT, lines[ln - 1].strip()))
    for lock_start, _s, _e, unclosed in regions:
        if unclosed:
            ln = line_of(lock_start)
            found.append(Finding(rel, ln, RULE_UNCLOSED, lines[ln - 1].strip()))
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
    print(
        f"PASS lwip core lock: {files} files, {locks} lock regions, every raw lwIP call locked, "
        "no hostByName anywhere (bare or under a lock), every host connect through connectHost by IPAddress"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
