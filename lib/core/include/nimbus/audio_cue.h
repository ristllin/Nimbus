#pragma once
#include <cstdint>
#include <string>

// nimbus::audio_cue - the silent-play honesty cue (CUM-296), portable + host-tested.
//
// The music player scales every sample by the master volume (sfxVolume()/100). On the
// device's small speaker, sustained music at very low volume is effectively inaudible -
// yet playback reports "playing" and completes normally, so a user who pressed Play and
// heard nothing has no idea it is a volume setting. (A beep's sharp transient still
// punches through at the same level, which is why the owner heard the built-in cues but
// not a 3-minute track: found live 2026-09-02, sfxVol=5.)
//
// This is not a decode bug and not a forced change: raising the volume plays audibly.
// The gap is UX honesty. So when playback starts below an audible floor, the player
// SAYS so instead of playing silently. Kept pure and header-only (the duty.h / saver.h
// precedent) so the floor and the exact copy are host-testable with no speaker.

namespace nimbus {

// Master volume (percent) below which sustained music is effectively silent on the
// small speaker. The owner's 5% produced no audible track; the ticket suggests a floor
// around 10-15%. A tap-through transient (a beep) is deliberately NOT gated by this - it
// stays audible - so this governs the music/play cue only.
constexpr uint8_t kAudibleFloorPct = 15;

// True when starting playback at `volPct` should surface the low-volume cue.
constexpr bool volumeInaudible(uint8_t volPct) { return volPct < kAudibleFloorPct; }

// The one-line honest cue for a playback start at `volPct`, or an empty string when the
// volume is audible. ASCII only, no em dash and no space-hyphen-space (device + copy
// rules), and it reads well aloud over Telegram. It names the current level (so the user
// sees it is a setting, not a fault) and the one next step (turn it up in Sound).
inline std::string lowVolumeCue(uint8_t volPct) {
  if (!volumeInaudible(volPct)) return std::string();
  return "Volume is at " + std::to_string(volPct) +
         "%, so playback may be silent. Turn it up in Sound.";
}

}  // namespace nimbus
