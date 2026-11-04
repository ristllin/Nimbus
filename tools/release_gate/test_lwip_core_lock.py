"""Host tests for the lwIP core-lock scan (check_lwip_core_lock.py).

Run: python3 -m pytest tools/release_gate
"""

import os

import check_lwip_core_lock as lw

HERE = os.path.dirname(os.path.abspath(__file__))
SEAM_SRC = os.path.join(lw.REPO, lw.SEAM)


def lines(src, path="x.cpp"):
    return [f.line for f in lw.scan_source(path, src)]


def rules(src, path="x.cpp"):
    return [f.rule for f in lw.scan_source(path, src)]


# The shape of the bench A03 crash on candidate 28813029: the relay task dials by
# hostname, and the core's hostByName inside connect() runs dns_clear_cache()
# unlocked on the IP-state flip (decoded in evidence/crash2/a03-decode-28813029.txt).
A03_DIAL = """
void dialSession(WiFiClientSecure& c, const String& host) {
  const bool tlsOk = c.connect(host.c_str(), 443, 12000);
}
"""


def test_the_a03_shape_fails_on_its_line():
    assert lines(A03_DIAL) == [3]
    assert rules(A03_DIAL) == ["host-based connect not through connectHost"]


def test_bare_hostbyname_fails_outside_the_seam():
    src = 'void f() { IPAddress ip; WiFi.hostByName("api.mistral.ai", ip); }'
    assert lines(src) == [1]
    assert rules(src) == ["bare hostByName outside the locked seam"]


def test_raw_lwip_calls_fail_outside_a_lock_region():
    assert lines("void f() { dns_clear_cache(); }") == [1]
    assert lines('void f() { ip_addr_t a; dns_gethostbyname("x", &a, 0, 0); }') == [1]
    assert lines("void f() { struct udp_pcb* p = udp_new_ip_type(0); }") == [1]
    assert lines("void f() { struct tcp_pcb* p = tcp_new_ip_type(0); }") == [1]
    assert lines("void f() { struct pbuf* p = pbuf_alloc(PBUF_RAM, 8, PBUF_POOL); }") == [1]


def test_every_raw_api_name_fails_outside_a_lock():
    names = [
        "dns_clear_cache",
        "dns_gethostbyname",
        "dns_gethostbyname_addrtype",
        "dns_setserver",
        "udp_new",
        "udp_new_ip_type",
        "udp_bind",
        "udp_sendto",
        "tcp_new",
        "tcp_bind",
        "tcp_connect",
        "tcp_write",
        "tcp_output",
        "pbuf_alloc",
        "pbuf_free",
        "netbuf_new",
        "netbuf_delete",
        "netif_set_ipaddr",
        "ip_addr_set",
        "ip4_addr_set",
        "etharp_output",
        "igmp_start",
    ]
    for n in names:
        assert lines(f"void f() {{ {n}(); }}") == [1], n


def test_the_locked_pattern_passes():
    src = """
bool webDnsReady(const char* host) {
  ip_addr_t addr;
  const bool own = sys_thread_tcpip(LWIP_CORE_LOCK_QUERY_HOLDER);
  if (!own) LOCK_TCPIP_CORE();
  const err_t e = dns_gethostbyname(host, &addr, 0, 0);
  if (!own) UNLOCK_TCPIP_CORE();
  return e == ERR_OK;
}
"""
    assert lines(src) == []


def test_connecthost_and_ip_connects_pass():
    src = """
void f(WiFiClientSecure& c, Client& k, const char* host) {
  IPAddress self = WiFi.localIP();
  connectHost(c, host, 443);
  connectHost(c, host, 443, 12000);
  connectHost(k, host, 443);
  c.connect(self, 80);
  c.connect(IPAddress(127, 0, 0, 1), 80);
}
"""
    assert lines(src) == []


def test_string_literal_and_member_hosts_fail():
    assert lines('void f(WiFiClientSecure& c) { c.connect("api.mistral.ai", 443); }') == [1]
    assert lines("void f(WiFiClientSecure& c, UrlParts& u) { c.connect(u.host.c_str(), u.port); }") == [1]


def test_the_seam_file_itself_is_allowed():
    with open(SEAM_SRC, encoding="utf-8") as f:
        src = f.read()
    assert lw.scan_source(SEAM_SRC, src) == []


def test_the_same_text_outside_the_seam_fails():
    with open(SEAM_SRC, encoding="utf-8") as f:
        src = f.read()
    assert lw.scan_source("src/other.cpp", src) != []


def test_the_tree_is_clean():
    assert lw.main([]) == 0
