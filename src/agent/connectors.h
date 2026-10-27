#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Forward declaration so the outbound-MCP sync() can take the registry by
// reference without this widely-included header pulling the whole registry TU.
namespace nimbus { namespace orch { class ToolRegistry; struct ConnectorInfo; struct PdfRoute; } }

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

// Fold a Mistral GET /v1/connectors response body into the live workspace answer:
// a Mistral Studio connector (gcal/notion/slack: no device credential by design) is
// usable once the configured key's workspace LISTS it as active (is_authenticated is
// kept only as a catalog hint - it reads false for connectors that demonstrably
// work). Called from the Mistral provider-verify path. A bad/empty body is ignored
// (keeps the last good answer) and returns false. No-op-safe to call with garbage.
// `generation` is mistralProbeGeneration() read BEFORE the fetch: if the Mistral key
// changed meanwhile (resetMistralConnectorsProbe), the stale answer is dropped.
bool noteMistralConnectorsProbe(const char* v1ConnectorsBody, uint32_t generation);
uint32_t mistralProbeGeneration();

// Whether an enabled Mistral Studio connector exists, i.e. whether the (large)
// workspace probe is worth fetching on a Mistral verify. Parses the blob through the
// portable parser (no Info[] array).
bool wantsMistralWorkspaceProbe();

// CUM-537: whether the workspace answer is stale for the CURRENT connector set (an
// enabled Studio connector it does not list as usable, or no answer yet) - the
// rule a connectors save and boot use to queue the workspace probe.
bool mistralWorkspaceStale();

// The web card's workspace slug for one connector ("", "checking", "unchecked",
// "unlisted"; see nimbus::orch::studioCardState), against the live answer.
const char* studioCardState(const nimbus::orch::ConnectorInfo& c, bool probePending);

// The provider slug running the enabled connector a model called by `name` as if
// it were a tool ("gcal" -> "mistral"; "" = no such connector). Feeds the
// unknown-tool recovery text (CUM-538); parses the blob only when called, so it
// costs nothing on the normal tool path.
std::string providerForName(const std::string& name);

// The honest credential state for one connector (-1 n/a builtin, 1 present / listed
// in the Mistral workspace, 0 mint failed, 2 missing / not listed / workspace not
// checked yet). One source of truth for the model catalog, the capability scope,
// GET /api/connectors, and the /api/tools Capabilities table, so the surfaces never
// disagree.
int8_t connectorAuthState(const nimbus::orch::ConnectorInfo& c);

// Forget the Mistral workspace answer (Studio connectors revert to auth=2 until the
// next verify re-probes). Call on a Mistral key change so a connector listed under the
// previous key's workspace cannot read usable under a new key.
void resetMistralConnectorsProbe();

// 22 Oct honesty (2): the automatic workspace check (probe queue) did not land, with
// the reason provider_verify logs ("low memory", "a turn is running", "tls busy",
// "no answer", ...). The card shows it as plain words until a check lands.
void noteWorkspaceCheckMiss(const char* why);
// The slow re-check: while an enabled Studio connector is unconfirmed, queue a fresh
// workspace check every 30 minutes (the probe queue gives up after a few quick
// tries). Called on every main-loop pass from mcp::oauth::pump().
void pumpWorkspaceRecheck();
// The connectors card's live status for GET /api/connectors: the last check miss
// (wsMiss slug + wsMissAgeS), seconds to the next slow re-check (wsRecheckS), and how
// many tools a Mistral request carries (mistralTools, warned on above
// mistralToolsAdvised).
void webStatusInto(JsonObject out);

// wf_plan 3: the one-time move of any stored Code Interpreter card into the Code
// sandbox switch (critique B2). Removes every code_interpreter entry from the blob
// (a smaller, shrink-safe write) and, when one was enabled, turns the switch on.
// Called at Orchestrator start; true when it changed the blob.
bool migrateCodeInterpreterCards();

// How a PDF can reach the owner right now (the portable pdfRoute over the live
// connector list, keys, SD card and capture): the spawn routing of a file task,
// the Tools status line (pdfRouteSlug) and the catalog's PDF line read one rule.
nimbus::orch::PdfRoute pdfRoute();
const char* pdfRouteSlug();

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
// Is any connector enabled (hosted tools may then run on the head's turn, out of
// the device's sight)? The claim check reads it once per turn.
bool anyEnabled();

void attachOpenAI(JsonDocument& d);
void attachMistral(JsonDocument& d);
void attachAnthropic(JsonDocument& agentBody);

// "[PROVIDERS & CONNECTORS]" - the honest, provider-aware capability catalog for
// the model's context: key present/verified, native tool surface, enabled
// connectors, and which are callable on the current provider's own turns vs only
// via a sub-agent spawn.
// `headCallsTools`: the head runs a tool loop on the config's tool list (the engine's
// models.list rule), so its own web.search can be offered.
String catalog(bool headCallsTools = true);

// The known-connector catalog (Tier-1 + Mistral built-ins) as a JSON array, for
// GET /api/connectors so the UI can describe/link not-yet-configured connectors.
String knownCatalog();

// ---- outbound MCP client (N4) -------------------------------------------------
// The device is already an MCP SERVER (memory::handleMcp over the LAN). This is
// the CLIENT half: the device dialing REMOTE MCP servers over Streamable HTTP,
// discovering their tools, and registering them into the on-device tool registry
// namespaced mcp.<serverSlug>.<tool> so the model can call them like any native
// tool. A device-dialed server is a connectors-blob kind:"mcp" entry with
// "dev":1; it is only dialed once the owner sets "appr":1 (fail-closed). The
// pure JSON-RPC + Streamable-HTTP framing is host-tested in lib/core
// (nimbus::orch::mcp::*); this device layer does the TLS/HTTP under the work
// arbiter and the registry wiring.
namespace mcp {

// Reconcile the tool registry with the approved + enabled + device-dialed MCP
// servers in the blob: discover newly-approved servers (initialize + tools/list)
// and register their tools; retract the tools of servers now gone, disabled, or
// unapproved. Idempotent and cheap once discovery has settled (network only for
// a not-yet-discovered server, at most one per call). Call it on the turn path
// (WiFi is up). Network runs Lock-free under the work arbiter; the brief registry
// mutation takes memory::Lock, so it is safe against the AsyncTCP /mcp reader.
void sync(nimbus::orch::ToolRegistry& reg);

// A short model-facing status block for the device-dialed MCP servers - which
// are ready (with a tool count), pending owner approval, or cooling down after
// failures - appended to catalog() so the model plans around real state.
std::string catalogSection();

// ---- OAuth 2.1 acquisition (CUM-256) -----------------------------------------
// The FRONT half of OAuth: getting a refresh token for a hosted, OAuth-only MCP
// server (mcp.linear.app-class) so the existing T3 broker (bearerFor/refreshAccess)
// can mint access tokens from then on. Authorization Code + PKCE with the device
// web UI as the registered redirect target (see docs/mcp.md). The pure decisions
// live in nimbus::orch::mcp::oauth (host-tested); this device seam drives them
// over the shared TLS work arbiter - NO new task, single slot (frozen invariant).
// The flow is advanced ONE bounded network step at a time by pump() on the main
// loop, exactly the discipline mcp::sync() uses for discovery.
namespace oauth {

// Arm an OAuth flow for the named device-dialed MCP connector. `redirectBase` is
// the device's own base URL as the owner's browser reaches it (e.g.
// "http://nimbus.local" or "http://192.168.1.42"); the callback is
// redirectBase + "/oauth/cb". Returns "" when armed, or a short owner-facing
// error (unknown connector, no URL, a flow already running). Does NO network.
String begin(const String& name, const String& redirectBase);

// Advance the armed flow by at most one bounded network step. A cheap no-op when
// no flow is active or the per-step throttle has not elapsed. Call from the main
// loop. Rides arbiter::acquireWork - never opens a second TLS slot.
void pump();

// The browser was redirected back with ?state=&code=. Validate the state against
// the active flow and stash the code for the next pump(). Returns true when
// accepted; on mismatch fills `errOut` and returns false. Safe on the web task.
bool callback(const String& state, const String& code, String& errOut);

// The long authorization URL of the active flow, for the device `/oauth/go`
// redirect (the on-screen QR encodes the short device URL, not this). Returns it
// ONLY when `launchKey` matches the per-flow key minted at the authenticated flow
// start and revealed only to the owner (CUM-274); "" otherwise (no flow, or an
// unauthenticated caller with a missing/wrong key), so `state` cannot leak to a
// LAN peer that hits the ungated /oauth/go.
String authorizeUrl(const String& launchKey);

// JSON status for the web UI + device screen data path:
// {"active":bool,"phase":"...","conn":"..","userCode":"..","verifyUrl":"..","error":".."}.
String statusJson();

// Clear any active flow (owner cancelled, or a fresh start supersedes it).
void cancel();

}  // namespace oauth

}  // namespace mcp

}  // namespace connectors
}  // namespace agent
