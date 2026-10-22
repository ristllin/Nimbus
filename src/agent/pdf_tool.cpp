#include "pdf_tool.h"

#include <ArduinoJson.h>

#include <string>

#include "../sys/agent_log.h"
#include "files_subsystem.h"
#include "nimbus/orch/file_store.h"
#include "nimbus/orch/pdf_writer.h"
#include "nimbus/orch/tool_registry.h"

namespace agent {
namespace pdf_tool {

namespace {

std::string arg(ArduinoJson::JsonObjectConst a, const char* k) {
  const char* v = a[k] | "";
  return v;
}

// "<name>.pdf" for any name the model gives, so the file opens as a PDF.
std::string pdfName(std::string name) {
  const size_t n = name.size();
  if (n < 4 || (name.compare(n - 4, 4, ".pdf") != 0 && name.compare(n - 4, 4, ".PDF") != 0))
    name += ".pdf";
  return name;
}

nimbus::orch::ToolResult writeTool(ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) {
  using nimbus::orch::ToolResult;
  // The store keys a file by project and name and a write replaces: only the owner
  // writes here, so a member can never replace a file the owner keeps.
  if (!who.owner) return ToolResult::fail("files.pdf is only available to the device owner");
  const std::string project = arg(a, "project"), title = arg(a, "title"), text = arg(a, "text");
  const std::string name = pdfName(arg(a, "name"));
  const nimbus::orch::FileStore::Limits lim{};
  if (!nimbus::orch::FileStore::validSegment(project, lim.maxProjectLen) ||
      !nimbus::orch::FileStore::validSegment(name, lim.maxNameLen))
    return ToolResult::fail("need a 'project' and a 'name' (letters, digits, - _ .)");
  if (!files::available()) return ToolResult::fail("no SD card: the device cannot keep the PDF");
  std::string err;
  if (!files::binaryWriteBegin(project, name, 0, who.ns, err)) return ToolResult::fail(err);
  const nimbus::orch::PdfResult r = nimbus::orch::writePdf(title, text, [](const char* d, size_t n) {
    return files::writeChunk(reinterpret_cast<const uint8_t*>(d), n);
  });
  if (!files::finishWrite(r.ok, err)) return ToolResult::fail(r.ok ? err : r.error);
  alogf("files: wrote %s/%s (%u B, %d page%s) with the device PDF writer", project.c_str(),
        name.c_str(), (unsigned)r.bytes, r.pages, r.pages == 1 ? "" : "s");
  // The same note a captured sandbox file carries, so delivery is one flow.
  return ToolResult::ok("[file saved: " + project + "/" + name + "] (" + std::to_string(r.pages) +
                        " page" + (r.pages == 1 ? "" : "s") + ")");
}

}  // namespace

void registerTool(nimbus::orch::ToolRegistry& reg) {
  reg.add("files.pdf",
          "Write a PDF into the file store with the device's own PDF writer: a title plus "
          "text in light Markdown (# headings, - bullets, | table | rows |, blank lines "
          "between paragraphs). Text and simple tables only, no images or charts; up to "
          "48 KB of text. Saves <project>/<name>.pdf (replacing one of the same name); "
          "deliver it with files.send. Use it when the TO DELIVER A PDF line names the "
          "device's PDF writer.",
          writeTool,
          R"({"type":"object","properties":{"project":{"type":"string"},"name":{"type":"string"},"title":{"type":"string"},"text":{"type":"string"}},"required":["project","name","text"]})");
  reg.setGate("files.pdf", [] { return files::available(); },
              "no SD card: the device cannot keep a PDF");
}

bool available() { return files::available(); }

}  // namespace pdf_tool
}  // namespace agent
