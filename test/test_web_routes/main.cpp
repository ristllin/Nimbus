// test_web_routes - every device web route is reachable (CUM-718).
//
// ESPAsyncWebServer answers a request with the first registered handler whose route
// equals the path or is a "/"-bounded prefix of it. A generic route registered
// before a more specific one therefore swallows it: /api/connectors answered every
// /api/connectors/oauth/* request, so the page could not start an MCP sign-in, and
// /api/cloud answered /api/cloud/mintkey. This suite scans every `.on("<route>", ...)`
// registration in src/net/*.cpp (the device web surface) and fails on any such pair,
// so the class cannot come back with a new route. (onNotFound is the catch-all the
// server tries only after every route declined, so it can never shadow one.)
#include <unity.h>

#include <cstdio>
#include <filesystem>
#include <regex>
#include <string>
#include <vector>

#include "nimbus/net/route_table.h"

using nimbus::net::RouteReg;
using nimbus::net::shadowedRoutes;

void setUp() {}
void tearDown() {}

static std::string readFile(const std::string& path) {
  std::string out;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

static unsigned methodBits(const std::string& expr) {
  unsigned m = 0;
  if (expr.find("HTTP_ANY") != std::string::npos) return nimbus::net::kRouteAny;
  if (expr.find("HTTP_GET") != std::string::npos) m |= nimbus::net::kRouteGet;
  if (expr.find("HTTP_POST") != std::string::npos) m |= nimbus::net::kRoutePost;
  if (expr.find("HTTP_PUT") != std::string::npos) m |= nimbus::net::kRoutePut;
  if (expr.find("HTTP_DELETE") != std::string::npos) m |= nimbus::net::kRouteDelete;
  if (expr.find("HTTP_PATCH") != std::string::npos) m |= nimbus::net::kRoutePatch;
  if (expr.find("HTTP_HEAD") != std::string::npos || expr.find("HTTP_OPTIONS") != std::string::npos)
    m |= nimbus::net::kRouteOther;
  return m;
}

// Every `<server>.on("<route>", <methods>, ...)` registration in the device web files.
static std::vector<RouteReg> scanRoutes() {
  std::vector<RouteReg> regs;
  const std::regex re(R"re(\b\w+\.on\(\s*"([^"]+)"\s*,\s*([A-Z_|\s]+),)re");
  std::vector<std::string> files;
  for (const auto& e : std::filesystem::directory_iterator("src/net"))
    if (e.path().extension() == ".cpp") files.push_back(e.path().string());
  std::sort(files.begin(), files.end());
  for (const std::string& f : files) {
    const std::string src = readFile(f);
    for (auto it = std::sregex_iterator(src.begin(), src.end(), re); it != std::sregex_iterator();
         ++it) {
      RouteReg r;
      r.file = f;
      r.line = 1 + (int)std::count(src.begin(), src.begin() + it->position(0), '\n');
      r.uri = (*it)[1].str();
      r.methods = methodBits((*it)[2].str());
      regs.push_back(r);
    }
  }
  return regs;
}

// The class guard: no registered route is unreachable behind a broader one.
static void test_no_route_is_shadowed() {
  const std::vector<RouteReg> regs = scanRoutes();
  // The scan must see the real surface (well over a hundred routes), or a regex
  // that silently matches nothing would pass this test forever.
  TEST_ASSERT_GREATER_THAN_MESSAGE(100, (int)regs.size(), "route scan found too few routes");
  const auto bad = shadowedRoutes(regs);
  for (const auto& p : bad) {
    const RouteReg& a = regs[p.first];
    const RouteReg& b = regs[p.second];
    std::printf("SHADOWED: %s:%d %s answers every request for %s:%d %s\n", a.file.c_str(), a.line,
                a.uri.c_str(), b.file.c_str(), b.line, b.uri.c_str());
  }
  TEST_ASSERT_EQUAL_MESSAGE(0, (int)bad.size(),
                            "a route is registered after a broader one that swallows it: "
                            "register the specific route first");
}

// The OAuth routes the page calls are among the scanned registrations.
static void test_scan_sees_the_oauth_routes() {
  const std::vector<RouteReg> regs = scanRoutes();
  int seen = 0;
  for (const RouteReg& r : regs)
    if (r.uri == "/api/connectors/oauth/start" || r.uri == "/api/connectors/oauth/status" ||
        r.uri == "/api/connectors/oauth/cancel" || r.uri == "/api/cloud/mintkey")
      seen++;
  TEST_ASSERT_EQUAL(5, seen);   // mintkey has GET and POST
}

// The matcher rule itself, and the CUM-718 order before and after the fix.
static void test_matcher_and_the_cum718_order() {
  using nimbus::net::routeMatches;
  TEST_ASSERT_TRUE(routeMatches("/api/connectors", "/api/connectors"));
  TEST_ASSERT_TRUE(routeMatches("/api/connectors", "/api/connectors/oauth/start"));
  TEST_ASSERT_FALSE(routeMatches("/api/connectors", "/api/connectorsx"));
  TEST_ASSERT_FALSE(routeMatches("/api/connectors/oauth/start", "/api/connectors"));

  const unsigned P = nimbus::net::kRoutePost, G = nimbus::net::kRouteGet;
  std::vector<RouteReg> before = {{"webui.cpp", 10, "/api/connectors", P},
                                  {"webui.cpp", 20, "/api/connectors/oauth/start", P}};
  TEST_ASSERT_EQUAL(1, (int)shadowedRoutes(before).size());
  std::vector<RouteReg> after = {{"webui.cpp", 10, "/api/connectors/oauth/start", P},
                                 {"webui.cpp", 20, "/api/connectors", P}};
  TEST_ASSERT_EQUAL(0, (int)shadowedRoutes(after).size());
  // Different methods never collide; across files any overlap counts.
  std::vector<RouteReg> methods = {{"a.cpp", 1, "/api/x", G}, {"a.cpp", 2, "/api/x/y", P}};
  TEST_ASSERT_EQUAL(0, (int)shadowedRoutes(methods).size());
  std::vector<RouteReg> files = {{"b.cpp", 9, "/api/x/y", G}, {"a.cpp", 1, "/api/x", G}};
  TEST_ASSERT_EQUAL(1, (int)shadowedRoutes(files).size());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_matcher_and_the_cum718_order);
  RUN_TEST(test_scan_sees_the_oauth_routes);
  RUN_TEST(test_no_route_is_shadowed);
  return UNITY_END();
}
