#pragma once
#include <ArduinoJson.h>

#include <functional>
#include <string>
#include <vector>

// connectors_wire - the PORTABLE half of the connectors subsystem (host-tested
// via pio test -e native). It owns the pure decisions: given a parsed connector
// list + provider state, (a) build the per-provider request-body attach JSON and
// (b) render the model-facing catalog text. NO Arduino / store / TLS here - the
// device layer (src/agent/connectors.cpp) parses the NVS blob into ConnectorInfo,
// supplies the bearer closure (static token or OAuth-minted), and calls these.
//
// The three attach seams mirror where each provider actually runs connectors:
//   OpenAI    - Responses `tools[]` on both the head turn AND sub-agent dispatch
//               ({type:mcp, server_url|connector_id, authorization?}).
//   Mistral   - Conversations `tools[]` on the head single-shot turn AND on
//               sub-agent dispatch (mistralDispatch → POST /v1/conversations →
//               attachMistral): built-ins {type:<name>} + Studio-named
//               connectors both attach there. The HEAD TOOL-LOOP does NOT - it
//               runs /v1/chat/completions and forces tool_choice, which 422s
//               with built-in connectors (a documented Mistral limitation, not
//               a bug); the head's own web search on loop turns is the registry
//               `web.search` tool (Tavily), so a keyless Tavily means the head
//               must DELEGATE web work to a mistral sub-agent, which has it.
//   Anthropic - managed-agent creation body `mcp_servers[]` (sub-agents only;
//               the head is one forced tool, so MCP can't run mid-turn).

namespace nimbus {
namespace orch {

// A connector as the model/wire sees it - no secrets. `type` is the stable
// known-catalog id (defaults to `name` when the blob omits it).
struct ConnectorInfo {
  std::string name;         // display + OpenAI server_label / Mistral tool type
  std::string prov;         // "openai" | "anthropic" | "mistral" | "any"
  std::string kind;         // "builtin" | "mcp" | "connector"
  std::string url;          // remote MCP server URL (kind=mcp)
  std::string connectorId;  // OpenAI first-party connector id (kind=connector)
  std::string type;         // known-catalog id (UI/docs join); "" -> use name
  bool enabled = false;
  // W12: credential state (no secrets): -1 = no live signal / not applicable
  // (builtins auth provider-side), 1 = a credential is present and (for OAuth)
  // provably minted this boot, 0 = the last OAuth sign-in FAILED, 2 = a
  // credential is REQUIRED but missing (the attach skips this connector).
  int8_t auth = -1;
  // Credential PRESENCE (no secret value): whether the blob entry carries a
  // static token / OAuth broker fields. These are the same has-flags the web GET
  // already exposes, and let the portable parser compute `auth` without the
  // device secret-parse. Set by parseConnectorsJson.
  bool hasToken = false;
  bool hasOauth = false;
  // N4: the DEVICE dials this remote MCP server directly (blob "dev":1), as
  // opposed to only attaching it to a provider's request body. Only meaningful
  // for kind=="mcp".
  bool deviceDialed = false;
  // N4: the owner has APPROVED this server for device-side use (blob "appr":1).
  // Fail-closed: an unapproved device-dialed server is never dialed and its
  // tools are never registered. Set by parseConnectorsJson.
  bool approved = false;
};

// Parse the connectors NVS blob (a JSON array of entries; shape in connectors.h)
// into ConnectorInfo, NON-SECRET fields only (the device resolves the actual
// token/OAuth secrets separately). Returns the number written to `out` (<= maxN).
// If `totalEntries` is non-null it receives the number of array elements SEEN -
// including any past maxN and any nameless ones - so a caller can detect and
// LOUD-LOG a silent drop (the Info[8]-vs-kMaxConnectors regression). A malformed
// or non-array blob yields 0. Nameless entries are skipped (not written) but are
// still counted in totalEntries. This is the single no-silent-drop parser shared
// by the catalog/attach path and locked by host tests.
int parseConnectorsJson(const char* blobJson, std::vector<ConnectorInfo>& out,
                        int maxN, int* totalEntries = nullptr);

// Resolves the bearer for one connector (device: static tok or OAuth mint).
// Returns "" when unauthenticated (attach proceeds; provider surfaces the 401).
using BearerFn = std::function<std::string(const ConnectorInfo&)>;

// Provider availability + the resolved current head, for catalog framing.
struct ProviderState {
  bool openaiKeyed = false;
  bool anthropicKeyed = false;
  bool mistralKeyed = false;
  // Verify-cache result per provider: 1 = a live call succeeded, 0 = a live call
  // was REJECTED (bad key / no access), -1 = never checked. Lets the catalog say
  // "verified" vs "key present, unverified" vs "key REJECTED" instead of trusting
  // key-presence as if it were validity (the owner's "truly tested, not guessed").
  int8_t openaiVerified = -1;
  int8_t anthropicVerified = -1;
  int8_t mistralVerified = -1;
  // Capability-validation mode (W3b, from store::capProbe): 0 = off (trust key
  // presence, make NO "verified" claim), 1 = passive (default - report the verify
  // cache), 2 = active (passive + periodic re-verify). Only mode 0 changes the
  // catalog text; 1 and 2 render identically (both read the same cache).
  int8_t capProbe = 1;
  std::string currentHost;  // "openai" | "anthropic" | "mistral" | custom | ""
};

// A known connector the UI can describe / link even before it is configured.
struct KnownConnector {
  const char* id;             // stable catalog id (matches ConnectorInfo.type)
  const char* displayName;    // "GitHub"
  const char* providers;      // comma list where it can attach, e.g. "openai,anthropic,mistral"
  const char* kind;           // suggested kind: "mcp" | "connector" | "builtin"
  const char* connectorId;    // default OpenAI first-party connector_id ("" if none)
  const char* credentialLabel;// what the owner pastes, e.g. "GitHub PAT (ghp_…)"
  const char* oneLine;        // one-line capability blurb
  const char* docsSlug;       // anchor into docs/connectors.md, e.g. "github"
  const char* caps;           // real per-provider capability/limit surfaced to the
                              // model + UI, e.g. "Mistral: draft/read only - NO send.
                              // OpenAI (send-scoped token): send." "" = no caveat.
};

// The shipped Tier-1 catalog (+ Mistral built-ins). Returned by ref; count out.
const KnownConnector* knownConnectors(int& countOut);

// --- prov routing guard (CUM-255) ---------------------------------------------
// A LAN/private MCP URL must NEVER be forwarded to a provider's cloud head: the
// head cannot reach a private address and the request dies HTTP 424, killing the
// whole turn. Found live (CUM-61): a device-dialed entry with `prov` omitted
// defaulted to "any", so attachOpenAIWire forwarded its LAN URL to OpenAI. The
// guard is fail-closed and lives at the config/attach level: a bad entry degrades
// THAT tool, never the turn.

// True only when `url` is an address a provider's cloud could actually dial: an
// http/https URL whose host is not loopback, a private/link-local IP literal, or
// an mDNS `.local` name. Empty / non-http(s) / private -> false. Pure, host-tested.
bool urlRoutableToProviderHead(const std::string& url);

// The single routing predicate the three attach builders use. Returns true only
// when connector `c` may be forwarded to the named provider head
// ("openai"|"anthropic"|"mistral"). Fail-closed:
//   - disabled or prov mismatch (an unknown/future prov matches NO head) -> false;
//   - a device-dialed entry left at the default prov "any" is device-side only
//     (the device dials it; it is never handed to a head) -> false;
//   - a kind=="mcp" entry whose URL is not cloud-routable -> false.
bool forwardsToProviderHead(const ConnectorInfo& c, const char* head);

// Config-time validation for one connector entry (empty = safe to save). Returns
// a short, owner-facing error when the entry would be forwarded to a provider
// head but carries a private/unroutable URL, so the misconfig fails at SAVE time
// with a clear next step, not mid-turn. Callers (the token-gated connectors save
// endpoint) reject the save and surface the string.
std::string connectorConfigError(const ConnectorInfo& c);

// --- attach builders (append to an existing request/agent JsonDocument) -------
void attachOpenAIWire(JsonDocument& d, const std::vector<ConnectorInfo>& cs, const BearerFn& bearer);
void attachMistralWire(JsonDocument& d, const std::vector<ConnectorInfo>& cs);
void attachAnthropicWire(JsonDocument& agentBody, const std::vector<ConnectorInfo>& cs,
                         const BearerFn& bearer);

// --- model + UI text ----------------------------------------------------------
// The "[PROVIDERS & CONNECTORS]" block injected into every turn's context,
// provider-aware: the current host's connectors are marked callable on the
// model's OWN turns; the rest are reachable only via a sub-agent spawn.
std::string catalogText(const std::vector<ConnectorInfo>& cs, const ProviderState& ps);

// The known-catalog as a JSON array string, for GET /api/connectors.
std::string knownCatalogJson();

// --- capability scope (CUM-159) ----------------------------------------------
// Where a connector capability is reachable FROM. This is the machine-readable
// sibling of the "callable on YOUR OWN turns vs reachable only by spawning a
// sub-agent" prose that catalogText() renders (orch_connectors_wire.cpp) - the
// two encode the SAME rule and must stay in sync. The device web UI Capabilities
// table badges each connector row with one of these:
//   OrchestratorDirect - the head can use it on its own turns: a keyed + enabled
//                        connector on the CURRENT host provider (credential ok).
//   SubsessionsOnly    - reachable ONLY by spawning a sub-agent on the connector's
//                        provider: a keyed + enabled connector on a NON-host one.
//   Unavailable        - not usable now: provider unkeyed, connector disabled, or
//                        its credential failed / is missing.
enum class CapScope : uint8_t { OrchestratorDirect = 0, SubsessionsOnly = 1, Unavailable = 2 };

// Machine slug (FROZEN once shipped - it rides /api/tools and HIL reads it) and
// the short human label for the web badge.
const char* capScopeSlug(CapScope s);
const char* capScopeLabel(CapScope s);

// Classify one connector for a head currently running on ps.currentHost. Pure and
// host-tested; mirrors the callable-here-vs-spawn decision in catalogText().
CapScope connectorScope(const ConnectorInfo& c, const ProviderState& ps);

}  // namespace orch
}  // namespace nimbus
