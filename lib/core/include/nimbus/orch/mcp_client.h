#pragma once
#include <ArduinoJson.h>

#include <string>
#include <vector>

// mcp_client - the PORTABLE half of the OUTBOUND MCP client (host-tested via
// pio test -e native). The device is already an MCP SERVER (tool_registry's
// handleRpc, reachable over the LAN); this is the missing CLIENT half: the
// device dialing REMOTE MCP servers over Streamable HTTP, discovering their
// tools, and calling them.
//
// This layer owns the pure decisions and has NO Arduino / TLS / store: it
// (a) builds the JSON-RPC 2.0 request bodies the device POSTs, and (b) parses
// the responses back out of the two Streamable-HTTP shapes a server may use -
// a single `application/json` body OR a `text/event-stream` (SSE) where the
// JSON-RPC message rides `data:` lines. The device seam (src/agent/mcp_client.*)
// does the TLS POST under the work arbiter, buffers the body in PSRAM, and feeds
// the raw (status, content-type, body) triple to the parsers here.
//
// Contract (posted on CUM-18): handshake is initialize -> notifications/
// initialized -> tools/list (paginated) -> tools/call. Errors name the next
// step. Discovered tools are namespaced mcp.<serverSlug>.<tool>.
namespace nimbus {
namespace orch {
namespace mcp {

// The JSON-RPC id the client uses per method. Each device exchange is its own
// POST, so a small fixed id per method is enough. On the SSE path, the parser
// picks the last event whose object carries "result"/"error" (a JSON-RPC
// response), skipping any server notifications; it does not match on this id.
enum RpcId : int {
  kIdInitialize = 1,
  kIdToolsList  = 2,
  kIdToolsCall  = 3,
};

// The negotiated protocol version the client advertises (MCP 2025-06-18). Sent
// both in the initialize params and as the MCP-Protocol-Version header.
extern const char* kProtocolVersion;

// ---- request builders (return a serialized JSON-RPC request string) ----------

// initialize: announces protocol version + client info, asks for capabilities.
std::string buildInitialize(const std::string& clientName,
                            const std::string& clientVersion);
// notifications/initialized: a NOTIFICATION (no id) sent after initialize so the
// server may begin normal operation. Expects no response.
std::string buildInitializedNotification();
// tools/list, optionally continuing a previous page via its opaque cursor.
std::string buildToolsList(const std::string& cursor = "");
// tools/call with a tool name and a JSON object of arguments (serialized). A
// malformed/empty argsJson degrades to an empty object, never a broken request.
std::string buildToolsCall(const std::string& toolName, const std::string& argsJson);

// ---- response parsing --------------------------------------------------------

// Why the last exchange failed, so the caller can render one honest next step.
enum class ErrorKind : uint8_t {
  None = 0,
  Timeout,        // no/incomplete response within the budget (device-set)
  Connect,        // could not open the socket / TLS handshake (device-set)
  Http,           // a non-2xx HTTP status
  Unauthorized,   // 401/403 - the bearer is missing, wrong, or lacks scope
  Malformed,      // body was not parseable JSON / not a JSON-RPC envelope
  Rpc,            // a JSON-RPC error object came back
  TooLarge,       // the body exceeded the device buffer cap (device-set)
  Empty,          // a 2xx with no JSON-RPC message (unexpected for a request)
};

// One user-facing line that states what happened and the single next step, in
// the house copy style (no exclamation, names the step). `serverName` is the
// owner-facing connector name; `detail` is an optional short specifier (e.g. an
// HTTP code or an RPC message) folded into the sentence when present.
std::string nextStepError(ErrorKind kind, const std::string& serverName,
                          const std::string& detail = "");

// The result of the initialize handshake.
struct InitializeResult {
  bool        ok = false;
  ErrorKind   error = ErrorKind::None;
  std::string errorMsg;         // nextStepError() text when !ok
  std::string protocolVersion;  // the server's negotiated version
  std::string serverName;       // serverInfo.name (informational)
  std::string serverVersion;    // serverInfo.version (informational)
  bool        hasTools = false; // capabilities.tools present -> tools/list is meaningful
};

// One discovered remote tool, as the registry/model will see it (no execution).
struct ToolDef {
  std::string name;             // the server's own tool name (un-namespaced)
  std::string description;      // one-line, surfaced to the model
  std::string inputSchemaJson;  // JSON Schema for arguments ("{}" if none/invalid)
};

// The result of one tools/list page.
struct ToolsListResult {
  bool                 ok = false;
  ErrorKind            error = ErrorKind::None;
  std::string          errorMsg;
  std::vector<ToolDef> tools;
  std::string          nextCursor;  // non-empty -> more pages remain
};

// The result of one tools/call.
struct CallToolResult {
  bool        ok = false;       // transport + JSON-RPC level success
  ErrorKind   error = ErrorKind::None;
  std::string errorMsg;         // nextStepError() text when !ok
  std::string text;             // flattened text content of the result
  bool        isError = false;  // MCP tool-level error (ok can still be true)
};

// Parse an initialize response from the raw (status, content-type, body) triple.
// `serverName` frames the error line. Handles both application/json and SSE.
InitializeResult parseInitialize(int httpStatus, const std::string& contentType,
                                 const std::string& body, const std::string& serverName);

// Parse a tools/list response. Skips malformed tool entries (never fatal); an
// empty tool array is a valid (ok=true, tools empty) result.
ToolsListResult parseToolsList(int httpStatus, const std::string& contentType,
                               const std::string& body, const std::string& serverName);

// Parse a tools/call response. A JSON-RPC error -> ok=false; an MCP tool error
// (result.isError=true) -> ok=true, isError=true, text = the error text.
CallToolResult parseCallTool(int httpStatus, const std::string& contentType,
                             const std::string& body, const std::string& serverName);

// ---- namespacing -------------------------------------------------------------

// Slugify a server's owner-facing name into [a-z0-9_] so the namespaced tool
// name is safe for the provider function-call wire (which accepts only
// [A-Za-z0-9_-] and, via the head-loop sanitizer, maps '.'->'_'). Uppercase is
// lowered, any other char becomes '_', runs of '_' collapse, and a leading/
// trailing '_' is trimmed. An empty result falls back to "server".
std::string slugifyServer(const std::string& name);

// The registry name for a discovered tool: mcp.<serverSlug>.<tool>. The tool
// segment is slugified the same way so the whole name survives the wire.
std::string namespacedTool(const std::string& serverSlug, const std::string& tool);

// True if a registry name is an outbound-MCP tool (has the mcp. prefix). Used to
// gate/approve remote tools as a class and to strip them on server removal.
bool isNamespaced(const std::string& registryName);

// The server slug embedded in a namespaced tool name (mcp.<slug>.<tool> ->
// <slug>); "" if the name is not namespaced. Used by per-server approval.
std::string serverOf(const std::string& registryName);

}  // namespace mcp
}  // namespace orch
}  // namespace nimbus
