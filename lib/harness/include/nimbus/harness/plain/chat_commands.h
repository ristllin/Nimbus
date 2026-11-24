#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/command.h"
#include "nimbus/orch/tx/log.h"   // TxFiles, kTxRoot - /clear's generation file

// plain/chat_commands.h - where an owner's slash command goes on an engine-2 device (lane E2-CHAT).
// No command reaches the model. Each verb of nimbus::orch::kCommandVerbs has one route:
//   Shared      the handler engine 1 calls (it does not depend on the engine): /help, /start,
//               /update, /ota, /play, /fetch, /loops, /loop, /usage, /model, and /budget with
//               arguments (a limit change and its confirm); /remind (lane E2-SCHED: a reminder
//               made through the schedule tool's own code, as the owner's);
//   Clear       a new conversation on engine 2 (the stored one is kept on the card);
//   Compact     a compaction of this chat now;
//   BudgetView  a bare /budget: engine 2's budget view (the system prompt in tokens), then the
//               spending text a bare /budget gives on engine 1;
//   NotYet      engine 1 only for now, answered with one plain line: /skill (the skill store is
//               engine 1's), /mcp (engine 2 reaches no
//               tool server), /prefs (engine 2 reads notes, not standing rules);
//   Unknown     a verb the table does not know: the same "try /help" answer as engine 1.
// test_plain_chat_commands walks every verb of the table: a verb added there with no route here
// fails it.
namespace agent {
namespace plain {

enum class ChatCommandRoute : uint8_t { Shared = 0, Clear = 1, Compact = 2, BudgetView = 3, NotYet = 4, Unknown = 5 };

inline const char* chatCommandRouteSlug(ChatCommandRoute r) {
  switch (r) {
    case ChatCommandRoute::Shared: return "shared";
    case ChatCommandRoute::Clear: return "clear";
    case ChatCommandRoute::Compact: return "compact";
    case ChatCommandRoute::BudgetView: return "budget-view";
    case ChatCommandRoute::NotYet: return "not-yet";
    case ChatCommandRoute::Unknown: return "unknown";
  }
  return "unknown";
}

// The route of a parsed command (c.isCommand is the caller's check).
inline ChatCommandRoute chatCommandRoute(const nimbus::orch::Command& c) {
  const std::string& v = c.verb;
  if (!nimbus::orch::findCommandVerb(v)) return ChatCommandRoute::Unknown;
  if (v == "clear") return ChatCommandRoute::Clear;
  if (v == "compact") return ChatCommandRoute::Compact;
  if (v == "budget") return c.args.empty() ? ChatCommandRoute::BudgetView : ChatCommandRoute::Shared;
  if (v == "skill" || v == "mcp" || v == "prefs") return ChatCommandRoute::NotYet;
  return ChatCommandRoute::Shared;
}

// ---- lines engine 1's own would not make true on engine 2 (lane E2-FIX14) ---------------------------
// Device copy for the owner, never model text. Engine 2 binds no connectors and no sub-agents, so the
// switch "/loop grants on" is stored (it applies once they exist) and the reply says what routines can
// do now; "off" is engine 1's line, true on both.
inline const char* routineGrantsLine(bool on) {
  return on ? "Saved. Routines on the new engine have no connectors or sub-agents to use yet, so they still only "
              "read, set their follow-up and reply."
            : "Routines now only read, set their follow-up and reply.";
}
// The web settings' "Erase memory": engine 2 keeps no separate assistant memory (engine 1's running
// notes), so nothing is erased; the owner's notes and files are his and stay.
inline constexpr char kNoAssistantMemoryLine[] =
    "The new engine keeps no separate assistant memory, so nothing was erased. Your notes and files are kept.";

// ---- /clear's generation (lane E2-FIX2B, finding P7) ------------------------------------------
// A chat's conversation is the transcript of its generation: 0 is the chat id itself, n is
// "<chat>_c<n>". The number is kept in /mem/tx/<chat>.gen as one decimal line, written whole to
// <chat>.gen.tmp and renamed, so a cut leaves either number whole. Reading it has three answers: a
// value; Absent, only when /mem/tx listed whole and holds neither name (a chat never cleared:
// generation 0); Unknown otherwise (a refused read, a main file there that does not read as a
// number), which the device treats as an error for that turn, never as generation 0. Lane E2-FIX3
// (finding R2): a temp that does not read as a whole line is the leftover of a write that never
// committed (a cut inside it): it is removed (or, if the card refuses, ignored) and the main file,
// or its absence, decides. Only an unreadable main file cannot tell, and /clear still gets out of it
// (freeGeneration).
enum class GenRead : uint8_t { Value = 0, Absent = 1, Unknown = 2 };

inline std::string generationPath(const std::string& chatId) {
  return std::string(nimbus::orch::tx::kTxRoot) + "/" + chatId + ".gen";
}

// Digits, then the line end the writer puts there (`needEnd`), or digits alone (an older build's
// in-place write).
inline bool parseGeneration(const char* buf, size_t n, bool needEnd, uint32_t& g) {
  size_t digits = 0;
  uint64_t v = 0;
  while (digits < n && buf[digits] >= '0' && buf[digits] <= '9' && digits < 10) v = v * 10 + (uint64_t)(buf[digits++] - '0');
  if (!digits || v > UINT32_MAX) return false;
  const bool ended = digits + 1 == n && buf[digits] == '\n';
  if (!(ended || (!needEnd && digits == n))) return false;
  g = (uint32_t)v;
  return true;
}

inline GenRead readGeneration(nimbus::orch::tx::TxFiles& f, const std::string& chatId, uint32_t& g) {
  g = 0;
  const std::string path = generationPath(chatId), tmp = path + ".tmp";
  char buf[16];
  size_t got = 0;
  // The temp holds a whole line only when a cut stopped its rename: then it is the newer number.
  const bool tmpRead = f.readAt(tmp, 0, buf, sizeof buf, got);
  if (tmpRead && parseGeneration(buf, got, true, g)) return GenRead::Value;
  got = 0;
  if (f.readAt(path, 0, buf, sizeof buf, got) && parseGeneration(buf, got, false, g)) return GenRead::Value;
  g = 0;
  std::vector<std::string> names;
  if (!f.list(nimbus::orch::tx::kTxRoot, names)) return GenRead::Unknown;
  const std::string base = chatId + ".gen", baseTmp = base + ".tmp";
  bool torn = false;
  for (const std::string& n : names) {
    if (n == base) return GenRead::Unknown;   // there, and not a number
    if (n == baseTmp && !tmpRead) return GenRead::Unknown;   // there, and not read: it may be whole
    torn = torn || n == baseTmp;
  }
  if (torn) (void)f.remove(tmp);   // read, not whole, never renamed: its number was never in force
  return GenRead::Absent;
}

// The number /clear moves a chat to (lane E2-FIX3, findings R2 and R4): the first n above `from`
// (the number read; 0 when it could not be read) whose transcript "<chat>_c<n>" holds no file, so
// no conversation is ever reused, also after a number that was lost or never stored. False when a
// folder cannot be told (the caller refuses the /clear and says so).
inline constexpr uint32_t kGenerationProbe = 256;
inline bool freeGeneration(nimbus::orch::tx::TxFiles& f, const std::string& chatId, uint32_t from, uint32_t& next) {
  const uint64_t last = std::min<uint64_t>(UINT32_MAX, (uint64_t)from + kGenerationProbe);
  for (uint64_t n = (uint64_t)from + 1; n <= last; ++n) {
    std::vector<std::string> names;
    if (!f.list(std::string(nimbus::orch::tx::kTxRoot) + "/" + chatId + "_c" + std::to_string(n), names)) return false;
    if (names.empty()) {
      next = (uint32_t)n;
      return true;
    }
  }
  return false;
}

// Lane E2-FIX5 (finding L1): a whole temp is the newest number (a rename that was refused or cut,
// perhaps after the main file was already removed), so it is renamed into place before the temp is
// written again: a cut inside the new write then leaves the main file, never no number at all.
inline bool writeGeneration(nimbus::orch::tx::TxFiles& f, const std::string& chatId, uint32_t g) {
  const std::string path = generationPath(chatId), tmp = path + ".tmp";
  char buf[16];
  size_t got = 0;
  uint32_t was = 0;
  if (f.readAt(tmp, 0, buf, sizeof buf, got) && parseGeneration(buf, got, true, was) && !f.rename(tmp, path))
    return false;
  return f.writeWhole(tmp, std::to_string(g) + "\n") && f.rename(tmp, path);
}

// /clear's write (lane E2-FIX3, finding R4): true only when the card holds `g` now. A refused
// rename can leave the whole temp, which reads as `g`: that counts. Anything else did not happen.
inline bool storeGeneration(nimbus::orch::tx::TxFiles& f, const std::string& chatId, uint32_t g) {
  if (writeGeneration(f, chatId, g)) return true;
  uint32_t back = 0;
  return readGeneration(f, chatId, back) == GenRead::Value && back == g;
}

}  // namespace plain
}  // namespace agent
