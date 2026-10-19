#pragma once
#include <cstdint>
#include <string>

// turn_journal - the in-flight turn record a crash leaves behind (crash UX, row 42).
//
// On 18 Oct a panic in round 2 of a photo turn lost the reply, and the owner was never
// told: the next boot simply answered the next message. Now the turn task writes a tiny
// journal (/crash/turn.json on LittleFS, never NVS) at turn start, tracks each round in
// the RTC breadcrumb, and clears the journal at turn end. A boot whose reset reason is a crash reads it and
// sends the owner one plain-words notice through the reply queue, naming the request
// that was lost. A clean turn end, or a non-crash boot, just deletes it.
//
// Portable: the JSON codec, the request kind and the notice copy are host-tested
// (test_crash_forensics); the file I/O lives in src/sys/crash_report.cpp.
namespace nimbus::orch {

struct TurnJournal {
  std::string chat;      // routing chat id ("web", "voice", a Telegram chat id)
  std::string kind;      // turnKindFor() below
  uint64_t at = 0;       // wall clock at turn start (0 = clock not set)
  uint32_t upMs = 0;     // uptime at turn start
  int round = 0;         // the last head-loop round that started
};

// Mirrors agent::TurnSource (lib/harness hooks.h) by value; lib/core cannot include it.
inline constexpr int kTurnSourceOwner = 0;
inline constexpr int kTurnSourceSynthesis = 1;
inline constexpr int kTurnSourceLoop = 2;
inline constexpr int kTurnSourceSerial = 3;

// The request kind from the turn source, its chat and the start of the user text:
// "photo", "file", "voice", "message", "routine", "background" or "console".
std::string turnKindFor(int source, const std::string& chat, const std::string& textHead);

// Compact JSON, at most 200 B.
std::string renderTurnJournal(const TurnJournal& j);
// False for anything that is not a journal this module wrote.
bool parseTurnJournal(const std::string& json, TurnJournal& out);

// The journal file is written once, at turn start, and only while internal memory is
// above the forensic floors (nimbus/sys/log_window.h): every file open makes newlib
// allocate a mutex from internal RAM and IDF aborts when that fails, so a journal write
// must never be the allocation that turns a low-memory turn into a crash. Round
// progress rides the RTC breadcrumb instead (no file I/O): its mark is "<kind>/r<N>".
bool journalWriteAllowed(uint32_t intFree, uint32_t intLargest);
std::string turnMark(const std::string& kind, int round);
// The kind from a breadcrumb mark ("photo/r2" -> "photo"); "" when it names no kind.
// Lets the crash notice name the request when the journal write was skipped.
std::string kindFromTurnMark(const std::string& mark);

// The owner notice after a crash boot. `j` is the journal (nullptr when no turn was in
// flight); `hhmm` is the local start time of that turn ("" when the clock was not set).
// Plain words, no technical terms: the owner reads it on a phone.
std::string crashNotice(const std::string& deviceName, const TurnJournal* j,
                        const std::string& hhmm);

}  // namespace nimbus::orch
