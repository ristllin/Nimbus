#include "tools_plain_files.h"

#include <string>

#include "nimbus/orch/path_rules.h"   // storePathFor, pathRuleFor - engine 2's path rule

#ifdef ARDUINO
#include "../sfx/music.h"        // playTool, pauseTool, stopTool - media.*
#include "files_subsystem.h"     // statTool, shareTool, fetchTool, imageTool
#include "nimbus/harness/turn_memory.h"   // PsramFirstScope
#include "pdf_tool.h"            // writeTool - files.pdf
#endif

// tools_plain_files - see the header. Engine 2 only (tools/engine/engine2_only.txt). The literals in
// namespace tool_descriptions and tool_results are model text, catalogued by
// tools/release_gate/gen_model_text_catalog.py (coverage/PROMPT-CHANGES.md PC-0240, PC-0241).
namespace agent::plainfiles {

namespace orch = nimbus::orch;
using ArduinoJson::JsonDocument;
using ArduinoJson::JsonObjectConst;

namespace tool_descriptions {
struct Decl {
  const char* description;
  const char* schema;
};
const Decl kImage = {
    "Generate an image from a text prompt (needs an OpenAI key) and store it as a PNG; path (project/name) is "
    "optional. Returns the stored path.",
    R"({"type":"object","properties":{"prompt":{"type":"string"},"path":{"type":"string"},"size":{"type":"string"},"quality":{"type":"string","enum":["low","medium","high"]}},"required":["prompt"]})"};
const Decl kPdf = {
    "Write a PDF at path (project/name) from a title and text in light Markdown (# headings, - bullets, | table "
    "| rows |); text and tables only. Returns the stored path.",
    R"({"type":"object","properties":{"path":{"type":"string"},"title":{"type":"string"},"text":{"type":"string"}},"required":["path","text"]})"};
const Decl kShare = {
    "Share one of your files read-only with everyone who uses this device, or stop (share false).",
    R"({"type":"object","properties":{"path":{"type":"string"},"share":{"type":"boolean"}},"required":["path"]})"};
const Decl kStat = {"A stored file's facts: size, kind, created time.",
                    R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})"};
const Decl kFetch = {
    "Download an https URL into path (project/name) in the background, under the owner's download setting (it "
    "may wait for the owner's approval).",
    R"({"type":"object","properties":{"url":{"type":"string"},"path":{"type":"string"}},"required":["url","path"]})"};
const Decl kPlay = {
    "Play music from the card's /music folder on the speaker: every track, or one (track); queue true adds it "
    "after the current one.",
    R"({"type":"object","properties":{"track":{"type":"string"},"queue":{"type":"boolean"}}})"};
const Decl kPause = {"Pause the music.", R"({"type":"object","properties":{}})"};
const Decl kStop = {"Stop the music and clear the queue.", R"({"type":"object","properties":{}})"};
const Decl kList = {"List the tracks in the card's /music folder and the player's state.",
                    R"({"type":"object","properties":{}})"};
}  // namespace tool_descriptions

namespace tool_results {
const char kNotAPath[] = " is not a path (project/name)";
const char kReadOnly[] = " is read-only here";
// Engine 1's handlers name their own tools in a few lines; engine 2 names its tools.
const char* const kNames[][2] = {
    {"files.list", "list_files"},
    {"files.pdf", "pdf_write"},
    {"files.send", "send"},
    {"image.generate", "image_generate"},
};
}  // namespace tool_results

namespace {

namespace td = tool_descriptions;
namespace tr = tool_results;

// Argument and field names (engine 1's and engine 2's), not text a model reads as a sentence.
constexpr char kPath[] = "path";
constexpr char kProject[] = "project";
constexpr char kName[] = "name";

std::string str(JsonObjectConst a, const char* k) { return a[k].is<const char*>() ? a[k].as<const char*>() : ""; }

void rename(std::string& text) {
  for (const auto& n : tr::kNames) {
    const std::string from = n[0], to = n[1];
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
      text.replace(at, from.size(), to);
  }
}

orch::ToolResult renamed(orch::ToolResult r) {
  rename(r.output);
  rename(r.error);
  return r;
}

// path -> engine 1's {project, name} in `out`. A path that cannot be one, or (for a write) one the file
// tools may not write, is one true line in `err`.
bool splitPath(const std::string& path, bool write, JsonDocument& out, std::string& err) {
  const orch::StorePath sp = orch::storePathFor(path);
  if (!sp.ok()) return (err = path + tr::kNotAPath).empty();
  if (write && !orch::pathRuleFor(sp.project, sp.name).writable) return (err = sp.str() + tr::kReadOnly).empty();
  out[kProject] = sp.project;
  out[kName] = sp.name;
  return true;
}

using Map = bool (*)(JsonObjectConst in, JsonDocument& out, std::string& err);

// Engine 1's handler `old` behind engine 2's arguments: `map` turns them into engine 1's, then the answer
// names engine 2's tools.
orch::ToolHandler bound(const orch::ToolHandler& old, Map map) {
  return [old, map](JsonObjectConst a, const orch::Principal& who) {
#ifdef ARDUINO
    const PsramFirstScope psram;
#endif
    JsonDocument args;
    args.to<ArduinoJson::JsonObject>();
    std::string err;
    if (!map(a, args, err)) return orch::ToolResult::fail(err);
    return renamed(old(args.as<JsonObjectConst>(), who));
  };
}

bool mapStat(JsonObjectConst a, JsonDocument& o, std::string& err) { return splitPath(str(a, kPath), false, o, err); }

bool mapShare(JsonObjectConst a, JsonDocument& o, std::string& err) {
  if (!splitPath(str(a, kPath), false, o, err)) return false;
  if (a["share"].is<bool>()) o["share"] = a["share"].as<bool>();
  return true;
}

bool mapFetch(JsonObjectConst a, JsonDocument& o, std::string& err) {
  o["url"] = str(a, "url");
  return splitPath(str(a, kPath), true, o, err);
}

bool mapPdf(JsonObjectConst a, JsonDocument& o, std::string& err) {
  o["title"] = str(a, "title");
  o["text"] = str(a, "text");
  return splitPath(str(a, kPath), true, o, err);
}

// The path is optional: without one engine 1 names the file from the prompt under images/.
bool mapImage(JsonObjectConst a, JsonDocument& o, std::string& err) {
  for (const char* k : {"prompt", "size", "quality"})
    if (!str(a, k).empty()) o[k] = str(a, k);
  const std::string path = str(a, kPath);
  return path.empty() || splitPath(path, true, o, err);
}

bool mapPlay(JsonObjectConst a, JsonDocument& o, std::string&) {
  if (!str(a, "track").empty()) o[kPath] = str(a, "track");
  if (a["queue"].is<bool>()) o["queue"] = a["queue"].as<bool>();
  return true;
}

bool mapNone(JsonObjectConst, JsonDocument&, std::string&) { return true; }

}  // namespace

// The names are literals here: the class, access-rule and taint tests read registrations from the source.
void registerTools(orch::ToolRegistry& reg, const OldHandlers& old) {
  if (old.image) reg.add("image_generate", td::kImage.description, bound(old.image, mapImage), td::kImage.schema);
  if (old.pdf) reg.add("pdf_write", td::kPdf.description, bound(old.pdf, mapPdf), td::kPdf.schema);
  if (old.share) reg.add("share_file", td::kShare.description, bound(old.share, mapShare), td::kShare.schema);
  if (old.stat) reg.add("file_stat", td::kStat.description, bound(old.stat, mapStat), td::kStat.schema);
  if (old.fetch) reg.add("fetch_url", td::kFetch.description, bound(old.fetch, mapFetch), td::kFetch.schema);
  if (old.play) reg.add("media_play", td::kPlay.description, bound(old.play, mapPlay), td::kPlay.schema);
  if (old.pause) reg.add("media_pause", td::kPause.description, bound(old.pause, mapNone), td::kPause.schema);
  if (old.stop) reg.add("media_stop", td::kStop.description, bound(old.stop, mapNone), td::kStop.schema);
  if (old.list) reg.add("media_list", td::kList.description, bound(old.list, mapNone), td::kList.schema);
}

#ifdef ARDUINO
void registerDevice(orch::ToolRegistry& reg) {
  OldHandlers h;
  h.stat = files::statTool;
  h.share = files::shareTool;
  h.fetch = files::fetchTool;
  h.pdf = pdf_tool::writeTool;
  h.image = files::imageTool;
  h.play = music::playTool;
  h.pause = music::pauseTool;
  h.stop = music::stopTool;
  h.list = music::listTool;
  registerTools(reg, h);
}
#endif

}  // namespace agent::plainfiles
