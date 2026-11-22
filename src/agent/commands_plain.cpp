// commands_plain.cpp - the owner's slash commands on engine 2 (lane E2-CHAT; commands_plain.h).
//
// The shared verbs call what engine 1's dispatcher calls (orchestrator_e1.cpp handleMessage), with
// the same gate (a Telegram member may converse, not run an owner verb; web, serial and voice are
// the owner's own channels) and the same replies, each tagged with the device name and version.
#include "commands_plain.h"

#if !defined(NIMBUS_ENGINE) || NIMBUS_ENGINE != 2
#error "commands_plain.cpp is engine 2's: build it only in an env with -DNIMBUS_ENGINE=2"
#endif

#include <time.h>

#include <string>

#include "../sfx/music.h"           // /play (CUM-40)
#include "../sys/agent_log.h"
#include "../sys/config_nvs.h"      // sys::deviceName - the reply tag
#include "../sys/ota_update.h"      // /ota (RB-OTA2)
#include "budget_command.h"         // /usage, /budget, /model (lane BUD)
#include "files_subsystem.h"        // /fetch (W18)
#include "loops_subsystem.h"        // /loops, /loop
#include "memory_subsystem.h"       // captureMessage - a budget change's copy for the web chat
#include "nimbus/harness/plain/chat_commands.h"
#include "nimbus/orch/command.h"
#include "nimbus/orch/media.h"      // validMusicName
#include "nimbus/orch/rbac.h"
#include "nimbus/ota/ota_channel.h"
#include "orchestrator.h"           // roleOfChat, isChatAllowed, setOtaInstallHook's hook
#include "orchestrator_common.h"    // deliver
#include "schedule_device.h"        // /remind through the schedule tool, /loop delete's state (E2-SCHED)
#include "telegram.h"               // turnMessageDate, turnIsVoiceNote
#include "version.h"

namespace agent {
namespace orchestrator {

String otaInstallReply();   // orchestrator_plain.cpp: the hook the web route set

namespace e2cmd {

namespace {

namespace plain = agent::plain;

// Device copy (owner-facing, never model text). PC-0177.
constexpr const char* kNotYetLine = "This command is not available on the new engine yet.";
// orchestrator_plain.cpp's line for a chat the engine could not start, reused.
constexpr const char* kNoStartLine = "The new engine could not start for this chat. Restart the device and try again.";

String selfTag() { return String(nimbus::sys::deviceName().c_str()) + " \xC2\xB7 " NIMBUS_FW_VERSION ": "; }

void say(const String& chatId, const String& body) { deliver(chatId, body, /*system=*/true); }

// /play, as engine 1 answers it.
void play(const String& chatId, const std::string& args) {
  String arg = String(args.c_str());
  arg.trim();
  String low = arg;
  low.toLowerCase();
  if (low == "stop") {
    music::stop();
    say(chatId, selfTag() + "Stopped music.");
    return;
  }
  if (low == "pause") {
    music::pause();
    say(chatId, selfTag() + "Paused music.");
    return;
  }
  if (arg.length() == 0) {
    const int n = music::playAll();
    say(chatId, selfTag() + (n ? ("Playing " + String(n) + " track" + (n == 1 ? "" : "s") + " from the music folder.")
                               : String("No tracks in the music folder (SD /music).")));
    return;
  }
  if (!nimbus::orch::validMusicName(arg.c_str())) {
    say(chatId, selfTag() + "That is not a valid track name. Use a .wav or .mp3 file in the music folder.");
    return;
  }
  music::playNow({std::string(arg.c_str())});
  say(chatId, selfTag() + "Playing " + arg + ".");
}

// One /loop action on routine `id`, as engine 1 words it ("" for an action it does not know).
String loopAction(const String& action, const String& id) {
  const char* none = "No routine with that ID.";
  if (action == "approve") return loops::approveLoop(id) ? "Approved - it will run on schedule." : none;
  if (action == "deny" || action == "kill" || action == "delete")
  {
    if (loops::isReservedId(id)) return "This is a system routine - use /loop off " + id + " to pause it.";
    if (!loops::cancelLoop(id)) return none;
    scheduledev::forget(std::string(id.c_str()));   // its state file goes with it (lane E2-SCHED)
    return "Removed.";
  }
  if (action == "off" || action == "pause") return loops::setEnabled(id, false) ? "Paused." : none;
  if (action == "on" || action == "resume") return loops::setEnabled(id, true) ? "Resumed." : none;
  return String();
}

// /loop, as engine 1 answers it (the routines table is engine-neutral; routines fire again once the
// schedule lane binds them).
void loop(const String& chatId, const std::string& args) {
  String rest = String(args.c_str());
  const int sp = rest.indexOf(' ');
  String action = (sp < 0) ? rest : rest.substring(0, sp);
  String id = (sp < 0) ? String("") : rest.substring(sp + 1);
  id.trim();
  if (action.length() == 0) {
    say(chatId, selfTag() + "Usage: /loops \xC2\xB7 /loop approve|deny|off|on <id>");
    return;
  }
  if (action == "grants" && (id == "on" || id == "off")) {
    loops::setRoutineGrants(id == "on");
    say(chatId, selfTag() + (id == "on" ? "Routines may now use your connectors and start sub-agents."
                                        : "Routines now only read, set their follow-up and reply."));
    return;
  }
  const String msg = loopAction(action, id);
  if (msg.length() == 0) {
    say(chatId, selfTag() + "Usage: /loops  |  /loop approve|deny|off|on <id>");
    return;
  }
  say(chatId, selfTag() + "Routine " + id + ": " + msg);
}

// /remind <when> <what> (lane E2-SCHED): engine 1's words and duration forms; the reminder is made
// by the schedule tool's own code (schedule::call, kind reminder) as the owner's: approved, and
// engine 1 reads it as its own /remind record. The moment is rounded up to the minute.
void remind(const String& chatId, const std::string& args) {
  String rest = String(args.c_str());
  rest.trim();
  const int sp = rest.indexOf(' ');
  const String dur = (sp < 0) ? rest : rest.substring(0, sp);
  String note = (sp < 0) ? String("") : rest.substring(sp + 1);
  note.trim();
  const long secs = nimbus::orch::parseDurationSecs(std::string(dur.c_str()));
  if (secs < 0 || note.length() == 0) {
    say(chatId, selfTag() + "Usage: /remind <when> <what> - e.g. /remind 30m take the cake out. Time is a span like "
                            "45s, 30m, 2h, or 1d.");
    return;
  }
  const uint64_t at = ((uint64_t)time(nullptr) + (uint64_t)secs + 59) / 60 * 60;
  const nimbus::orch::CivilTime c = loops::localOf(at);
  char when[24];
  snprintf(when, sizeof(when), "%04d-%02d-%02d %02d:%02d", (int)c.year, (int)c.month, (int)c.day, (int)c.hour,
           (int)c.min);
  JsonDocument a;
  a["op"] = "create";
  a["kind"] = "reminder";
  a["when"] = when;
  a["text"] = note.c_str();
  nimbus::orch::schedule::CallCtx ctx;
  ctx.origin = nimbus::orch::notes::Origin::Attended;
  ctx.chatId = chatId.c_str();
  ctx.owner = true;
  const nimbus::orch::ToolResult r = nimbus::orch::schedule::call(scheduledev::host(), a.as<JsonObjectConst>(), ctx);
  if (!r.success) {
    say(chatId, selfTag() + "Couldn't set that reminder: " + String(r.error.c_str()));
    return;
  }
  // "stored <id> reminder <when>: <text>"
  const std::string& o = r.output;
  const size_t idAt = o.find(' ') + 1, idEnd = o.find(' ', idAt);
  const std::string id = o.substr(idAt, idEnd - idAt);
  say(chatId, selfTag() + "Reminder set for " + when + ": " + note + "  (cancel with /loop delete " + String(id.c_str()) +
                  ")");
}

// /fetch, as engine 1 answers it.
void fetch(const String& chatId, const std::string& args) {
  String rest = String(args.c_str());
  const int sp = rest.indexOf(' ');
  String action = (sp < 0) ? rest : rest.substring(0, sp);
  String id = (sp < 0) ? String("") : rest.substring(sp + 1);
  id.trim();
  const uint32_t n = (uint32_t)id.toInt();
  if (action == "approve" && n)
    say(chatId, selfTag() + (files::fetchApprove(n) ? "Approved - downloading in the background; I'll confirm when it lands."
                                                    : "No pending download with that ID."));
  else if (action == "deny" && n)
    say(chatId, selfTag() + (files::fetchDeny(n) ? "Denied - it will not be downloaded." : "No pending download with that ID."));
  else
    say(chatId, selfTag() + "Usage: /fetch approve|deny <id>");
}

// /usage, /budget <...>, /model: lane BUD's module, as engine 1 calls it.
String spending(const nimbus::orch::Command& cmd, const String& chatId, bool owner, bool pseudo) {
  nimbus::orch::BudgetMsg facts;
  if (!pseudo) {
    facts.date = telegram::turnMessageDate();
    facts.voiceNote = telegram::turnIsVoiceNote();
  }
  const budget::Reply r = budget::handle(cmd.verb, cmd.args, chatId, owner, facts);
  if (!r.mirror.empty() && chatId != "web")
    memory::captureMessage("web", "assistant", nimbus::orch::MsgKind::Message, String(r.mirror.c_str()), "", "ev:budget");
  return String(r.text.c_str());
}

void help(const String& chatId) {
  say(chatId, "Hi - I'm " + String(nimbus::sys::deviceName().c_str()) +
                  " (firmware " NIMBUS_FW_VERSION
                  "). Send a message and I'll reply here.\n\nOwner commands:\n"
                  "/update - install a pending firmware update\n"
                  "/loops - list routines\n"
                  "/remind <when> <what> - one-time reminder (e.g. /remind 30m ...)\n"
                  "/compact - summarize this conversation into memory\n"
                  "/clear - forget this conversation (keeps memory and files)\n"
                  "/mcp approve|deny <name> - approve a tool server\n"
                  "/play [track|stop|pause] - play music from the SD card\n"
                  "/loop approve|deny|off|on <id> - manage a routine\n"
                  "/loop grants on|off - may routines use your connectors and start sub-agents\n"
                  "/skill list|show <id> - see saved skills and what waits for you\n"
                  "/skill approve|deny|delete <id> - decide on the assistant's skill changes\n"
                  "/skill auto on|off - may the assistant approve its own skill changes\n"
                  "/prefs - list your standing rules; /prefs add <rule> saves one\n"
                  "/usage - this month's spending, limits and what is paused\n"
                  "/budget <provider|all> <USD> - change a monthly limit (asks you to confirm)\n"
                  "/model - which provider and model the assistant uses, and what is paused");
}

void shared(const nimbus::orch::Command& cmd, const String& chatId, bool owner, bool pseudo) {
  const std::string& v = cmd.verb;
  if (v == "help" || v == "start") return help(chatId);
  if (v == "update") {
    say(chatId, selfTag() + (cmd.args.empty() ? otaInstallReply() : String("Send /update on its own to install a pending update.")));
    return;
  }
  if (v == "ota") {
    say(chatId, selfTag() + nimbus::ota::channelCommandReply(cmd.args.data(), cmd.args.size(), otaupd::channelOps()));
    return;
  }
  if (v == "play") return play(chatId, cmd.args);
  if (v == "fetch") return fetch(chatId, cmd.args);
  if (v == "loops") return say(chatId, selfTag() + loops::loopsText());
  if (v == "loop") return loop(chatId, cmd.args);
  if (v == "remind") return remind(chatId, cmd.args);
  if (v == "usage" || v == "budget" || v == "model") return say(chatId, selfTag() + spending(cmd, chatId, owner, pseudo));
  alogf("e2: /%s has no shared handler", v.c_str());   // the route table and this list disagree
  say(chatId, selfTag() + kNotYetLine);
}

// PC-0177: the question and the answer of engine 2's /clear.
void clear(const nimbus::orch::Command& cmd, const String& chatId, const EngineOps& ops) {
  String arg = String(cmd.args.c_str());
  arg.trim();
  arg.toLowerCase();
  if (arg != "yes" && arg != "confirm") {
    say(chatId, selfTag() + "Start a new conversation? The current one stays stored on the device. Send /clear yes to "
                            "confirm.");
    return;
  }
  say(chatId, selfTag() + (ops.clear && ops.clear(chatId) ? "Started a new conversation. The earlier one stays stored on "
                                                            "the device."
                                                          : kNoStartLine));
}

void routed(plain::ChatCommandRoute route, const nimbus::orch::Command& cmd, const String& chatId, const EngineOps& ops,
            bool owner, bool pseudo) {
  using R = plain::ChatCommandRoute;
  if (route == R::Shared) return shared(cmd, chatId, owner, pseudo);
  if (route == R::NotYet) return say(chatId, selfTag() + kNotYetLine);
  if (route == R::Clear) return clear(cmd, chatId, ops);
  if (route == R::Compact) {
    if (!ops.compact || !ops.compact(chatId)) say(chatId, selfTag() + kNoStartLine);
    return;
  }
  if (route == R::BudgetView) {
    const String view = ops.budgetView ? ops.budgetView(chatId) : String();
    const String spend = spending(cmd, chatId, owner, pseudo);
    return say(chatId, selfTag() + (view.length() ? view + "\n\n" + spend : spend));
  }
  say(chatId, selfTag() + "I don't recognize /" + String(cmd.verb.c_str()) + " - try /help. To chat, just write without "
                          "the slash.");
}

}  // namespace

bool handle(const String& text, const String& chatId, const EngineOps& ops) {
  const nimbus::orch::Command cmd = nimbus::orch::parseCommand(std::string(text.c_str()));
  // Lane BUD: a waiting /budget change is cancelled by any other message from that chat.
  budget::noteMessage(chatId, cmd);
  const bool pseudo = chatId == "web" || chatId == "serial" || chatId == "voice";
  if (!cmd.isCommand || !(pseudo || isChatAllowed(chatId))) return false;
  const bool owner = pseudo || nimbus::orch::mayRunOwnerCommands(roleOfChat(chatId));
  if (nimbus::orch::isOwnerVerb(cmd.verb) && !owner) {
    say(chatId, selfTag() + "Only the device's owner can use that command. You're welcome to keep chatting with me here.");
    return true;
  }
  const plain::ChatCommandRoute route = plain::chatCommandRoute(cmd);
  alogf("e2: /%s from %s: %s", cmd.verb.c_str(), chatId.c_str(), plain::chatCommandRouteSlug(route));
  routed(route, cmd, chatId, ops, owner, pseudo);
  return true;
}

}  // namespace e2cmd
}  // namespace orchestrator
}  // namespace agent
