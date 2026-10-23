#pragma once
// Deterministic slash-command parsing for the owner/manual control path.
// Portable + host-tested (no Arduino). The device dispatches on the parsed verb
// with EXACT matching so a message like "/updates" or "/update now" can never
// trigger the "/update" OTA install by a loose prefix match, and Telegram's
// group-command form "/loops@MyBot" is recognized the same as "/loops".
//
// Rules:
//   - the message is trimmed first, so " /update" is still a command;
//   - the verb is the token after '/', lowercased, with any "@botname" stripped;
//   - args is the trimmed remainder (empty for a bare command);
//   - isCommand is true only when the trimmed text begins with '/'.

#include <cctype>
#include <cstddef>
#include <string>

namespace nimbus {
namespace orch {

struct Command {
  std::string verb;             // lowercased, no leading '/', @botname removed
  std::string args;             // trimmed remainder after the verb ("" if none)
  bool        isCommand = false;  // trimmed text begins with '/'
};

inline Command parseCommand(const std::string& raw) {
  Command c;
  auto isws = [](char ch) { return std::isspace((unsigned char)ch) != 0; };
  size_t a = 0, b = raw.size();
  while (a < b && isws(raw[a])) a++;
  while (b > a && isws(raw[b - 1])) b--;
  if (b - a < 2 || raw[a] != '/') return c;   // "", "/", or not a slash command
  c.isCommand = true;
  // verb token: from just after '/' to the first whitespace.
  size_t vs = a + 1, ve = vs;
  while (ve < b && !isws(raw[ve])) ve++;
  std::string verb = raw.substr(vs, ve - vs);
  size_t at = verb.find('@');                 // strip Telegram's "@botname" suffix
  if (at != std::string::npos) verb.resize(at);
  for (char& ch : verb) ch = (char)std::tolower((unsigned char)ch);
  c.verb = verb;
  // args: trimmed remainder after the verb token.
  size_t as = ve;
  while (as < b && isws(raw[as])) as++;
  size_t ae = b;
  while (ae > as && isws(raw[ae - 1])) ae--;
  c.args = raw.substr(as, ae - as);
  return c;
}

// ---- the verb table ------------------------------------------------------------
// Every verb the device answers deterministically (agent::orchestrator::handleMessage
// dispatches each one, before any model turn), and whether only the owner may run
// it. Lifted out of an inline boolean so one list drives the owner gate and the tests
// that cover every command: the batch rule below (test_orch_msg_batch) and the
// dispatcher and docs guards (test_orch_command_parse). A verb added here without a
// handler, or a handler without an entry here, fails those guards.
struct CommandVerb {
  const char* verb;       // lowercase, no leading '/'
  bool        ownerOnly;  // an allow-listed member may converse but not run it
};

inline constexpr CommandVerb kCommandVerbs[] = {
    {"help", false},   {"start", false},  {"update", true}, {"loops", true},
    {"loop", true},    {"compact", true}, {"skill", true},  {"fetch", true},
    {"remind", true},  {"clear", true},   {"mcp", true},    {"play", true},
    {"prefs", true},
};
inline constexpr size_t kCommandVerbCount = sizeof(kCommandVerbs) / sizeof(kCommandVerbs[0]);

// The table entry for a parsed verb, or nullptr for a verb the device does not know
// (it still answers those deterministically: "try /help").
inline const CommandVerb* findCommandVerb(const std::string& verb) {
  for (const CommandVerb& v : kCommandVerbs)
    if (verb == v.verb) return &v;
  return nullptr;
}

inline bool isOwnerVerb(const std::string& verb) {
  const CommandVerb* v = findCommandVerb(verb);
  return v && v->ownerOnly;
}

// A message that must run as its own turn and never share one with other messages:
// any slash command, known verb or not. Inside a "Message 1 of N" batch the text no
// longer starts with '/', so the command would reach the model as conversation
// instead of running (the 14 Oct "/skill approve" that never ran). An unknown verb is
// included: its deterministic "try /help" answer must not be swallowed either.
inline bool isCommandText(const std::string& text) { return parseCommand(text).isCommand; }

// The text a FORWARDED message enters a turn with. A forward carries someone else's
// words, so a forwarded "/update" must never run as the owner's command: a
// command-looking forward is labeled (the label does not start with '/', so neither
// the batcher nor the dispatcher reads it as a command); any other forward is unchanged.
inline constexpr char kForwardedCommandLabel[] = "Forwarded message (not run as a command):\n";
inline std::string forwardedText(const std::string& text) {
  return isCommandText(text) ? kForwardedCommandLabel + text : text;
}

}  // namespace orch
}  // namespace nimbus
