#pragma once
#include <string>
#include <vector>

// Portable, host-testable parsers for the per-key ACCESS AUDIT: given a provider's
// own API responses, normalize what the key/account can actually reach. No Arduino
// deps (ArduinoJson v7 is header-only, host-safe). The device seam (src/agent) does
// the TLS fetch and feeds raw bodies here; the same code path is asserted by
// test/test_provider_audit so the device and test can't drift.
//
// Why this exists: the harness used to ASSUME connector ids/access (e.g. Mistral's
// GitHub connector was addressed by the name "github_app" and a default id, not the
// account's real UUID), so a key silently lacked a service the harness thought it
// had. This turns "assume" into "ask the provider".
namespace core {

struct AuditConnector {
  std::string name;      // e.g. "github_app"
  std::string id;        // the real connector UUID to pass as connector_id
  std::string protocol;  // e.g. "mcp"
};

// Parse a Mistral `GET /v1/connectors` body ({"items":[{id,name,protocol,...}]}) into
// out[]. Returns the number appended. Tolerant of missing fields / non-list bodies.
int parseMistralConnectors(const char* json, std::vector<AuditConnector>& out);

// Parse a `GET /v1/models` body ({"data":[{"id":...}]}) into model-id strings. Mistral,
// OpenAI, and Anthropic all use this shape. Returns the number appended.
int parseModelsList(const char* json, std::vector<std::string>& out);

// Find a connector by name in a parsed list; returns its id ("" if absent). Used to
// self-heal a device connector's configured id against the account's real one.
std::string connectorIdByName(const std::vector<AuditConnector>& cs, const std::string& name);

}  // namespace core
