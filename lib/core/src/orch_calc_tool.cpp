// The calc tool's handler and registration (calc_tool.h).
#include "nimbus/orch/calc_tool.h"

#include <string_view>

#include "nimbus/orch/calc.h"

namespace nimbus::orch {

namespace {

constexpr char kDescription[] = "Exact decimal arithmetic on one expression: + - * / ( ), 15% of 80, 80+15%.";
constexpr char kSchema[] =
    R"({"type":"object","properties":{"expression":{"type":"string","maxLength":256}},"required":["expression"]})";

std::string_view expressionOf(ArduinoJson::JsonObjectConst args) {
  for (const char* name : calc::kArgNames) {
    ArduinoJson::JsonVariantConst v = args[name];
    if (v.is<const char*>()) return v.as<const char*>();
  }
  return std::string_view();
}

}  // namespace

ToolResult calcToolCall(ArduinoJson::JsonObjectConst args) {
  const calc::Result r = calc::evaluate(expressionOf(args));
  const std::string line(r.text());
  return r.ok() ? ToolResult::ok(line) : ToolResult::fail(line);
}

const char* calcDescription() { return kDescription; }
const char* calcSchema() { return kSchema; }

void registerCalcTool(ToolRegistry& reg) {
  static_assert(calc::kName[0] == 'c' && calc::kName[4] == 0, "the literal below is calc::kName");
  reg.add("calc", kDescription,
          [](ArduinoJson::JsonObjectConst a, const Principal&) { return calcToolCall(a); }, kSchema);
  alsoReadCalcNames(reg);
}

void alsoReadCalcNames(ToolRegistry& reg) {
  // calc::kArgNames after the schema's own, as one comma-separated list (the registry keeps
  // them that way); test_calc_tool calls with each name and sees no "not read" note.
  reg.alsoReads(calc::kName, {"expr,input,query,formula,calculation,math,text"});
}

}  // namespace nimbus::orch
