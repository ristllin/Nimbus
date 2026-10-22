#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// pending_events - device-timeline events captured before the clock synced, kept in a
// small file so they survive a reset (crash forensics F3).
//
// A timeline row needs a real timestamp: one stamped from the boot-relative clock would
// land in day 0 and be deleted by the first retention prune once the clock jumps. So an
// event captured before SNTP sync (the boot row of a power-on or brownout boot, which
// loses the RTC time) is held until the first sync. It used to be held in RAM only, so
// every reset before sync lost it: offline boots, and early crash loops, left no boot
// row at all. Now each held event is also appended to /mem/pending_ev.txt (SD) or
// /data/pending_ev.txt (flash), and the first sync on ANY later boot stamps and writes
// them, then truncates the file. Never NVS.
//
// File format (one event per line; a header line only when rows were dropped):
//   # dropped=<n>
//   <bootNo> <uptimeMs> <ev> <text>
// Bounded to kMaxRows rows and kMaxBytes bytes: the oldest rows go first and are
// counted, so the flush can say how many were lost.
namespace nimbus::orch {

inline constexpr size_t kPendingEvMaxRows = 16;
inline constexpr size_t kPendingEvMaxBytes = 4096;
inline constexpr size_t kPendingEvMaxText = 300;

struct PendingEvent {
  uint32_t bootNo = 0;   // the boot that captured it (0 = unknown)
  uint32_t upMs = 0;     // uptime at capture
  std::string ev;        // event class ("boot", "ota", ...)
  std::string text;
};

struct PendingFile {
  std::vector<PendingEvent> rows;   // oldest first
  uint32_t dropped = 0;             // rows dropped to stay within the bounds
};

// One line, '\n' terminated. The event class and text are sanitized (no control
// bytes, no line breaks) and the text is capped at kPendingEvMaxText.
std::string encodePendingEvent(const PendingEvent& e);
// False for a malformed line (it is skipped, never fatal).
bool decodePendingEvent(const std::string& line, PendingEvent& out);

PendingFile parsePendingFile(const std::string& content);
std::string renderPendingFile(const PendingFile& f);

// The device rewrites the file through a ".tmp" sibling and a rename, so a reset can
// leave both copies (the .tmp complete and newer, or torn mid-write) or the .tmp alone.
// Every rewrite appends one row to the previous content, so the copy that accounts for
// more rows (kept plus dropped) is the newer complete one; a tie, or a .tmp that does
// not end a line (torn), keeps the main file. Returns the content to use ("" when
// neither exists).
std::string choosePendingContent(const std::string& mainFile, const std::string& tmpFile);

// Append one event and enforce the bounds; returns the new file content.
std::string appendPendingEvent(const std::string& content, const PendingEvent& e);

// The row text written at sync. This boot's rows keep the existing suffix
// " [pre-sync, stamped at first clock sync]"; rows from an earlier boot say which boot
// and that its clock never synced, or "an earlier boot" when its number was not
// recorded (its boot-counter write failed). Whether a row is this boot's is decided by
// the caller (planPendingFlush), never from the boot number: a number of 0 or one that
// repeats used to label an earlier boot's row as this boot's own (CUM-714).
std::string stampedPendingText(const PendingEvent& e, bool thisBoot);

// The extra row written when rows were dropped ("" when none were).
std::string droppedPendingText(uint32_t dropped);

// The timeline rows the first sync writes, in order: the dropped-count row (if any),
// then every held row oldest first, each with its stamped text. `ownTail` is how many
// rows this boot appended to the file: rows are only ever appended at the end (the
// bounds drop the oldest from the front), so exactly the last `ownTail` rows (at most
// all of them) are this boot's and every other row is an earlier boot's.
struct StampedRow {
  std::string ev;
  std::string text;
};
std::vector<StampedRow> planPendingFlush(const PendingFile& f, size_t ownTail);

}  // namespace nimbus::orch
