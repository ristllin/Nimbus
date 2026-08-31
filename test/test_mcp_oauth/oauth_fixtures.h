// GENERATED FIXTURE - do not edit by hand.
// Real OAuth 2.1 discovery documents captured 2026-08-31 (curl) from the remote
// target named in CUM-256's DoD, https://mcp.linear.app - used to prove the
// portable OAuth decision layer parses a REAL authorization server, not just
// synthetic input. No credentials appear in any captured body (these are public
// .well-known metadata documents; the 401 header carries only a public hint URL).
#pragma once

namespace oauthfix {

// GET https://mcp.linear.app/.well-known/oauth-protected-resource
inline const char* kLinearProtectedResource =
    R"OAUTH({"resource":"https://mcp.linear.app/mcp","authorization_servers":["https://mcp.linear.app"],"scopes_supported":["read","write"],"bearer_methods_supported":["header"]})OAUTH";

// The `WWW-Authenticate` header value on a 401 from POST https://mcp.linear.app/mcp
// with no credential (RFC 9728 §5.1 resource_metadata hint).
inline const char* kLinearWwwAuthenticate =
    R"OAUTH(Bearer realm="OAuth", resource_metadata="https://mcp.linear.app/.well-known/oauth-protected-resource/mcp", error="invalid_token", error_description="Missing or invalid access token")OAUTH";

// GET https://mcp.linear.app/.well-known/oauth-authorization-server
inline const char* kLinearAuthServer =
    R"OAUTH({"issuer":"https://mcp.linear.app","authorization_endpoint":"https://mcp.linear.app/authorize","token_endpoint":"https://mcp.linear.app/token","registration_endpoint":"https://mcp.linear.app/register","scopes_supported":["read","write","openid","email"],"response_types_supported":["code"],"response_modes_supported":["query"],"grant_types_supported":["authorization_code","refresh_token","urn:ietf:params:oauth:grant-type:jwt-bearer"],"authorization_grant_profiles_supported":["urn:ietf:params:oauth:grant-profile:id-jag"],"token_endpoint_auth_methods_supported":["client_secret_basic","client_secret_post","none"],"revocation_endpoint":"https://mcp.linear.app/token","code_challenge_methods_supported":["S256"],"client_id_metadata_document_supported":true,"resource":"https://mcp.linear.app/mcp","resource_metadata":"https://mcp.linear.app/.well-known/oauth-protected-resource/mcp"})OAUTH";

// A representative RFC 7591 registration SUCCESS response (public client, no
// secret). Synthetic (registration is a POST we cannot capture without consent),
// but shaped exactly per RFC 7591 §3.2.1.
inline const char* kRegistrationOk =
    R"OAUTH({"client_id":"cid_abc123","client_id_issued_at":1756640000,"redirect_uris":["http://nimbus.local/oauth/cb"],"grant_types":["authorization_code","refresh_token"],"response_types":["code"],"token_endpoint_auth_method":"none","client_name":"Nimbus"})OAUTH";

// A token endpoint SUCCESS response (RFC 6749 §5.1). Synthetic; tokens are fake.
inline const char* kTokenOk =
    R"OAUTH({"access_token":"at_fake_value","token_type":"Bearer","expires_in":3600,"refresh_token":"rt_fake_value","scope":"read write"})OAUTH";

// A token endpoint ERROR response (RFC 6749 §5.2).
inline const char* kTokenError =
    R"OAUTH({"error":"invalid_grant","error_description":"authorization code expired"})OAUTH";

}  // namespace oauthfix
