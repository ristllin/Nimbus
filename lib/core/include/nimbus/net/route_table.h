#pragma once
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// route_table - the matching rule of the device web server, so a route the server
// can never reach fails a host test (CUM-718). ESPAsyncWebServer matches a plain
// string route as "the path equals it, or starts with it plus '/'" and tries the
// handlers in registration order: a generic /api/connectors registered before
// /api/connectors/oauth/start answered every OAuth request ("patch must be a JSON
// object"), so the page could never start an MCP sign-in, and /api/cloud swallowed
// /api/cloud/mintkey the same way. The host test test_web_routes scans every
// s_server.on(...) registration in src/net and fails on any such pair.
namespace nimbus {
namespace net {

// Method bits, as the registration names them (HTTP_GET, HTTP_POST, ...).
enum : unsigned {
  kRouteGet = 1u << 0,
  kRoutePost = 1u << 1,
  kRoutePut = 1u << 2,
  kRouteDelete = 1u << 3,
  kRoutePatch = 1u << 4,
  kRouteOther = 1u << 5,   // HEAD, OPTIONS
  kRouteAny = 0x3Fu,
};

struct RouteReg {
  std::string file;   // where it is registered (handlers in one file run in line order)
  int line = 0;
  std::string uri;    // the literal route string
  unsigned methods = 0;
};

// The server's plain-string match: the path equals `route`, or starts with it + "/".
inline bool routeMatches(const std::string& route, const std::string& path) {
  if (path == route) return true;
  return path.size() > route.size() && path.compare(0, route.size(), route) == 0 &&
         path[route.size()] == '/';
}

// Each pair (shadowing, shadowed) of registrations where the first answers every
// request meant for the second: the same file with the broader route on an earlier
// line, or two different files (their relative order depends on startup order, so
// any overlap there counts). Methods must overlap.
inline std::vector<std::pair<size_t, size_t>> shadowedRoutes(const std::vector<RouteReg>& regs) {
  std::vector<std::pair<size_t, size_t>> out;
  for (size_t i = 0; i < regs.size(); i++) {
    for (size_t j = 0; j < regs.size(); j++) {
      if (i == j || !(regs[i].methods & regs[j].methods)) continue;
      if (regs[i].uri == regs[j].uri) continue;   // one route, two methods or a duplicate
      if (!routeMatches(regs[i].uri, regs[j].uri)) continue;
      const bool sameFile = regs[i].file == regs[j].file;
      if (!sameFile || regs[i].line < regs[j].line) out.emplace_back(i, j);
    }
  }
  return out;
}

}  // namespace net
}  // namespace nimbus
