// test_connectors - offline (T1) proof of the hosted connector registry
// (CUM-424) and the router route resolution the cumulo head now shares with the
// device (CUM-425). Covers the CLASS rules the device contract encodes: secrets
// are write-only (never echoed, preserved on a blank patch), the save path
// validates and caps, a write survives a reload (0600 file, atomic rename), the
// attach builders receive the parsed set and produce the provider wire shapes,
// and a zai/<model> selector resolves to the zai router path. No sockets, no
// engine, no keys.
#include <sys/stat.h>

#include <string>
#include <vector>

#include "connectors_store.h"
#include "nimbus/orch/connectors_wire.h"
#include "nimbus/orch/router_route.h"
#include "test_util.h"

using namespace nimbusd;

static bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

static std::string sanitized(const ConnectorsStore& s) {
  JsonDocument d;
  s.sanitizedConfigured(d.to<JsonArray>());
  std::string out;
  serializeJson(d, out);
  return out;
}

static void checkStoreOps(ndtest::Ctx& c) {
  const std::string dir = ndtest::scratchDir("conns");
  fsutil::mkdirs(dir);
  ConnectorsStore s;
  s.setPath(dir + "/connectors.json");

  // Upsert with a secret; the GET view masks it to hasTok and never echoes it.
  c.eq(s.patchUpsert(R"({"name":"github","type":"github","prov":"openai","kind":"mcp","url":"https://api.githubcopilot.com/mcp/","tok":"ghp_secret123","en":1})"),
       "", "patch upsert of a valid mcp entry saves");
  const std::string view = sanitized(s);
  c.ok(has(view, "\"hasTok\":true"), "GET view carries hasTok=true");
  c.ok(!has(view, "ghp_secret123"), "GET view NEVER contains the token");
  c.ok(has(view, "\"name\":\"github\"") && has(view, "\"en\":1"), "GET view carries the entry fields");

  // A blank tok in a card edit preserves the stored secret (device rule).
  c.eq(s.patchUpsert(R"({"name":"github","prov":"openai","kind":"mcp","url":"https://api.githubcopilot.com/mcp/","tok":"","en":0})"),
       "", "patch with blank tok saves");
  c.eq(s.bearerForName("github"), "ghp_secret123", "blank-tok patch preserved the stored secret");
  c.ok(has(sanitized(s), "\"en\":0"), "the non-secret field DID update");

  // A private URL is rejected at save time with the owner-facing message class.
  const std::string err =
      s.patchUpsert(R"({"name":"lan","prov":"openai","kind":"mcp","url":"http://192.168.1.5/mcp","en":1})");
  c.ok(!err.empty(), "a private MCP URL is refused at save time");
  c.ok(!has(s.blob(), "192.168.1.5"), "the refused entry was not stored");

  // del removes by name; deleting a missing name is a no-op success.
  c.eq(s.removeByName("nope"), "", "deleting a missing name is a no-op success");
  c.eq(s.removeByName("github"), "", "delete by name succeeds");
  c.eq(sanitized(s), "[]", "the set is empty after delete");

  // blob replace: cap + shape errors use the device's exact strings.
  c.eq(s.replaceBlob("{\"not\":\"array\"}"), "blob must be a JSON array (<=3500B)",
       "a non-array blob is refused with the device error string");
  c.eq(s.replaceBlob(std::string(3600, ' ')), "blob must be a JSON array (<=3500B)",
       "an oversize blob is refused");
  c.eq(s.replaceBlob(R"([{"name":"ws","prov":"mistral","kind":"builtin","type":"web_search","en":1}])"),
       "", "a valid blob replace saves");

  // Reload from disk: the write was atomic and 0600.
  ConnectorsStore s2;
  s2.setPath(dir + "/connectors.json");
  s2.load();
  c.ok(has(sanitized(s2), "\"name\":\"ws\""), "a reload sees the persisted set");
  struct stat st{};
  c.ok(::stat((dir + "/connectors.json").c_str(), &st) == 0 &&
           (st.st_mode & 0777) == 0600,
       "the blob file is 0600 (it can carry pasted tokens)");

  // patch error strings match the device byte-for-byte.
  c.eq(s.patchUpsert(""), "patch must be a JSON object", "empty patch refused");
  c.eq(s.patchUpsert(R"({"prov":"openai"})"), "patch needs a name", "nameless patch refused");

  ndtest::rmTree(dir);
}

static void checkParseAndAttach(ndtest::Ctx& c) {
  const std::string dir = ndtest::scratchDir("conns-attach");
  fsutil::mkdirs(dir);
  ConnectorsStore s;
  s.setPath(dir + "/connectors.json");
  c.eq(s.replaceBlob(
           R"([{"name":"github","type":"github","prov":"openai","kind":"mcp","url":"https://api.githubcopilot.com/mcp/","tok":"ghp_x","en":1},)"
           R"({"name":"web_search","prov":"mistral","kind":"builtin","type":"web_search","en":1},)"
           R"({"name":"off","prov":"openai","kind":"mcp","url":"https://example.com/mcp","en":0}])"),
       "", "a three-entry blob (one disabled) saves");

  auto cs = s.parsed();
  c.eqi((long)cs.size(), 3, "parse yields every entry (enabled or not)");

  // The bearer seam resolves the stored token by name (T2) and "" otherwise.
  nimbus::orch::BearerFn bearer = [&s](const nimbus::orch::ConnectorInfo& ci) {
    return s.bearerForName(ci.name);
  };
  c.eq(bearer(cs[0]), "ghp_x", "bearer resolves the stored static token");
  c.eq(bearer(cs[1]), "", "a builtin resolves no bearer");

  // OpenAI wire: the enabled MCP entry rides with its label + auth; the disabled
  // one does not.
  JsonDocument oa;
  nimbus::orch::attachOpenAIWire(oa, cs, bearer, /*builtinsOnly=*/false);
  std::string oaS;
  serializeJson(oa, oaS);
  c.ok(has(oaS, "\"type\":\"mcp\"") && has(oaS, "\"server_label\":\"github\""),
       "openai attach carries the mcp entry with its server_label");
  c.ok(has(oaS, "ghp_x"), "openai attach carries the bearer authorization");
  c.ok(!has(oaS, "example.com"), "a disabled entry never rides the wire");

  // Mistral wire: the builtin rides as a bare type entry.
  JsonDocument mi;
  nimbus::orch::attachMistralWire(mi, cs, /*builtinsOnly=*/false);
  std::string miS;
  serializeJson(mi, miS);
  c.ok(has(miS, "\"type\":\"web_search\""), "mistral attach carries the builtin");

  // The catalog composer renders a non-empty block for a configured set.
  nimbus::orch::ProviderState ps;
  ps.openaiKeyed = true;
  ps.capProbe = 0;
  ps.currentHost = "openai";
  c.ok(!nimbus::orch::catalogText(cs, ps).empty(),
       "catalogText renders a block for a configured set");
  ndtest::rmTree(dir);
}

static void checkRouterRoute(ndtest::Ctx& c) {
  // CUM-425: the cumulo head resolves the router path from the model selector
  // through the SAME portable rule as the device head and sub-session adapter.
  auto zai = nimbus::orch::resolveRouterRoute("zai/glm-4.5-flash");
  c.eq(zai.basePath, "/router/zai", "zai selector routes to the zai upstream (no /v1)");
  c.eq(zai.model, "glm-4.5-flash", "the router prices the bare model id");
  auto bare = nimbus::orch::resolveRouterRoute("gpt-5.6");
  c.eq(bare.basePath, "/router/openai/v1", "a bare id keeps the openai upstream");
  c.eq(bare.model, "gpt-5.6", "a bare id is sent unchanged");
}

int main() {
  ndtest::Ctx c;
  std::printf("=== connectors registry (T1, offline, CUM-424/425) ===\n");
  checkStoreOps(c);
  checkParseAndAttach(c);
  checkRouterRoute(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
