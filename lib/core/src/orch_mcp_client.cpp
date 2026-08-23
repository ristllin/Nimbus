#include "nimbus/orch/mcp_client.h"

#include <cctype>

namespace nimbus {
namespace orch {
namespace mcp {

using ArduinoJson::DeserializationError;
using ArduinoJson::JsonArrayConst;
using ArduinoJson::JsonDocument;
using ArduinoJson::JsonObject;
using ArduinoJson::JsonObjectConst;
using ArduinoJson::JsonVariantConst;

const char* kProtocolVersion = "2025-06-18";

// ---- request builders --------------------------------------------------------

namespace {
std::string serializeDoc(const JsonDocument& d) {
  std::string out;
  serializeJson(d, out);
  return out;
}
}  // namespace

std::string buildInitialize(const std::string& clientName,
                            const std::string& clientVersion) {
  JsonDocument d;
  d["jsonrpc"] = "2.0";
  d["id"] = (int)kIdInitialize;
  d["method"] = "initialize";
  JsonObject p = d["params"].to<JsonObject>();
  p["protocolVersion"] = kProtocolVersion;
  // We advertise no client capabilities we cannot honor (no sampling, no roots).
  p["capabilities"].to<JsonObject>();
  JsonObject ci = p["clientInfo"].to<JsonObject>();
  ci["name"] = clientName.empty() ? "nimbus" : clientName;
  ci["version"] = clientVersion.empty() ? "0" : clientVersion;
  return serializeDoc(d);
}

std::string buildInitializedNotification() {
  JsonDocument d;
  d["jsonrpc"] = "2.0";
  d["method"] = "notifications/initialized";  // no id -> notification
  return serializeDoc(d);
}

std::string buildToolsList(const std::string& cursor) {
  JsonDocument d;
  d["jsonrpc"] = "2.0";
  d["id"] = (int)kIdToolsList;
  d["method"] = "tools/list";
  if (!cursor.empty()) d["params"]["cursor"] = cursor;
  return serializeDoc(d);
}

std::string buildToolsCall(const std::string& toolName, const std::string& argsJson) {
  JsonDocument d;
  d["jsonrpc"] = "2.0";
  d["id"] = (int)kIdToolsCall;
  d["method"] = "tools/call";
  JsonObject p = d["params"].to<JsonObject>();
  p["name"] = toolName;
  // Arguments MUST be an object. A blank or malformed argsJson degrades to {}
  // rather than emitting a request the server will reject.
  JsonDocument ad;
  if (!argsJson.empty() && deserializeJson(ad, argsJson) == DeserializationError::Ok &&
      ad.is<JsonObjectConst>()) {
    p["arguments"] = ad;
  } else {
    p["arguments"].to<JsonObject>();
  }
  return serializeDoc(d);
}

// ---- error copy --------------------------------------------------------------

std::string nextStepError(ErrorKind kind, const std::string& serverName,
                          const std::string& detail) {
  const std::string s = serverName.empty() ? "the MCP server" : ("MCP server " + serverName);
  const std::string d = detail.empty() ? "" : (" (" + detail + ")");
  switch (kind) {
    case ErrorKind::Timeout:
      return s + " did not respond in time" + d +
             ". Check that the server URL is reachable and the server is up, then try again.";
    case ErrorKind::Connect:
      return "Could not connect to " + s + d +
             ". Check the server URL and the network, then try again.";
    case ErrorKind::Unauthorized:
      return s + " rejected the credential" + d +
             ". Update this connector's token on the device web page, then try again.";
    case ErrorKind::Http:
      return s + " returned an HTTP error" + d +
             ". Check the server URL is the MCP endpoint, then try again.";
    case ErrorKind::Malformed:
      return s + " sent a response that could not be read" + d +
             ". Confirm the URL points at a Streamable HTTP MCP endpoint, then try again.";
    case ErrorKind::Rpc:
      return s + " reported an error" + d + ".";
    case ErrorKind::TooLarge:
      return s + " sent more data than the device can hold" + d +
             ". Ask the server for fewer items, or narrow the request.";
    case ErrorKind::Empty:
      return s + " returned no result" + d + ". Try again; if it persists, check the server.";
    case ErrorKind::None:
    default:
      return "";
  }
}

// ---- Streamable HTTP body extraction -----------------------------------------

namespace {

bool containsCI(const std::string& hay, const char* needle) {
  std::string h = hay, n = needle;
  for (char& c : h) c = (char)std::tolower((unsigned char)c);
  for (char& c : n) c = (char)std::tolower((unsigned char)c);
  return h.find(n) != std::string::npos;
}

// True if `doc` looks like the JSON-RPC RESPONSE we asked for: an object that
// carries "result" or "error". (Server-to-client requests/notifications on the
// SSE stream carry "method" instead and are ignored.)
bool isRpcResponse(const JsonDocument& doc) {
  if (!doc.is<JsonObjectConst>()) return false;
  return !doc["result"].isNull() || !doc["error"].isNull();
}

// Pull the JSON-RPC response object out of an SSE (text/event-stream) body. The
// stream is a sequence of events; each event's payload is one or more `data:`
// lines concatenated with '\n'. We scan every event, parse its data as JSON, and
// keep the LAST one that is a JSON-RPC response (a stream may carry unrelated
// server notifications before the answer). Returns true and fills `out` on hit.
bool extractFromSse(const std::string& body, JsonDocument& out) {
  std::string data;          // accumulated data for the current event
  bool found = false;
  size_t i = 0;
  const size_t n = body.size();
  auto flush = [&]() {
    if (!data.empty()) {
      JsonDocument tmp;
      if (deserializeJson(tmp, data) == DeserializationError::Ok && isRpcResponse(tmp)) {
        out = tmp;
        found = true;
      }
      data.clear();
    }
  };
  while (i <= n) {
    // Read one line (up to '\n'); treat end-of-string as a final line.
    size_t nl = body.find('\n', i);
    std::string line = (nl == std::string::npos) ? body.substr(i) : body.substr(i, nl - i);
    if (!line.empty() && line.back() == '\r') line.pop_back();  // CRLF tolerant
    if (line.empty()) {
      flush();  // blank line ends the current event
    } else if (line.rfind("data:", 0) == 0) {
      std::string payload = line.substr(5);
      if (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);  // strip one lead space
      if (!data.empty()) data += "\n";
      data += payload;
    }
    // other fields (event:, id:, retry:, comments) are ignored
    if (nl == std::string::npos) break;
    i = nl + 1;
  }
  flush();  // a stream that does not end in a blank line
  return found;
}

// Common front end for every parser: validate the HTTP status, select the body
// shape (JSON vs SSE), deserialize, and split into "got a result object" vs an
// ErrorKind. On success `out` holds the JSON-RPC "result" object.
ErrorKind extractResult(int httpStatus, const std::string& contentType,
                        const std::string& body, JsonDocument& out,
                        std::string& rpcDetail) {
  if (httpStatus == 401 || httpStatus == 403) return ErrorKind::Unauthorized;
  if (httpStatus < 200 || httpStatus >= 300) {
    rpcDetail = "HTTP " + std::to_string(httpStatus);
    return ErrorKind::Http;
  }
  JsonDocument env;  // the JSON-RPC envelope
  bool haveEnv = false;
  if (containsCI(contentType, "text/event-stream")) {
    haveEnv = extractFromSse(body, env);
    if (!haveEnv) {
      // An SSE body with no JSON-RPC response is either empty (a 202 ack) or junk.
      return body.empty() ? ErrorKind::Empty : ErrorKind::Malformed;
    }
  } else {
    if (body.empty()) return ErrorKind::Empty;
    if (deserializeJson(env, body) != DeserializationError::Ok) return ErrorKind::Malformed;
    if (!isRpcResponse(env)) return ErrorKind::Malformed;
    haveEnv = true;
  }
  (void)haveEnv;
  if (!env["error"].isNull()) {
    JsonObjectConst e = env["error"].as<JsonObjectConst>();
    const char* msg = e["message"] | "";
    long code = e["code"] | 0;
    rpcDetail = msg[0] ? std::string(msg) : ("code " + std::to_string(code));
    return ErrorKind::Rpc;
  }
  out = env["result"];
  return ErrorKind::None;
}

}  // namespace

// ---- typed parsers -----------------------------------------------------------

InitializeResult parseInitialize(int httpStatus, const std::string& contentType,
                                 const std::string& body, const std::string& serverName) {
  InitializeResult r;
  JsonDocument result;
  std::string detail;
  ErrorKind k = extractResult(httpStatus, contentType, body, result, detail);
  if (k != ErrorKind::None) {
    r.error = k;
    r.errorMsg = nextStepError(k, serverName, detail);
    return r;
  }
  r.ok = true;
  r.protocolVersion = (const char*)(result["protocolVersion"] | "");
  JsonObjectConst si = result["serverInfo"].as<JsonObjectConst>();
  r.serverName = (const char*)(si["name"] | "");
  r.serverVersion = (const char*)(si["version"] | "");
  r.hasTools = !result["capabilities"]["tools"].isNull();
  return r;
}

ToolsListResult parseToolsList(int httpStatus, const std::string& contentType,
                               const std::string& body, const std::string& serverName) {
  ToolsListResult r;
  JsonDocument result;
  std::string detail;
  ErrorKind k = extractResult(httpStatus, contentType, body, result, detail);
  if (k != ErrorKind::None) {
    r.error = k;
    r.errorMsg = nextStepError(k, serverName, detail);
    return r;
  }
  r.ok = true;
  r.nextCursor = (const char*)(result["nextCursor"] | "");
  for (JsonObjectConst t : result["tools"].as<JsonArrayConst>()) {
    ToolDef td;
    td.name = (const char*)(t["name"] | "");
    if (td.name.empty()) continue;  // a nameless tool is not callable; skip it
    td.description = (const char*)(t["description"] | "");
    JsonVariantConst schema = t["inputSchema"];
    if (schema.is<JsonObjectConst>()) {
      serializeJson(schema, td.inputSchemaJson);
    } else {
      td.inputSchemaJson = "{}";
    }
    r.tools.push_back(std::move(td));
  }
  return r;
}

CallToolResult parseCallTool(int httpStatus, const std::string& contentType,
                             const std::string& body, const std::string& serverName) {
  CallToolResult r;
  JsonDocument result;
  std::string detail;
  ErrorKind k = extractResult(httpStatus, contentType, body, result, detail);
  if (k != ErrorKind::None) {
    r.error = k;
    r.errorMsg = nextStepError(k, serverName, detail);
    return r;
  }
  r.ok = true;
  r.isError = (result["isError"] | false);
  // Flatten the content[] array into text. MCP content blocks are typed; we
  // surface text blocks and name the others so a non-text result is not silent.
  std::string text;
  for (JsonObjectConst block : result["content"].as<JsonArrayConst>()) {
    const char* type = block["type"] | "";
    if (std::string(type) == "text") {
      if (!text.empty()) text += "\n";
      text += (const char*)(block["text"] | "");
    } else if (type[0]) {
      if (!text.empty()) text += "\n";
      text += "[" + std::string(type) + " content]";
    }
  }
  // Some servers return a structuredContent object and no text block; carry it
  // so the model still gets the payload rather than an empty result.
  if (text.empty() && !result["structuredContent"].isNull()) {
    serializeJson(result["structuredContent"], text);
  }
  r.text = text;
  return r;
}

// ---- namespacing -------------------------------------------------------------

std::string slugifyServer(const std::string& name) {
  std::string out;
  out.reserve(name.size());
  bool lastUnderscore = false;
  for (char c : name) {
    char lc;
    if (c >= 'A' && c <= 'Z') lc = (char)(c - 'A' + 'a');
    else lc = c;
    if ((lc >= 'a' && lc <= 'z') || (lc >= '0' && lc <= '9')) {
      out += lc;
      lastUnderscore = false;
    } else if (!lastUnderscore && !out.empty()) {
      out += '_';
      lastUnderscore = true;
    }
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  return out.empty() ? std::string("server") : out;
}

std::string namespacedTool(const std::string& serverSlug, const std::string& tool) {
  // The tool segment is slugified too so a server tool named "get-file" or
  // "search/all" cannot smuggle a wire-hostile char into the registry name.
  return "mcp." + serverSlug + "." + slugifyServer(tool);
}

bool isNamespaced(const std::string& registryName) {
  return registryName.rfind("mcp.", 0) == 0;
}

std::string serverOf(const std::string& registryName) {
  if (!isNamespaced(registryName)) return "";
  size_t a = 4;  // after "mcp."
  size_t b = registryName.find('.', a);
  if (b == std::string::npos) return "";
  return registryName.substr(a, b - a);
}

}  // namespace mcp
}  // namespace orch
}  // namespace nimbus
