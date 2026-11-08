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
// flight); `hhmm` is the local start time of that turn ("" when the clock was not set);
// `cause` is what went wrong in plain words (nimbus::crash::plainCause), "" to leave it
// out. Plain words, no technical terms: the owner reads it on a phone.
std::string crashNotice(const std::string& deviceName, const TurnJournal* j,
                        const std::string& hhmm, const std::string& cause = std::string());

// What the boot after a poll-guard restart knows about the Telegram side of the request
// it cut short (the device fills it from telegram::enabled() and guardStatus()).
struct StallReplay {
  bool telegramPolling = false;   // the Telegram poll runs this boot (a token, the task up)
  uint8_t poisonTries = 0;        // crash resets counted against the Telegram work in flight
};

// A Telegram chat id: digits with an optional leading minus (a group). The device's own
// channels ("web", "voice", "serial") and "" (a journal rebuilt from the breadcrumb, which
// keeps no chat) are not.
bool isTelegramChatId(const std::string& chat);

// Whether the request a guard restart cut short runs again on its own. Only a Telegram
// chat's page is served again: its offset is stored, and the message recorded as
// answered, only after its turn returned (nimbus/net/tg_dup_guard.h, at-least-once). That
// needs the Telegram poll running this boot, and the poison guard not to skip the work
// (at kPoisonMaxTries crash resets it is acked unrun and the owner told to send it
// again). A web chat turn (the cloud app reaches the same page), an on-device voice turn
// and a console turn wait in the RAM inbound queue, which the restart empties: nothing
// runs them again. Neither does anything when the journal names no chat.
bool lostTurnRunsAgain(const TurnJournal& j, const StallReplay& r);

// The owner notice after the poll task's liveness guard restarted the device
// (nimbus/sys/poll_guard.h): it had stopped answering, so it restarted itself. It says
// the request is tried again only when lostTurnRunsAgain() holds (and, for a message, to
// check a logged row was not saved twice); otherwise that the reply was lost and to send
// the request again. With no turn in flight, it says messages sent here meanwhile are
// being answered only while the Telegram poll runs and nothing was skipped. Same
// arguments as crashNotice, plus `r`.
std::string stallNotice(const std::string& deviceName, const TurnJournal* j, const std::string& hhmm,
                        const StallReplay& r);

}  // namespace nimbus::orch
