// The calc tool on both engines (calc_tool.h, lane RB-REC8 phase 2).
//
// Class rules proven here:
//   - one declaration: the old engine's registration and engine 2's texts.h row carry the
//     same name, description and schema bytes (both host sizes);
//   - one result: the same arguments give the same bytes through the old engine's registry
//     and through an engine-2 registry built from plain::toolDecl, for every case of the
//     calculator's own tables and the three domain fixtures below;
//   - every accepted argument name reaches the expression (with no "not read" note); a call
//     without one gives the empty-expression line, never a figure;
//   - calc is always on (no group), pure (Internal, Read), open to every caller.
//   - three domains with identical expectations: a food log, an expense log and a training
//     log ask the same three questions (what is left against a target, a scaled quantity,
//     a percent of a budget) and get the same figures; the tool knows no domain.
#include <unity.h>

#include <ArduinoJson.h>

#include <string>

#include "nimbus/harness/plain/request.h"
#include "nimbus/harness/plain/texts.h"
#include "nimbus/orch/action_rules.h"
#include "nimbus/orch/calc.h"
#include "nimbus/orch/calc_tool.h"
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/tool_taint.h"
#include "fixtures.h"

using namespace nimbus::orch;
namespace plain = agent::plain;

void setUp() {}
void tearDown() {}

namespace {

Principal owner() { return principalForRole("c", Role::Admin); }

// Engine 2's registry: the same handler under texts.h's declaration.
void registerEngine2(ToolRegistry& reg, bool shortHost) {
  const auto* d = plain::toolDecl("calc");
  TEST_ASSERT_NOT_NULL(d);
  reg.add(d->name, d->description,
          [](ArduinoJson::JsonObjectConst a, const Principal&) { return calcToolCall(a); },
          plain::declSchema(*d, shortHost));
  alsoReadCalcNames(reg);
}

ToolRegistry::Spec specOf(const ToolRegistry& reg, const char* name) {
  for (const ToolRegistry::Spec& s : reg.toolSpecs())
    if (s.name == name) return s;
  return ToolRegistry::Spec{};
}

std::string lineOf(const ToolResult& r) { return r.success ? r.output : r.error; }

ToolResult callWith(const ToolRegistry& reg, const char* argName, const char* expr) {
  JsonDocument d;
  d[argName] = expr;
  return reg.dispatch("calc", d.as<ArduinoJson::JsonObjectConst>(), owner());
}

void test_one_declaration_on_both_engines() {
  ToolRegistry e1;
  registerCalcTool(e1);
  const ToolRegistry::Spec s1 = specOf(e1, "calc");
  TEST_ASSERT_EQUAL_STRING("calc", s1.name.c_str());
  for (bool shortHost : {false, true}) {
    ToolRegistry e2;
    registerEngine2(e2, shortHost);
    const ToolRegistry::Spec s2 = specOf(e2, "calc");
    TEST_ASSERT_EQUAL_STRING(s2.description.c_str(), s1.description.c_str());
    TEST_ASSERT_EQUAL_STRING(s2.schemaJson.c_str(), s1.schemaJson.c_str());
  }
  TEST_ASSERT_EQUAL_STRING(calcDescription(), s1.description.c_str());
  TEST_ASSERT_EQUAL_STRING(calcSchema(), s1.schemaJson.c_str());
  TEST_ASSERT_EQUAL(75, std::string(calcDescription()).size());
  TEST_ASSERT_EQUAL(105, std::string(calcSchema()).size());
  JsonDocument schema;
  TEST_ASSERT_FALSE(deserializeJson(schema, calcSchema()));
  TEST_ASSERT_EQUAL_STRING(calc::kArgNames[0], schema["required"][0].as<const char*>());
  TEST_ASSERT_EQUAL((int)calc::kMaxExpr, schema["properties"]["expression"]["maxLength"].as<int>());
}

void test_one_result_on_both_engines() {
  ToolRegistry e1, e2;
  registerCalcTool(e1);
  registerEngine2(e2, false);
  for (const char* expr : calc_fixtures::kExpressions) {
    const ToolResult a = callWith(e1, "expression", expr), b = callWith(e2, "expression", expr);
    TEST_ASSERT_EQUAL_MESSAGE(a.success, b.success, expr);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(lineOf(a).c_str(), lineOf(b).c_str(), expr);
    const calc::Result direct = calc::evaluate(expr);
    TEST_ASSERT_EQUAL_MESSAGE(direct.ok(), a.success, expr);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(std::string(direct.text()).c_str(), lineOf(a).c_str(), expr);
  }
}

void test_every_argument_name_reaches_the_expression() {
  ToolRegistry reg;
  registerCalcTool(reg);
  for (const char* name : calc::kArgNames)
    TEST_ASSERT_EQUAL_STRING_MESSAGE("= 687.5", lineOf(callWith(reg, name, "2300-1612.5")).c_str(), name);
  // A name outside the list is not read, and the registry says so.
  TEST_ASSERT_EQUAL_STRING("error at position 1: empty expression\n(not read: sum: this tool has no such argument)",
                           lineOf(callWith(reg, "sum", "15% of 80")).c_str());
  // No expression: the empty-expression line, a failure, never a figure.
  const char* none = "error at position 1: empty expression";
  JsonDocument empty;
  empty.to<JsonObject>();
  ToolResult r = reg.dispatch("calc", empty.as<ArduinoJson::JsonObjectConst>(), owner());
  TEST_ASSERT_FALSE(r.success);
  TEST_ASSERT_EQUAL_STRING(none, r.error.c_str());
  JsonDocument number;
  number["expression"] = 42;
  r = reg.dispatch("calc", number.as<ArduinoJson::JsonObjectConst>(), owner());
  TEST_ASSERT_EQUAL_STRING(none, r.error.c_str());
}

void test_calc_is_always_on_pure_and_open() {
  ToolRegistry reg;
  registerCalcTool(reg);
  TEST_ASSERT_TRUE(reg.classOf("calc").alwaysOn());
  TEST_ASSERT_NOT_NULL(toolTaintRow("calc"));
  TEST_ASSERT_EQUAL((int)Taint::Internal, (int)toolTaint("calc"));
  TEST_ASSERT_EQUAL((int)ToolEffect::Read, (int)toolEffect("calc"));
  installActionRules(reg, [] { return true; }, [] { return std::string("c"); });
  for (Role role : {Role::Admin, Role::User, Role::Guest}) {
    JsonDocument d;
    d["expression"] = "2+2";
    const ToolResult r = reg.dispatch("calc", d.as<ArduinoJson::JsonObjectConst>(), principalForRole("c", role));
    TEST_ASSERT_EQUAL_STRING("= 4", lineOf(r).c_str());
  }
}

// ---- three domains, one set of figures ---------------------------------------------------
struct Question {
  const char* expr;
  const char* want;
};

struct Domain {
  const char* name;
  Question left;     // what is left against a target
  Question scaled;   // a quantity scaled to another amount
  Question share;    // a percent of a budget
};

// The same numbers in each log: only the domain changes, and the figures do not.
const Domain kDomains[] = {
    {"food log", {"2300 - 1612.5", "= 687.5"}, {"250 * 3.12 / 100", "= 7.8"}, {"15% of 2300", "= 345"}},
    {"expense log", {"2300 - 1612.5", "= 687.5"}, {"250 * 3.12 / 100", "= 7.8"}, {"15% of 2300", "= 345"}},
    {"training log", {"2300 - 1612.5", "= 687.5"}, {"250 * 3.12 / 100", "= 7.8"}, {"15% of 2300", "= 345"}},
};

void test_three_domains_get_the_same_figures() {
  ToolRegistry e1, e2;
  registerCalcTool(e1);
  registerEngine2(e2, true);
  for (const Domain& dom : kDomains) {
    for (const Question& q : {dom.left, dom.scaled, dom.share}) {
      TEST_ASSERT_EQUAL_STRING_MESSAGE(q.want, lineOf(callWith(e1, "expression", q.expr)).c_str(), dom.name);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(q.want, lineOf(callWith(e2, "expr", q.expr)).c_str(), dom.name);
    }
  }
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_one_declaration_on_both_engines);
  RUN_TEST(test_one_result_on_both_engines);
  RUN_TEST(test_every_argument_name_reaches_the_expression);
  RUN_TEST(test_calc_is_always_on_pure_and_open);
  RUN_TEST(test_three_domains_get_the_same_figures);
  return UNITY_END();
}
