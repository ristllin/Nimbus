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
// store's figures; a step installs the new profile, writes the file, logs a durable line,
// adds a row to the Usage page's change record and owes the owner one line (nextNotice).
namespace agent {
namespace budgetprofile {

void load();
void tick();

// The /usage line: the profile in force and who set it.
std::string statusLine();

// /budget profile [<name>|auto] from the owner (`arg` "" = show). `changed` is set when the
// file changed; `record` is the change record's line for an applied change.
std::string command(const std::string& arg, bool& changed, std::string& record);

// The owner's line a governor step owes ("" = none), and the mark that it was delivered.
std::string nextNotice();
void noticeDelivered();

}  // namespace budgetprofile
}  // namespace agent
