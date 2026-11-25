#pragma once

// Firmware identity - THE version constant for the whole build. Bump on release
// and tag the repo to match (git tag vX.Y.Z). Surfaced in /api/state (fw/build),
// the STATUS console line, the web UI header, and the settings menu title, so a
// running unit is always attributable to an exact build.
//
// NIMBUS_FW_BUILD is injected by tools/git_version.py (platformio extra_scripts)
// as `git describe --tags --dirty --always`; this fallback covers builds outside
// a git checkout (e.g. exported source).
//
// Engine 2 (the *-e2 envs, -DNIMBUS_ENGINE=2) reports its own line, NIMBUS_FW_VERSION_ENGINE2,
// above every engine-1 release: the updater installs only a strictly newer stable version
// (ota_logic eligibility against NIMBUS_FW_VERSION), so an engine-2 image is never pulled back to
// an engine-1 release, and the field tells the two apart by version. Engine-1 builds from the same
// tree keep the quoted NIMBUS_FW_VERSION line below: it is the one line tools/release_gate/
// tag_shape.py and release.yml read (they match the name followed by a quoted value, and the
// engine-2 name and the engine-2 alias do not match that; test_tag_shape.py pins it).
#define NIMBUS_FW_VERSION_ENGINE2 "v5.9.0"
#if defined(NIMBUS_ENGINE) && NIMBUS_ENGINE == 2
#define NIMBUS_FW_VERSION NIMBUS_FW_VERSION_ENGINE2
#else
#define NIMBUS_FW_VERSION "v5.2.0"
#endif

#ifndef NIMBUS_FW_BUILD
#define NIMBUS_FW_BUILD NIMBUS_FW_VERSION
#endif
