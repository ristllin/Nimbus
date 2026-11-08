#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

// poll_guard - the liveness guard of the Telegram poll task (lane TGSTALL).
//
// The poll task (tg_poll) is the device's only turn task: it polls Telegram, runs every
// turn and its tool loop, the staged update check and the outbound queues. On 4 Nov 2026
// Lumi's poll task entered a turn at 20:26:56Z and never came back: 28 hours with no
// reply, no poll and no log line, until a restart. The web page answered, the cloud flag
// read online, the loop task ran its daily retention and the stuck-turn reaper freed the
// ring after 12 minutes; nothing noticed that the task itself had stopped. Two more
// stalls of the same shape followed on 6 and 7 Nov. During each one the lower-priority
// tasks on the same core (the sound task, the cloud relay) went silent too while heap
// and PSRAM stayed frozen to the byte: the task was spinning, not waiting
// (coverage/TGSTALL-2026-11.md has the evidence).
//
// The rule: the poll task beats wherever it shows progress (its loop top, every socket
// wait, every TLS connect and close, every turn, round and tool). The loop task checks
// the beat about once a second. A task silent for kPollSilentLimitMs is stuck, whatever
// it is stuck in: a CPU spin, a lock that is never given back, a socket read with no
// deadline, a queue that never drains, staged work that never returns. Nothing the task
// legitimately does goes that long without a beat (one provider exchange is capped at
// 120 s, one long-poll at 35 s).
//
// The recovery is a device restart, never a task restart: a stuck task holds what it
// held when it stopped (the TLS work slot, a mutex, the crypto engine's lock, its sockets
// and buffers), and a deleted FreeRTOS task gives none of them back. The restart is a
// deliberate panic so the poison guard (tg_dup_guard.h) counts a try against the
// Telegram work in flight (a message that stalls the device twice is skipped and the
// owner told) and the core dump keeps every task's stack. The panic reason carries the
// guard's evidence (silence, last step, on-CPU share, the stuck task's PC and backtrace)
// and comes back in the next boot's crash lines. The owner is told once per streak.
//
// A streak of guard restarts (no healthy half hour between them) is bounded: after
// kPollStallRestartsMax in a row the guard waits kPollSilentLongMs before the next one,
// so a deterministic stall can never become a tight restart loop, and a stuck task is
// still never left stuck for good.
namespace nimbus::sys {

inline constexpr uint32_t kPollSilentLimitMs = 5u * 60u * 1000u;    // stuck: restart
inline constexpr uint32_t kPollSilentLongMs = 60u * 60u * 1000u;    // stuck again and again
inline constexpr uint32_t kPollSampleAfterMs = 10u * 1000u;         // silence the guard samples
inline constexpr uint32_t kPollHealthyMs = 30u * 60u * 1000u;       // a streak ends
inline constexpr uint8_t kPollStallRestartsMax = 3;
// Every guard panic reason starts with this; it contains "watchdog", so the crash notice
// machinery reads it as "part of it stopped responding" (crash_summary.h crashKind).
inline constexpr char kPollStallTag[] = "poll watchdog:";
inline constexpr size_t kPollStallReasonMax = 200;   // crash_summary.h kMaxWhyBytes
inline constexpr size_t kPollBacktraceMax = 6;

// What the loop task sees of the poll task (FreeRTOS eTaskState, mirrored).
enum class RunState : uint8_t { Running, Ready, Blocked, Suspended, Gone };

// One silent episode, as the guard measured it.
struct PollStall {
  uint32_t silentMs = 0;
  const char* step = "";   // the last step that beat (a string literal)
  uint16_t samples = 0;    // checks made while the silence was past kPollSampleAfterMs
  uint16_t onCpu = 0;      // of them, the task was running or ready to run
  bool gone = false;       // the task no longer exists although the guard was armed
  // Mostly on the CPU: a spin. Otherwise it waits on something that never comes.
  bool spinning() const { return samples > 0 && onCpu * 4u >= samples * 3u; }
};

// The guard. beat() runs on the poll task; arm(), disarm() and check() on the loop task
// (arm() also from the task's own start). The beat fields are atomics: the loop task
// reads them while the poll task writes them.
class PollLiveness {
 public:
  void arm(uint32_t nowMs);
  void disarm();
  bool armed() const { return armed_.load(std::memory_order_acquire); }
  void beat(uint32_t nowMs, const char* step);
  uint32_t silentMs(uint32_t nowMs) const { return nowMs - lastMs_.load(std::memory_order_relaxed); }
  const char* lastStep() const { return step_.load(std::memory_order_relaxed); }
  // One check: true exactly once per silent episode, when the silence reaches
  // `limitMs`; `out` then holds the episode. A beat starts a new episode.
  bool check(uint32_t nowMs, RunState state, uint32_t limitMs, PollStall& out);

 private:
  std::atomic<uint32_t> lastMs_{0};
  std::atomic<const char*> step_{""};
  std::atomic<bool> armed_{false};
  // Loop task only.
  uint32_t episodeMs_ = 0;   // the beat the current episode is measured from
  uint16_t samples_ = 0;
  uint16_t onCpu_ = 0;
  bool fired_ = false;
};

// The streak record: RTC no-init memory on the device, so it survives the restart the
// guard asks for (and a panic, a watchdog, a software restart) but not a power cycle.
// Magic, the firmware build id and a CRC-32 reject what a power-on leaves.
struct PollStallStore {
  uint32_t magic;
  uint32_t build;
  uint8_t streak;    // guard restarts in a row, no healthy half hour between them
  uint8_t pending;   // the guard restarted the device and the next boot has not read it
  uint8_t pad[2];
  uint32_t silentS;  // the last stall's silence, seconds
  char step[24];     // the last stall's step
  uint32_t crc;
};
inline constexpr uint32_t kPollStallMagic = 0x50475244u;   // "PGRD" + layout

class PollStallRecord {
 public:
  explicit PollStallRecord(PollStallStore& st) : st_(st) {}
  // Once at boot. Clears a record that is not intact for `build`. True when this boot
  // follows a guard restart (the record's pending flag, consumed here).
  bool boot(uint32_t build);
  // The guard fired: counts the stall into the streak and marks the restart pending.
  void noteStall(const PollStall& s);
  // The poll task has been healthy for kPollHealthyMs: the streak ends.
  void noteHealthy();
  uint8_t streak() const { return st_.streak; }
  // The silence the guard tolerates before the next restart.
  uint32_t limitMs() const {
    return st_.streak >= kPollStallRestartsMax ? kPollSilentLongMs : kPollSilentLimitMs;
  }
  bool intact(uint32_t build) const;

 private:
  void seal();
  void clear(uint32_t build);
  PollStallStore& st_;
  uint32_t build_ = 0;
};

// The panic reason: "poll watchdog: tg_poll silent 301 s after provider read, on the
// CPU 29/30 (a spin), pc 0x4200a1b2 bt 0x4200a1b2 0x42011c3d". Printable ASCII, at most
// kPollStallReasonMax bytes, always starts with kPollStallTag.
std::string pollStallReason(const PollStall& s, uint32_t pc, const uint32_t* bt, size_t btLen);
bool isPollStallReason(const std::string& why);
// The durable log line written just before the restart (same facts, plain words).
std::string pollStallLogLine(const PollStall& s, uint8_t streak, uint32_t limitMs, bool restarting);

}  // namespace nimbus::sys
