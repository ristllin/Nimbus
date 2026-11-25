#pragma once
#include <ArduinoJson.h>

#include "nimbus/orch/tool_registry.h"   // ToolResult, Principal

// pdf_tool - the files.pdf tool: the device's own PDF writer (ruling R7, OA-107),
// the route a PDF takes when no OpenAI or Anthropic sandbox can (pdfRoute). The
// portable writer (nimbus/orch/pdf_writer.h) streams the document page by page
// straight into the file store, so the device never holds the whole PDF.
namespace agent {
namespace pdf_tool {

// Register files.pdf on `reg`, gated on the file store (an SD card): without one
// the tool is left out of every listing and a call refuses with the reason.
void registerTool(nimbus::orch::ToolRegistry& reg);

// files.pdf's handler with engine 1's arguments ({project, name, title, text}): engine 1 registers it,
// engine 2 reaches it as pdf_write (lane E2-P-OUT2, tools_plain_files.cpp).
nimbus::orch::ToolResult writeTool(ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who);

// The writer is compiled in and can keep a file right now (an SD card).
bool available();

}  // namespace pdf_tool
}  // namespace agent
