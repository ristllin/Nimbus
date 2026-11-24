#pragma once
#include <cstdint>
#include <string>

// nimbus/sys/image_boot.h - has this firmware image booted on this device before? (lane E2-FIX12)
//
// A cable flash ends in a reset the chip reports as a watchdog reset, so the reset reason alone
// called the first boot of every newly written image a crash, and the owner was told "<name>
// restarted after a crash (watchdog)" after every flash. The device now remembers the image that
// last booted: one small state file beside the crash dedupe record (/log/image.seen, the same
// errlog state-file store as /log/crash.seen; no NVS key: that space is scarce and frozen). It
// holds the running image's ELF id (the id the core dump record carries, so "this firmware" means
// the same thing in both places) and the boot number that wrote it, and is written only when the
// image changes (once per flash or update, never per boot).
//
// The crash notice is then due on a crash-type reset unless this is the first boot of a new image
// and no crash record of this firmware describes the reset. What the device cannot know is never
// taken as a new image (ruling 116): no storage, no id of its own, or a marker it could not write
// keeps the notice.
namespace nimbus::sys {

inline constexpr char kImageSeenName[] = "image.seen";   // the state file's name (/log/<name>)
inline constexpr size_t kImageSeenMaxBytes = 48;          // what is read back, and the most a write holds

enum class ImageBoot : uint8_t {
  Unknown = 0,   // no storage, no id, or the new marker was not written: treated as seen before
  First = 1,     // the stored marker names another image (or none) and this image's marker was written
  Again = 2,     // the stored marker names this image
};
const char* imageBootSlug(ImageBoot b);   // "unknown", "first", "again"

// The image id a marker names ("" when the content is empty or not a marker).
std::string imageSeenId(const std::string& content);
// The marker for `elf`, written at boot `bootNo`: "<elf> <bootNo>\n" (the id bounded to 32 chars).
std::string renderImageSeen(const std::string& elf, uint32_t bootNo);
// The marker must be written: this image has an id and the stored marker does not name it.
bool imageSeenWriteDue(const std::string& stored, const std::string& elf);
// This boot, from the stored marker, this image's id and whether the new marker was written.
ImageBoot imageBootOf(const std::string& stored, const std::string& elf, bool wrote);

// The crash notice is due: a crash-type reset (isCrashReset), unless it is the first boot of a new
// image and no crash record of this firmware describes the reset (`ownDump`).
bool crashNoticeDue(int resetReason, ImageBoot image, bool ownDump);

}  // namespace nimbus::sys
