#pragma once
#include <string>

// budget_profile_store - the device side of the history profile and its governor (lane
// RB-BG; the rules are portable: lib/core nimbus/orch/budget_governor.h).
//
// One flash file, /data/budget.json, written atomically (fs_atomic), never NVS: the owner's
// pinned profile (/budget profile <name>) and the governor's nightly state. It lives on the
// file area, so it survives restarts and updates. A missing or unreadable file reads as the
// default profile (standard), and /usage says so.
//
// load() once the file area is mounted, before any turn (it installs the profile every
// head's context is derived under). tick() once a minute from the Telegram poll task: on the
// first tick after the device's day start it runs the governor's night against the spend
// store's figures; a step that stores installs the new profile, logs a durable line, adds a
// row to the Usage page's change record and owes the owner one line (nextNotice). A night
// whose file write fails is undone and tried again the next minute. While the file reads as
// corrupt the governor does not run (the status says so) until the owner's next /budget
// profile writes it again; a pin whose own field still reads is kept.
namespace agent {
namespace budgetprofile {

void load();
void tick();

// The /usage line: the profile in force and who set it.
std::string statusLine();

// /budget profile [<name>|auto] from the owner (`arg` "" = show). `changed` is set when the
// file changed; `record` is the change record's line for an applied change.
std::string command(const std::string& arg, bool& changed, std::string& record);

// The owner's line a governor step owes on Telegram (`telegram`) or in the web chat ("" =
// none), and the mark that the channel took it. Each channel keeps it owed until then; a newer
// step's line replaces an older one still owed.
std::string nextNotice(bool telegram);
void noticeDelivered(bool telegram);

}  // namespace budgetprofile
}  // namespace agent
