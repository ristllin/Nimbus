#pragma once
// safety_routes - the /api/safety* web surface for the Safety ACTIVITY tab
// (CUM-215). Token-gated with the same device token as every other /api route
// (nimbus::net::webAuthOk). Self-registers into the deferred-route registry at
// static-init time (see errlog_routes.h), so no edit to webui.cpp is needed.
//
// Routes (all token-gated):
//   GET  /api/safety                 -> {entries:[...], allow:[...], report:{...}}
//   POST /api/safety/dismiss  id=    -> mark an entry dismissed
//   POST /api/safety/approve  id= scope=sender|content-class|pattern
//                                    -> add a SCOPED allow-rule + mark approved
//   POST /api/safety/allow/revoke id= -> revoke an allow-rule
//   POST /api/safety/report   id=    -> report to Cumulo (subscription-gated);
//                                       mirrors the wire status codes to the browser

#include <ESPAsyncWebServer.h>

namespace nimbus::net {

// Bind the Safety routes on `server`. Invoked via the deferred-route registry;
// also callable directly.
void registerSafetyRoutes(AsyncWebServer& server);

}  // namespace nimbus::net
