#include "nimbus/orch/openq_tools.h"

#include <ArduinoJson.h>

namespace nimbus {
namespace orch {

namespace {

std::string strArg(ArduinoJson::JsonObjectConst a, const char* k) {
  return a[k].is<const char*>() ? std::string(a[k].as<const char*>()) : std::string();
}

bool scheduled(const OpenQToolContext& c) { return c.inScheduledTurn && c.inScheduledTurn(); }
bool tainted(const OpenQToolContext& c) { return c.externalContentSeen && c.externalContentSeen(); }
uint32_t nowH(const OpenQToolContext& c) { return c.nowHours ? c.nowHours() : 0; }

constexpr const char kUnavailable[] = "the open list is unavailable right now (storage not ready)";
constexpr const char kOwnerOnly[] =
    "the open list belongs to the device's owner; only the owner can see or change it";

std::string listText(const OpenQuestions& q) {
  if (q.count() == 0) return "no open items";
  std::string out;
  for (const OpenQEntry& e : q.entries()) {
    out += "(" + e.idStr() + ") " + e.text + (e.raised ? " [raised]\n" : "\n");
  }
  out.pop_back();
  return out;
}

ToolResult doAdd(const OpenQToolContext& ctx, const std::string& text) {
  OpenQErr err = OpenQErr::PersistFailed;
  std::string id;
  const bool ran = ctx.withOpenQ && ctx.withOpenQ([&](OpenQuestions& q) {
    err = q.add(text, nowH(ctx), &id);
  });
  if (!ran) return ToolResult::fail(kUnavailable);
  if (err != OpenQErr::Ok) return ToolResult::fail(std::string("not saved: ") + openQErrWords(err));
  return ToolResult::ok("saved as open item " + id +
                        ". It is shown to you every turn as [OPEN QUESTIONS]; ask the owner about it "
                        "at most once, and remove it with openq.edit when it is resolved.");
}

ToolResult doRemove(const OpenQToolContext& ctx, const std::string& idArg) {
  // "q3" / "3". More than 10 digits is not an id (the wrap would remove the
  // wrong item: q4294967297 would parse as q1).
  std::string digits = idArg;
  if (!digits.empty() && (digits[0] == 'q' || digits[0] == 'Q')) digits.erase(0, 1);
  uint32_t id = 0;
  if (digits.empty() || digits.size() > 10) return ToolResult::fail("missing 'id' (for example q3)");
  for (char c : digits) {
    if (c < '0' || c > '9') { id = 0; break; }
    id = id * 10 + (uint32_t)(c - '0');
  }
  if (!id) return ToolResult::fail("missing 'id' (for example q3)");
  OpenQErr err = OpenQErr::PersistFailed;
  const bool ran = ctx.withOpenQ && ctx.withOpenQ([&](OpenQuestions& q) { err = q.remove(id); });
  if (!ran) return ToolResult::fail(kUnavailable);
  if (err != OpenQErr::Ok) return ToolResult::fail(std::string("not removed: ") + openQErrWords(err));
  return ToolResult::ok("removed open item q" + std::to_string(id));
}

ToolResult doOpenQEdit(const OpenQToolContext& ctx, ArduinoJson::JsonObjectConst a,
                       const Principal& who) {
  if (!who.owner || !who.perms().manageTenants) return ToolResult::fail(kOwnerOnly);
  const std::string action = strArg(a, "action");
  if (action == "list") {
    std::string out;
    const bool ran = ctx.withOpenQ && ctx.withOpenQ([&](OpenQuestions& q) { out = listText(q); });
    return ran ? ToolResult::ok(out) : ToolResult::fail(kUnavailable);
  }
  if (action != "add" && action != "remove")
    return ToolResult::fail("action must be add, remove or list");
  if (scheduled(ctx)) return ToolResult::fail("a scheduled turn cannot change the open list");
  if (tainted(ctx))
    return ToolResult::fail("this turn read outside content, so it cannot change the open list");
  if (action == "add") return doAdd(ctx, strArg(a, "text"));
  return doRemove(ctx, strArg(a, "id"));
}

}  // namespace

void registerOpenQTools(ToolRegistry& reg, const OpenQToolContext& ctx) {
  reg.add("openq.edit",
          std::string("The owner's OPEN QUESTIONS: follow-ups the owner left open (a photo to "
                      "take next time, a thing to check later), shown to you every turn under "
                      "[OPEN QUESTIONS]. Add one when the owner leaves something open, remove it "
                      "when it is resolved or they withdraw it. The device marks an item raised "
                      "once your reply mentions it, and tells you not to raise it again. "
                      "add {text} | remove {id} | list."),
          [ctx](ArduinoJson::JsonObjectConst a, const Principal& who) {
            return doOpenQEdit(ctx, a, who);
          },
          R"({"type":"object","properties":{"action":{"type":"string",)"
          R"("enum":["add","remove","list"]},"id":{"type":"string"},)"
          R"("text":{"type":"string"}},"required":["action"]})");
  // Owner-only: never advertised to a member or guest turn (the handler refuses too).
  reg.setAdminOnly("openq.edit");
}

}  // namespace orch
}  // namespace nimbus
