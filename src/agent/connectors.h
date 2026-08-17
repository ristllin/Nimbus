#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// connectors - per-provider external-tool wiring (Phase C). The OWNER configures
// connectors once (web UI -> one NVS JSON blob, store::connectorsJson); the
// firmware then (a) ATTACHES them to provider requests and (b) SURFACES an
// honest per-provider catalog into the model's context so it can choose
// providers/spawns knowingly.
//
// Auth tiers (verified against provider docs 2026-07, see docs/plan):
//   T1 Mistral: connectors/custom MCP are authenticated in STUDIO (workspace) -
//      the API references them by name; the device stores NO secret.
//   T2 static token: MCP servers that take a long-lived bearer (e.g. GitHub's
//      MCP endpoint + a PAT) - one owner paste, NVS, same rail as API keys.
//   T3 OAuth refresh broker: connectors needing short-lived tokens (Gmail-class
//      on the OpenAI/Anthropic APIs) - the blob stores {refresh URL, client id,
//      client secret, refresh token}; bearerFor() mints/caches access tokens.
//
// RAILS: the blob is writable ONLY via the token-gated web endpoint; the model's
// `connector` config key is protected-BLOCKED in orch_device_actions.cpp.
//
// Blob shape (JSON array, ~a handful of entries):
//   [{"name":"github","prov":"openai","kind":"mcp",
//     "url":"https://api.githubcopilot.com/mcp/","tok":"ghp_...","en":1},
//    {"name":"web_search","prov":"mistral","kind":"builtin","en":1},
//    {"name":"gmail","prov":"openai","kind":"connector","cid":"connector_gmail",
//     "oauth":{"rurl":"https://oauth2.googleapis.com/token","cid":"..","sec":"..",
//              "rtok":".."},"en":1}]

namespace agent {
namespace connectors {

struct Info {
  String name;         // display + server_label
  String prov;         // "openai" | "anthropic" | "mistral" | "any"
  String kind;         // "builtin" (Mistral) | "mcp" (remote server) | "connector" (OpenAI first-party)
  String url;          // MCP server URL (kind=mcp)
  String connectorId;  // OpenAI first-party connector id (kind=connector)
  String type;         // known-catalog id (UI/docs join); "" -> use name
  String tok;          // static bearer (T2); "" if none
  bool   enabled = false;
  bool   hasOauth = false;   // T3 broker fields present
  String oauthUrl, oauthClientId, oauthSecret, oauthRefresh;
};

// The most connector entries any consumer loads. ⚠ Every call site MUST use
// this, and heap-allocate the array - an Info is ~11 Strings (~200 B), so a
// kMaxConnectors stack array is ~3 KB, which does not fit tg_poll (recurring
// stack-overflow panics) or the AsyncTCP task (an Info[12] on it reset the
// connection - see web_memory.cpp). Found live (Board 1, 2026-08-09): the old
// hardcoded Info[8] at three sites silently dropped every blob entry past the
// eighth - slack, linear and a freshly-added openai code_interpreter simply did
// not exist to the catalog OR the wire attach, while /api/connectors (which
// parses the blob directly) showed them fine. The blob cap (3500 B) bounds a
// realistic set well under this.
constexpr int kMaxConnectors = 24;

// Parse the NVS blob into out[]; returns the count (<= maxN). Malformed entries
// are skipped, never fatal.
int list(Info out[], int maxN);

// The bearer for a connector: static tok, or a broker-minted OAuth access token
// (cached in RAM until ~60 s before expiry; refreshed via one TLS POST). Returns
// "" when no auth is configured/available (attach proceeds unauthenticated).
String bearerFor(const Info& c);
// W12: live OAuth outcome for a connector since boot - 1 = mint succeeded,
// 0 = mint FAILED (credential needs the owner), -1 = no signal yet.
int8_t authStateOf(const String& name);

// Attach enabled connectors to a provider request body (call before serialize):
//   OpenAI (head + sub dispatch): tools[] += {type:"mcp", server_label/url or
//     connector_id, authorization?, require_approval:"never"}.
//   Mistral (head single-shot): tools[] += {type:"<name>"} for built-ins AND
//     Studio-named connectors (authenticated in Studio, referenced by name).
//   Anthropic (managed-agent creation body): mcp_servers[] += {type:"url", url,
//     name, authorization_token?} - sub-agents only (the head is one forced tool).
// The pure JSON-building + catalog logic is host-tested in lib/core
// (nimbus::orch::*Wire / catalogText); these are the thin device wrappers that
// parse the NVS blob and supply the bearer closure.
void attachOpenAI(JsonDocument& d);
void attachMistral(JsonDocument& d);
void attachAnthropic(JsonDocument& agentBody);

// "[PROVIDERS & CONNECTORS]" - the honest, provider-aware capability catalog for
// the model's context: key present/verified, native tool surface, enabled
// connectors, and which are callable on the current provider's own turns vs only
// via a sub-agent spawn.
String catalog();

// The known-connector catalog (Tier-1 + Mistral built-ins) as a JSON array, for
// GET /api/connectors so the UI can describe/link not-yet-configured connectors.
String knownCatalog();

}  // namespace connectors
}  // namespace agent
