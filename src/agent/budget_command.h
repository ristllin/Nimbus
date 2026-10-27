#pragma once
#include <Arduino.h>

#include <string>

#include "nimbus/orch/budget_command.h"   // BudgetMsg
#include "nimbus/orch/command.h"

// budget_command - the device side of the owner's spending commands and /model (the
// parse, the bounds, the plan and the words are portable: lib/core
// nimbus/orch/budget_command.h).
//
// /usage (and a bare /budget) reads the spend store's state and the provider park;
// /budget <provider|all> <USD> [force] and /budget <provider> off propose a change that
// applies only on "/budget confirm" from the same chat within 2 minutes (RAM only: a
// restart drops it); an applied change goes through the same setters the web page's
// Usage tab uses (the overall ceiling, the provider's dollar budget), is logged to the
// durable device log, kept in a small flash record the Usage page lists
// (/data/budget_log.txt, never NVS) and copied once into the web chat's history. /model answers
// from the stored head choice, the last reply's provider and model (the turn-end hook)
// and the park. None of them calls a model, so they work while every provider is paused.
//
// Runs on the Telegram poll task (handleMessage); changesJson() is read by the web task.

namespace agent {
namespace budget {

struct Reply {
  std::string text;     // the reply to the chat that sent the command
  std::string mirror;   // an applied change's line for the web chat ("" = none)
};

// The reply to /usage, /budget [...] or /model from `chatId`. `owner`: the device's
// owner check passed (the dispatcher's gate); the module's own access rule still
// refuses a group chat and a change by voice (hold-to-talk or a voice note's transcript,
// `msg.voiceNote`). `msg.date`: the Telegram message's send time (0 for the web chat and
// serial): a late message changes nothing and a confirm is held to its proposal's date.
Reply handle(const std::string& verb, const std::string& args, const String& chatId, bool owner,
             const nimbus::orch::BudgetMsg& msg = nimbus::orch::BudgetMsg());

// Every message handleMessage receives, before any command runs: a change `chatId`
// proposed is cancelled unless this message is a /budget confirm, cancel or a new
// proposal.
void noteMessage(const String& chatId, const nimbus::orch::Command& cmd);

// The turn-end hook (owner, serial and synthesis turns): the provider the last reply ran
// on, the model key it billed under, the head it started on, and whether it answered.
void noteTurnEnd(const std::string& host, const std::string& model, const std::string& requestedHost,
                 const char* src, bool ok);

// The newest 3 applied changes for the Usage page, newest first:
// [{"t":<epoch s, 0 = clock unset>,"src":"telegram"|"web"|"serial","text":".."}].
std::string changesJson();

}  // namespace budget
}  // namespace agent
