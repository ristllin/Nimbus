"""platformio extra_scripts - inject the git build id as -DNIMBUS_FW_BUILD.

`git describe --tags --dirty --always` gives "v2.0.0", "v2.0.0-3-g1a2b3c4" or
"v2.0.0-3-g1a2b3c4-dirty", so /api/state's `build` pins the exact commit a
running unit was flashed from. include/version.h falls back to NIMBUS_FW_VERSION
when git is unavailable (exported source, CI cache).
"""

import subprocess

Import("env")  # noqa: F821  (platformio construction environment)

try:
    desc = subprocess.check_output(
        ["git", "describe", "--tags", "--dirty", "--always"],
        cwd=env["PROJECT_DIR"],
        text=True,
        stderr=subprocess.DEVNULL,
    ).strip()
except Exception:  # noqa: BLE001 - any git failure -> header fallback
    desc = ""

if desc:
    env.Append(CPPDEFINES=[("NIMBUS_FW_BUILD", env.StringifyMacro(desc))])
