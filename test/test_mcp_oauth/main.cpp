#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/orch/mcp_oauth.h"
#include "oauth_fixtures.h"

namespace o = nimbus::orch::mcp::oauth;

void setUp() {}
void tearDown() {}

// ---- PKCE (RFC 7636) ---------------------------------------------------------

void test_pkce_all_zero_bytes() {
  uint8_t rand[32] = {0};
  o::Pkce p = o::makePkce(rand);
  // base64url of 32 zero bytes = 43 'A's (no padding).
  TEST_ASSERT_EQUAL_STRING("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", p.verifier.c_str());
  TEST_ASSERT_EQUAL(43u, (unsigned)p.verifier.size());
  TEST_ASSERT_EQUAL_STRING("S256", p.method.c_str());
  // challenge = base64url(sha256(verifier)); 32-byte digest -> 43 base64url chars.
  TEST_ASSERT_EQUAL(43u, (unsigned)p.challenge.size());
  TEST_ASSERT_TRUE(p.challenge != p.verifier);
}

void test_pkce_verifier_charset_is_url_safe() {
  uint8_t rand[32];
  for (int i = 0; i < 32; i++) rand[i] = (uint8_t)(i * 7 + 3);
  o::Pkce p = o::makePkce(rand);
  for (char c : p.verifier + p.challenge) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_';
    TEST_ASSERT_TRUE(ok);
  }
}

// ---- percent / form encoding -------------------------------------------------

void test_pct_encode() {
  TEST_ASSERT_EQUAL_STRING("a%20b", o::pctEncode("a b").c_str());
  TEST_ASSERT_EQUAL_STRING("a-b_c.d~e", o::pctEncode("a-b_c.d~e").c_str());
  TEST_ASSERT_EQUAL_STRING("https%3A%2F%2Fmcp.linear.app%2Fmcp",
                           o::pctEncode("https://mcp.linear.app/mcp").c_str());
  TEST_ASSERT_EQUAL_STRING("%26%3D%3F%23", o::pctEncode("&=?#").c_str());
}

void test_form_encode_omits_empty() {
  std::string body = o::formEncode({{"a", "1"}, {"secret", ""}, {"b", "x y"}});
  TEST_ASSERT_EQUAL_STRING("a=1&b=x%20y", body.c_str());
}

// ---- discovery over the REAL Linear metadata ---------------------------------

void test_www_authenticate_hint() {
  std::string url = o::resourceMetadataFromWwwAuth(oauthfix::kLinearWwwAuthenticate);
  TEST_ASSERT_EQUAL_STRING("https://mcp.linear.app/.well-known/oauth-protected-resource/mcp", url.c_str());
  TEST_ASSERT_EQUAL_STRING("", o::resourceMetadataFromWwwAuth("Bearer realm=\"x\"").c_str());
}

void test_well_known_derivation() {
  TEST_ASSERT_EQUAL_STRING("https://mcp.linear.app/.well-known/oauth-protected-resource",
                           o::wellKnownProtectedResource("https://mcp.linear.app/mcp").c_str());
  TEST_ASSERT_EQUAL_STRING("https://mcp.linear.app/.well-known/oauth-authorization-server",
                           o::wellKnownAuthServer("https://mcp.linear.app").c_str());
  // port preserved, non-http rejected
  TEST_ASSERT_EQUAL_STRING("https://ex.com:8443/.well-known/oauth-protected-resource",
                           o::wellKnownProtectedResource("https://ex.com:8443/mcp/v1").c_str());
  TEST_ASSERT_EQUAL_STRING("", o::wellKnownProtectedResource("ftp://x/y").c_str());
  TEST_ASSERT_EQUAL_STRING("", o::wellKnownProtectedResource("not a url").c_str());
}

void test_parse_protected_resource_real() {
  o::ProtectedResourceMeta m = o::parseProtectedResourceMetadata(oauthfix::kLinearProtectedResource);
  TEST_ASSERT_TRUE(m.ok);
  TEST_ASSERT_EQUAL_STRING("https://mcp.linear.app/mcp", m.resource.c_str());
  TEST_ASSERT_EQUAL(1u, (unsigned)m.authorizationServers.size());
  TEST_ASSERT_EQUAL_STRING("https://mcp.linear.app", m.authorizationServers[0].c_str());
}

void test_parse_auth_server_real() {
  o::AuthServerMeta m = o::parseAuthServerMetadata(oauthfix::kLinearAuthServer);
  TEST_ASSERT_TRUE(m.ok);
  TEST_ASSERT_EQUAL_STRING("https://mcp.linear.app/authorize", m.authorizationEndpoint.c_str());
  TEST_ASSERT_EQUAL_STRING("https://mcp.linear.app/token", m.tokenEndpoint.c_str());
  TEST_ASSERT_EQUAL_STRING("https://mcp.linear.app/register", m.registrationEndpoint.c_str());
  TEST_ASSERT_TRUE(o::supportsS256(m));
}

void test_auth_server_missing_endpoints_not_ok() {
  o::AuthServerMeta m = o::parseAuthServerMetadata(R"({"issuer":"https://x","authorization_endpoint":"https://x/a"})");
  TEST_ASSERT_FALSE(m.ok);  // no token_endpoint
  o::AuthServerMeta bad = o::parseAuthServerMetadata("not json");
  TEST_ASSERT_FALSE(bad.ok);
  o::AuthServerMeta noS256 = o::parseAuthServerMetadata(
      R"({"authorization_endpoint":"https://x/a","token_endpoint":"https://x/t","code_challenge_methods_supported":["plain"]})");
  TEST_ASSERT_TRUE(noS256.ok);
  TEST_ASSERT_FALSE(o::supportsS256(noS256));  // never silently downgrade to plain
}

// ---- registration ------------------------------------------------------------

void test_build_registration_request() {
  o::RegistrationParams p;
  p.redirectUris = {"http://nimbus.local/oauth/cb"};
  std::string body = o::buildRegistrationRequest(p);
  TEST_ASSERT_TRUE(body.find("\"client_name\":\"Nimbus\"") != std::string::npos);
  TEST_ASSERT_TRUE(body.find("\"token_endpoint_auth_method\":\"none\"") != std::string::npos);
  TEST_ASSERT_TRUE(body.find("http://nimbus.local/oauth/cb") != std::string::npos);
  TEST_ASSERT_TRUE(body.find("authorization_code") != std::string::npos);
  TEST_ASSERT_TRUE(body.find("refresh_token") != std::string::npos);
}

void test_parse_registration_ok_and_error() {
  o::RegistrationResult r = o::parseRegistrationResponse(oauthfix::kRegistrationOk);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_STRING("cid_abc123", r.clientId.c_str());
  TEST_ASSERT_EQUAL_STRING("", r.clientSecret.c_str());  // public client
  o::RegistrationResult err = o::parseRegistrationResponse(R"({"error":"invalid_redirect_uri"})");
  TEST_ASSERT_FALSE(err.ok);
  TEST_ASSERT_EQUAL_STRING("invalid_redirect_uri", err.error.c_str());
  o::RegistrationResult none = o::parseRegistrationResponse(R"({"redirect_uris":["x"]})");
  TEST_ASSERT_FALSE(none.ok);  // no client_id
}

// ---- authorize URL -----------------------------------------------------------

void test_build_authorize_url() {
  o::AuthorizeParams p;
  p.authorizationEndpoint = "https://mcp.linear.app/authorize";
  p.clientId = "cid_abc123";
  p.redirectUri = "http://nimbus.local/oauth/cb";
  p.codeChallenge = "CHAL";
  p.state = "st8/9";
  p.resource = "https://mcp.linear.app/mcp";
  std::string url = o::buildAuthorizeUrl(p);
  TEST_ASSERT_TRUE(url.rfind("https://mcp.linear.app/authorize?", 0) == 0);
  TEST_ASSERT_TRUE(url.find("response_type=code") != std::string::npos);
  TEST_ASSERT_TRUE(url.find("client_id=cid_abc123") != std::string::npos);
  TEST_ASSERT_TRUE(url.find("code_challenge=CHAL") != std::string::npos);
  TEST_ASSERT_TRUE(url.find("code_challenge_method=S256") != std::string::npos);
  TEST_ASSERT_TRUE(url.find("redirect_uri=http%3A%2F%2Fnimbus.local%2Foauth%2Fcb") != std::string::npos);
  TEST_ASSERT_TRUE(url.find("state=st8%2F9") != std::string::npos);
  TEST_ASSERT_TRUE(url.find("resource=https%3A%2F%2Fmcp.linear.app%2Fmcp") != std::string::npos);
}

void test_authorize_url_preserves_existing_query() {
  o::AuthorizeParams p;
  p.authorizationEndpoint = "https://as.example/auth?foo=bar";
  p.clientId = "c";
  p.redirectUri = "http://d/cb";
  p.codeChallenge = "X";
  p.state = "s";
  std::string url = o::buildAuthorizeUrl(p);
  TEST_ASSERT_TRUE(url.find("https://as.example/auth?foo=bar&response_type=code") != std::string::npos);
}

// ---- token endpoint ----------------------------------------------------------

void test_build_code_exchange_form() {
  o::CodeExchangeParams p;
  p.code = "authcode";
  p.codeVerifier = "verifier123";
  p.clientId = "cid";
  p.redirectUri = "http://nimbus.local/oauth/cb";
  p.resource = "https://mcp.linear.app/mcp";
  std::string body = o::buildCodeExchangeForm(p);
  TEST_ASSERT_TRUE(body.find("grant_type=authorization_code") != std::string::npos);
  TEST_ASSERT_TRUE(body.find("code=authcode") != std::string::npos);
  TEST_ASSERT_TRUE(body.find("code_verifier=verifier123") != std::string::npos);
  TEST_ASSERT_TRUE(body.find("client_secret") == std::string::npos);  // public client: omitted
  TEST_ASSERT_TRUE(body.find("resource=https%3A%2F%2Fmcp.linear.app%2Fmcp") != std::string::npos);
}

void test_build_refresh_form() {
  o::RefreshParams p;
  p.refreshToken = "rt";
  p.clientId = "cid";
  std::string body = o::buildRefreshForm(p);
  TEST_ASSERT_TRUE(body.find("grant_type=refresh_token") != std::string::npos);
  TEST_ASSERT_TRUE(body.find("refresh_token=rt") != std::string::npos);
}

void test_parse_token_ok_and_error() {
  o::TokenResponse ok = o::parseTokenResponse(oauthfix::kTokenOk);
  TEST_ASSERT_TRUE(ok.ok);
  TEST_ASSERT_EQUAL_STRING("at_fake_value", ok.accessToken.c_str());
  TEST_ASSERT_EQUAL_STRING("rt_fake_value", ok.refreshToken.c_str());
  TEST_ASSERT_EQUAL(3600, ok.expiresIn);
  TEST_ASSERT_EQUAL_STRING("Bearer", ok.tokenType.c_str());

  o::TokenResponse err = o::parseTokenResponse(oauthfix::kTokenError);
  TEST_ASSERT_FALSE(err.ok);
  TEST_ASSERT_EQUAL_STRING("invalid_grant", err.error.c_str());
  TEST_ASSERT_EQUAL_STRING("authorization code expired", err.errorDescription.c_str());

  o::TokenResponse garbage = o::parseTokenResponse("<html>502</html>");
  TEST_ASSERT_FALSE(garbage.ok);
  TEST_ASSERT_EQUAL_STRING("invalid_response", garbage.error.c_str());
}

// ---- flow planner (the class rule: table over every state) -------------------

void test_plan_next_step_table() {
  struct Row { o::FlowState s; o::Step want; };
  std::vector<Row> rows;
  o::FlowState base;  // all false
  rows.push_back({base, o::Step::DiscoverProtectedResource});
  { o::FlowState s = base; s.haveIssuer = true; rows.push_back({s, o::Step::DiscoverAuthServer}); }
  { o::FlowState s = base; s.haveIssuer = s.haveEndpoints = true; rows.push_back({s, o::Step::Register}); }
  { o::FlowState s = base; s.haveIssuer = s.haveEndpoints = s.haveClientId = true;
    rows.push_back({s, o::Step::Authorize}); }
  { o::FlowState s = base; s.haveIssuer = s.haveEndpoints = s.haveClientId = s.authorizeShown = true;
    rows.push_back({s, o::Step::AwaitConsent}); }
  { o::FlowState s = base; s.haveIssuer = s.haveEndpoints = s.haveClientId = s.authorizeShown = s.haveCode = true;
    rows.push_back({s, o::Step::Exchange}); }
  { o::FlowState s = base; s.haveRefreshToken = true; rows.push_back({s, o::Step::Done}); }
  { o::FlowState s = base; s.failed = true; rows.push_back({s, o::Step::Failed}); }
  // failed wins even with a token half-collected
  { o::FlowState s = base; s.haveIssuer = true; s.failed = true; rows.push_back({s, o::Step::Failed}); }
  for (const auto& r : rows)
    TEST_ASSERT_EQUAL((int)r.want, (int)o::planNextStep(r.s));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_pkce_all_zero_bytes);
  RUN_TEST(test_pkce_verifier_charset_is_url_safe);
  RUN_TEST(test_pct_encode);
  RUN_TEST(test_form_encode_omits_empty);
  RUN_TEST(test_www_authenticate_hint);
  RUN_TEST(test_well_known_derivation);
  RUN_TEST(test_parse_protected_resource_real);
  RUN_TEST(test_parse_auth_server_real);
  RUN_TEST(test_auth_server_missing_endpoints_not_ok);
  RUN_TEST(test_build_registration_request);
  RUN_TEST(test_parse_registration_ok_and_error);
  RUN_TEST(test_build_authorize_url);
  RUN_TEST(test_authorize_url_preserves_existing_query);
  RUN_TEST(test_build_code_exchange_form);
  RUN_TEST(test_build_refresh_form);
  RUN_TEST(test_parse_token_ok_and_error);
  RUN_TEST(test_plan_next_step_table);
  return UNITY_END();
}
