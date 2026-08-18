#pragma once

// Firmware identity - THE version constant for the whole build. Bump on release
// and tag the repo to match (git tag vX.Y.Z). Surfaced in /api/state (fw/build),
// the STATUS console line, the web UI header, and the settings menu title, so a
// running unit is always attributable to an exact build.
//
// NIMBUS_FW_BUILD is injected by tools/git_version.py (platformio extra_scripts)
// as `git describe --tags --dirty --always`; this fallback covers builds outside
// a git checkout (e.g. exported source).
#define NIMBUS_FW_VERSION "v4.2.0"

#ifndef NIMBUS_FW_BUILD
#define NIMBUS_FW_BUILD NIMBUS_FW_VERSION
#endif
