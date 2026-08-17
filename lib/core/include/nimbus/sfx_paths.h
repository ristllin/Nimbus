#pragma once
#include <cstddef>

// sfx_paths - portable path hygiene for the SFX asset system, shared by the
// sync worker (src/sfx/sfx_sync.cpp: manifest `path` vetting) and the player
// (src/sfx/sound_fx.cpp: clip filename parsing). No Arduino/FS deps ->
// host-tested via test/test_sfx_paths.
namespace nimbus::sfx {

// The sync manifest is fetched over setInsecure() TLS (owner-accepted,
// docs/security.md), so every `path` field is hostile input. A path is honored
// only if it is a clean `sd/<...>` relative path - no ".." (escape /sfx/ into
// /mem, the memory system of record), only filesystem-safe chars (a space/CR/
// LF would also corrupt the outbound GET request line), and NEVER `sd/custom/`:
// /sfx/custom/ is the OWNER's local clip folder, and a synced manifest must
// not be able to write or overwrite anything there.
bool safeRepoPath(const char* p);

// Parse a clip basename "<slug>-<n>.wav" (no directory components) into its
// slug and variant number. `<n>` is the run of digits after the LAST dash; the
// slug is everything before it. (A slug MAY contain dashes - "a-b-1.wav" parses
// to slug "a-b", n=1 - though the pack's own slugs never do.) Returns false -
// filling nothing - on any malformation, including a slug that would not fit
// slugCap (truncation is a failure, not a partial result).
bool parseClipFilename(const char* name, char* slugOut, size_t slugCap, unsigned* nOut);

}  // namespace nimbus::sfx
