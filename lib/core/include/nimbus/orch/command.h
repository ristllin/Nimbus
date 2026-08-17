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

}  // namespace orch
}  // namespace nimbus
