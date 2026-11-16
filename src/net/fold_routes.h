#pragma once
// fold_routes - Summarize now and the summary's health on the web (lane F of the history
// fix, v5.1.2). Owner-authenticated with the device sign-in token, the gate every owner
// /api route checks (nimbus::net::webAuthOk): without it both answer 401 and nothing is
// staged. Self-registers into the deferred-route registry (errlog_routes.h); /api/state is
// not changed.
//
//   GET  /api/fold/status[?chat=]  -> the chat's fold health (agent::foldStatusJson) plus
//                                     "chats": every conversation with its badge
//   POST /api/fold/run   [chat=]   -> stage a manual catch-up (the queue the fold driver
//                                     keeps), 202; 404 for a chat the device does not know
// "" chat = the default: the owner's Telegram chat, else the web chat.

#include <ESPAsyncWebServer.h>

namespace nimbus::net {

void registerFoldRoutes(AsyncWebServer& server);

}  // namespace nimbus::net
