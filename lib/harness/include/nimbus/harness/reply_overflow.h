#pragma once
#include <cstddef>
#include <string>

// reply_overflow - the WHOLE text of a sub-agent reply that did not fit the 16 KB
// ResultEnvelope.reply (L19-36).
//
// On 19 Oct 2026 eight of nine research reports were cut at 16,383 B and presented
// as complete: the envelope was the only copy, so the tail was gone. Every adapter
// writes the reply through wire::ReplyWriter; when it cuts, the writer keeps the
// whole text here, and the job engine spills it to the card as a file and ends the
// envelope with a marker naming that file and its real size.
//
// One record, read right after the poll that wrote it: the poll path is single-task
// by design (AGENTS.md: no on-device concurrency), and every new reply resets it.
namespace agent {
namespace providers {

// The most a reply keeps here (PSRAM on the device); past it the rest is dropped
// and the spill says so.
inline constexpr size_t kReplyOverflowMax = 256 * 1024;

// A new reply is being written: forget the last one.
void replyOverflowReset();
// The envelope just cut: keep `head` (the text that was in it) and what follows.
void replyOverflowStart(const char* head, size_t headLen);
// More reply text after the cut.
void replyOverflowAppend(const char* s, size_t n);

// The whole text of the reply just polled, "" when it fit the envelope; the record
// is cleared. `dropped` (optional) receives the bytes past kReplyOverflowMax.
std::string takeReplyOverflow(size_t* dropped = nullptr);

// Lane QW4 round 3: a run a dispatch may have started but could not hand over or stop
// (an Anthropic session whose starting turn got no answer and whose interrupt was not
// confirmed). The dispatcher notes its job id; the job engine takes it right after the
// failed dispatch and keeps stopping and counting it. One slot: a newer note replaces it.
void noteUnseenRun(const std::string& jobId);
std::string takeUnseenRun();   // "" when none; clears it

}  // namespace providers
}  // namespace agent
