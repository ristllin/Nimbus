#pragma once
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

// ---- /clear's generation (lane E2-FIX2B, finding P7) ------------------------------------------
// A chat's conversation is the transcript of its generation: 0 is the chat id itself, n is
// "<chat>_c<n>". The number is kept in /mem/tx/<chat>.gen as one decimal line, written whole to
// <chat>.gen.tmp and renamed, so a cut leaves either number whole. Reading it has three answers: a
// value; Absent, only when /mem/tx listed whole and holds neither name (a chat never cleared:
// generation 0); Unknown otherwise (a refused read, a file there that does not read as a number),
// which the device treats as an error for that turn, never as generation 0.
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
  if (f.readAt(tmp, 0, buf, sizeof buf, got) && parseGeneration(buf, got, true, g)) return GenRead::Value;
  got = 0;
  if (f.readAt(path, 0, buf, sizeof buf, got) && parseGeneration(buf, got, false, g)) return GenRead::Value;
  g = 0;
  std::vector<std::string> names;
  if (!f.list(nimbus::orch::tx::kTxRoot, names)) return GenRead::Unknown;
  const std::string base = chatId + ".gen", baseTmp = base + ".tmp";
  for (const std::string& n : names)
    if (n == base || n == baseTmp) return GenRead::Unknown;   // there, and not a number
  return GenRead::Absent;
}

inline bool writeGeneration(nimbus::orch::tx::TxFiles& f, const std::string& chatId, uint32_t g) {
  const std::string path = generationPath(chatId), tmp = path + ".tmp";
  return f.writeWhole(tmp, std::to_string(g) + "\n") && f.rename(tmp, path);
}

}  // namespace plain
}  // namespace agent
