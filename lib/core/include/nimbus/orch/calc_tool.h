#pragma once
#include <ArduinoJson.h>

#include "nimbus/orch/tool_registry.h"

// calc_tool - the calc tool's handler and registration (lane RB-REC8, ruling 42 item 3).
//
// One handler for every engine: the old engine's registry and nimbusd register it with
// registerCalcTool(reg) (always on, not behind more); engine 2 registers the same handler
// under plain::toolDecl("calc")'s bytes, which equal calcDescription() and calcSchema() (a
// test compares them). The result is calc.h's one line: ok with "= <figure>", or fail with
// "error at position N: <reason>". A pure function of its argument: no outside content, no
// effect (tool_taint.h row: Internal, Read).
namespace nimbus::orch {

// The expression: the first of calc::kArgNames present as a string; else none (the
// empty-expression line).
ToolResult calcToolCall(ArduinoJson::JsonObjectConst args);

// The declaration's bytes (the same as engine 2's texts.h row).
const char* calcDescription();
const char* calcSchema();

// Registers "calc" with calcToolCall under calcDescription() and calcSchema(), and the
// aliases as read arguments.
void registerCalcTool(ToolRegistry& reg);
// Marks calc::kArgNames beyond the schema's own as read arguments of a registered "calc"
// (engine 2 calls it after registering texts.h's declaration), so the registry adds no
// "not read" note for them.
void alsoReadCalcNames(ToolRegistry& reg);

}  // namespace nimbus::orch
