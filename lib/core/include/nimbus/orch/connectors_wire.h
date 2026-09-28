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
  // credential is REQUIRED but missing (the attach skips this connector). For a
  // Mistral Studio connector 1/2 come from the workspace probe (see `workspace`).
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
  // What the Mistral workspace probe says about a Mistral Studio connector
  // (prov=mistral, kind=connector); NotApplicable for every other entry. Stamped
  // by applyConnectorAuth together with `auth`. Only Listed* map to auth=1; the
  // SignedIn distinction is an informational hint for the catalog, never a gate.
  enum class Workspace : int8_t {
    NotApplicable = -1,
    Unprobed = 0,           // no workspace answer yet (fail-closed: auth=2)
    NotListed = 1,          // absent from GET /v1/connectors, or listed inactive
    ListedNotSignedIn = 2,  // listed + active, is_authenticated:false (usable)
    ListedSignedIn = 3,     // listed + active, is_authenticated:true (usable)
  };
  Workspace workspace = Workspace::NotApplicable;
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
  // Whether a Mistral HEAD turn carries the Mistral Studio connectors (true = the
  // device's historical wiring: they attach to the single-shot head turn). false =
  // Studio connectors run ONLY on spawned mistral sub-agents: the catalog says so and
  // connectorScope reports them SubsessionsOnly even with mistral as the head.
  // Measured 2026-09-27: a Conversations call that carries a Studio connector AND the
  // head's strict response_format json_schema never answers (Mistral's edge closes
  // the stream at 60 s, 3 of 3), while the same call without the schema returns real
  // calendar events in ~5 s - which is exactly the sub-agent's free-text call.
  bool mistralHeadCarriesStudio = true;
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
//   - any entry whose URL would ride the wire but is not cloud-routable -> false
//     (keyed on url PRESENCE, not kind, so a connector entry that falls back to
//     server_url cannot smuggle a private URL past the guard).
bool forwardsToProviderHead(const ConnectorInfo& c, const char* head);

// Config-time validation for one connector entry (empty = safe to save). Returns
// a short, owner-facing error when the entry would be forwarded to a provider
// head but carries a private/unroutable URL, so the misconfig can fail at SAVE
// time with a clear next step, not mid-turn. Intended caller: the token-gated
// connectors save endpoint (POST /api/connectors), which rejects the save and
// surfaces the string. The turn path does NOT depend on it - forwardsToProviderHead
// already keeps a bad entry off the head regardless of whether the save was gated.
std::string connectorConfigError(const ConnectorInfo& c);

// --- attach builders (append to an existing request/agent JsonDocument) -------
// `builtinsOnly` (CUM-242 x1 item 2): when true, ONLY provider built-in tools are
// advertised (kind=="builtin"); the account's PRIVATE connectors (remote MCP +
// first-party) are skipped. A Cumulo-routed session sets this - it runs under the
// shared router org key, so the owner's private MCP servers / first-party
// connectors (authenticated against the owner's OWN provider account) must not
// ride the request; only the generic built-ins do. A direct-key session leaves it
// false and advertises built-ins + the account's private connectors. This is the
// per-route advertisement split that lets the orchestrator distinguish what a
// cumulo/<model> head brings vs a direct <model> head.
void attachOpenAIWire(JsonDocument& d, const std::vector<ConnectorInfo>& cs, const BearerFn& bearer,
                      bool builtinsOnly = false);
void attachMistralWire(JsonDocument& d, const std::vector<ConnectorInfo>& cs,
                       bool builtinsOnly = false);
void attachAnthropicWire(JsonDocument& agentBody, const std::vector<ConnectorInfo>& cs,
                         const BearerFn& bearer, bool builtinsOnly = false);

// --- model + UI text ----------------------------------------------------------
// The "[PROVIDERS & CONNECTORS]" block injected into every turn's context,
// provider-aware: the current host's connectors are marked callable on the
// model's OWN turns; the rest are reachable only via a sub-agent spawn.
std::string catalogText(const std::vector<ConnectorInfo>& cs, const ProviderState& ps);

// The known-catalog as a JSON array string, for GET /api/connectors.
std::string knownCatalogJson();

// The Mistral workspace connector id a device connector maps to (the id Mistral's
// Conversations API + GET /v1/connectors use, e.g. gcal -> "google_calendar"). An
// owner-set explicit connector id wins UNLESS it is an OpenAI-namespace default
// ("connector_*" carried by the known-catalog); those map through the canonical
// table by type/name. Shared by attachMistralWire (the wire attach) and the
// workspace probe (which asks Mistral which of these its workspace lists), so the
// two never drift on the id.
std::string mistralConnectorId(const ConnectorInfo& c);

// --- Mistral workspace probe (the Studio-connector usability signal) ---------
// A Mistral Studio connector has no device credential: it is referenced by id on
// the Conversations request and Mistral runs it server-side. The honest usability
// signal is whether the configured key's WORKSPACE lists that connector as active
// in GET /v1/connectors. `is_authenticated` is NOT that signal: measured live
// (2026-09-27) a key whose listing says google_calendar is_authenticated:false ran
// google_calendar_list_events server-side and returned the owner's real events. So
// listed + active => usable (attach it and let Mistral enforce); is_authenticated
// is kept only as a hint ("results may come back empty until it is connected").
//
// Storage is two newline-delimited id sets ("\nname\nid\n...") rather than vectors
// of strings: the device keeps this for its whole uptime, and one small block per
// set fragments its scarce internal heap far less than ~40 tiny allocations.
struct MistralWorkspace {
  bool probed = false;   // a definitive workspace answer has landed
  std::string listed;    // ids (name AND uuid) of items listed AND active: usable
  std::string signedIn;  // the subset reporting is_authenticated:true (hint only)
  bool isListed(const std::string& id) const;
  bool isSignedIn(const std::string& id) const;
};

// Upper bound on items read from one probe. Above the probe's page_size (100), so a
// full page can never be truncated into a false "not listed".
constexpr int kMistralWorkspaceMaxItems = 128;

// Parse a GET /v1/connectors body ({items:[{id,name,active,is_authenticated,...}]}).
// `listedOut` receives the identifiers (the name and, when present, the uuid id -
// the Conversations API accepts either as connector_id) of every item that is not
// explicitly inactive (an absent `active` counts as active); `signedInOut`
// (optional) the identifiers of the subset whose is_authenticated is true. Returns
// false when the body is not the expected shape (not JSON, no items[] array: an
// HTTP/transient error) - the caller then keeps its last answer; that is "no
// signal", never "nothing listed".
bool parseMistralWorkspaceConnectors(const char* body, std::vector<std::string>& listedOut,
                                     std::vector<std::string>* signedInOut = nullptr);

// Fold one probe body into `ws`: a parseable body REPLACES the whole answer (a
// connector removed from the workspace drops out) and sets probed; an unparseable
// one leaves `ws` untouched. Returns whether the body parsed.
bool noteMistralWorkspaceProbe(MistralWorkspace& ws, const char* body);

// True for a Mistral Studio connector: prov=mistral, kind=connector.
bool isMistralStudioConnector(const ConnectorInfo& c);

// What the workspace answer says about one connector (NotApplicable unless it is a
// Mistral Studio connector). Matches on mistralConnectorId, the id the wire sends.
ConnectorInfo::Workspace mistralWorkspaceState(const ConnectorInfo& c, const MistralWorkspace& ws);

// The single credential-state rule every surface shares (model catalog, wire-side
// capability scope, GET /api/connectors, the Capabilities table) on the device AND
// on a hosted instance: builtin -1 (provider-side); static token 1; OAuth ->
// `oauthState` (the caller's live mint outcome, -1 when none yet); Mistral Studio
// connector -> 1 when listed + active in the workspace, else 2 (unprobed included:
// fail-closed); anything else 2 (credential required but missing).
int8_t connectorAuthFor(const ConnectorInfo& c, const MistralWorkspace& ws, int8_t oauthState);

// Stamp `auth` and `workspace` on every entry with the rule above. `oauthState`
// resolves an OAuth entry's live mint outcome (nullptr => -1, no live signal).
using OauthStateFn = std::function<int8_t(const ConnectorInfo&)>;
void applyConnectorAuth(std::vector<ConnectorInfo>& cs, const MistralWorkspace& ws,
                        const OauthStateFn& oauthState = nullptr);

// Whether any ENABLED entry is a Mistral Studio connector, i.e. whether the (large,
// ~300 KB) workspace probe is worth fetching at all.
bool wantsMistralWorkspaceProbe(const std::vector<ConnectorInfo>& cs);

// The head a turn actually runs on - the turn engine's resolution rule, shared so the
// catalog's "YOU are here" (ProviderState.currentHost) can never name a different
// head than the one the turn dispatches to: an explicit `orchHost` pin, else the
// first provider in the comma-separated `priority` for which `keyed(slug)` is true,
// else `routerFallback` (the router head that runs a keyless-BYOK instance, "" if
// none), else the bare first priority token. Tokens are whitespace-trimmed.
std::string resolveHeadHost(const std::string& orchHost, const std::string& priority,
                            const std::function<bool(const std::string&)>& keyed,
                            const std::string& routerFallback);

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
