#include "nimbus/harness/websearch.h"

namespace agent {
namespace websearch {

namespace {

constexpr int    kMaxResultsCap = 10;
constexpr size_t kSnippetMax    = 200;   // per-hit snippet handed to the model

// Trim a snippet on a word boundary where one is close by, so the model reads a
// clean clause rather than a word cut in half.
std::string clip(const char* s, size_t maxLen) {
  std::string v(s ? s : "");
  if (v.size() <= maxLen) return v;
  size_t cut = v.rfind(' ', maxLen);
  if (cut == std::string::npos || cut + 40 < maxLen) cut = maxLen;
  return v.substr(0, cut) + "...";
}

}  // namespace

void buildFilter(JsonDocument& filter) {
  filter["answer"] = true;
  JsonObject r = filter["results"].add<JsonObject>();
  r["title"]   = true;
  r["url"]     = true;
  r["content"] = true;
  // The provider's own error text, so a 401/429 can say WHY rather than just
  // showing a bare status number.
  filter["detail"]["error"] = true;
}

std::string renderDigest(const JsonDocument& doc, int maxResults) {
  std::string out;
  const char* answer = doc["answer"] | "";
  if (answer && answer[0]) {
    out += "Answer: ";
    out += answer;
    out += "\n";
  }
  int n = 0;
  for (JsonObjectConst r : doc["results"].as<JsonArrayConst>()) {
    const char* title = r["title"] | "";
    const char* url   = r["url"] | "";
    out += "- ";
    out += (title && title[0]) ? title : "(untitled)";
    if (url && url[0]) { out += " ("; out += url; out += ")"; }
    out += "\n";
    std::string snip = clip(r["content"] | "", kSnippetMax);
    if (!snip.empty()) { out += "  "; out += snip; out += "\n"; }
    if (++n >= maxResults) break;
  }
  return out;
}

Result search(HttpTransport& http, const std::string& apiKey,
              const std::string& query, int maxResults, uint32_t timeoutMs,
              ArduinoJson::Allocator* alloc) {
  Result res;
  if (apiKey.empty()) { res.err = "no Tavily API key configured"; return res; }
  if (query.empty())  { res.err = "empty query"; return res; }
  if (maxResults < 1) maxResults = 1;
  if (maxResults > kMaxResultsCap) maxResults = kMaxResultsCap;

  HttpRequest req;
  req.method    = "POST";
  req.host      = "api.tavily.com";
  req.path      = "/search";
  req.timeoutMs = timeoutMs;
  req.headers.push_back({"Content-Type", "application/json"});
  req.headers.push_back({"Authorization", "Bearer " + apiKey});
  // Some provider edges compress even when the client never advertised it, which
  // yields a body that cannot be parsed. Ask for identity explicitly.
  req.headers.push_back({"Accept-Encoding", "identity"});
  req.headers.push_back({"User-Agent", "Nimbus"});

  {
    JsonDocument d;
    d["query"]          = query;
    d["max_results"]    = maxResults;
    d["include_answer"] = true;
    d["search_depth"]   = "basic";
    serializeJson(d, req.body);
  }

  JsonDocument filter;
  buildFilter(filter);

  JsonDocument doc = alloc ? JsonDocument(alloc) : JsonDocument();
  std::string err;
  const int status = http.execJson(req, doc, filter, err);

  if (status == 0) {
    res.err = "network: " + (err.empty() ? std::string("transport failed") : err);
    return res;
  }
  if (status < 200 || status >= 300) {
    const char* detail = doc["detail"]["error"] | "";
    res.err = "HTTP " + std::to_string(status);
    if (detail && detail[0]) { res.err += ": "; res.err += detail; }
    else if (status == 401 || status == 403) res.err += " (check the Tavily API key)";
    else if (status == 429) res.err += " (Tavily rate limit or quota)";
    return res;
  }
  // A 200 whose body did not parse must NOT be rendered: the partial document
  // would produce a confidently-wrong digest (an answer with its supporting
  // results silently missing). This is the shape the 6000-byte cap used to
  // create on every single search.
  if (!err.empty()) { res.err = err; return res; }

  // ...and a parse error is not sufficient on its own. Filtered parsing of a
  // NON-OBJECT body returns Ok with an empty document, so an HTML error page
  // from a gateway, proxy or captive portal arrives here looking exactly like a
  // search that legitimately found nothing. Require the response to actually
  // carry Tavily's shape: an empty result set has BOTH keys present (answer:"",
  // results:[]), whereas a non-JSON body has neither.
  if (!doc["results"].is<JsonArrayConst>() && !doc["answer"].is<const char*>()) {
    res.err = "unrecognized response (not a Tavily result - proxy or error page?)";
    return res;
  }

  res.digest = renderDigest(doc, maxResults);
  if (res.digest.empty()) {
    // A 200 that yielded neither an answer nor hits. This is a real (if unusual)
    // outcome, NOT an error - the model should be told the search ran and found
    // nothing, so it can say so instead of retrying forever.
    res.digest = "(no results)";
  }
  res.ok = true;
  return res;
}

}  // namespace websearch
}  // namespace agent
