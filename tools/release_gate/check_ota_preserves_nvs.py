#!/usr/bin/env python3
"""Release gate: prove the OTA update flow never writes a USER NVS key.

CUM-237 (settings-lost-across-OTA class): an owner's provisioned state - Wi-Fi
credentials, the provider API key, the measured touch calibration (tchCal), the
screen model, the LED theme, battery config - all live as NVS keys in the "solide"
namespace and MUST survive a firmware update. The OTA path is allowed to write
only its OWN bookkeeping (the pend/boots/prev rollback guard, the last-result
record, the typed-device slug, the crash-injection drill flag); if it ever wrote a
user key, an update could silently erase a fielded device's setup - a
lose-every-customer failure.

This is the host-provable half of the gate: a source guard over the updater seam
(src/sys/ota_update.cpp). It extracts every NVS WRITE the OTA glue performs
(nvs_set_*, nvs_erase_key) and asserts each target key resolves to an
OTA-bookkeeping key, never a user-data key. It FAILS the build if someone adds a
write to a user key inside the OTA flow. The full end-to-end proof (provision ->
OTA span -> power-cycle -> read NVS back) is the bench leg documented in
tests/release_gate/MANIFEST.md; this check is its fast, always-on backstop.

Usage:
    python3 tools/release_gate/check_ota_preserves_nvs.py
    python3 tools/release_gate/check_ota_preserves_nvs.py --root <repo-root>
Exit 0 = every OTA NVS write targets a bookkeeping key; exit 1 = a user key is
written (or the sources cannot be read).
"""

from __future__ import annotations

import argparse
import os
import re
import sys

# The OTA glue and the canonical NVS key registry.
OTA_SRC_REL = os.path.join("src", "sys", "ota_update.cpp")
KEYS_HDR_REL = os.path.join("src", "agent", "agent_config.h")

# NVS-write call sites in the OTA glue. Group 1 = the KEY argument token (the 2nd
# argument, after the handle). nvs_erase_key takes (handle, key); nvs_set_* take
# (handle, key, value).
WRITE_RE = re.compile(
    r"\bnvs_(?:set_(?:i8|u8|i16|u16|i32|u32|i64|u64|str|blob)|erase_key)\s*"
    r"\(\s*[A-Za-z_][A-Za-z0-9_]*\s*,\s*([^,\)]+?)\s*[,\)]"
)

# `#define AKEY_FOO  "literal"` in the key registry.
DEFINE_RE = re.compile(r'#define\s+(AKEY_[A-Za-z0-9_]+)\s+"([^"]*)"')
# `static const char* kName = "literal";` local key vars inside the OTA glue.
LOCAL_STR_RE = re.compile(r'\bconst\s+char\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*"([^"]*)"')


def load_defines(hdr_text: str) -> "dict[str, str]":
    """Map every AKEY_* macro to its string literal."""
    return {m.group(1): m.group(2) for m in DEFINE_RE.finditer(hdr_text)}


def load_locals(src_text: str) -> "dict[str, str]":
    """Map local `const char*` key vars in the OTA glue to their literal."""
    return {m.group(1): m.group(2) for m in LOCAL_STR_RE.finditer(src_text)}


def ota_bookkeeping_literals(defines: "dict[str, str]") -> "set[str]":
    """The keys the OTA flow is ALLOWED to write: every AKEY_OTA_* literal, plus
    the crash-injection drill flag the resilience gate toggles. Deriving this from
    the AKEY_OTA_ prefix means a legitimately new OTA bookkeeping key is allowed
    automatically, while any user key stays denied by construction."""
    allowed = {lit for name, lit in defines.items() if name.startswith("AKEY_OTA_")}
    allowed.add("otaSimCrash")  # kSimCrashKey: the TFTBREAK/crash-loop drill flag
    return allowed


def resolve_key(token: str, defines: "dict[str, str]", locals_: "dict[str, str]") -> "str | None":
    """Resolve a write's key argument to its NVS string literal. Handles an AKEY_*
    macro, a local const char* var, or a bare string literal. Returns None if it
    cannot be resolved to a literal (reported as a failure - an unresolvable key in
    the OTA flow must be looked at by a human, not waved through)."""
    token = token.strip()
    lit = re.fullmatch(r'"([^"]*)"', token)
    if lit:
        return lit.group(1)
    if token in defines:
        return defines[token]
    if token in locals_:
        return locals_[token]
    return None


def audit(src_text: str, hdr_text: str) -> "tuple[bool, list[str]]":
    """(ok, messages). ok=False if any OTA NVS write targets a non-bookkeeping key
    or an unresolvable key token."""
    defines = load_defines(hdr_text)
    locals_ = load_locals(src_text)
    allowed = ota_bookkeeping_literals(defines)
    # Every AKEY_* literal that is NOT an OTA bookkeeping key is user/system data
    # the OTA flow must never write.
    user_literals = {lit for name, lit in defines.items()} - allowed

    msgs: "list[str]" = []
    ok = True
    writes = WRITE_RE.findall(src_text)
    if not writes:
        return False, [
            "no NVS writes found in the OTA glue - the guard's assumption "
            "about src/sys/ota_update.cpp is stale; re-point it"
        ]

    seen: "set[str]" = set()
    for token in writes:
        lit = resolve_key(token, defines, locals_)
        if lit is None:
            ok = False
            msgs.append(f"OTA writes an UNRESOLVABLE key `{token}` (cannot prove it is not a user key)")
            continue
        if lit in allowed:
            seen.add(lit)
            continue
        ok = False
        tag = " (a USER data key)" if lit in user_literals else ""
        msgs.append(
            f"OTA writes NVS key \"{lit}\"{tag} - the update flow must only "
            f"write its own bookkeeping (AKEY_OTA_* / otaSimCrash)"
        )
    if ok:
        msgs.append("OTA writes only bookkeeping keys: " + ", ".join(f'"{k}"' for k in sorted(seen)))
    return ok, msgs


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Release gate: OTA preserves user NVS keys (CUM-237)")
    default_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--root", default=default_root, help="repo root")
    args = ap.parse_args(argv)

    try:
        with open(os.path.join(args.root, OTA_SRC_REL), "r", encoding="utf-8") as fh:
            src_text = fh.read()
        with open(os.path.join(args.root, KEYS_HDR_REL), "r", encoding="utf-8") as fh:
            hdr_text = fh.read()
    except OSError as exc:
        print(f"[gate:ota-nvs] FAIL: cannot read sources: {exc}")
        return 1

    ok, msgs = audit(src_text, hdr_text)
    print(f"[gate:ota-nvs] {'PASS' if ok else 'FAIL'}: {msgs[0]}")
    for extra in msgs[1:]:
        print(f"[gate:ota-nvs]   - {extra}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
