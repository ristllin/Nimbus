#include "nimbus/orch/connectors_wire.h"

#include <cstring>   // std::strcmp (provider match, connector tools[] scan)

namespace nimbus {
namespace orch {

// ---- known catalog -----------------------------------------------------------
// Compact Tier-1 set (+ Mistral built-ins). Descriptions stay one-line; the rich
// per-connector setup recipe lives in docs/connectors.md (docsSlug anchors it).
static const KnownConnector kKnown[] = {
    // caps verified LIVE against api.githubcopilot.com/mcp/ tools/list
    // (2026-08-13, 44 tools): create_repository + push_files/create_or_update_file
    // ARE exposed; Actions and Gists are NOT; releases are read-only.
    {"github", "GitHub", "openai,anthropic,mistral", "mcp", "",
     "GitHub PAT (ghp_…)",
     "Issues, PRs, code search, CREATE repos, push files.", "github",
     "MCP (OpenAI/Anthropic, toolset verified live): read + write issues/PRs/"
     "code+commit search, CREATE repositories, and push file contents via the "
     "GitHub API (the PAT needs repo scope; only repos the token/app can access "
     "- private repos otherwise return 'not accessible'); no Actions or Gists "
     "tools; releases are read-only. Mistral (Studio github_app): toolset "
     "unverified - confirm a write tool exists before promising it. It cannot "
     "CLONE, BUILD, or RUN code - for that, spawn an anthropic sandbox sub."},
    {"gmail", "Gmail", "openai,mistral", "connector", "connector_gmail",
     "OAuth refresh token (minted off-device)",
     "Read, search, draft, and label email.", "gmail",
     "Mistral (Studio): draft/read/search/label ONLY - NO send tool. OpenAI "
     "(first-party connector_gmail): READ-ONLY - read/search/profile, NO send, NO "
     "draft. You cannot SEND email on any provider - draft it and ask the owner "
     "to send; never claim an email was sent."},
    {"gcal", "Google Calendar", "openai,mistral", "connector", "connector_googlecalendar",
     "OAuth refresh token (minted off-device)",
     "Read and manage calendar events / digests.", "google-calendar", ""},
    {"gdrive", "Google Drive", "openai,mistral", "connector", "connector_googledrive",
     "OAuth refresh token (minted off-device)",
     "Read and write Drive files (report/corpus storage).", "google-drive",
     "Mistral (Studio): create + read + copy - NO share (can't grant access to an "
     "email) and NO edit of an existing doc. OpenAI (first-party connector_googledrive): "
     "READ-ONLY - search/read/fetch, NO create/edit/share."},
    {"notion", "Notion", "mistral,openai,anthropic", "mcp", "",
     "Internal Integration token (secret_…)",
     "Create/query pages & databases (publish, corpora).", "notion",
     "create/read/update/delete pages - verified. Pages must be shared with the "
     "integration first."},
    // CUM-514 (L20 docs audit): Slack runs as a Mistral Studio connector, attached
    // by connector_id with NO device secret (the old "Bot token (xoxb-...)" hint
    // asked for a credential nothing reads).
    {"slack", "Slack", "mistral", "mcp", "",
     "None on the device - connect Slack in your Mistral account",
     "Read channel/DM history, search, post messages.", "slack", ""},
    {"linear", "Linear", "openai,anthropic,mistral", "mcp", "",
     "Linear API key (Bearer)",
     "Find, create, and update issues and projects.", "linear", ""},
    // Mistral hosted built-ins - no secret on device (authenticated in Studio).
    {"web_search", "Web Search (Mistral built-in)", "mistral", "builtin", "",
     "None - enable in Mistral Studio",
     "Hosted web search with citations, on your own turns.", "mistral-builtins", ""},
    {"code_interpreter", "Code Interpreter (hosted Python)", "mistral,openai", "builtin", "",
     "None - Mistral: enable in Studio; OpenAI: no setup",
     "Hosted sandboxed Python; can produce files. To deliver one, SPAWN a "
     "sub-agent WITH A PROJECT (session_ops spawn: project=<name>) - the file it "
     "writes is captured to the file store, then you deliver it to the owner.",
     "mistral-builtins",
     "Mistral: images/text output only - its sandbox ERRORS on PDF/CSV files. "
     "OpenAI: full file output INCLUDING PDF. For a PDF, spawn the sub-agent on "
     "openai with a project."},
    {"image_generation", "Image Generation (Mistral built-in)", "mistral", "builtin", "",
     "None - enable in Mistral Studio",
     "Hosted image generation.", "mistral-builtins", ""},
    {"document_library", "Document Library (Mistral built-in)", "mistral", "builtin", "",
     "None - enable in Mistral Studio",
     "Hosted RAG over your Studio document library.", "mistral-builtins", ""},
};

const KnownConnector* knownConnectors(int& countOut) {
  countOut = (int)(sizeof(kKnown) / sizeof(kKnown[0]));
  return kKnown;
}

// ---- blob parse (no silent drop) ---------------------------------------------

int parseConnectorsJson(const char* blobJson, std::vector<ConnectorInfo>& out,
                        int maxN, int* totalEntries) {
  out.clear();
  if (totalEntries) *totalEntries = 0;
  if (!blobJson || !blobJson[0] || maxN <= 0) return 0;
  JsonDocument d;
  if (deserializeJson(d, blobJson)) return 0;   // malformed blob: no connectors
  JsonArrayConst arr = d.as<JsonArrayConst>();
  if (arr.isNull()) return 0;                    // not an array
  if (totalEntries) *totalEntries = (int)arr.size();
  int n = 0;
  for (JsonObjectConst c : arr) {
    if (n >= maxN) break;   // caller compares n to *totalEntries to see the drop
    ConnectorInfo ci;
    ci.name        = (const char*)(c["name"] | "");
    if (ci.name.empty()) continue;              // nameless: skipped, still counted
    ci.prov        = (const char*)(c["prov"] | "any");
    ci.kind        = (const char*)(c["kind"] | "mcp");
    ci.url         = (const char*)(c["url"]  | "");
    ci.connectorId = (const char*)(c["cid"]  | "");
    const char* ty = c["type"] | "";
    ci.type        = ty[0] ? ty : ci.name;      // type defaults to name (match list())
    ci.enabled     = (c["en"] | 0) != 0;
    ci.deviceDialed = (c["dev"] | 0) != 0;
    ci.approved    = (c["appr"] | 0) != 0;
    ci.hasToken    = std::string((const char*)(c["tok"] | "")).size() > 0;
    ci.hasOauth    = !c["oauth"].isNull();
    out.push_back(std::move(ci));
    n++;
  }
  return n;
}

// ---- helpers -----------------------------------------------------------------
static bool provMatches(const ConnectorInfo& c, const char* prov) {
  return c.enabled && (c.prov == prov || c.prov == "any");
}

// ---- prov routing guard (CUM-255) -------------------------------------------
namespace {

std::string lowerStr(std::string s) {
  for (char& ch : s)
    if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
  return s;
}

// Extract the lowercased host from an http/https URL. Returns "" if the scheme
// is not http(s) or the URL has no host. Strips the port and any IPv6 brackets.
std::string hostOf(const std::string& url) {
  std::string::size_type sep = url.find("://");
  if (sep == std::string::npos) return "";
  std::string scheme = lowerStr(url.substr(0, sep));
  if (scheme != "http" && scheme != "https") return "";
  std::string rest = url.substr(sep + 3);
  std::string::size_type end = rest.find_first_of("/?#");  // authority ends at path/query/fragment
  std::string hostport = (end == std::string::npos) ? rest : rest.substr(0, end);
  std::string::size_type at = hostport.rfind('@');       // strip user:pass@ userinfo
  if (at != std::string::npos) hostport = hostport.substr(at + 1);
  if (!hostport.empty() && hostport[0] == '[') {         // [IPv6]:port
    std::string::size_type close = hostport.find(']');
    return close == std::string::npos ? "" : lowerStr(hostport.substr(1, close - 1));
  }
  std::string::size_type colon = hostport.find(':');     // host:port
  if (colon != std::string::npos) hostport = hostport.substr(0, colon);
  std::string h = lowerStr(hostport);
  if (h.size() > 1 && h.back() == '.') h.pop_back();     // drop a trailing FQDN dot
  return h;
}

// Parse a dotted-quad into four octets; false if `h` is not a bare IPv4 literal.
bool parseIPv4(const std::string& h, int oct[4]) {
  int n = 0, val = 0, digits = 0;
  for (std::string::size_type i = 0; i <= h.size(); i++) {
    char ch = (i < h.size()) ? h[i] : '.';
    if (ch == '.') {
      if (digits == 0 || n >= 4) return false;
      oct[n++] = val; val = 0; digits = 0;
    } else if (ch >= '0' && ch <= '9') {
      val = val * 10 + (ch - '0');
      if (val > 255 || ++digits > 3) return false;
    } else {
      return false;   // any non-digit, non-dot char => not an IPv4 literal
    }
  }
  return n == 4;
}

bool hasSuffix(const std::string& s, const char* suf) {
  const std::string t(suf);
  return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

// RFC-1918 / loopback / link-local / unspecified IPv4.
bool privateIPv4(const int o[4]) {
  return o[0] == 127 || o[0] == 10 || o[0] == 0 ||
         (o[0] == 172 && o[1] >= 16 && o[1] <= 31) ||
         (o[0] == 192 && o[1] == 168) ||
         (o[0] == 169 && o[1] == 254);
}

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// Parse a non-empty base-10 or base-16 digit string into v; false on any bad digit.
bool parseUint(const std::string& s, int base, unsigned long& v) {
  v = 0;
  for (char c : s) {
    int d = (base == 16) ? hexDigit(c) : ((c >= '0' && c <= '9') ? c - '0' : -1);
    if (d < 0) return false;
    v = v * base + d;
  }
  return !s.empty();
}

// A bare-integer / 0x-hex host (`3232235521`, `0x7f000001`) is an obfuscated
// IPv4 - no legitimate hostname is all digits - so decode it to octets. Returns
// false when `h` is not one of those numeric forms.
bool numericHostToIPv4(const std::string& h, int oct[4]) {
  unsigned long v = 0;
  const bool hex = (h.rfind("0x", 0) == 0 || h.rfind("0X", 0) == 0);
  const bool ok = hex ? (h.size() >= 3 && h.size() <= 10 && parseUint(h.substr(2), 16, v))
                      : (h.size() <= 10 && parseUint(h, 10, v));
  if (!ok || v > 0xFFFFFFFFul) return false;
  oct[0] = (v >> 24) & 0xFF; oct[1] = (v >> 16) & 0xFF;
  oct[2] = (v >> 8) & 0xFF;  oct[3] = v & 0xFF;
  return true;
}

// The IPv4 embedded in a mapped/compat IPv6 literal (`::ffff:a.b.c.d`, `::a.b.c.d`),
// or "" if there is none. Lets a mapped RFC-1918/loopback address be classified
// by its IPv4 rather than slipping past the IPv6 prefix checks.
std::string embeddedIPv4(const std::string& h) {
  std::string::size_type dot = h.find('.');
  if (dot == std::string::npos) return "";
  std::string::size_type colon = h.rfind(':', dot);
  return colon == std::string::npos ? "" : h.substr(colon + 1);
}

// True for the loopback (`::1` and any zero-run spelling like `0:0:0:0:0:0:0:1`)
// or the unspecified `::` address. Errs closed on exotic all-but-one-zero forms.
bool ipv6Loopbackish(const std::string& h) {
  std::string digits;
  for (char ch : h) if (ch != ':') digits += ch;
  std::string::size_type nz = digits.find_first_not_of('0');
  return nz == std::string::npos || digits.substr(nz) == "1";
}

// Loopback / unique-local (fc00::/7) / link-local (fe80::/10) IPv6 literal,
// including IPv4-mapped forms that re-encode a private address.
bool privateIPv6(const std::string& h) {
  if (h.find(':') == std::string::npos) return false;   // not an IPv6 literal
  std::string emb = embeddedIPv4(h);
  if (!emb.empty()) { int o[4]; return parseIPv4(emb, o) && privateIPv4(o); }
  if (h.rfind("fc", 0) == 0 || h.rfind("fd", 0) == 0) return true;         // ULA fc00::/7
  if (h.rfind("fe", 0) == 0 && h.size() > 2 && h[2] >= '8' && h[2] <= 'b')  // fe80::/10
    return true;
  return ipv6Loopbackish(h);
}

// A host a provider's CLOUD cannot dial: loopback, private/link-local IP (in any
// spelling), the unspecified address, or an mDNS `.local` / localhost name.
bool privateHost(const std::string& h) {
  if (h.empty() || h == "localhost") return true;
  if (hasSuffix(h, ".local") || hasSuffix(h, ".localhost")) return true;
  if (privateIPv6(h)) return true;
  int o[4];
  if (parseIPv4(h, o)) return privateIPv4(o);
  return numericHostToIPv4(h, o) && privateIPv4(o);   // obfuscated integer/hex IPv4
}

// The provider heads an entry can be forwarded to. An unknown/future prov names
// none of them (fail-closed) - the class rule the host test locks.
bool provTargetsAnyHead(const std::string& prov) {
  return prov == "openai" || prov == "anthropic" || prov == "mistral" || prov == "any";
}

}  // namespace

bool urlRoutableToProviderHead(const std::string& url) {
  std::string h = hostOf(url);
  return !h.empty() && !privateHost(h);
}

bool forwardsToProviderHead(const ConnectorInfo& c, const char* head) {
  if (!provMatches(c, head)) return false;              // disabled / wrong prov
  // A device-dialed server left at the default "any" routes to the device's own
  // MCP client - it is never handed to a cloud head (CUM-61: doing so 424'd the
  // turn). An EXPLICIT prov still opts a public server into head-attach.
  if (c.deviceDialed && c.prov == "any") return false;
  // Hard invariant: ANY entry that would put a URL on the wire (a remote MCP, or a
  // connector whose first-party connector_id is absent so its url is emitted as
  // server_url) must carry a URL the cloud can reach. A private URL never leaves
  // the device (CUM-61: it only 424s the head). Keyed on url PRESENCE, not kind, so
  // the connector-with-empty-connector_id path cannot smuggle a LAN url past it.
  if (!c.url.empty() && !urlRoutableToProviderHead(c.url)) return false;
  return true;
}

std::string connectorConfigError(const ConnectorInfo& c) {
  if (c.url.empty()) return "";                          // no dialed URL to validate
  if (urlRoutableToProviderHead(c.url)) return "";       // reachable from a cloud head: fine
  // A device-dialed server is dialed by the device itself, so a private URL is
  // expected and safe there - not a config error. The trap is a NON-device-dialed
  // entry aimed at a provider head (explicit prov or the default "any"): nothing
  // can reach it, so reject the save with the exact next step.
  if (c.deviceDialed || !provTargetsAnyHead(c.prov)) return "";
  // CUM-514 (L20 docs audit): there is no "device-dialed" switch on the web page;
  // the flag is the entry's "dev":1 in the raw JSON editor, then an approval.
  return "MCP server \"" + c.name + "\" has a private or local URL that a "
         "provider's cloud can't reach. Use a public https address, or have the "
         "device connect to it directly: add \"dev\":1 to its entry under Advanced "
         "- edit raw JSON, save, then approve it on its card.";
}

// ---- attach builders ---------------------------------------------------------

void attachOpenAIWire(JsonDocument& d, const std::vector<ConnectorInfo>& cs, const BearerFn& bearer,
                      bool builtinsOnly) {
  for (const ConnectorInfo& c : cs) {
    if (builtinsOnly && c.kind != "builtin") continue;  // cumulo route: built-ins only
    if (!forwardsToProviderHead(c, "openai")) continue;
    // OpenAI hosted built-ins ride their own tool type, NOT the mcp shape (a
    // builtin routed through the mcp branch would 400 on the missing server_url).
    // code_interpreter is the one we attach: it can PRODUCE FILES (incl. PDF -
    // verified live 2026-08-08; Mistral's sandbox 500s on PDF/CSV, so OpenAI is
    // the PDF path) which the device captures via the container-files endpoint.
    if (c.kind == "builtin") {
      if ((c.type.empty() ? c.name : c.type) == "code_interpreter") {
        JsonObject t = d["tools"].add<JsonObject>();
        t["type"] = "code_interpreter";
        t["container"]["type"] = "auto";
      }
      continue;   // no other OpenAI builtins are wired; never fall into mcp shape
    }
    std::string b = bearer ? bearer(c) : std::string();
    // First-party connector_id entries REQUIRE authorization ("Must specify
    // 'authorization' parameter with 'connector_id'" - grid-validated
    // 2026-08-07): attaching one without a token 400s the ENTIRE turn, the
    // same blast radius as the server_label bug below. Enabled-but-unauthed
    // stays a catalog/setup note, never a poisoned request.
    if (c.kind == "connector" && b.empty()) continue;
    JsonObject t = d["tools"].add<JsonObject>();
    t["type"] = "mcp";
    t["require_approval"] = "never";  // headless: no approval UI exists
    // server_label is REQUIRED on every OpenAI MCP tool - first-party
    // connector_id entries included. Omitting it on the connector_id branch
    // 400'd EVERY head turn the moment an owner enabled a first-party
    // connector ("Missing required parameter: 'tools[N].server_label'",
    // live on nimbus-5 2026-08-07 - the L13-era verification only ever
    // exercised the MCP+PAT branch below).
    t["server_label"] = c.name;
    if (c.kind == "connector" && !c.connectorId.empty()) {
      t["connector_id"] = c.connectorId;  // first-party (Gmail, GCal…)
    } else {
      t["server_url"] = c.url;  // remote MCP (GitHub, custom…)
    }
    if (!b.empty()) t["authorization"] = b;
  }
}

std::string mistralConnectorId(const ConnectorInfo& c) {
  // Mistral's connector ids are its OWN namespace (workspace-listable via GET
  // /v1/connectors) - NOT OpenAI's "connector_*" first-party ids the known-catalog
  // defaults carry. An owner-set explicit id wins; a blank or OpenAI-namespace id
  // maps through the canonical table by type/name.
  std::string cid = c.connectorId;
  if (cid.empty() || cid.rfind("connector_", 0) == 0) {
    const std::string& t2 = c.type.empty() ? c.name : c.type;
    cid = (t2 == "github") ? "github_app"
        : (t2 == "gcal")   ? "google_calendar"
        : (t2 == "gdrive") ? "google_drive_mcp"
                           : t2;
  }
  return cid;
}

namespace {
// A forward-only, NON-RECURSIVE reader for the /v1/connectors body. The live body
// (~300 KB) nests 26 levels deep inside fields we do not read (tool input schemas),
// and a filtered ArduinoJson parse still descends into every skipped value: at its
// default nesting limit it rejects the body (TooDeep, so the probe never landed an
// answer), and raising the limit trades that for a stack that grows with the
// provider's schema depth - on the device that runs on a small verify task stack.
// This reader descends only root -> items[] -> item{} (a fixed depth); every other
// value, however deep, is skipped with a bracket counter. Stack use is constant.
class WorkspaceScanner {
 public:
  explicit WorkspaceScanner(const char* p) : p_(p) {}

  void ws() {
    while (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r') p_++;
  }
  char peek() {
    ws();
    return *p_;
  }
  // A JSON string at the cursor, into `out` (nullptr = skip). Escapes keep the
  // escaped byte (enough for ids/names; a \u sequence stays literal). false when
  // the cursor is not on a string or it is unterminated.
  bool str(std::string* out) {
    if (peek() != '"') return false;
    for (p_++; *p_ && *p_ != '"'; p_++) {
      if (*p_ == '\\') {
        if (!*++p_) return false;
      }
      if (out) out->push_back(*p_);
    }
    if (*p_ != '"') return false;
    p_++;
    return true;
  }
  // Skip one value of ANY depth: strings honored (a bracket inside one does not
  // count), containers balanced by a counter - never by recursion.
  bool skip() {
    const char c = peek();
    if (c == '"') return str(nullptr);
    if (c != '{' && c != '[') return scalar();
    int depth = 0;
    while (*p_) {
      if (*p_ == '"') {
        if (!str(nullptr)) return false;
        continue;
      }
      if (*p_ == '{' || *p_ == '[') depth++;
      if ((*p_ == '}' || *p_ == ']') && --depth == 0) {
        p_++;
        return true;
      }
      p_++;
    }
    return false;   // unterminated (e.g. a truncated body)
  }
  // true/false at the cursor -> 1/0; any other value is skipped -> -1; a broken
  // value -> -2.
  int boolean() {
    ws();
    if (!std::strncmp(p_, "true", 4)) { p_ += 4; return 1; }
    if (!std::strncmp(p_, "false", 5)) { p_ += 5; return 0; }
    return skip() ? -1 : -2;
  }
  // Walk an object's members; `fn(key)` is called with the cursor on the value and
  // must consume it. false on any structural error.
  template <class F>
  bool members(F fn) {
    if (peek() != '{') return false;
    p_++;
    if (peek() == '}') { p_++; return true; }
    for (;;) {
      std::string key;
      if (!str(&key) || peek() != ':') return false;
      p_++;
      if (!fn(key)) return false;
      const char c = peek();
      if (c != '}' && c != ',') return false;   // includes end of input
      p_++;
      if (c == '}') return true;
    }
  }
  // Walk an array's elements; `fn()` is called with the cursor on each element.
  template <class F>
  bool elements(F fn) {
    if (peek() != '[') return false;
    p_++;
    if (peek() == ']') { p_++; return true; }
    for (;;) {
      if (!fn()) return false;
      const char c = peek();
      if (c != ']' && c != ',') return false;   // includes end of input
      p_++;
      if (c == ']') return true;
    }
  }

 private:
  bool scalar() {
    const char* start = p_;
    while (*p_ && !std::strchr(",}] \t\r\n", *p_)) p_++;
    return p_ != start;
  }
  const char* p_;
};

// One items[] entry's four fields we read.
struct WorkspaceItem {
  std::string id, name;
  int active = -1;   // 1/0, -1 absent or not a bool (treated as active)
  int signedIn = -1; // is_authenticated: 1 only when literally true
};

bool scanWorkspaceItem(WorkspaceScanner& sc, WorkspaceItem& it) {
  return sc.members([&](const std::string& k) {
    if (k == "id" || k == "name") {
      std::string& dst = (k == "id") ? it.id : it.name;
      return sc.peek() == '"' ? sc.str(&dst) : sc.skip();
    }
    if (k == "active" || k == "is_authenticated") {
      const int b = sc.boolean();
      (k == "active" ? it.active : it.signedIn) = b;
      return b != -2;
    }
    return sc.skip();
  });
}
}  // namespace

bool parseMistralWorkspaceConnectors(const char* body, std::vector<std::string>& listedOut,
                                     std::vector<std::string>* signedInOut) {
  if (!body || !body[0]) return false;
  WorkspaceScanner sc(body);
  std::vector<std::string> listed, signedIn;   // committed only on a clean parse
  bool sawItems = false;
  int seen = 0;
  const bool ok = sc.members([&](const std::string& key) {
    if (key != "items") return sc.skip();
    sawItems = true;
    return sc.elements([&]() {
      if (sc.peek() != '{') return sc.skip();   // a non-object element is ignored
      WorkspaceItem it;
      if (!scanWorkspaceItem(sc, it)) return false;
      // Bounded, above the probe's page size; the rest is still walked (so a
      // malformed tail is still caught) but not kept.
      if (++seen > kMistralWorkspaceMaxItems) return true;
      if (it.active == 0) return true;   // explicitly inactive: not usable; absent = active
      // is_authenticated is a hint only: a listed connector reporting false still
      // works (measured live), so it never decides usability.
      for (const std::string* v : {&it.name, &it.id}) {
        if (v->empty()) continue;
        listed.push_back(*v);
        if (it.signedIn == 1) signedIn.push_back(*v);
      }
      return true;
    });
  });
  if (!ok || !sawItems) return false;   // not the expected shape: "no signal"
  listedOut.insert(listedOut.end(), listed.begin(), listed.end());
  if (signedInOut) signedInOut->insert(signedInOut->end(), signedIn.begin(), signedIn.end());
  return true;
}

namespace {
// Membership in a "\nid\nid\n" set. An id carrying a newline can never match (and
// could otherwise straddle two entries).
bool inIdSet(const std::string& set, const std::string& id) {
  if (id.empty() || id.find('\n') != std::string::npos) return false;
  return set.find("\n" + id + "\n") != std::string::npos;
}
std::string toIdSet(const std::vector<std::string>& ids) {
  std::string s = "\n";
  for (const std::string& id : ids)
    if (id.find('\n') == std::string::npos) s += id + "\n";
  return s;
}
}  // namespace

bool MistralWorkspace::isListed(const std::string& id) const { return inIdSet(listed, id); }
bool MistralWorkspace::isSignedIn(const std::string& id) const { return inIdSet(signedIn, id); }

bool noteMistralWorkspaceProbe(MistralWorkspace& ws, const char* body) {
  std::vector<std::string> listed, signedIn;
  if (!parseMistralWorkspaceConnectors(body, listed, &signedIn)) return false;
  ws.listed = toIdSet(listed);       // full replace: a connector removed in Mistral drops out
  ws.signedIn = toIdSet(signedIn);
  ws.probed = true;
  return true;
}

bool isMistralStudioConnector(const ConnectorInfo& c) {
  return c.prov == "mistral" && c.kind == "connector";
}

namespace {
// Lowercase with '.', '-' and ' ' folded to '_' (Google-Calendar == google_calendar).
std::string foldId(const std::string& s) {
  std::string out = lowerStr(s);
  for (char& ch : out)
    if (ch == '.' || ch == '-' || ch == ' ') ch = '_';
  return out;
}
}  // namespace

// The catalog id a connector reference maps to, case-insensitive and alias-aware:
// a Mistral workspace id (github_app, google_calendar, google_drive_mcp) or an
// OpenAI connector id (connector_gmail, connector_googlecalendar) IS the catalog
// entry (github, gcal, gdrive, gmail) - the attach remap in reverse.
static std::string canonicalConnectorId(const std::string& raw) {
  std::string t = lowerStr(raw);
  if (t == "github_app") return "github";
  if (t == "google_calendar") return "gcal";
  if (t == "google_drive_mcp") return "gdrive";
  if (t.rfind("connector_", 0) == 0) t = t.substr(10);   // connector_gmail -> gmail
  if (t == "googlecalendar") return "gcal";
  if (t == "googledrive") return "gdrive";
  return t;
}

std::string connectorProviderForName(const std::vector<ConnectorInfo>& cs, const std::string& name) {
  const std::string want = foldId(name);
  if (want.empty()) return "";
  for (const ConnectorInfo& c : cs) {
    if (!c.enabled || c.deviceDialed) continue;
    const std::string ids[] = {c.name, c.type, c.connectorId,
                               c.prov == "mistral" ? mistralConnectorId(c) : std::string()};
    for (const std::string& id : ids)
      if (!id.empty() && foldId(id) == want) return c.prov.empty() ? std::string("any") : c.prov;
  }
  return "";
}

// ---- connector provenance (CUM-536) -------------------------------------------

bool isProviderBuiltinTool(const std::string& name) {
  return name == "web_search" || name == "web_search_premium" || name == "code_interpreter" ||
         name == "image_generation" || name == "document_library";
}

namespace {
// What a connector's data IS for the owner ("not your calendar"), and the
// phrases a task may use for the OWNER'S data instead of the connector's name.
// Phrases, not bare words: "research the Gregorian calendar" must not read as a
// request for the owner's calendar. A phrase is matched case-insensitively,
// except one with a capital letter, which is matched as written or with its
// first letter capitalized ("in Linear", "In Linear," are the product; "in linear
// time" is not). `tail` limits what may follow it, and `notAfter` names the words
// (space-separated) that may not come right before it. Ids are catalog ids; lists
// end in a null text.
enum class Tail : uint8_t {
  Any,           // any word boundary
  NotTitleCase,  // not another capitalized word ("from Linear Algebra Done Right" is a title)
  NotBookTitle,  // nor a colon and a capitalized word: "from Slack: Getting Past Burnout"
                 // cites a book ("post this to Slack: Deploy finished" is still an ask)
  NoWord,        // no further word ("in my linear" ends the ask; "my linear algebra" does not)
  NextStep,      // no further word, or one that starts the next step, a time or a
                 // topic of the ask (kAfterFromSlack: "and", "then", "about", "since",
                 // ...), lowercase, and no book subtitle
};
struct Phrase {
  const char* text;
  Tail tail = Tail::Any;
  const char* notAfter = nullptr;   // "up": "picked up my slack" is the idiom
};
const Phrase kGcalPhrases[] = {{"my calendar"},     {"your calendar"}, {"'s calendar"},
                               {"calendar event"},  {"calendar events"}, {"my schedule"},
                               {"your schedule"},   {"'s schedule"},   {nullptr}};
const Phrase kGmailPhrases[] = {{"my inbox"},  {"your inbox"}, {"'s inbox"}, {"my email"},
                                {"my emails"}, {"'s email"},   {"'s emails"}, {nullptr}};
// "notion" has no ordinary reading after these prepositions, so they match in any
// case ("add this to notion"). "my notion" is the owner's workspace only after a
// preposition and at the end of the ask ("put the plan in my notion"): "my notion
// of fairness" and "the paragraph is my notion." are the word.
const Phrase kNotionPhrases[] = {{"notion page"},      {"notion pages"},     {"notion database"},
                                 {"notion databases"}, {"notion workspace"}, {"notion doc"},
                                 {"notion docs"},      {"notion connector"}, {"in notion"},
                                 {"from notion"},      {"to notion"},        {"on notion"},
                                 {"in my notion", Tail::NoWord},             {"into my notion", Tail::NoWord},
                                 {"to my notion", Tail::NoWord},             {nullptr}};
const Phrase kGithubPhrases[] = {{"my repo"},           {"my repos"},   {"my repository"},
                                 {"my repositories"},   {"your repo"},  {"your repos"},
                                 {"your repository"},   {"your repositories"},
                                 {"'s repo"},           {"'s repos"},   {nullptr}};
// "slack" is an ordinary word after "in", "on", "to" and "from" ("in slack
// periods", "low on slack this sprint", "tend to slack in August", "from slack to
// taut", "slack off"), so those lowercase forms must end the ask ("post the
// summary in slack", "send it to slack"), "from slack" may go on only with the next
// step, a time or a topic ("summarize the messages from slack about the outage" is
// an ask; "from slack demand" is not), and none counts after the
// words that make it the ordinary one: an infinitive frame before "to slack"
// ("teams tend to slack.", "do they want to slack?") or "low" / "short" before "on
// slack" ("we ran low on slack."). The capitalized product name may go on ("post
// it in Slack for the team") unless a title follows; Slack's own features (Slack
// Connect, a canvas, a huddle, a workflow) name it in any case. "my slack" is the
// owner's workspace ("read my slack and summarize the day") except in the idiom
// "pick up my slack".
// constexpr: a const char* variable is no constant expression, so without it the
// phrase table may be built at boot in RAM instead of flash (it was: +320 B).
constexpr const char* kInfinitiveFrame =
    "tend tends tended tending want wants wanted wanting going begin begins began start starts "
    "started starting seem seems seemed need needs needed try tries tried trying like likes "
    "liked used allowed able how not never";
// The words that may follow a lowercase "from slack" in an ask: the next step, a
// time or a topic ("from slack and draft a reply", "from slack about the outage",
// "from slack since Monday"). Any other word reads "slack" as the plain word ("from
// slack to taut", "from slack demand", "from slack labor markets").
constexpr const char* kAfterFromSlack =
    "and then about for this that these those today yesterday tonight tomorrow since on at over "
    "during regarding re last past";
const Phrase kSlackPhrases[] = {{"slack channel"},   {"slack channels"}, {"slack message"},
                                {"slack messages"},  {"slack thread"},   {"slack threads"},
                                {"slack dm"},        {"slack dms"},      {"slack workspace"},
                                {"slack connector"}, {"slack connect"},  {"slack canvas"},
                                {"slack canvases"},  {"slack huddle"},   {"slack huddles"},
                                {"slack workflow"},  {"slack workflows"},
                                {"in slack", Tail::NoWord},
                                {"on slack", Tail::NoWord, "low short"},
                                {"from slack", Tail::NextStep},
                                {"to slack", Tail::NoWord, kInfinitiveFrame},
                                {"in Slack", Tail::NotTitleCase},    {"on Slack", Tail::NotTitleCase},
                                {"from Slack", Tail::NotBookTitle},  {"to Slack", Tail::NotTitleCase},
                                {"my slack", Tail::Any, "up"},       {nullptr}};
// "linear" is an ordinary word after any preposition ("in linear time"), so those
// phrases need the capital, and a title ("in Linear Programming") is not the product.
// "my linear" is the workspace only after a preposition and at the end of the ask
// ("file it in my linear"), as for Notion.
const Phrase kLinearPhrases[] = {{"linear issue"},     {"linear issues"},  {"linear ticket"},
                                 {"linear tickets"},   {"linear project"}, {"linear projects"},
                                 {"linear workspace"}, {"linear connector"},
                                 {"in Linear", Tail::NotTitleCase},   {"from Linear", Tail::NotBookTitle},
                                 {"to Linear", Tail::NotTitleCase},   {"in my linear", Tail::NoWord},
                                 {"into my linear", Tail::NoWord},    {"to my linear", Tail::NoWord},
                                 {nullptr}};
struct ProvenanceWords {
  const char* id;
  const char* noun;
  const Phrase* phrases;   // nullptr = none
  // The catalog name is an ordinary English word ("explain linear regression",
  // "the notion of", "some slack in the plan"): the bare id or display name never
  // names the connector, only a phrase does.
  bool phraseOnly;
};
const ProvenanceWords kProvenance[] = {
    {"gcal", "your calendar", kGcalPhrases, false},
    {"gmail", "your email", kGmailPhrases, false},
    {"gdrive", "your Drive files", nullptr, false},
    {"notion", "your Notion pages", kNotionPhrases, true},
    {"github", "your GitHub data", kGithubPhrases, false},
    {"slack", "your Slack messages", kSlackPhrases, true},
    {"linear", "your Linear issues", kLinearPhrases, true},
};

const ProvenanceWords* provenanceFor(const std::string& canonId) {
  for (const auto& p : kProvenance)
    if (canonId == p.id) return &p;
  return nullptr;
}

const KnownConnector* catalogEntry(const std::string& canonId) {
  int n = 0;
  const KnownConnector* k = knownConnectors(n);
  for (int i = 0; i < n; i++)
    if (canonId == k[i].id) return &k[i];
  return nullptr;
}

// The catalog id an attached connector stands for: its wire id's, else its device
// type's (an owner-set explicit id like "notion_mcp" on a notion connector), else
// the wire id itself (a custom connector).
std::string canonOf(const AttachedConnector& a) {
  const std::string fromId = canonicalConnectorId(a.id);
  if (catalogEntry(fromId) || a.type.empty()) return fromId;
  const std::string fromType = canonicalConnectorId(a.type);
  return catalogEntry(fromType) ? fromType : fromId;
}

// The owner-facing name: the catalog name, else the device name, else the wire id.
std::string displayOf(const AttachedConnector& a, const std::string& canon) {
  if (const KnownConnector* k = catalogEntry(canon)) return k->displayName;
  return a.name.empty() ? a.id : a.name;
}

bool isWordChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// text[at, at + n) is `word` (lowercase, n characters), in any case.
bool sameWordAt(const std::string& text, size_t at, const char* word, size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (tolower((unsigned char)text[at + i]) != word[i]) return false;
  return true;
}

// The word starting at `at` of `text` is one of `words` (lowercase, space-separated),
// whole, in any case.
bool wordIsOneOf(const std::string& text, size_t at, const char* words) {
  for (const char* w = words; *w;) {
    const char* e = std::strchr(w, ' ');
    const size_t n = e ? (size_t)(e - w) : std::strlen(w);
    const bool whole = at + n <= text.size() && (at + n == text.size() || !isWordChar(text[at + n]));
    if (whole && sameWordAt(text, at, w, n)) return true;
    w += n + (e ? 1 : 0);
  }
  return false;
}

// A colon at `at` followed by a capitalized word: a subtitle ("from Slack: Getting
// Past Burnout" names a book, not the product).
bool subtitleAt(const std::string& text, size_t at) {
  if (at >= text.size() || text[at] != ':') return false;
  size_t i = at + 1;
  while (i < text.size() && text[i] == ' ') i++;
  return i < text.size() && text[i] >= 'A' && text[i] <= 'Z';
}

// What follows a phrase that ended at `end` of `text` is allowed by `tail`. `text`
// is the task as written, so a subtitle is seen after a lowercase phrase too.
bool tailOk(const std::string& text, size_t end, Tail tail) {
  size_t at = end;
  while (at < text.size() && text[at] == ' ') at++;
  const bool word = at < text.size() && isWordChar(text[at]);
  const bool capital = word && text[at] >= 'A' && text[at] <= 'Z';
  switch (tail) {
    case Tail::Any: return true;
    case Tail::NoWord: return !word;
    case Tail::NextStep:
      return !subtitleAt(text, at) && (!word || (!capital && wordIsOneOf(text, at, kAfterFromSlack)));
    case Tail::NotTitleCase: return !capital;
    case Tail::NotBookTitle: return !capital && !subtitleAt(text, at);
  }
  return true;   // unreachable: every Tail is a case above (-Wswitch flags a new one)
}

// The word right before `at` in `text` (spaces skipped) is one of `words`
// (lowercase, space-separated), in any case.
bool wordBefore(const std::string& text, size_t at, const char* words) {
  while (at > 0 && text[at - 1] == ' ') at--;
  size_t b = at;
  while (b > 0 && isWordChar(text[b - 1])) b--;
  for (const char* w = words; *w;) {
    const char* e = std::strchr(w, ' ');
    const size_t n = e ? (size_t)(e - w) : std::strlen(w);
    if (n == at - b && sameWordAt(text, b, w, n)) return true;
    w += n + (e ? 1 : 0);
  }
  return false;
}

// `needle` occurs in `hay` with no letter or digit against either edge that is
// itself a letter or digit ("calendar events" matches "Google Calendar events",
// not "calendar eventsx"; "'s calendar" matches "dana's calendar"), and `tail`
// allows what follows it, and the word before it is none of `notAfter` (when set).
// Exact bytes: the caller lowercases both sides for a case-insensitive match and
// passes the task as written (`cased`, same length) for the tail to read.
bool containsWord(const std::string& hay, const std::string& needle, Tail tail = Tail::Any,
                  const char* notAfter = nullptr, const std::string* cased = nullptr) {
  if (needle.empty()) return false;
  const bool wordStart = isWordChar(needle.front());
  const bool wordEnd = isWordChar(needle.back());
  for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) {
    const bool startOk = !wordStart || at == 0 || !isWordChar(hay[at - 1]);
    const size_t end = at + needle.size();
    const bool endOk = !wordEnd || end >= hay.size() || !isWordChar(hay[end]);
    const bool leadOk = !notAfter || !wordBefore(hay, at, notAfter);
    if (startOk && endOk && leadOk && tailOk(cased ? *cased : hay, end, tail)) return true;
  }
  return false;
}

bool hasUpper(const std::string& s) {
  for (char c : s)
    if (c >= 'A' && c <= 'Z') return true;
  return false;
}

// One phrase against the task: case-insensitively (`hay` is lowercased), or, with a
// capital in it, as written or with its first letter capitalized (sentence start).
bool phraseMatches(const std::string& task, const std::string& hay, const Phrase& ph) {
  const std::string text = ph.text;
  if (!hasUpper(text)) return containsWord(hay, text, ph.tail, ph.notAfter, &task);
  std::string initial = text;
  initial[0] = (char)toupper((unsigned char)initial[0]);
  return containsWord(task, text, ph.tail) || containsWord(task, initial, ph.tail);
}

// An owner-chosen name cannot be an ordinary word when it holds a digit, a
// separator or a space, or mixes case past its first letter ("crm-eu", "work_cal",
// "HubSpot"). One made of plain letters in one case ("notes", "work", "CRM") may be
// one.
bool distinctiveName(const std::string& n) {
  bool innerUpper = false, lower = false;
  for (size_t i = 0; i < n.size(); ++i) {
    const char c = n[i];
    const bool upper = c >= 'A' && c <= 'Z';
    if (!upper && !(c >= 'a' && c <= 'z')) return true;
    innerUpper = innerUpper || (upper && i > 0);
    lower = lower || !upper;
  }
  return innerUpper && lower;
}

// A plain-letter owner-chosen name counts only in an owner-data frame: "my notes",
// "our crm", "your crm", "the notes connector", "from crm".
bool inOwnerFrame(const std::string& hay, const std::string& name) {
  for (const char* pre : {"my ", "our ", "your ", "from "})
    if (containsWord(hay, pre + name)) return true;
  return containsWord(hay, name + " connector");
}

// The provenance words for an attached connector: its catalog entry's, else, for
// a connector outside the catalog whose device name or type is an ordinary-word
// catalog id (a custom connector named "Linear"), that entry's, so the word stays
// phrase-only however the connector was added.
const ProvenanceWords* provenanceWordsFor(const AttachedConnector& a, const std::string& canon) {
  if (const ProvenanceWords* p = provenanceFor(canon)) return p;
  for (const std::string& form : {lowerStr(a.name), lowerStr(a.type)}) {
    const ProvenanceWords* p = provenanceFor(form);
    if (p && p->phraseOnly) return p;
  }
  return nullptr;
}

// Whether the task names connector `a` by one of its own names (`hay` lowercased).
// A catalog connector is named by its catalog id, display name or standard wire
// id, never by the name the owner gave it (a Notion connector named "notes" must
// not claim "summarize the meeting notes"). A connector outside the catalog has
// only its own names: a distinctive one counts bare, a plain-letter one only in an
// owner-data frame. An ordinary-word catalog id (linear, notion, slack) never
// counts bare, however the connector was added.
bool namesByName(const std::string& hay, const AttachedConnector& a, const std::string& canon,
                 const std::string& display, const ProvenanceWords* p) {
  struct Form { const std::string* text; bool fixed; };   // fixed: the catalog's own name, counts bare
  const bool catalog = catalogEntry(canon) != nullptr;
  // A fixed array pointing at the strings already held: no container is built per
  // call on the device turn path, and no static storage either.
  const Form forms[] = {{&a.id, catalog && canonicalConnectorId(a.id) == canon},
                        {&canon, catalog},
                        {&display, catalog},
                        {catalog ? nullptr : &a.name, false},
                        {catalog ? nullptr : &a.type, false}};
  for (const Form& form : forms) {
    if (!form.text) continue;
    const std::string f = lowerStr(*form.text);
    if (f.empty() || (p && p->phraseOnly && f == p->id)) continue;   // an ordinary word: phrases only
    const bool bare = form.fixed || distinctiveName(*form.text);
    if (bare ? containsWord(hay, f) : inOwnerFrame(hay, f)) return true;
  }
  return false;
}

// Whether the task (`task` as written, `hay` lowercased) names this connector: by
// one of its names, or by an owner-data phrase.
bool taskNames(const std::string& task, const std::string& hay, const AttachedConnector& a,
               const std::string& canon, const std::string& display) {
  const ProvenanceWords* p = provenanceWordsFor(a, canon);
  if (namesByName(hay, a, canon, display, p)) return true;
  if (!p || !p->phrases) return false;
  for (const Phrase* ph = p->phrases; ph->text; ++ph)
    if (phraseMatches(task, hay, *ph)) return true;
  return false;
}

// `name` (a tool.execution name, lowercase) is a run of attached connector `a`:
// it starts with the wire id, the catalog id, the wire id without a "_mcp"/"_app"
// suffix (google_drive_mcp runs google_drive_*), the device name or the device
// type, then "_" or end.
bool runNamedFor(const std::string& name, const AttachedConnector& a) {
  std::string forms[5] = {lowerStr(a.id), canonOf(a), lowerStr(a.id), lowerStr(a.name),
                          lowerStr(a.type)};
  for (const char* suffix : {"_mcp", "_app"}) {
    const size_t n = std::strlen(suffix);
    if (forms[2].size() > n && forms[2].compare(forms[2].size() - n, n, suffix) == 0)
      forms[2].resize(forms[2].size() - n);
  }
  for (const std::string& f : forms) {
    if (f.empty() || name.compare(0, f.size(), f) != 0) continue;
    if (name.size() == f.size() || name[f.size()] == '_') return true;
  }
  return false;
}
}  // namespace

std::string connectorAskedFor(const std::string& task, const std::string& skill,
                              const std::vector<AttachedConnector>& attached,
                              std::string* askedIdOut) {
  const std::string hay = lowerStr(task);
  const std::string wantSkill = canonicalConnectorId(skill);
  for (const AttachedConnector& a : attached) {
    if (a.id.empty()) continue;
    const std::string canon = canonOf(a);
    const std::string display = displayOf(a, canon);
    const bool skillNames =
        !wantSkill.empty() && (wantSkill == canon || wantSkill == lowerStr(a.name));
    if (skillNames || taskNames(task, hay, a, canon, display)) {
      if (askedIdOut) *askedIdOut = a.id;
      return display;
    }
  }
  return "";
}

bool countsAsConnectorRun(const std::string& toolName, const std::string& askedId,
                          const std::vector<AttachedConnector>& attached) {
  if (isProviderBuiltinTool(toolName)) return false;
  const std::string name = lowerStr(toolName);
  if (askedId.empty()) return true;
  for (const AttachedConnector& a : attached)
    if (a.id == askedId && runNamedFor(name, a)) return true;
  for (const AttachedConnector& other : attached)
    if (other.id != askedId && runNamedFor(name, other)) return false;   // another connector ran
  return true;   // nameless or unrecognized: some connector ran, give it the benefit of the doubt
}

std::vector<AttachedConnector> mistralAttachedConnectors(const JsonDocument& doc) {
  std::vector<AttachedConnector> out;
  for (JsonObjectConst t : doc["tools"].as<JsonArrayConst>()) {
    if (std::strcmp(t["type"] | "", "connector") != 0) continue;
    out.emplace_back(std::string(t["connector_id"] | ""), std::string(t["device_name"] | ""),
                     std::string(t["device_type"] | ""));
  }
  return out;
}

std::string connectorNotCalledNote(const std::string& displayName) {
  std::string noun = "data from your " + displayName;
  int n = 0;
  const KnownConnector* k = knownConnectors(n);
  for (int i = 0; i < n; i++) {
    if (displayName != k[i].displayName) continue;
    if (const ProvenanceWords* p = provenanceFor(k[i].id)) noun = p->noun;
    break;
  }
  return "The " + displayName + " connector was NOT called. The text below is the model's own "
         "guess, not " + noun + ".";
}

ConnectorInfo::Workspace mistralWorkspaceState(const ConnectorInfo& c, const MistralWorkspace& ws) {
  using W = ConnectorInfo::Workspace;
  if (!isMistralStudioConnector(c)) return W::NotApplicable;
  if (!ws.probed) return W::Unprobed;
  const std::string id = mistralConnectorId(c);
  if (!ws.isListed(id)) return W::NotListed;
  return ws.isSignedIn(id) ? W::ListedSignedIn : W::ListedNotSignedIn;
}

int8_t connectorAuthFor(const ConnectorInfo& c, const MistralWorkspace& ws, int8_t oauthState) {
  using W = ConnectorInfo::Workspace;
  if (c.kind == "builtin") return -1;   // authenticates provider-side
  if (c.hasToken)          return 1;
  if (c.hasOauth)          return oauthState;
  if (isMistralStudioConnector(c)) {
    const W w = mistralWorkspaceState(c, ws);
    return (w == W::ListedSignedIn || w == W::ListedNotSignedIn) ? 1 : 2;
  }
  return 2;   // a credential is required and none is stored
}

void applyConnectorAuth(std::vector<ConnectorInfo>& cs, const MistralWorkspace& ws,
                        const OauthStateFn& oauthState) {
  for (ConnectorInfo& c : cs) {
    const int8_t oauth = (c.hasOauth && oauthState) ? oauthState(c) : (int8_t)-1;
    c.auth = connectorAuthFor(c, ws, oauth);
    c.workspace = mistralWorkspaceState(c, ws);
  }
}

std::string resolveHeadHost(const std::string& orchHost, const std::string& priority,
                            const std::function<bool(const std::string&)>& keyed,
                            const std::string& routerFallback) {
  auto trim = [](const std::string& t) {
    const size_t b = t.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    return t.substr(b, t.find_last_not_of(" \t\r\n") - b + 1);
  };
  const std::string pin = trim(orchHost);
  if (!pin.empty()) return pin;
  std::string head;
  for (size_t start = 0; start <= priority.size();) {
    const size_t comma = priority.find(',', start);
    const size_t end = comma == std::string::npos ? priority.size() : comma;
    const std::string tok = trim(priority.substr(start, end - start));
    if (!tok.empty()) {
      if (head.empty()) head = tok;
      if (keyed && keyed(tok)) return tok;
    }
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  const std::string rf = trim(routerFallback);
  return rf.empty() ? head : rf;
}

bool wantsMistralWorkspaceProbe(const std::vector<ConnectorInfo>& cs) {
  for (const ConnectorInfo& c : cs)
    if (c.enabled && isMistralStudioConnector(c)) return true;
  return false;
}

bool mistralWorkspaceStale(const std::vector<ConnectorInfo>& cs, const MistralWorkspace& ws) {
  using W = ConnectorInfo::Workspace;
  for (const ConnectorInfo& c : cs) {
    if (!c.enabled || !isMistralStudioConnector(c)) continue;
    const W st = mistralWorkspaceState(c, ws);
    if (st != W::ListedSignedIn && st != W::ListedNotSignedIn) return true;
  }
  return false;
}

const char* studioCardState(const ConnectorInfo& c, const MistralWorkspace& ws, bool probePending) {
  using W = ConnectorInfo::Workspace;
  // A pending probe outranks an old "not listed": the owner may have just
  // connected it in Mistral and saved, which is exactly what queued the probe.
  switch (mistralWorkspaceState(c, ws)) {
    case W::Unprobed:  return probePending ? "checking" : "unchecked";
    case W::NotListed: return probePending ? "checking" : "unlisted";
    default:           return "";   // not a Studio connector, or listed (usable)
  }
}

void attachMistralWire(JsonDocument& d, const std::vector<ConnectorInfo>& cs, bool builtinsOnly) {
  // Two shapes, per the Mistral Conversations API (docs.mistral.ai):
  //   hosted built-in TOOL (web_search, code_interpreter, image_generation,
  //     document_library): {type:"<name>"} - the name IS the tool type.
  //   Studio CONNECTOR (github/gmail/notion or a custom beta.connectors MCP):
  //     {type:"connector", connector_id:"<name-or-uuid>"} - authenticated in
  //     Studio, referenced by name/UUID; no secret on device.
  for (const ConnectorInfo& c : cs) {
    if (builtinsOnly && c.kind != "builtin") continue;  // cumulo route: built-ins only
    if (!forwardsToProviderHead(c, "mistral")) continue;
    if (c.kind == "builtin") {
      // document_library needs a library id - bare it 422s the whole request
      // (grid-validated). Until a library id rides the blob, skip it.
      if (c.name == "document_library") continue;
      d["tools"].add<JsonObject>()["type"] = c.name;
    } else {
      JsonObject t = d["tools"].add<JsonObject>();
      t["type"] = "connector";
      // The Mistral workspace id (shared mistralConnectorId, also used by the
      // connector-verify probe). An unknown id is SILENTLY ignored by the API (the
      // request 200s, the tools never appear - field-hit on nimbus-5: "github"
      // instead of "github_app" made every GitHub probe come back empty).
      t["connector_id"] = mistralConnectorId(c);
      if (d[kMistralIdentityProbe] | false) {   // scratch doc only (never sent)
        t["device_name"] = c.name;
        t["device_type"] = c.type;
      }
    }
  }
}

void attachAnthropicWire(JsonDocument& agentBody, const std::vector<ConnectorInfo>& cs,
                         const BearerFn& bearer, bool builtinsOnly) {
  for (const ConnectorInfo& c : cs) {
    // Anthropic has no built-in connector kind here, so a cumulo route (built-ins
    // only) attaches NOTHING - the owner's BYO MCP servers stay off the shared key.
    if (builtinsOnly) continue;
    if (!forwardsToProviderHead(c, "anthropic")) continue;
    if (c.kind != "mcp" || c.url.empty()) continue;  // Anthropic = BYO MCP by URL
    JsonObject s = agentBody["mcp_servers"].add<JsonObject>();
    s["type"] = "url";
    s["url"] = c.url;
    s["name"] = c.name;
    std::string b = bearer ? bearer(c) : std::string();
    if (!b.empty()) s["authorization_token"] = b;
  }
}

// ---- catalog text ------------------------------------------------------------

// The parenthetical after an enabled connector's name in its provider row. W12:
// enabled is a CHECKBOX, not health - only the states that need intervention (or
// that change what a result means) get a note, so a bare name means "no known
// problem" and enabled-but-unusable can't masquerade as working.
static std::string connectorRowNote(const ConnectorInfo& c, const char* prov,
                                    bool probePending) {
  using W = ConnectorInfo::Workspace;
  if (c.auth == 0) return " (sign-in FAILED - tell the owner)";
  if (c.auth == 2) {
    // Fail-closed, but honest about WHY: an unchecked workspace is not the same
    // next step as a connector the workspace does not offer, and a check that is
    // running right now (a save or restart queued it) settles both.
    if (probePending && (c.workspace == W::Unprobed || c.workspace == W::NotListed))
      return " (not usable yet: the Mistral workspace check is running now, so try "
             "again in a minute rather than sending the owner to Mistral)";
    if (c.workspace == W::Unprobed)
      return " (not usable yet: the Mistral workspace has not been checked; it is "
             "checked after a restart, when a connector is saved, and when the "
             "Mistral key is verified or saved)";
    // A Mistral Studio connector / hosted built-in has NO device credential: it
    // is authorized in the owner's Mistral account and referenced by name here,
    // so "add a credential" is the wrong instruction - point at Mistral instead.
    if (!std::strcmp(prov, "mistral") && c.kind != "mcp")
      return " (not usable until the owner enables it in their Mistral Studio account)";
    return " (NO credential - not usable until the owner adds one)";
  }
  // Usable. is_authenticated is a hint, never a gate: a listed connector that
  // reports it false still ran live, so say what an empty result may mean.
  if (c.workspace == W::ListedNotSignedIn)
    return " (usable: always try it first. Mistral's signed-in flag is unreliable for "
           "it, so never skip it on that basis; only if it returns nothing or asks to "
           "sign in, tell the owner to connect it in Mistral)";
  return "";
}

std::string catalogText(const std::vector<ConnectorInfo>& cs, const ProviderState& ps) {
  std::string out = "\n[PROVIDERS & CONNECTORS]\n";
  const std::string& host = ps.currentHost;
  const bool hostKnown = (host == "openai" || host == "anthropic" || host == "mistral");
  if (hostKnown) {
    out += "You are currently running on ";
    out += host;
    out += ". Connectors on ";
    out += host;
    out += " are callable on YOUR OWN turns; connectors on the other providers are "
           "reachable only by spawning a sub-agent on that provider.\n";
    if (host == "mistral" && !ps.mistralHeadCarriesStudio)
      out += "EXCEPTION: Mistral Studio connectors (calendar, Notion, Slack, Gmail, "
             "Drive, GitHub...) are NOT attached to your own turns here; they run ONLY "
             "on a sub-agent you spawn on mistral (session_ops spawn: provider = "
             "mistral, skill = the connector name).\n";
    out += "For a HEAVY connector action - creating or updating a document/page/"
           "issue, or fetching a large file - prefer to SPAWN a sub-agent on the "
           "connector's provider (session_ops spawn: provider = that provider, "
           "skill = the connector name) rather than doing it on your own turn. The "
           "sub-agent runs the connector on the provider's compute and returns a "
           "short result; use your own turn for light reads.\n";
  }

  struct Row {
    const char* prov;
    bool keyed;
    int8_t verified;    // 1 ok / 0 rejected / -1 unchecked
    const char* native;
  };
  const Row rows[] = {
      {"openai", ps.openaiKeyed, ps.openaiVerified,
       "connectors/MCP run server-side on your own turns AND on sub-agents; "
       "hosted web_search on sub-agents"},
      {"anthropic", ps.anthropicKeyed, ps.anthropicVerified,
       "sub-agents run in a cloud sandbox with bash, file read/write, web "
       "search/fetch; connectors (BYO MCP by URL) attach to SUB-AGENTS only, "
       "not your own turns"},
      {"mistral", ps.mistralKeyed, ps.mistralVerified,
       ps.mistralHeadCarriesStudio
           ? "connectors + built-ins attach to a single-shot turn AND to sub-agents "
             "you spawn on mistral, but NOT to your tool-loop turns (the loop forces "
             "tool_choice, which the provider rejects with built-ins) - so on your own "
             "loop turns you have only the registry tools; spawn a mistral sub for "
             "connector work"
           : "built-ins attach to a single-shot turn AND to sub-agents you spawn on "
             "mistral; Studio connectors run ONLY on sub-agents you spawn on mistral, "
             "never on your own turns: spawn a mistral sub for connector work"},
  };
  for (const Row& r : rows) {
    out += "- ";
    out += r.prov;
    if (hostKnown && host == r.prov) out += " (YOU are here)";
    // key-presence is NOT validity: say which it is (owner: truly tested). With
    // validation OFF (capProbe==0) the device makes no verified/rejected claim -
    // it just reports the key is present and trusts it.
    out += !r.keyed          ? ": NO KEY (unavailable). "
         : ps.capProbe == 0  ? ": key present (validation off - trusting key presence). "
         : r.verified == 1   ? ": available, VERIFIED. "
         : r.verified == 0   ? ": key present but REJECTED on last check (likely unusable). "
                             : ": key present, not yet verified. ";
    out += r.native;
    bool any = false;
    for (const ConnectorInfo& c : cs) {
      if (!provMatches(c, r.prov)) continue;
      out += any ? ", " : ". Enabled connectors: ";
      out += c.name;
      out += connectorRowNote(c, r.prov, ps.mistralProbePending);   // W12 state note ("" when healthy)
      any = true;
    }
    out += "\n";
  }
  // Per-connector real capabilities/limits - so you use only tools that EXIST and
  // never fake an outcome (owner-caught: claimed 'email sent' when Gmail only drafts).
  int kn = 0;
  const KnownConnector* kk = knownConnectors(kn);
  std::string capsBlock;
  for (const ConnectorInfo& c : cs) {
    if (!c.enabled) continue;
    const char* caps = nullptr;
    for (int i = 0; i < kn; i++)
      if (c.type == kk[i].id || c.name == kk[i].id) { caps = kk[i].caps; break; }
    if (caps && caps[0]) {
      capsBlock += "  - " + c.name + ": " + caps + "\n";
    } else if (c.kind == "mcp" && !c.url.empty()) {
      // A CUSTOM MCP the owner added (not in the built-in catalog): the model
      // used to see only its bare name with no idea what it does. Surface the
      // endpoint + an honest "capabilities unknown - discover its tools, don't
      // assume" so it's usable without a hand-authored caps entry.
      capsBlock += "  - " + c.name + " (custom MCP at " + c.url +
                   "): capabilities not catalogued - call it to discover its "
                   "tools; do not assume what it can do.\n";
    }
  }
  if (!capsBlock.empty())
    out += "Connector capabilities/limits (use only tools that exist; verify writes "
           "by reading them back; if an action has no tool, say so):\n" + capsBlock;

  // Discoverability (live bad answer: "can you create git repos?" - GitHub was
  // unconfigured, so the caps block said NOTHING about it and the model was left
  // to guess). Two bounded lines name the available-but-unconfigured catalog
  // entries and the configured-but-DISABLED ones, so the model can answer
  // "could you do X?" honestly: yes, once the owner sets it up / re-enables it.
  // Placed before [SUB-AGENT CAPABILITIES]; the section clips from the tail, so
  // early lines are safe. Rules (prism-folded):
  //  - matching is case-insensitive and alias-aware (a Mistral-namespace
  //    "github_app" row IS the github entry - the attach remap in reverse);
  //  - an id whose providers intersect NO keyed provider is skipped (advertising
  //    "owner can add slack" with no Mistral key would be a false promise);
  //  - document_library is skipped until its library-id plumbing exists.
  {
    auto keyedFor = [&](const char* providers) {
      const std::string p(providers);
      return (ps.openaiKeyed && p.find("openai") != std::string::npos) ||
             (ps.anthropicKeyed && p.find("anthropic") != std::string::npos) ||
             (ps.mistralKeyed && p.find("mistral") != std::string::npos);
    };
    std::string notCfg, disabled;
    for (int i = 0; i < kn; i++) {
      const std::string id = kk[i].id;
      if (id == "document_library") continue;   // needs library-id plumbing
      if (!keyedFor(kk[i].providers)) continue; // no keyed provider can run it
      bool have = false, en = false;
      for (const ConnectorInfo& c : cs)
        if (canonicalConnectorId(c.type.empty() ? c.name : c.type) == id ||
            canonicalConnectorId(c.name) == id) {
          have = true;
          en = en || c.enabled;
        }
      if (!have) {
        if (!notCfg.empty()) notCfg += ", ";
        notCfg += id;
      } else if (!en) {
        if (!disabled.empty()) disabled += ", ";
        disabled += id;
      }
    }
    if (!notCfg.empty())
      out += "Not configured (owner can add on the device web page, Assistant > "
             "Connectors): " + notCfg + "\n";
    if (!disabled.empty())
      out += "Configured but turned OFF (owner can re-enable on the device web page, "
             "Assistant > Connectors): " +
             disabled + "\n";
  }

  // [SUB-AGENT CAPABILITIES] - generated per KEYED provider from the live enabled
  // set, so the model plans spawns against what actually exists (owner: dynamic,
  // not hardcoded). The universal truth first: a sub-agent is text-only.
  out += "\n[SUB-AGENT CAPABILITIES]\n"
         "A sub-agent returns TEXT ONLY to you and has NO DEVICE tools: it cannot "
         "send Telegram, write the device file store, or touch memory - so YOU "
         "(the head) do every OWNER-delivery step after it returns (artifact.save, "
         "then delivery). It CAN run its provider's connectors server-side, so to "
         "DRAFT an email or CREATE a Notion/Drive page, spawn a sub on a provider "
         "that has that connector and it performs the action in its own run and "
         "reports back. Decompose a big ask this way: sub-agents GATHER/RESEARCH "
         "(and run connectors); the HEAD assembles the results and delivers to the "
         "owner. Give each sub a project so its full reply auto-saves as a doc you "
         "can then deliver.\n";
  auto spawnTargets = [&](const char* prov, bool keyed) {
    if (!keyed) return;
    std::string line = std::string("- spawn on ") + prov + ": ";
    std::vector<std::string> names;
    for (const ConnectorInfo& c : cs)
      if (provMatches(c, prov)) names.push_back(c.name);
    if (!std::strcmp(prov, "anthropic"))
      line += "cloud sandbox (bash, file read/write, web search/fetch)";
    else
      line += "web_search + hosted tools";
    if (!names.empty()) {
      line += "; connectors: ";
      for (size_t i = 0; i < names.size(); i++) line += (i ? ", " : "") + names[i];
    }
    out += line + "\n";
  };
  spawnTargets("openai", ps.openaiKeyed);
  spawnTargets("anthropic", ps.anthropicKeyed);
  spawnTargets("mistral", ps.mistralKeyed);
  // W12 (owner ask): what each sandbox is FOR - "can a sub be used for coding?"
  // had no honest answer in the prompt.
  // Verified live against the hosted GitHub MCP (tools/list): create_repository
  // and push_files/create_or_update_file ARE exposed - the old "no sub can push"
  // line was false in the API-write sense. The true limit is clone/build/run.
  out += "For CODING tasks: an anthropic sub has a real sandbox (writes AND runs "
         "code, bash + files); openai/mistral subs run Python via code_interpreter "
         "when it is enabled. The github connector works the GitHub API - issues/"
         "PRs/code search, and with a repo-scoped PAT it can CREATE repositories "
         "and push file contents - but it cannot clone, build, or run a repo.\n";

  // ⚠ HOW LONG A SUB MAY RUN - this was disclosed NOWHERE, and the model had no
  // way to know it. Live (2026-08-09, Nimbus-4): asked for an AI-news digest, the
  // model sent a multi-source sweep to a mistral sub; Mistral's Conversations API
  // is SYNCHRONOUS (no background/job id), the device's read deadline is 60 s, and
  // the sub's work was simply lost. The finance sub, a lighter ask, came back
  // fine. Routing is the ONLY lever - the cap is a provider limitation, not a knob
  // (and NEVER a reason to add device concurrency; see the stability constraint in
  // AGENTS.md). Emitted only for the providers that are actually keyed.
  if (ps.mistralKeyed && (ps.openaiKeyed || ps.anthropicKeyed)) {
    out += "HOW LONG A SUB MAY RUN: a mistral sub is capped at ~60 SECONDS - its "
           "API is synchronous, so the device cannot wait past that and the sub's "
           "work is LOST, not delayed. Keep mistral subs to ONE focused lookup. "
           "For anything longer - a multi-source sweep, reading several pages, "
           "producing a document - spawn on ";
    out += ps.openaiKeyed ? (ps.anthropicKeyed ? "openai or anthropic" : "openai")
                          : "anthropic";
    out += ", which run asynchronously and may take minutes.\n";
  } else if (ps.mistralKeyed) {
    out += "HOW LONG A SUB MAY RUN: a mistral sub is capped at ~60 SECONDS (its "
           "API is synchronous) and its work is LOST past that, so keep each sub "
           "to ONE focused lookup and split a big ask across several waves.\n";
  }

  // PDF/binary delivery. The device cannot render a PDF, but a sub CAN produce one
  // in its provider sandbox and the firmware captures the generated file - so
  // "I can't make PDFs" is FALSE and was said on hardware twice. State the recipe
  // where the model plans decomposition, not only as a per-connector footnote.
  // ⚠ The recipe is only true when an OpenAI-reachable code_interpreter is
  // ENABLED. Live (Board 1, 2026-08-09) the first version of this block promised
  // the openai PDF path on a board whose only code_interpreter was registered
  // under mistral: the model dutifully spawned on openai, the sub had no sandbox,
  // it returned prose ("I can produce a Markdown layout so you can convert it")
  // and a retry sub came back "(completed, no text output)". Promising a
  // capability the board is not configured for is the same class of lie this
  // whole block exists to remove - so gate on the connector, not just the key.
  bool openaiCodeInterp = false;
  for (const ConnectorInfo& c : cs) {
    if (!c.enabled) continue;
    if ((c.type.empty() ? c.name : c.type) != "code_interpreter") continue;
    // kind==builtin mirrors attachOpenAIWire exactly (prism): a custom MCP or
    // credential-less connector merely NAMED code_interpreter never attaches a
    // sandbox, so it must not light the PDF recipe either.
    if (c.kind != "builtin") continue;
    if (provMatches(c, "openai")) { openaiCodeInterp = true; break; }
  }
  if (ps.openaiKeyed && openaiCodeInterp) {
    // W15 (owner): the PROCEDURE lives in the deliver-pdf SKILL (owner-editable,
    // no OTA to fix a recipe); the catalog carries only this config-gated
    // DISCLOSURE. The honest negative stays ambient - the field failures were
    // the model claiming inability, and a skill can't correct a belief that
    // never goes looking.
    out += "TO DELIVER A PDF (or other generated document): POSSIBLE on this "
           "device. Do NOT say you cannot produce a PDF and do not downgrade to "
           "markdown - read the deliver-pdf skill (skill.get) and follow it.\n";
  } else if (ps.openaiKeyed) {
    out += "TO DELIVER A PDF: not possible right now - openai is the PDF path but "
           "its Code Interpreter is not enabled on this device, so no sub can "
           "render a file. Offer markdown or text, and tell the owner PDF needs "
           "Code Interpreter enabled for OpenAI on the device's web page. Do not "
           "spawn a sub to 'make a PDF' - it has no sandbox and will return "
           "prose.\n";
  } else {
    // Precise wording (prism): an anthropic sub CAN build a file in its sandbox
    // - but a sub returns text only and file CAPTURE exists for mistral/openai
    // (provider_file_fetch), so nothing can DELIVER it. Saying "can't generate"
    // 40 lines below "anthropic subs have file read/write" invites the model to
    // argue with its own prompt.
    out += "TO DELIVER A PDF: not possible right now - no keyed provider can "
           "get a generated file BACK to this device (openai's sandbox is the "
           "capture path and it has no key; an anthropic sub can build files "
           "in its sandbox but returns text only). Offer markdown or text and "
           "say plainly that PDF delivery needs an OpenAI key.\n";
  }

  // Code sandbox availability (CUM-49): tell the model whether it can run code to
  // build files, and the owner-facing way to turn it on when it is off.
  {
    bool sandbox = ps.openaiKeyed && openaiCodeInterp;   // must be KEYED to actually run
    for (const ConnectorInfo& c : cs) {
      if (!c.enabled || c.kind != "builtin") continue;
      if ((c.type.empty() ? c.name : c.type) != "code_interpreter") continue;
      if (provMatches(c, "mistral") && ps.mistralKeyed) sandbox = true;
    }
    if (sandbox)
      out += "CODE SANDBOX: available - you can run code to build files; finished "
             "files land under Files on the device (via the provider Files API).\n";
    else
      out += "CODE SANDBOX: off. To enable it, the owner turns on Code sandbox on the "
             "device web page (Assistant > Tools > Tool use); it needs an OpenAI "
             "key (or Mistral where supported).\n";
  }

  bool anyEnabled = false;
  for (const ConnectorInfo& c : cs)
    if (c.enabled) { anyEnabled = true; break; }
  if (!anyEnabled)
    out += "(no connectors configured; the owner adds them on the device web page)\n";
  return out;
}

std::string knownCatalogJson() {
  JsonDocument d;
  JsonArray arr = d.to<JsonArray>();
  int n = 0;
  const KnownConnector* k = knownConnectors(n);
  for (int i = 0; i < n; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = k[i].id;
    o["name"] = k[i].displayName;
    o["providers"] = k[i].providers;
    o["kind"] = k[i].kind;
    o["cid"] = k[i].connectorId;
    o["cred"] = k[i].credentialLabel;
    o["desc"] = k[i].oneLine;
    o["docs"] = k[i].docsSlug;
    o["caps"] = k[i].caps;
  }
  std::string out;
  serializeJson(d, out);
  return out;
}

// --- capability scope (CUM-159) ----------------------------------------------
const char* capScopeSlug(CapScope s) {
  switch (s) {
    case CapScope::OrchestratorDirect: return "orchestrator-direct";
    case CapScope::SubsessionsOnly:    return "subsessions-only";
    case CapScope::Unavailable:        return "unavailable";
  }
  return "unavailable";
}

const char* capScopeLabel(CapScope s) {
  switch (s) {
    case CapScope::OrchestratorDirect: return "Orchestrator";
    case CapScope::SubsessionsOnly:    return "Sub-agents";
    case CapScope::Unavailable:        return "Unavailable";
  }
  return "Unavailable";
}

static bool providerKeyed(const std::string& prov, const ProviderState& ps) {
  if (prov == "openai")    return ps.openaiKeyed;
  if (prov == "anthropic") return ps.anthropicKeyed;
  if (prov == "mistral")   return ps.mistralKeyed;
  // A provider-agnostic connector ("any") rides whatever provider the turn uses,
  // so it is usable as long as ANY provider is keyed.
  if (prov == "any")       return ps.openaiKeyed || ps.anthropicKeyed || ps.mistralKeyed;
  return false;   // a custom/unknown provider name has no key state here
}

CapScope connectorScope(const ConnectorInfo& c, const ProviderState& ps) {
  if (!providerKeyed(c.prov, ps)) return CapScope::Unavailable;   // NO KEY
  if (!c.enabled)                 return CapScope::Unavailable;   // owner turned it off
  if (c.auth == 0 || c.auth == 2) return CapScope::Unavailable;   // sign-in FAILED / credential missing
  // Keyed + enabled + credential ok. The head can run it on its OWN turns only
  // when the connector lives on the current host provider (or is provider-
  // agnostic); otherwise the head must spawn a sub-agent on that provider - the
  // exact split catalogText() states in prose.
  const bool onHost = (c.prov == "any") ||
                      (!ps.currentHost.empty() && c.prov == ps.currentHost);
  // A Studio connector that does not ride the Mistral head turn is sub-agent-only
  // even when mistral is the head (see ProviderState.mistralHeadCarriesStudio).
  if (onHost && !ps.mistralHeadCarriesStudio && isMistralStudioConnector(c))
    return CapScope::SubsessionsOnly;
  return onHost ? CapScope::OrchestratorDirect : CapScope::SubsessionsOnly;
}

}  // namespace orch
}  // namespace nimbus
