#pragma once
#include <functional>
#include <string>
#include <vector>

#include "engine2_host.h"
#include "engine_thread.h"
#include "nimbus/harness/plain/agents.h"          // deliverReports
#include "nimbus/harness/plain/chat_commands.h"   // chatCommandRoute
#include "nimbus/orch/command.h"
#include "rig.h"

// engine2_route - lane E2-P-HOST2: which messages engine 2 takes on a hosted instance and how, built once at
// start while engine 2 runs (main.cpp startEngine2):
//   * every approved chat: the owner's (the web chat and the owner's Telegram chats, one conversation) and each
//     member's (a conversation of their own, on their role; Engine2Host::say);
//   * the slash commands as on the device (commands_plain.cpp, plain::chatCommandRoute): /clear, /compact and a
//     bare /budget act on engine 2 for the owner; every other command, and a member's owner verb, gets the
//     instance's own engine-agnostic answer (NimbusdRig::handleCommand), never a model turn;
//   * a finished sub-agent's report as an engine-2 turn in the chat that started it (plain::deliverReports).
namespace nimbusd {

using Engine2Deliver = std::function<void(const std::string& chat, const std::string& text)>;

// One slash command on engine 2. The owner is the web chat or an Admin chat.
inline void engine2Command(NimbusdRig& rig, Engine2Host& host, const Engine2Deliver& deliver, const std::string& chat,
                           const std::string& text) {
  const nimbus::orch::Command c = nimbus::orch::parseCommand(text);
  const bool owner = rig.telegramAccess().principalFor(chat).owner;
  using R = agent::plain::ChatCommandRoute;
  const R route = agent::plain::chatCommandRoute(c);
  std::fprintf(stderr, "[nimbusd] engine2: /%s from %s: %s\n", c.verb.c_str(), chat.c_str(),
               agent::plain::chatCommandRouteSlug(route));
  if (owner && deliver && route == R::Clear) return deliver(chat, host.clearCommand(c.args));
  if (owner && deliver && route == R::Compact) return deliver(chat, host.compactCommand());
  if (owner && deliver && route == R::BudgetView) {
    const std::string view = host.budgetViewText(chat);
    return deliver(chat, view.empty() ? std::string(kNoStartLine) : view);
  }
  (void)rig.handleCommand(chat, text);
}

inline Engine2Route engine2Route(NimbusdRig& rig, Engine2Host& host, Engine2Deliver deliver) {
  Engine2Route r;
  r.takes = [&rig](const std::string& chat, const std::string&, bool) {
    return chat == kWebChatId || rig.telegramAccess().mayConverse(chat);
  };
  r.say = [&rig, &host, deliver](const std::string& chat, const std::string& text, const std::vector<PhotoIn>& photos) {
    if (nimbus::orch::parseCommand(text).isCommand) return engine2Command(rig, host, deliver, chat, text);
    const char* ch = chat == kWebChatId ? "web" : "tg";
    if (photos.empty()) {
      (void)host.say(chat, text, ch);
      return;
    }
    // E2-P-PHOTO: the photos as engine 1's channel offers them (kept, rows written), taken by engine 2.
    (void)host.say(chat, rig.offerPhotoTurn(chat, text, photos), ch);
    nimbus::orch::dropTurnImages();   // a set the turn never took
  };
  r.idle = [&host](bool waiting) { return host.idle(waiting); };
  rig.setEngine2Reports([&rig, &host](const std::string& chat) {
    int ran = 0;
    const agent::plain::Status st = agent::plain::deliverReports(
        rig.jobs(), chat, [&host](const std::string& c, const std::string& note) { return host.reportTurn(c, note); },
        ran);
    if (ran || !st.ok())
      std::fprintf(stderr, "[nimbusd] engine2: agent reports for %s: %d ran, %s\n", chat.c_str(), ran,
                   nimbus::orch::tx::errSlug(st.err));
  });
  return r;
}

}  // namespace nimbusd
