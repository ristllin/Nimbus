#include "nimbus/orch/prefs_tools.h"

#include <ArduinoJson.h>

#include <algorithm>

#include "nimbus/orch/memory_tools.h"   // kMemOutcome* codes for the standing route

namespace nimbus {
namespace orch {

namespace {

std::string strArg(ArduinoJson::JsonObjectConst a, const char* k) {
  return a[k].is<const char*>() ? std::string(a[k].as<const char*>()) : std::string();
}

bool scheduled(const PrefsToolContext& c) { return c.inScheduledTurn && c.inScheduledTurn(); }
bool tainted(const PrefsToolContext& c) { return c.externalContentSeen && c.externalContentSeen(); }
uint32_t now(const PrefsToolContext& c) { return c.nowHours ? c.nowHours() : 0; }

void notify(const PrefsToolContext& c, const std::string& text) {
  if (c.notify) c.notify(text);
}

constexpr const char kUnavailable[] = "the standing rules are unavailable right now (storage not ready)";
constexpr const char kOwnerOnly[] =
    "standing rules belong to the device's owner; only the owner can see or change them";

// "(p3) text" plus what the device does about it, and (lane CTX) how old it is.
std::string ruleLine(const PrefEntry& e, uint32_t nowH) {
  std::string out = "(" + e.idStr() + ") " + e.text;
  const std::string age = prefAgeText(e.createdHours, nowH);
  if (!age.empty()) out += " [" + age + "]";
  if (e.pending) out += " [waits for the owner's approval]";
  if (e.filters & kPrefFilterNoDash) out += " [em dashes are also removed from replies]";
  return out;
}

// The rules' lines, each ending in a newline ("" when there are none).
std::string ruleLines(const OwnerPrefs& p, uint32_t nowH) {
  std::string out;
  for (const PrefEntry& e : p.entries())
    if (!e.moved) out += ruleLine(e, nowH) + "\n";
  return out;
}

// (Lane CTX) the dated facts that were moved to memory, under their heading and
// each with the way back ("" when there are none).
std::string movedLines(const OwnerPrefs& p) {
  if (!p.movedCount()) return std::string();
  std::string out = "Moved to memory (dated facts, not rules):\n";
  for (const PrefEntry& e : p.entries())
    if (e.moved) out += "(" + e.idStr() + ") " + e.text + " [restore: /prefs restore " + e.idStr() + "]\n";
  return out;
}

// The rules, then the moved facts (the model's list).
std::string listText(const OwnerPrefs& p, uint32_t nowH) {
  std::string out = ruleLines(p, nowH);
  if (out.empty()) out = "no standing rules yet\n";
  out += movedLines(p);
  out.pop_back();
  return out;
}

// One add from the model (prefs.edit add, or a standing:true mem_write).
struct ModelAdd {
  PrefErr     err = PrefErr::PersistFailed;
  std::string id;
  std::string text;
  bool        pending = false;
  bool        existed = false;
};

ModelAdd modelAdd(const PrefsToolContext& ctx, const std::string& text) {
  ModelAdd r;
  r.pending = tainted(ctx);
  const bool ran = ctx.withPrefs && ctx.withPrefs([&](OwnerPrefs& p) {
    const size_t before = p.entries().size();
    r.err = p.add(text, PrefOrigin::Chat, now(ctx), r.pending, &r.id);
    r.existed = r.err == PrefErr::Ok && p.entries().size() == before;
    if (const PrefEntry* e = p.find(parsePrefId(r.id))) {
      r.text = e->text;
      r.pending = e->pending;
    }
  });
  if (!ran) r.err = PrefErr::PersistFailed;
  return r;
}

void noticeForAdd(const PrefsToolContext& ctx, const ModelAdd& r) {
  if (r.err != PrefErr::Ok || r.existed) return;
  if (r.pending)
    notify(ctx, "A standing rule waits for your approval (" + r.id + "): " + r.text +
                    "\nIt came from a conversation that read outside content. Approve it with /prefs approve " +
                    r.id + ", or drop it with /prefs rm " + r.id + ".");
  else
    notify(ctx, "Standing rule saved (" + r.id + "): " + r.text + "\nReview or remove: /prefs");
}

std::string modelAddWords(const ModelAdd& r) {
  if (r.err != PrefErr::Ok) return std::string("not saved: ") + prefErrWords(r.err);
  if (r.existed && !r.pending) return "already a standing rule (" + r.id + "); nothing changed";
  if (r.pending)
    return "saved as PENDING rule " + r.id +
           ": this turn read outside content, so it is NOT active until the owner approves it "
           "(/prefs approve " + r.id + " or the web page). Tell the owner it waits for approval.";
  return "saved as standing rule " + r.id +
         "; it applies from your next reply on and appears under [STANDING RULES]. The owner was told.";
}

ToolResult doAdd(const PrefsToolContext& ctx, const std::string& text) {
  const ModelAdd r = modelAdd(ctx, text);
  noticeForAdd(ctx, r);
  return r.err == PrefErr::Ok ? ToolResult::ok(modelAddWords(r)) : ToolResult::fail(modelAddWords(r));
}

ToolResult doChange(const PrefsToolContext& ctx, const std::string& action, const std::string& idArg,
                    const std::string& text) {
  if (tainted(ctx))
    return ToolResult::fail("this turn read outside content, so it cannot " + action +
                            " a standing rule; the owner can, with /prefs or the web page "
                            "(adding a new rule is allowed: it waits for approval)");
  const uint32_t id = parsePrefId(idArg);
  if (!id) return ToolResult::fail("missing 'id' (for example p3)");
  PrefErr err = PrefErr::PersistFailed;
  std::string before, after;
  const bool ran = ctx.withPrefs && ctx.withPrefs([&](OwnerPrefs& p) {
    if (const PrefEntry* e = p.find(id)) before = e->text;
    err = action == "remove" ? p.remove(id) : p.replace(id, text, PrefOrigin::Chat, now(ctx));
    if (const PrefEntry* e = p.find(id)) after = e->text;
  });
  if (!ran) return ToolResult::fail(kUnavailable);
  if (err != PrefErr::Ok) return ToolResult::fail(std::string("not changed: ") + prefErrWords(err));
  const std::string pid = "p" + std::to_string(id);
  if (action == "remove") {
    notify(ctx, "Standing rule removed (" + pid + "): " + before);
    return ToolResult::ok("removed standing rule " + pid + "; the owner was told");
  }
  notify(ctx, "Standing rule changed (" + pid + "): " + (after.empty() ? text : after));
  return ToolResult::ok("changed standing rule " + pid + "; the owner was told");
}

ToolResult doPrefsEdit(const PrefsToolContext& ctx, ArduinoJson::JsonObjectConst a, const Principal& who) {
  if (!who.owner || !who.perms().manageTenants) return ToolResult::fail(kOwnerOnly);
  const std::string action = strArg(a, "action");
  if (action == "list") {
    std::string out;
    const bool ran = ctx.withPrefs && ctx.withPrefs([&](OwnerPrefs& p) { out = listText(p, now(ctx)); });
    return ran ? ToolResult::ok(out) : ToolResult::fail(kUnavailable);
  }
  if (action != "add" && action != "replace" && action != "remove")
    return ToolResult::fail("action must be add, replace, remove or list");
  if (scheduled(ctx)) return ToolResult::fail("a scheduled turn cannot change standing rules");
  if (action == "add") return doAdd(ctx, strArg(a, "text"));
  return doChange(ctx, action, strArg(a, "id"), strArg(a, "text"));
}

// ---- /prefs ------------------------------------------------------------------

// The first word (lowercased) and the trimmed rest.
std::string firstWord(const std::string& s, std::string& rest) {
  const size_t b = s.find_first_not_of(' ');
  rest.clear();
  if (b == std::string::npos) return std::string();
  const size_t e = std::min(s.find(' ', b), s.size());
  std::string w = s.substr(b, e - b);
  const size_t r = s.find_first_not_of(' ', e);
  if (r != std::string::npos) rest = s.substr(r);
  while (!rest.empty() && rest.back() == ' ') rest.pop_back();
  for (char& c : w)
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  return w;
}

constexpr const char kUsage[] =
    "Standing rules: /prefs lists them. /prefs add <rule> saves one (for example /prefs add Never use "
    "em dashes). /prefs rm <id> removes one. /prefs approve <id> turns on a rule that waits for "
    "approval. /prefs restore <id> brings back a rule that was moved to memory. /prefs clear yes "
    "removes them all.";

std::string commandList(const OwnerPrefs& p, uint32_t nowH) {
  if (p.entries().empty())
    return "No standing rules yet. Add one with /prefs add <rule>, for example /prefs add Never use em dashes.";
  const size_t rules = p.entries().size() - p.movedCount();
  const std::string head =
      rules ? "Standing rules (" + std::to_string(rules) + "):\n" + ruleLines(p, nowH) : "No standing rules yet.\n";
  return head + movedLines(p) + "Add: /prefs add <rule>. Remove: /prefs rm <id>.";
}

std::string commandAct(OwnerPrefs& p, const std::string& verb, const std::string& rest, uint32_t nowH) {
  std::string id;
  if (verb == "add") {
    const PrefErr e = p.add(rest, PrefOrigin::Command, nowH, /*pending=*/false, &id);
    if (e != PrefErr::Ok) return std::string("Not saved: ") + prefErrWords(e) + ".";
    const PrefEntry* r = p.find(parsePrefId(id));
    return "Saved (" + id + "): " + (r ? r->text : rest) + "\nIt applies from the next reply.";
  }
  const uint32_t n = parsePrefId(rest);
  if (!n) return kUsage;
  const std::string pid = "p" + std::to_string(n);
  if (verb == "approve") {
    const PrefErr e = p.approve(n);
    return e == PrefErr::Ok ? "Approved " + pid + ". It applies from the next reply."
                            : std::string("Not changed: ") + prefErrWords(e) + ".";
  }
  if (verb == "restore") {
    const PrefErr e = p.restore(n);
    return e == PrefErr::Ok ? "Restored " + pid + " as a standing rule. It applies from the next reply."
                            : std::string("Not changed: ") + prefErrWords(e) + ".";
  }
  const PrefErr e = p.remove(n);
  return e == PrefErr::Ok ? "Removed " + pid + "." : std::string("Not changed: ") + prefErrWords(e) + ".";
}

}  // namespace

void registerPrefsTools(ToolRegistry& reg, const PrefsToolContext& ctx) {
  reg.add("prefs.edit",
          std::string("Your owner's STANDING RULES: always/never rules and standing defaults they want in "
                      "EVERY reply whatever the topic (punctuation, tone, length, format, language, units, "
                      "how to address them, how to log or read their numbers), shown to you every turn "
                      "under [STANDING RULES]. ") +
              kStandingRuleTest +
              " Then add it here, only when the owner asks in their own words; facts and one-off "
              "details go to memory, and a meal or a day's total to the food ledger (a dated "
              "one-off fact is refused here). add {text} | replace {id,text} | remove {id} | list.",
          [ctx](ArduinoJson::JsonObjectConst a, const Principal& who) { return doPrefsEdit(ctx, a, who); },
          R"({"type":"object","properties":{"action":{"type":"string",)"
          R"("enum":["add","replace","remove","list"]},"id":{"type":"string"},)"
          R"("text":{"type":"string"}},"required":["action"]})");
  // Owner-only: never advertised to a member or guest turn (the handler refuses too).
  reg.setAdminOnly("prefs.edit");
}

ToolResult prefsSaveStanding(const PrefsToolContext& ctx, const std::string& text, const Principal& who,
                             bool& handled) {
  // Lane CTX: a dated measurement marked standing is a fact: it is stored as an
  // ordinary memory (the caller's route). A dated meal is refused below with the
  // food ledger named.
  handled = who.owner && who.perms().manageTenants && !scheduled(ctx) &&
            datedKindOf(text) != DatedKind::Measurement;
  if (!handled) return ToolResult::fail("not a standing rule here");
  const ModelAdd r = modelAdd(ctx, text);
  noticeForAdd(ctx, r);
  ToolResult out = r.err == PrefErr::Ok ? ToolResult::ok(modelAddWords(r)) : ToolResult::fail(modelAddWords(r));
  out.code = r.err != PrefErr::Ok ? kMemOutcomeRefused : (r.pending ? kMemOutcomeQueued : kMemOutcomeStored);
  return out;
}

// The clear verb (its confirm gate keeps it out of prefsCommand's body).
std::string commandClear(const PrefsToolContext& ctx, const std::string& rest) {
  if (rest != "yes") return "This removes every standing rule. Send /prefs clear yes to confirm.";
  std::string reply = kUnavailable;
  const bool ran = ctx.withPrefs && ctx.withPrefs([&](OwnerPrefs& p) {
    const PrefErr e = p.clear();
    reply = e == PrefErr::Ok ? "Removed every standing rule." : std::string("Not changed: ") + prefErrWords(e) + ".";
  });
  return ran ? reply : kUnavailable;
}

// The verb a /prefs word means ("rm", "delete" and "del" all remove), or "".
std::string canonicalVerb(const std::string& word) {
  static const char* const kVerbs[][2] = {{"", "list"},        {"list", "list"},       {"clear", "clear"},
                                          {"add", "add"},      {"approve", "approve"}, {"restore", "restore"},
                                          {"rm", "remove"},    {"remove", "remove"},   {"delete", "remove"},
                                          {"del", "remove"}};
  for (const auto& v : kVerbs)
    if (word == v[0]) return v[1];
  return std::string();
}

std::string prefsCommand(const PrefsToolContext& ctx, const std::string& args) {
  std::string rest;
  const std::string verb = canonicalVerb(firstWord(args, rest));
  if (verb == "clear") return commandClear(ctx, rest);
  if (verb.empty() || (verb != "list" && rest.empty())) return kUsage;
  std::string reply = kUnavailable;
  const auto fn = [&](OwnerPrefs& p) {
    reply = verb == "list" ? commandList(p, now(ctx)) : commandAct(p, verb, rest, now(ctx));
  };
  if (ctx.withPrefs) ctx.withPrefs(fn);
  return reply;
}

std::string prefsJson(const OwnerPrefs& p, uint32_t nowHours) {
  JsonDocument d;
  JsonArray items = d["items"].to<JsonArray>();
  for (const PrefEntry& e : p.entries()) {
    JsonObject o = items.add<JsonObject>();
    o["id"] = e.idStr();
    o["text"] = e.text;
    o["state"] = e.moved ? "moved" : e.pending ? "pending" : "active";
    o["created"] = e.createdHours;
    const std::string age = prefAgeText(e.createdHours, nowHours);
    if (!age.empty()) o["age"] = age;
    o["origin"] = prefOriginSlug(e.origin);
    o["nodash"] = (e.filters & kPrefFilterNoDash) != 0;
  }
  JsonObject max = d["max"].to<JsonObject>();
  max["items"] = kPrefsMaxItems;
  max["itemBytes"] = kPrefItemMax;
  max["totalBytes"] = kPrefsMaxBytes;
  max["pending"] = kPrefsPendingMax;
  d["bytes"] = p.activeBytes();
  std::string out;
  serializeJson(d, out);
  return out;
}

PrefErr prefsWebAction(OwnerPrefs& p, const std::string& action, const std::string& id,
                       const std::string& text, uint32_t nowHours, std::string& idOut) {
  if (action == "add" || action == "import")
    return p.add(text, action == "add" ? PrefOrigin::Web : PrefOrigin::Import, nowHours, false, &idOut);
  if (action == "clear") return p.clear();
  const uint32_t n = parsePrefId(id);
  if (!n) return PrefErr::NotFound;
  idOut = "p" + std::to_string(n);
  if (action == "replace") return p.replace(n, text, PrefOrigin::Web, nowHours);
  if (action == "remove") return p.remove(n);
  if (action == "approve") return p.approve(n);
  if (action == "restore") return p.restore(n);
  return PrefErr::NotFound;
}

int prefErrHttpStatus(PrefErr e) {
  switch (e) {
    case PrefErr::Ok:            return 200;
    case PrefErr::Empty:
    case PrefErr::TooLong:
    case PrefErr::DatedFood:
    case PrefErr::DatedFact:     return 400;
    case PrefErr::NotFound:      return 404;
    case PrefErr::Full:
    case PrefErr::PendingFull:
    case PrefErr::NotPending:
    case PrefErr::NotMoved:      return 409;
    case PrefErr::PersistFailed: return 507;
  }
  return 500;
}

}  // namespace orch
}  // namespace nimbus
