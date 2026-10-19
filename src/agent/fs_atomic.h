#pragma once
#include <string>

// fs_atomic - the one LittleFS whole-file writer that a power loss can never leave
// truncated (moved here from orch_persist.cpp so the usage files share it).
//
// LittleFS FILE_WRITE truncates on open, so the previous content is gone the instant
// the file is opened and only reappears if every byte lands. writeFileAtomic writes a
// sibling ".tmp" and renames it over the target, so a reader sees either the old blob
// or the new one, never a torn half. A short write or a failed rename is reported
// (false) and the previous copy stays.
//
// Factory-erase barrier: while agent::memory::erasing() is set the write is refused,
// so a persist racing the sweep cannot recreate a file after /data was erased.

namespace agent {

// Is the internal file area (LittleFS) mounted? It is not in Notifier mode, while a
// factory reset scrubs it, or if the mount failed at boot. Cheap (no file-system walk);
// safe from any task.
bool flashMounted();

// Refused at once (false, one log line) when the file area is not mounted, so a caller
// on an unmounted store fails fast instead of logging a file-system error per call.
// `tag` names the file in the log line. `logFailure` = false keeps a refusal silent
// (a per-turn writer rate-limits its own log line instead of one per turn).
bool writeFileAtomic(const char* path, const std::string& blob, const char* tag,
                     bool logFailure = true);

}  // namespace agent
