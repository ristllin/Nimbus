"""Host tests for the TLS name scan (check_tls_sni.py), lane SNI.

Run: python3 -m pytest tools/release_gate
"""

import os
import re

import pytest

import check_tls_sni as sni


def rules(src, path="src/x.cpp"):
    return [f.rule for f in sni.scan_source(path, src)]


# ---- the two shapes that shipped on main from e697fe87 ------------------------------------------

# The seam's secure overload on main: resolved under the lock, then dialed by address, which in
# arduino-esp32 3.3.9 sets the TLS name to the address string (no SNI).
MAIN_SEAM = """
inline int connectHost(Client& c, const char* host, uint16_t port) {
  IPAddress ip;
  if (!resolveHostLocked(host, ip)) return 0;
  return c.connect(ip, port);
}
inline int connectHost(WiFiClientSecure& c, const char* host, uint16_t port, int32_t timeoutMs) {
  IPAddress ip;
  if (!resolveHostLocked(host, ip)) return 0;
  return c.connect(ip, port, timeoutMs);
}
"""

FIXED_SEAM = """
inline int connectHost(WiFiClientSecure& c, const char* host, uint16_t port, int32_t timeoutMs) {
  return nimbus::net::connectSecure<IPAddress>(c, host, port, timeoutMs, Resolve{});
}
inline int connectHost(WiFiClient& c, const char* host, uint16_t port) {
  return nimbus::net::connectPlain<IPAddress>(c, host, port, -1, Resolve{});
}
int connectHost(Client& c, const char* host, uint16_t port) = delete;
"""


def test_the_seam_that_shipped_on_main_fails():
    found = sni.scan_source(sni.SEAM, MAIN_SEAM)
    texts = " | ".join(f.text for f in found)
    assert all(f.rule == sni.RULE_SEAM for f in found)
    assert "a Client& overload that dials" in texts
    assert "does not call connectSecure" in texts
    assert "the seam dials itself" in texts


def test_the_fixed_seam_shape_passes():
    assert sni.scan_source(sni.SEAM, FIXED_SEAM) == []


def test_a_seam_without_the_deleted_base_overload_fails():
    src = FIXED_SEAM.replace("int connectHost(Client& c, const char* host, uint16_t port) = delete;", "")
    assert [f.text for f in sni.scan_source(sni.SEAM, src)] == [
        "no deleted connectHost(Client&, ...) overload poisons the base class"
    ]


def test_a_plain_overload_that_skips_connect_plain_fails():
    src = FIXED_SEAM.replace(
        "nimbus::net::connectPlain<IPAddress>(c, host, port, -1, Resolve{})", "c.connect(ip, port)"
    )
    assert sni.RULE_SEAM in rules(src, sni.SEAM)


# The providers' TlsIo on main: a Client* that points at the TLS client, so the call lands in the
# Client& overload, which has no way to pass a name.
TLSIO_MAIN = """
struct TlsIo {
  Client* client;
  WiFiClientSecure& tls;
  bool connect() {
    if (connectHost(*client, req.host.c_str(), req.port)) return true;
    return false;
  }
};
"""

MCP_MAIN = """
bool mcpConnect(Client& c, const UrlParts& u, uint32_t deadline) {
  if (connectHost(c, u.host.c_str(), u.port)) return true;
  return false;
}
"""


@pytest.mark.parametrize("src", [TLSIO_MAIN, MCP_MAIN])
def test_a_base_class_client_into_connect_host_fails(src):
    assert rules(src) == [sni.RULE_ARG]


def test_declared_secure_and_plain_clients_pass():
    src = """
WiFiClientSecure* g_ws = nullptr;
bool tg(WiFiClientSecure& sc) { return connectHost(sc, "api.telegram.org", 443, 10000); }
void a() { WiFiClientSecure client; connectHost(client, host, 443); }
void b() { WiFiClient plain; connectHost(plain, host, 80); }
void c() { connectHost(*g_ws, host.c_str(), 443, 12000); }
"""
    assert rules(src) == []


@pytest.mark.parametrize(
    "src,why",
    [
        ("void f() { auto& c = pick(); connectHost(c, h, 443); }", "auto"),
        ("void f() { connectHost(clients[0], h, 443); }", "not a name"),
        ("void f() { connectHost(other, h, 443); }", "no client declaration"),
        ("WiFiClientSecure* p; void f() { connectHost(p, h, 443); }", "does not match"),
    ],
)
def test_a_client_the_scan_cannot_prove_fails(src, why):
    found = sni.scan_source("src/x.cpp", src)
    assert [f.rule for f in found] == [sni.RULE_ARG]
    assert why in found[0].text


@pytest.mark.parametrize(
    "dial",
    [
        "c.connect(ip, 443);",
        "c.connect(ip, 443, 15000);",
        "c.connect(ip, 443, host, nullptr, nullptr, nullptr);",  # even the right form: only the seam dials TLS
        'c.connect("api.mistral.ai", 443);',
    ],
)
def test_a_direct_dial_on_a_secure_client_fails(dial):
    assert rules("void f() { WiFiClientSecure c; " + dial + " }") == [sni.RULE_DIAL]


def test_a_direct_dial_through_a_base_class_or_an_unknown_receiver_fails():
    assert rules("void f(Client& c) { c.connect(ip, 443); }") == [sni.RULE_DIAL]
    assert rules("void f() { pick().connect(ip, 443); }") == [sni.RULE_DIAL]
    assert rules("void f() { connect(fd, addr); }") == [sni.RULE_DIAL]


def test_the_relay_loopback_dials_on_plain_clients_pass():
    src = """
void up() { WiFiClient c; bool ok = c.connect(IPAddress(127, 0, 0, 1), 80, kLoopbackConnectMs); }
bool connectUploadLoopback(WiFiClient& c) { return c.connect(self, 80, kLoopbackConnectMs); }
"""
    assert rules(src) == []


def test_a_connect_with_no_address_is_not_a_dial():
    assert rules("void f() { WiFiClientSecure c; const bool ok = io.connect(); }") == []


def test_comments_and_strings_are_not_dials():
    src = 'void f() { // c.connect(ip, 443);\n  log("c.connect(ip, 443)"); }'
    assert rules(src) == []


# ---- the template --------------------------------------------------------------------------------


def template(dial):
    return (
        "template <class Addr, class Secure, class Resolve>\n"
        "int connectSecure(Secure& c, const char* host, uint16_t port, int32_t timeoutMs, Resolve resolve) {\n"
        "  Addr ip;\n  if (!resolve(host, ip)) return 0;\n  return " + dial + ";\n}\n"
    )


@pytest.mark.parametrize(
    "dial",
    [
        "c.connect(ip, port)",
        "c.connect(ip, port, timeoutMs)",
        "c.connect(ip, port, host, nullptr, nullptr)",  # binds to (ip, port, CA, cert, key): the name as a CA
        "c.connect(ip, port, nullptr, nullptr, nullptr, nullptr)",
    ],
)
def test_a_template_dial_without_the_name_fails(dial):
    assert rules(template(dial), sni.TEMPLATE) == [sni.RULE_TEMPLATE]


def test_the_template_dial_with_the_name_passes():
    assert rules(template("c.connect(ip, port, host, nullptr, nullptr, nullptr)"), sni.TEMPLATE) == []


def test_a_template_that_never_dials_fails():
    assert rules(template("1"), sni.TEMPLATE) == [sni.RULE_TEMPLATE]


# ---- every adapter path, on the tree -------------------------------------------------------------

# Each path the firmware dials out on, by the function that dials. Every connectHost call there must
# reach a secure overload by its declared type, and the seam's secure overloads must name the host.
ADAPTER_PATHS = [
    ("providers", "src/agent/transport_tls.cpp", "bool connect("),
    ("telegram", "src/agent/telegram.cpp", "bool tlsConnect("),
    ("relay pairing", "src/net/relay_client.cpp", "int httpsPostJson("),
    ("relay session", "src/net/relay_client.cpp", "Dial dialSession("),
    ("cloud mint", "src/net/cloud_mint.cpp", "static void runOne("),
    ("ota", "src/sys/ota_update.cpp", "static int httpsGetOnce("),
    ("sfx sync", "src/sfx/sfx_sync.cpp", "int httpsGet("),
    ("archive downloads", "src/agent/tg_archive.cpp", "bool connectTg("),
    ("mcp connectors", "src/agent/connectors.cpp", "bool mcpConnect("),
]


def _read(rel):
    with open(os.path.join(sni.REPO, rel), encoding="utf-8") as f:
        return f.read()


@pytest.mark.parametrize("label,rel,head", ADAPTER_PATHS, ids=[p[0] for p in ADAPTER_PATHS])
def test_every_adapter_path_dials_tls_with_the_host(label, rel, head):
    routes = sni.connect_host_routes(rel, _read(rel), head)
    assert routes, f"{label}: no connectHost call in {head}"
    for arg, kind, why in routes:
        assert kind in ("secure", "plain"), f"{label}: connectHost({arg}, ...) {why}"
    assert any(kind == "secure" for _a, kind, _w in routes), f"{label}: no TLS dial reaches a secure overload"
    assert sni.seam_findings() == [], f"{label}: the secure overload it reaches does not name the host"


def test_the_seam_files_are_clean():
    assert sni.seam_findings() == []


def test_the_tree_is_clean():
    assert sni.main([]) == 0


# ---- the framework fact the seam rests on --------------------------------------------------------

FRAMEWORK_HEADER = os.path.expanduser(
    "~/.platformio/packages/framework-arduinoespressif32/libraries/NetworkClientSecure/src/NetworkClientSecure.h"
)


@pytest.mark.skipif(not os.path.exists(FRAMEWORK_HEADER), reason="arduino-esp32 is not installed on this host")
def test_only_the_six_argument_address_overload_carries_a_host():
    with open(FRAMEWORK_HEADER, encoding="utf-8") as f:
        decls = re.findall(r"\bint\s+connect\s*\(\s*IPAddress\s+\w+\s*,([^)]*)\)", f.read())
    with_host = [d for d in decls if re.search(r"\bconst\s+char\s*\*\s*host\b", d)]
    assert len(with_host) == 1, decls
    assert len(with_host[0].split(",")) == 5, with_host  # port, host, CA, cert, key after the address
