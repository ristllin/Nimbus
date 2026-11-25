#pragma once
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <functional>
#include <string>

// web_prefs - the owner's standing-rules routes (lane J), its own translation
// unit like web_skills. Registered from beginWeb() via registerPrefsRoutes.
// The owner is the only writer here (authBlocked gates every route); the
// assistant writes through the prefs.edit tool with its own rails.
//
//   GET  /api/prefs           the rules (id, text, state, origin, nodash) + the caps
//   POST /api/prefs action=add|replace|remove|approve|clear|import
//                            id= and text= where needed; import is a restore
//                            (origin import); both are active at once
//
// Lane E2-P-SKILL: an engine may add rows to the page (engine 2: the notes held for the owner's
// approval) and answer their actions; with none set the page is engine 1's, unchanged.
namespace nimbus::net {

struct PrefsPageRows {
  std::function<void(ArduinoJson::JsonArray items)> add;   // more rows for GET's items
  // An action on one of its rows: true when it took it (`message` is the page's line).
  std::function<bool(const std::string& action, const std::string& id, std::string& message)> act;
};
void setPrefsPageRows(PrefsPageRows rows);

void registerPrefsRoutes(AsyncWebServer& server);

}  // namespace nimbus::net
