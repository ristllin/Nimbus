#pragma once
// prefs_tools - every way a standing rule is read or changed, with one set of
// rails (owner_prefs.h holds the store):
//   - the model: the prefs.edit tool (tool loop on) and a mem_write item marked
//     standing:true (the loop-off route, via memory.write's doWrite);
//   - the owner: Telegram /prefs and the web routes (GET/POST /api/prefs).
//
// Rails for the model (each has a precedent: skill.save, loop.create, tenant.*):
//   - owner only: a member or guest turn never sees or changes the owner's rules;
//   - never from an unattended turn (routine, wakeup, sub-agent synthesis, the
//     nightly dream): prefs.edit refuses, and a standing:true mem_write there is
//     stored as an ordinary memory instead;
//   - a turn that already read outside content (a web page, a file, a connector,
//     a sub-agent's result: tool_taint.h) can only ADD, and what it adds waits as
//     pending until the owner approves it; it cannot replace or remove a rule;
//   - the owner sees every change in a line the device writes itself.
// The owner's own surfaces (/prefs, the web page) are active at once.
//
// Portable, Arduino-free, host-tested (test_owner_prefs).

#include <cstdint>
#include <functional>
#include <string>

#include "nimbus/orch/owner_prefs.h"
#include "nimbus/orch/tool_registry.h"

namespace nimbus {
namespace orch {

// The words every routing place repeats (compose, the tool description), so the
// model meets one test for "rule or memory?".
inline constexpr const char kStandingRuleTest[] =
    "Would it apply every time, whatever the topic, without the owner reminding you?";

struct PrefsToolContext {
  // Run fn with the store under its lock. false = the store is unavailable (the
  // file area is not mounted): nothing ran.
  std::function<bool(const std::function<void(OwnerPrefs&)>&)> withPrefs;
  std::function<bool()> inScheduledTurn;       // null = never
  std::function<bool()> externalContentSeen;   // null = never (this turn read outside content)
  std::function<uint32_t()> nowHours;          // null = 0
  std::function<void(const std::string&)> notify;   // device-written owner notice; may be null
};

// prefs.edit: add | replace | remove | list.
void registerPrefsTools(ToolRegistry& reg, const PrefsToolContext& ctx);

// The standing:true route of a mem_write item. handled=false means the rules do
// not take it (not the owner, or an unattended turn) and the caller stores it as
// an ordinary memory. When handled, the result carries a memory outcome code
// (stored = active rule, queued = pending rule, refused = not saved).
ToolResult prefsSaveStanding(const PrefsToolContext& ctx, const std::string& text,
                             const Principal& who, bool& handled);

// Telegram /prefs (owner only; the verb table gates it): the reply text, device
// copy. "" | list, add <rule>, rm|remove <id>, approve <id>, clear yes.
std::string prefsCommand(const PrefsToolContext& ctx, const std::string& args);

// The web routes. GET /api/prefs body.
std::string prefsJson(const OwnerPrefs& p);
// POST /api/prefs: action add | import | replace | remove | approve | clear.
// add is the owner typing on the page (origin web); import is a restore (origin
// import). Both are active at once.
PrefErr prefsWebAction(OwnerPrefs& p, const std::string& action, const std::string& id,
                       const std::string& text, uint32_t nowHours, std::string& idOut);
// The HTTP status for a result (400 bad text, 404 unknown id, 409 full, 507 not saved).
int prefErrHttpStatus(PrefErr e);

}  // namespace orch
}  // namespace nimbus
