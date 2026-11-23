#pragma once
#include <Arduino.h>

#include <functional>

// commands_plain - the owner's slash commands on an engine-2 device (lane E2-CHAT). Every command
// is answered by the device; none reaches the model. The route of each verb is
// plain::chatCommandRoute (nimbus/harness/plain/chat_commands.h): the handlers that do not depend
// on the engine are the ones engine 1 calls (its own dispatcher in orchestrator_e1.cpp is left as
// it is); /clear, /compact and a bare /budget act on engine 2 through `EngineOps`; the rest answer
// one plain line. Engine-2 images only (tools/engine/engine2_only.txt). The poll task.
namespace agent {
namespace orchestrator {
namespace e2cmd {

struct EngineOps {
  // A new conversation for `chatId` (the stored one is kept). False when it could not start, with
  // `why` the owner's line naming the cause ("" for the could-not-start line).
  std::function<bool(const String& chatId, String& why)> clear;
  // A compaction of `chatId` now; the engine answers it on the chat. False: no chat to compact.
  std::function<bool(const String& chatId)> compact;
  // engine 2's budget view for `chatId` in the owner's words ("" when no chat is running).
  std::function<String(const String& chatId)> budgetView;
};

// true when `text` was a slash command (answered here: no turn runs for it).
bool handle(const String& text, const String& chatId, const EngineOps& ops);

}  // namespace e2cmd
}  // namespace orchestrator
}  // namespace agent
