#pragma once
#include <ESPAsyncWebServer.h>

// web_prefs - the owner's standing-rules routes (lane J), its own translation
// unit like web_skills. Registered from beginWeb() via registerPrefsRoutes.
// The owner is the only writer here (authBlocked gates every route); the
// assistant writes through the prefs.edit tool with its own rails.
//
//   GET  /api/prefs           the rules (id, text, state, origin, nodash) + the caps
//   POST /api/prefs action=add|replace|remove|approve|clear|import
//                            id= and text= where needed; import is a restore
//                            (origin import); both are active at once
namespace nimbus::net {

void registerPrefsRoutes(AsyncWebServer& server);

}  // namespace nimbus::net
