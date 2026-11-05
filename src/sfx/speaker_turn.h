#pragma once
#include <cstdint>

#include <solide/boards/active_board.h>

#include "nimbus/speaker_memory.h"

// speaker_turn - whose turn it is on the speaker, at the players' own level.
//
// The speaker's memory check (nimbus/speaker_memory.h) reads what is free now, so it only means
// something when no other sound holds the driver's buffers. The sound task and the music task
// both play: each takes its turn BEFORE it reads the memory, and keeps it until the clip closed.
// A spoken reply waits a bounded time for the track it stopped to let go. A track waits for the
// clip in front of it. A cue does not wait: it is dropped while a track plays, since it would hold
// up the sound task, and any reply behind it, for the rest of the track.
namespace sfx {

class SpeakerTurn {
 public:
  SpeakerTurn() = default;
  SpeakerTurn(const SpeakerTurn&) = delete;
  SpeakerTurn& operator=(const SpeakerTurn&) = delete;
  ~SpeakerTurn();
  // Waits up to `waitMs` (0: only if free). True once this task has the turn.
  bool take(uint32_t waitMs);

 private:
  bool held_ = false;
};

// What this board's speaker takes to open (compile-time: the board's audio hardware).
inline constexpr nimbus::sfx::SpeakerProfile speakerProfileHere() {
  return nimbus::sfx::speakerProfile(solide::activeBoard().audioKind == solide::AudioKind::Es8311Codec);
}

}  // namespace sfx
