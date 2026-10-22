#include "nimbus/orch/models_tool.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "nimbus/orch/model_catalog.h"    // ModelRole + role tokens
#include "nimbus/orch/provider_slots.h"

namespace nimbus {
namespace orch {

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

bool containsCi(const std::string& hay, const std::string& needleLower) {
  return needleLower.empty() || lower(hay).find(needleLower) != std::string::npos;
}

// Days since 1970-01-01 -> civil y/m/d (H. Hinnant's days_from_civil inverse).
void civil(uint32_t days, int& y, unsigned& m, unsigned& d) {
  const int64_t z = (int64_t)days + 719468;
  const int64_t era = z / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = (int)(yoe + era * 400 + (m <= 2 ? 1 : 0));
}

// A role token -> its bit (0 when unknown).
uint16_t roleBit(const std::string& token) {
  for (unsigned i = 0; i < 16; ++i) {
    const char* t = roleToken((ModelRole)(1u << i));
    if (t[0] && token == t) return (uint16_t)(1u << i);
  }
  return 0;
}

const uint16_t kChatRoles = RoleOrchestrator | RoleSubAgent;

bool rowMatches(const ModelRow& r, const ModelsListQuery& q, uint16_t roleMask,
                const std::string& lineLc, const std::string& queryLc) {
  const uint16_t roles = r.roles ? r.roles : kChatRoles;
  if (!(roles & roleMask)) return false;
  if (!q.includeLegacy && (r.lifecycle == "legacy" || r.lifecycle == "deprecated")) return false;
  if (!containsCi(r.line, lineLc)) return false;
  return containsCi(r.id, queryLc) || containsCi(r.name, queryLc);
}

void putText(JsonObject o, const char* k, const std::string& v) {
  if (v.empty()) o[k] = nullptr;
  else o[k] = v;
}

void putRow(JsonArray models, const std::string& provider, const ModelRow& r) {
  JsonObject o = models.add<JsonObject>();
  o["provider"] = provider;
  o["id"] = r.id;
  putText(o, "name", r.name);
  putText(o, "line", r.line);
  if (r.size) o["size"] = std::string(1, r.size);
  else o["size"] = nullptr;
  putText(o, "released", utcDate(r.released));
  if (r.ctx) o["ctx"] = r.ctx;
  else o["ctx"] = nullptr;
  o["lifecycle"] = r.lifecycle.empty() ? std::string("active") : r.lifecycle;
  JsonObject p = o["price"].to<JsonObject>();
  const bool priced = (r.priceIn || r.priceOut) && !r.priceSource.empty() && r.priceSource != "none";
  if (priced) {
    p["in"] = (double)r.priceIn / 1e6;
    p["out"] = (double)r.priceOut / 1e6;
  } else {
    p["in"] = nullptr;
    p["out"] = nullptr;
  }
  p["source"] = priced ? r.priceSource : std::string("none");
}

const RefreshNote* noteFor(const std::vector<RefreshNote>& notes, const std::string& p) {
  for (const RefreshNote& n : notes)
    if (n.provider == p) return &n;
  return nullptr;
}

// The matching rows of each list, in list order.
std::vector<std::vector<const ModelRow*>> matchingRows(const std::vector<ProviderModels>& lists,
                                                        const ModelsListQuery& q) {
  const uint16_t roleMask = q.role.empty() ? kChatRoles : roleBit(q.role);
  const std::string lineLc = lower(q.line), queryLc = lower(q.query);
  std::vector<std::vector<const ModelRow*>> out(lists.size());
  for (size_t i = 0; i < lists.size(); ++i)
    for (const ModelRow& r : lists[i].rows)
      if (rowMatches(r, q, roleMask, lineLc, queryLc)) out[i].push_back(&r);
  return out;
}

std::string noteText(const std::vector<ProviderModels>& lists, size_t more, bool builtIn) {
  std::string note;
  if (builtIn) note = "a provider with source built-in has no fetched list yet; ask with refresh:true. ";
  if (more) note += "More models match: narrow with provider, query or line.";
  if (lists.empty()) note = "No provider has a key.";
  while (!note.empty() && note.back() == ' ') note.pop_back();
  return note;
}

}  // namespace

const char* refreshAskToken(RefreshAsk a) {
  switch (a) {
    case RefreshAsk::Queued:  return "queued";
    case RefreshAsk::Pending: return "pending";
    case RefreshAsk::Recent:  return "recent";
    case RefreshAsk::NoKey:   return "no-key";
  }
  return "pending";
}

std::string utcDate(uint32_t epoch) {
  if (!epoch) return "";
  int y;
  unsigned m, d;
  civil(epoch / 86400u, y, m, d);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", y, m, d);
  return buf;
}

std::string utcMinute(uint32_t epoch) {
  if (!epoch) return "";
  const uint32_t s = epoch % 86400u;
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%sT%02u:%02uZ", utcDate(epoch).c_str(), (unsigned)(s / 3600u),
                (unsigned)(s / 60u % 60u));
  return buf;
}

bool parseModelsListQuery(ArduinoJson::JsonObjectConst a, ModelsListQuery& q, std::string& err) {
  struct Str { const char* key; std::string* out; };
  const Str strs[] = {{"provider", &q.provider}, {"line", &q.line}, {"query", &q.query}, {"role", &q.role}};
  for (const Str& s : strs) {
    if (a[s.key].isNull()) continue;
    if (!a[s.key].is<const char*>()) { err = std::string("'") + s.key + "' must be a string"; return false; }
    *s.out = a[s.key].as<const char*>();
  }
  struct Flag { const char* key; bool* out; };
  const Flag flags[] = {{"include_legacy", &q.includeLegacy}, {"refresh", &q.refresh}};
  for (const Flag& f : flags) {
    if (a[f.key].isNull()) continue;
    if (!a[f.key].is<bool>()) { err = std::string("'") + f.key + "' must be true or false"; return false; }
    *f.out = a[f.key].as<bool>();
  }
  q.provider = lower(q.provider);
  q.role = lower(q.role);
  if (!q.provider.empty() && !isProviderSlug(q.provider.c_str())) {
    err = "unknown provider '" + q.provider + "' (one of: cumulo, openai, anthropic, mistral, zai)";
    return false;
  }
  if (!q.role.empty() && !roleBit(q.role)) {
    err = "unknown role '" + q.role + "' (orchestrator, sub-agent, vision, embedding, stt, tts, image)";
    return false;
  }
  return true;
}

std::string modelsListJson(const std::vector<ProviderModels>& lists, const ModelsListQuery& q,
                           const std::vector<RefreshNote>& refresh) {
  const std::vector<std::vector<const ModelRow*>> match = matchingRows(lists, q);
  JsonDocument doc;
  JsonArray provs = doc["providers"].to<JsonArray>();
  bool builtIn = false;
  size_t total = 0;
  for (size_t i = 0; i < lists.size(); ++i) {
    const ProviderModels& pm = lists[i];
    JsonObject o = provs.add<JsonObject>();
    o["provider"] = pm.provider;
    putText(o, "fetchedAt", pm.fetched ? utcMinute(pm.fetchedAt) : std::string());
    o["source"] = pm.fetched ? "fetched" : "built-in";
    o["total"] = (uint32_t)match[i].size();
    if (pm.more) o["notStored"] = pm.more;
    const RefreshNote* n = noteFor(refresh, pm.provider);
    if (n) o["refresh"] = refreshAskToken(n->ask);
    else o["refresh"] = nullptr;
    builtIn = builtIn || !pm.fetched;
    total += match[i].size();
  }
  // Take the providers' rows in turn, so one long list cannot crowd out the rest.
  JsonArray models = doc["models"].to<JsonArray>();
  size_t shown = 0;
  for (size_t k = 0; shown < kModelsListMaxRows && shown < total; ++k)
    for (size_t i = 0; i < lists.size() && shown < kModelsListMaxRows; ++i)
      if (k < match[i].size()) {
        putRow(models, lists[i].provider, *match[i][k]);
        ++shown;
      }
  doc["more"] = (uint32_t)(total - shown);
  const std::string note = noteText(lists, total - shown, builtIn);
  if (!note.empty()) doc["note"] = note;
  std::string out;
  serializeJson(doc, out);
  return out;
}

void registerModelsTool(ToolRegistry& reg, const ModelsToolHandlers& h) {
  reg.add(
      "models.list",
      "List the models a provider offers, from the device's cached list: the id (what "
      "orch_model and a spawn take), name, line, size, release date, context, lifecycle, "
      "price per 1M tokens with its source, and when the list was fetched. At most 20 "
      "rows; narrow with provider, query or line. refresh:true also queues a fresh "
      "fetch from the provider for after this turn (at most once an hour per provider); "
      "this call still answers from the cache.",
      [h](ArduinoJson::JsonObjectConst a, const Principal&) -> ToolResult {
        ModelsListQuery q;
        std::string err;
        if (!parseModelsListQuery(a, q, err)) return ToolResult::fail(err);
        const std::vector<std::string> keyed =
            h.keyedProviders ? h.keyedProviders() : std::vector<std::string>();
        std::vector<std::string> provs = keyed;
        if (!q.provider.empty()) {
          if (std::find(keyed.begin(), keyed.end(), q.provider) == keyed.end())
            return ToolResult::fail(q.provider + " has no API key on this device, so it has no "
                                    "model list; only the owner adds keys");
          provs.assign(1, q.provider);
        }
        std::vector<RefreshNote> notes;
        if (q.refresh && h.requestRefresh)
          for (const std::string& p : provs) notes.push_back(RefreshNote{p, h.requestRefresh(p)});
        std::vector<ProviderModels> lists;
        for (const std::string& p : provs) {
          ProviderModels pm;
          if (h.load) h.load(p, pm);
          pm.provider = p;
          lists.push_back(std::move(pm));
        }
        return ToolResult::ok(modelsListJson(lists, q, notes));
      },
      R"({"type":"object","properties":{)"
      R"("provider":{"type":"string","description":"cumulo, openai, anthropic, mistral or zai; omit for every provider with a key"},)"
      R"("query":{"type":"string","description":"part of an id or name, e.g. sonnet or 5.5"},)"
      R"("line":{"type":"string","description":"a model line, e.g. claude-opus or gpt-6"},)"
      R"("role":{"type":"string","description":"orchestrator, sub-agent, vision, embedding, stt, tts or image; default the chat models"},)"
      R"("include_legacy":{"type":"boolean","description":"also legacy and deprecated models"},)"
      R"("refresh":{"type":"boolean","description":"queue a fresh fetch from the provider for after this turn"}}})");
}

std::string modelsPointerLine(const std::vector<std::string>& keyedProviders) {
  if (keyedProviders.empty()) return "[AVAILABLE MODELS] (none configured)\n";
  std::string names;
  for (const std::string& p : keyedProviders) names += (names.empty() ? "" : ", ") + p;
  return "[AVAILABLE MODELS] providers with a key: " + names +
         ". Call models.list for the model ids each one offers before you name one in "
         "orch_model or a spawn; ids written <upstream>/<model> are router ids and run on "
         "cumulo only.\n";
}

bool routerIdOnDirectProvider(const std::string& provider, const std::string& id) {
  if (provider == "cumulo" || !isProviderSlug(provider.c_str())) return false;
  const size_t slash = id.find('/');
  return slash != std::string::npos && slash > 0 && slash + 1 < id.size();
}

std::string routerIdMissText(const std::string& provider, const std::string& id,
                             const std::string& listedBare) {
  // The fix first: a head that read "runs on cumulo" first tried cumulo (bench, 22 Oct).
  if (!listedBare.empty())
    return "not in " + provider + " list: use " + listedBare + " (" + id + " is a cumulo router id)";
  return "not in " + provider + " list: " + id + " is a cumulo router id; pick one of " + provider +
         "'s ids from models.list";
}

std::string modelMissText(const std::string& provider, uint32_t fetchedAt, RefreshAsk ask) {
  std::string s = "not in " + provider + " list cached " +
                  (fetchedAt ? utcDate(fetchedAt) : std::string("(date unknown)"));
  switch (ask) {
    case RefreshAsk::Queued:
    case RefreshAsk::Pending: return s + "; refresh queued";
    case RefreshAsk::Recent:  return s + "; the list was refreshed within the hour, pick an id from models.list";
    case RefreshAsk::NoKey:   return s + "; " + provider + " has no key";
  }
  return s;
}

}  // namespace orch
}  // namespace nimbus
