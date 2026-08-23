#include "nimbus/orch/connectors_wire.h"

#include <cstring>   // std::strcmp (sub-agent-capabilities provider match)

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
    {"slack", "Slack", "mistral", "mcp", "",
     "Bot token (xoxb-…)",
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
     "writes is captured to the file store, then you files.send it to the owner.",
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

// ---- attach builders ---------------------------------------------------------

void attachOpenAIWire(JsonDocument& d, const std::vector<ConnectorInfo>& cs, const BearerFn& bearer) {
  for (const ConnectorInfo& c : cs) {
    if (!provMatches(c, "openai")) continue;
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

void attachMistralWire(JsonDocument& d, const std::vector<ConnectorInfo>& cs) {
  // Two shapes, per the Mistral Conversations API (docs.mistral.ai):
  //   hosted built-in TOOL (web_search, code_interpreter, image_generation,
  //     document_library): {type:"<name>"} - the name IS the tool type.
  //   Studio CONNECTOR (github/gmail/notion or a custom beta.connectors MCP):
  //     {type:"connector", connector_id:"<name-or-uuid>"} - authenticated in
  //     Studio, referenced by name/UUID; no secret on device.
  for (const ConnectorInfo& c : cs) {
    if (!provMatches(c, "mistral")) continue;
    if (c.kind == "builtin") {
      // document_library needs a library id - bare it 422s the whole request
      // (grid-validated). Until a library id rides the blob, skip it.
      if (c.name == "document_library") continue;
      d["tools"].add<JsonObject>()["type"] = c.name;
    } else {
      JsonObject t = d["tools"].add<JsonObject>();
      t["type"] = "connector";
      // Mistral's connector ids are its OWN namespace (workspace-listable via
      // GET /v1/connectors) - NOT OpenAI's "connector_*" first-party ids the
      // catalog defaults carry. An unknown id is SILENTLY ignored by the API
      // (the request 200s, the tools never appear - field-hit on nimbus-5:
      // "github" instead of "github_app" made every GitHub probe come back
      // empty). An owner-set explicit id wins; a blank or OpenAI-namespace id
      // maps through the canonical table.
      std::string cid = c.connectorId;
      if (cid.empty() || cid.rfind("connector_", 0) == 0) {
        const std::string& t2 = c.type.empty() ? c.name : c.type;
        cid = (t2 == "github") ? "github_app"
            : (t2 == "gcal")   ? "google_calendar"
            : (t2 == "gdrive") ? "google_drive_mcp"
                               : t2;
      }
      t["connector_id"] = cid;
    }
  }
}

void attachAnthropicWire(JsonDocument& agentBody, const std::vector<ConnectorInfo>& cs,
                         const BearerFn& bearer) {
  for (const ConnectorInfo& c : cs) {
    if (!provMatches(c, "anthropic")) continue;
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
       "connectors + built-ins attach to a single-shot turn AND to sub-agents "
       "you spawn on mistral, but NOT to your tool-loop turns (the loop forces "
       "tool_choice, which the provider rejects with built-ins) - so on your own "
       "loop turns you have only the registry tools; spawn a mistral sub for "
       "connector work"},
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
      // W12: enabled is a CHECKBOX, not health - surface only the states that
      // need intervention, so a bare name means "no known problem" (a healthy
      // list stays cheap; enabled-but-unusable can't masquerade as working).
      if (c.auth == 0)      out += " (sign-in FAILED - tell the owner)";
      else if (c.auth == 2) out += " (NO credential - not usable until the owner adds one)";
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
    auto lower = [](std::string s) {
      for (char& ch : s) if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
      return s;
    };
    auto canonical = [&](const std::string& raw) {
      std::string t = lower(raw);
      if (t == "github_app") return std::string("github");
      if (t == "google_calendar") return std::string("gcal");
      if (t == "google_drive_mcp") return std::string("gdrive");
      if (t.rfind("connector_", 0) == 0) t = t.substr(10);   // connector_gmail -> gmail
      if (t == "googlecalendar") return std::string("gcal");
      if (t == "googledrive") return std::string("gdrive");
      return t;
    };
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
        if (canonical(c.type.empty() ? c.name : c.type) == id ||
            canonical(c.name) == id) {
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
      out += "Not configured (owner can add on the web page, Capabilities > "
             "Connectors): " + notCfg + "\n";
    if (!disabled.empty())
      out += "Configured but turned OFF (owner can re-enable on the web page): " +
             disabled + "\n";
  }

  // [SUB-AGENT CAPABILITIES] - generated per KEYED provider from the live enabled
  // set, so the model plans spawns against what actually exists (owner: dynamic,
  // not hardcoded). The universal truth first: a sub-agent is text-only.
  out += "\n[SUB-AGENT CAPABILITIES]\n"
         "A sub-agent returns TEXT ONLY to you and has NO DEVICE tools: it cannot "
         "send Telegram, write the device file store, or touch memory - so YOU "
         "(the head) do every OWNER-delivery step after it returns (artifact.save, "
         "files.send). It CAN run its provider's connectors server-side, so to "
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
             "device web page (Capabilities > Assistant > Tools); it needs an OpenAI "
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

}  // namespace orch
}  // namespace nimbus
