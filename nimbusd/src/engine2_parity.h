#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <ArduinoJson.h>

#include "daemon_config.h"
#include "nimbus/harness/jobs.h"
#include "nimbus/harness/plain/agents.h"         // kAgentsProject, agentReportPath
#include "nimbus/harness/plain/chat_binding.h"   // the binding's owner lines (hostedOwnerLine)
#include "nimbus/harness/plain/history.h"        // FileSearch (scope files)
#include "nimbus/harness/plain/rails.h"          // readsOutsidePath
#include "nimbus/orch/episodic.h"                // textMatchScore
#include "nimbus/orch/lexical.h"
#include "nimbus/orch/path_rules.h"              // storePathFor
#include "nimbus/utf8.h"
#include "posix_files.h"
#include "posix_fs.h"

// engine2_parity - lane E2-P-HOST2: what the hosted daemon gives engine 2 beside the owner chat, each a
// binding of what engine 1 or the device's engine 2 already does (no second copy of a rule):
//   * the engine choice per instance: <data>/mem/engine.json ({"engine":1|2}) when it holds a valid value,
//     else NIMBUSD_ENGINE (the environment or config.env), else engine 1. The file is written by
//     POST /api/engine and read at start; the way back is engine 1 again over the same volume (engine 2's
//     turns are kept as tagged rows in engine 1's day stream, so engine 1 reads the conversation);
//   * the outside-content mark of a call (the device's file_tools_device.cpp rule): web_search, a read
//     or listing under inbox/ or agents/ (plain::readsOutsidePath), or another person's file;
//   * search scope files (the device's files::searchText scan over the one hosted file store);
//   * a sub-agent's brief files and its whole report (the device's readDoc and persistResult);
//   * the binding's owner lines that name the card or the device, in the instance's words;
//   * engine 2's counters for /api/state.
namespace nimbusd {

// ---- the engine choice -----------------------------------------------------------------------
struct EngineChoice {
  int engine = 1;
  std::string from = "default";   // "instance" (mem/engine.json), "env" (NIMBUSD_ENGINE) or "default"
};

inline std::string engineFilePath(const std::string& dataDir) { return dataDir + "/mem/engine.json"; }

// The instance's stored choice; false when there is none or it does not hold 1 or 2.
inline bool readEngineFile(const std::string& dataDir, int& engine) {
  std::string text;
  if (!fsutil::readFile(engineFilePath(dataDir), text)) return false;
  JsonDocument d;
  if (deserializeJson(d, text) != DeserializationError::Ok || !d["engine"].is<int>()) return false;
  const int e = d["engine"].as<int>();
  if (e != 1 && e != 2) return false;
  engine = e;
  return true;
}

inline bool storeEngineFile(const std::string& dataDir, int engine) {
  if (engine != 1 && engine != 2) return false;
  fsutil::mkdirs(dataDir + "/mem");
  return fsutil::writeFileAtomic(engineFilePath(dataDir), std::string("{\"engine\":") + char('0' + engine) + "}\n");
}

inline EngineChoice engineChoice(const Config& cfg, const std::string& dataDir) {
  EngineChoice c;
  int e = 0;
  if (readEngineFile(dataDir, e)) {
    c.engine = e;
    c.from = "instance";
    return c;
  }
  const std::string env = cfg.get("NIMBUSD_ENGINE");
  if (env == "1" || env == "2") {
    c.engine = env == "2" ? 2 : 1;
    c.from = "env";
  }
  return c;
}

// ---- engine 2's counters (/api/state) --------------------------------------------------------
struct Engine2Stats {
  std::atomic<uint32_t> turns{0};
  std::atomic<uint64_t> sessIn{0}, sessOut{0};
  std::atomic<uint32_t> lastIn{0}, lastOut{0};
};

// ---- the outside-content mark ----------------------------------------------------------------
// A file of another person: its entry is not in `who`'s namespace (the device's files::readsOthersFile).
inline bool readsOthersFile(PosixFiles& files, const nimbus::orch::Principal& who, const std::string& path) {
  const nimbus::orch::StorePath sp = nimbus::orch::storePathFor(path);
  if (!sp.ok()) return false;
  std::lock_guard<std::recursive_mutex> g(files.mutex());
  const nimbus::orch::FileEntry* e = files.toolsIndex().find(sp.project, sp.name);
  return e && !files.toolsIndex().ownedBy(*e, who.ns, false);
}

// Did this call read content the person did not write? web_search always (the device marks it even on
// its own refusal); a read_file or list_files that succeeded under inbox/ or agents/, or a read of
// another person's file.
inline bool hostedReadsOutside(PosixFiles& files, const std::string& tool, const std::string& argsJson, bool ok,
                               const nimbus::orch::Principal& who) {
  if (tool == "web_search") return true;
  if (!ok || (tool != "read_file" && tool != "list_files")) return false;
  if (agent::plain::readsOutsidePath(tool, argsJson)) return true;
  if (tool != "read_file") return false;
  JsonDocument a;
  if (deserializeJson(a, argsJson) != DeserializationError::Ok || !a["path"].is<const char*>()) return false;
  return readsOthersFile(files, who, a["path"].as<std::string>());
}

// ---- search scope files ------------------------------------------------------------------------
inline bool hostedTextDoc(const std::string& name) {
  for (const char* ext : {".md", ".txt", ".json", ".csv", ".log"}) {
    const size_t n = std::strlen(ext);
    if (name.size() < n) continue;
    bool same = true;
    for (size_t i = 0; i < n && same; i++) same = std::tolower((unsigned char)name[name.size() - n + i]) == ext[i];
    if (same) return true;
  }
  return false;
}

// The person's readable text files, newest first, at most `max` (the device's scan order).
inline std::vector<const nimbus::orch::FileEntry*> readableTextDocs(nimbus::orch::FileStore& s,
                                                                   const nimbus::orch::Principal& who, size_t max,
                                                                   size_t& candidates) {
  const bool shared = who.owner || who.perms().readShared;
  std::vector<const nimbus::orch::FileEntry*> docs;
  for (const nimbus::orch::FileEntry* e : s.list())
    if (hostedTextDoc(nimbus::orch::plainCopy(e->name)) && s.readableBy(*e, who.ns, who.owner, shared)) docs.push_back(e);
  candidates = docs.size();
  std::sort(docs.begin(), docs.end(), [](const nimbus::orch::FileEntry* x, const nimbus::orch::FileEntry* y) {
    return x->createdAt != y->createdAt ? x->createdAt > y->createdAt : x->name < y->name;
  });
  if (docs.size() > max) docs.resize(max);
  return docs;
}

// One file's hit: its path, the window around its first matching word on one line, and whether it is
// outside content (inbox/, agents/ or another person's file).
inline agent::plain::FileHit fileHitOf(const nimbus::orch::FileStore& s, const nimbus::orch::FileEntry& e,
                                       const std::string& path, const std::string& body,
                                       const nimbus::orch::LexQuery& lex, const nimbus::orch::Principal& who) {
  agent::plain::FileHit h;
  h.path = path;
  h.excerpt = nimbus::utf8Window(body, nimbus::orch::lexFirstHit(body.data(), body.size(), lex), 40, 160);
  for (char& ch : h.excerpt)
    if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
  JsonDocument args;
  args["path"] = h.path;
  std::string json;
  serializeJson(args, json);
  h.outside = agent::plain::readsOutsidePath("read_file", json) || !s.ownedBy(e, who.ns, false);
  return h;
}

// The words inside the person's readable text files, newest 512 read, best first (the device's scan).
inline bool hostedFileSearch(PosixFiles& files, const nimbus::orch::Principal& who, const std::string& query,
                             agent::plain::FileSearch& out) {
  constexpr size_t kScanMax = 512, kReadMax = 64 * 1024;
  out = agent::plain::FileSearch();
  const nimbus::orch::LexQuery lex = nimbus::orch::lexParse(query);
  std::lock_guard<std::recursive_mutex> g(files.mutex());
  nimbus::orch::FileStore& s = files.toolsIndex();
  std::vector<std::pair<int, agent::plain::FileHit>> hits;
  for (const nimbus::orch::FileEntry* e : readableTextDocs(s, who, kScanMax, out.candidates)) {
    ++out.scanned;
    out.oldestT = e->createdAt;
    const std::string project = nimbus::orch::plainCopy(e->project), name = nimbus::orch::plainCopy(e->name);
    std::string body;
    if (!files.fetch(project, name, body)) continue;
    if (body.size() > kReadMax) body.resize(kReadMax);
    const int score = nimbus::orch::textMatchScore(body, query);
    if (score > 0) hits.emplace_back(score, fileHitOf(s, *e, project + "/" + name, body, lex, who));
  }
  std::stable_sort(hits.begin(), hits.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
  for (auto& h : hits) out.hits.push_back(std::move(h.second));
  return true;
}

// ---- sub-agents: the brief's files and the whole report --------------------------------------
// A brief's file, text only, when `who` may read it ("" otherwise: the job notes it instead).
inline std::string hostedReadDoc(PosixFiles& files, const nimbus::orch::Principal& who, const std::string& project,
                                 const std::string& name) {
  std::lock_guard<std::recursive_mutex> g(files.mutex());
  nimbus::orch::FileStore& s = files.toolsIndex();
  const nimbus::orch::FileEntry* e = s.find(project, name);
  if (!e || !hostedTextDoc(name) || !s.readableBy(*e, who.ns, who.owner, who.owner || who.perms().readShared))
    return std::string();
  std::string body;
  return files.fetch(project, name, body) ? body : std::string();
}

// The JobEngine's persistResult on engine 2 (the device's jobFileDeps): a report of the agents project saved
// whole as agents/<id>-<name>.md; the outcome line the JobEngine appends ("" for any other project).
inline std::string persistAgentReport(PosixFiles& files, const std::string& project, const std::string& name,
                                      const std::string& tag, const std::string& text) {
  std::string path, err;
  if (project != agent::plain::kAgentsProject || !agent::plain::agentReportPath(tag, name, path).ok())
    return std::string();
  if (!files.put(project, path.substr(project.size() + 1), text, err)) return "[persist FAILED: " + err + "]";
  return "[saved: " + path + "]";
}

// ---- the binding's owner lines in the instance's words ---------------------------------------
// The binding names the device's card; a hosted instance keeps the conversation on its disk.
inline std::string hostedOwnerLine(const std::string& part) {
  if (part == agent::plain::kNotLoadedLine)
    return "The stored conversation could not be loaded; what you say now is kept in memory only until the "
           "instance's disk answers again.";
  if (part == agent::plain::kImportStuckLine)
    return "The earlier conversation could not be imported, so what you say now is kept in memory only. Send /clear "
           "to keep it in a new conversation; the earlier one stays stored on this instance.";
  if (part == agent::plain::kHeldWaitingLine)
    return "This chat cannot start yet: another conversation is kept in memory because the instance's disk is not "
           "storing it now. It is stored by itself when the disk answers again. Send the message again later.";
  return part;
}

}  // namespace nimbusd
