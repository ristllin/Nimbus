#pragma once
// errlog_routes - HTTP retrieval of the durable error log (CUM-401), so we can pull
// the log for investigation. Token-gated with the same device token as every other
// /api route (nimbus::net::webAuthOk). The log is already redacted at the source, so
// these routes only ever serve masked lines.
//
// Self-registration: the shared AsyncWebServer is file-static in src/net/webui.cpp,
// which this lane must not touch. Instead this module registers itself into a small
// deferred-route registry at static-init time; the web layer drains the registry once
// with a single generic line right after it builds the server. That keeps a new route
// addable with no edit to webui.cpp.

#include <ESPAsyncWebServer.h>

#include <functional>
#include <utility>
#include <vector>

namespace nimbus::net {

using WebRouteRegistrar = std::function<void(AsyncWebServer&)>;

// The registry (Meyers singleton so it is constructed on first use, independent of
// static-init order across translation units).
inline std::vector<WebRouteRegistrar>& deferredWebRoutes() {
  static std::vector<WebRouteRegistrar> v;
  return v;
}

// A route module calls this at static-init time to defer its registration.
inline void registerDeferredWebRoute(WebRouteRegistrar fn) {
  deferredWebRoutes().push_back(std::move(fn));
}

// The web layer calls this ONCE, after creating the server, to bind every deferred
// route. One generic line in beginWeb() wires this and any future self-registered route.
inline void drainDeferredWebRoutes(AsyncWebServer& server) {
  for (auto& fn : deferredWebRoutes()) fn(server);
}

// Bind the error-log retrieval routes on `server`. Invoked via the registry above;
// also callable directly if a caller prefers explicit wiring.
void registerErrlogRoutes(AsyncWebServer& server);

}  // namespace nimbus::net
