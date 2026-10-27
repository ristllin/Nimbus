#pragma once
#include <cstdint>
#include <string>

#include "nimbus/orch/models_tool.h"   // RefreshAsk

// model_refresh - the device side of "models on demand" (owner ruling R5): the
// models.list tool, the refresh queue and the passive refresh.
//
// Every refresh request (the head's models.list refresh:true or an orch_model the
// cached list does not hold, the owner opening a model dropdown, the passive check)
// joins one RAM queue with one rule: at most one refresh per provider per hour
// (nimbus::orch::ModelRefreshQueue, host-tested). tick() on the main loop starts a
// queued refresh only when the device is free - no turn in flight, online, the
// verify slot idle, the resting heap above the one-connection floor
// (nimbus::orch::kRefreshMinLargest) - one provider at a time, on the self-deleting
// verify task (provider_verify::requestRefresh), or on the Telegram poll task when that
// task's stack does not fit in one internal block. A refresh that ends with the list
// still due is queued again on a growing delay (2 min doubling to 1 hour). Nothing
// here opens a connection or blocks: the tool and the web route only set a flag.
//
// The passive refresh: weekly, or after a firmware update. It reads the header of lane
// F's model store (/data/models_<slug>.txt: "#models v1 fetchedAt=<s> fw=<version>
// ...", written by every list fetch); at boot plus 10 minutes, then every 6 hours, a
// keyed provider whose store is missing, only the boot merge of an older build's
// lists, a week old or written by another firmware version is queued, paused or
// parked or not (the list GET is free). Never NVS.
namespace agent {
namespace model_refresh {

// Orchestrator mode, once at boot after the tool registry is up: registers
// models.list and arms the passive check.
void begin();

// Main loop, every pass (returns at once on most passes): starts one due refresh
// and runs the passive check on its slow clock.
void tick(uint32_t nowMs);

// Any task. Ask for a refresh of `provider`; `why` labels the log line ("head",
// "miss", "web", "passive").
nimbus::orch::RefreshAsk request(const char* provider, const char* why);

// The answer to an orch_model the cached list does not hold (ApplyDeps::modelMiss):
// asks for a refresh and returns "not in <provider> list cached <date>; refresh
// queued".
std::string missText(const std::string& provider, const std::string& model);

// The fetch time of `provider`'s list from its store header (unix seconds; 0 = never,
// or fetched while the clock was unset).
uint32_t fetchedAt(const char* provider);

// The firmware version that wrote `provider`'s store ("" = no store).
std::string stampFw(const char* provider);

// A refresh of `provider` is queued or running (a failed one waiting out its retry
// delay is neither).
bool pending(const char* provider);

// The Telegram poll task's staged work (main.cpp stagedWorkPending / stagedWorkRun): a
// refresh whose own task does not fit in one internal block runs on that task, at its
// loop top, never inside a turn (nimbus::orch::refreshRoute).
bool stagedPending();
void runStaged();

// Run the passive check now instead of on its clock (the test console's MODELS
// passive); returns how many providers it queued.
int passiveCheckNow();

}  // namespace model_refresh
}  // namespace agent
