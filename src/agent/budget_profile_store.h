#pragma once
#include <string>

// budget_profile_store - the device side of the owner's history profile and the nightly
// spending advice (lane RB-BG; the rules are portable: lib/core nimbus/orch/budget_governor.h).
//
// One flash file, /data/budget.json, written atomically (fs_atomic), never NVS: the owner's
// profile (/budget profile <name>; full, the default, when never chosen) and the advice's
// nightly state. It lives on the file area, so it survives restarts and updates. A missing or
// unreadable file reads as full, and /usage says so; an unreadable one keeps the owner's
// profile if its own field still reads.
//
// load() once the file area is mounted, before any turn (it installs the profile every head's
// context is derived under). tick() once a minute from the Telegram poll task: on the first
// tick after the device's day start it runs the night against the spend store's figures. It
// never changes the profile (owner ruling 81): when the month is on course past 0.9 of the
// ceiling it owes the owner one line, once a month (nextNotice), logs it and adds a row to the
// Usage page's record. A night whose file write fails is undone and tried again the next
// minute. While the file reads as corrupt the advice does not run (the status says so) until
// the owner's next /budget profile writes it again.
namespace agent {
namespace budgetprofile {

void load();
void tick();

// The /usage line: the profile in force and who set it.
std::string statusLine();

// /budget profile [<name>] from the owner (`arg` "" = show). `changed` is set when the file
// changed; `record` is the change record's line for an applied change.
std::string command(const std::string& arg, bool& changed, std::string& record);

// The owner's line the advice owes on Telegram (`telegram`) or in the web chat ("" =
// none), and the mark that the channel took it. Each channel keeps it owed until then; a newer
// line replaces an older one still owed.
std::string nextNotice(bool telegram);
void noticeDelivered(bool telegram);

}  // namespace budgetprofile
}  // namespace agent
