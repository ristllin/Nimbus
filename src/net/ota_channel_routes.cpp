// RB-OTA2: the owner's update channel over the web API, without a cable.
//   GET  /api/ota/channel             -> 200 {"channel":"stable|candidate","line":"..."}
//   POST /api/ota/channel channel=... -> 200 set, 400 unknown value (nothing changes),
//                                        500 the switch failed (stable is in force)
// Owner only, with the checks every owner route has: the device sign-in code in the
// X-Nimbus-Token header or the t form field (webAuthOk; a ?t= query never counts, and a
// cross-site page can neither set the header nor know the code), 401 without it; bound
// for GET and POST only, so any other method meets the catch-all and changes nothing.
// The new value is read from the POST body only, never the query. The decisions are
// nimbus::ota::channelRoute (host-tested in test/test_ota_channel); the switch is
// otaupd::setChannel, which fails closed. Self-registers into the deferred-route
// registry (errlog_routes.h), so webui.cpp and the web page are unchanged.
#include <ESPAsyncWebServer.h>

#include "errlog_routes.h"          // registerDeferredWebRoute (self-registration)
#include "webui.h"                  // nimbus::net::webAuthOk (token gate)
#include "../sys/ota_update.h"      // otaupd::channelOps
#include "nimbus/ota/ota_channel.h"

namespace nimbus::net {
namespace {

void handleChannel(AsyncWebServerRequest* r) {
  const bool post = r->method() == HTTP_POST;
  const AsyncWebParameter* p = post ? r->getParam("channel", /*post=*/true) : nullptr;
  const nimbus::ota::RouteReply rep =
      nimbus::ota::channelRoute(webAuthOk(r), post, p ? p->value().c_str() : nullptr, otaupd::channelOps());
  AsyncWebServerResponse* res = r->beginResponse(rep.status, "application/json", String(rep.body.c_str()));
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}

void registerOtaChannelRoutes(AsyncWebServer& server) {
  server.on("/api/ota/channel", HTTP_GET, handleChannel);
  server.on("/api/ota/channel", HTTP_POST, handleChannel);
}

struct OtaChannelRouteReg {
  OtaChannelRouteReg() { registerDeferredWebRoute(&registerOtaChannelRoutes); }
};
OtaChannelRouteReg g_otaChannelRouteReg;

}  // namespace
}  // namespace nimbus::net
