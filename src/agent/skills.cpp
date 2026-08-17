#include "skills.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <cstring>

#include "memory_subsystem.h"          // dataFs() + haveSd() + Lock (SD serialization)
#include "../sys/agent_log.h"
#include "nimbus/harness/skill_md.h"   // portable parseSkillMd (host-tested)

namespace agent::skills {

namespace {

// ---- built-in PROGMEM capsules ----------------------------------------------
// Kept deliberately concise (distilled from docs/ + AGENTS.md). PROGMEM so they
// cost flash, not RAM. Update these when the architecture they describe changes.
const char kDeepResearch[] PROGMEM =
    "# Deep research (multi-stage fan-out playbook)\n"
    "A head-side playbook for thorough, cited research using waves of sub-agents and\n"
    "the durable file store. Requires the SD card: if files are unavailable, say so\n"
    "honestly and OFFER the degraded mode instead (one wave of <=6 sub-agents,\n"
    "synthesized straight into your reply, nothing persisted).\n\n"
    "## Setup\n"
    "1. Pick ONE project tag for the whole run: dr-<topic-slug>-<MMDDHHMM> (<=24\n"
    "   chars, e.g. dr-eink-mkt-08061530). Every spawn in this run carries it in\n"
    "   session_ops project - each sub-agent's full result then AUTO-SAVES as\n"
    "   <project>/<name>-<tag>.md (the [saved: ...] line in FRESH RESULTS confirms it).\n"
    "2. Plan ORTHOGONAL sub-questions that together cover the ask - not overlapping\n"
    "   rephrasings. Default breadth 4-8 for a normal ask; scale up only for genuinely\n"
    "   broad briefs. HARD CAP: 100 sub-agents per run, and the project holds at most\n"
    "   128 docs. Keep the wave plan + a checklist in your SCRATCHPAD (the `scratchpad`\n"
    "   field: put steps in scratchpad.short, tick them off as sub-agents finish) - it\n"
    "   persists across the run's turns, unlike the next-turn-only memory field.\n\n"
    "## Waves\n"
    "3. Spawn up to 6 sub-agents per turn (the job table's limit) - one per\n"
    "   sub-question, small/default model, with: a clear brief (what to find, what to\n"
    "   IGNORE), an instruction to CITE sources with URLs and dates, prefer primary\n"
    "   sources, and end with a one-line TL;DR. Give each a short name (its doc name).\n"
    "4. Between waves you get an automatic synthesis turn when results land. There:\n"
    "   check the [saved: ...] confirmations, update the scratchpad checklist, note\n"
    "   GAPS (unanswered questions, claims needing a second source, new angles), and\n"
    "   spawn the next wave AIMED AT THE GAPS. For calculations or data crunching,\n"
    "   spawn a calculation sub-agent between research waves (multi-stage: research ->\n"
    "   compute -> more research is normal for e.g. finance).\n"
    "5. STOP when saturated: a wave that mostly repeats what earlier docs already say\n"
    "   means more waves will not help. Do not burn the cap for its own sake.\n\n"
    "## The report\n"
    "6. Select the STRONGEST docs (files.list the project; files.read a PAGE to skim\n"
    "   - never full-read every doc yourself, that burns your tool budget). Then\n"
    "   spawn ONE final sub-agent on a LARGE model (provider default, model from\n"
    "   [AVAILABLE MODELS]) with project set and the attach ARRAY FIELD listing the\n"
    "   selected docs (up to 4; the device splices their full content into its brief\n"
    "   - doc names in the task text alone attach nothing). Its task: write the\n"
    "   final report - TL;DR bullets, key findings with inline [n] citations, an\n"
    "   'Open questions / contested points' section naming disagreements between\n"
    "   sources, and a numbered Sources list (URL + date + one line on what it\n"
    "   contributed). Every non-obvious claim cites a source. No filler.\n"
    "7. The report auto-saves into the project. Deliver it the way the owner asked:\n"
    "   files.send for Telegram, or a Gmail DRAFT via a Mistral sub-agent (email can\n"
    "   never be SENT from this device - say 'drafted, not sent'). If the report\n"
    "   sub-agent fails, write the report YOURSELF from files.read of the best docs -\n"
    "   degraded but delivered beats silent loss.\n"
    "8. VERIFY grounding before delivering: if the report says its sources were\n"
    "   missing or it wrote from general knowledge, the attachment did not happen -\n"
    "   do NOT deliver it as the report; re-spawn with the attach field set, and say\n"
    "   so if you already told the owner it was under way.\n\n"
    "## Honesty rails\n"
    "- Only claim a doc/report exists after the [saved: ...] line or a files.list/\n"
    "  files.read confirms it. Quote real doc names.\n"
    "- Report partial progress plainly (waves run, docs saved, gaps left) - never\n"
    "  round up. A failed wave is stated, not papered over.\n";

const char kArchitecture[] PROGMEM =
    "# Device architecture (how you run)\n"
    "You are the head orchestrator on a Solide S3 (ESP32-S3) desk device with two modes:\n"
    "- NOTIFIER: a BLE-driven status light for AI coding sessions (you are inactive).\n"
    "- ORCHESTRATOR (current): a hosted-LLM agent reachable over Telegram, the device\n"
    "  microphone (hold-to-talk), the web UI, and the serial console.\n\n"
    "A turn = one user message -> you produce ONE orch_turn (reply/ask + optional\n"
    "device actions + memory/session ops). With the tool loop ON (the default) you may\n"
    "first call tools across several rounds, THEN finish by emitting orch_turn.\n\n"
    "Your reply routes back to the channel the message arrived on (see [CHANNEL] each\n"
    "turn). To reach a DIFFERENT channel, use reply.speak (device speaker) or\n"
    "reply.telegram. Sub-work you delegate via session.spawn runs on a cloud provider\n"
    "(OpenAI/Anthropic/Mistral) as a fire-and-forget job; poll session.list for status.\n\n"
    "Memory: a vector store (memory.write/search) + a scratchpad + episodic history,\n"
    "backed by the SD card when present (degraded but functional on internal flash).\n"
    "You may NEVER set provider keys, the orchestrator host, or provider priority -\n"
    "those are human-only. You MAY retune the sub-session provider preference.\n";

const char kHardware[] PROGMEM =
    "# Device hardware\n"
    "Board: ESP32-S3-DevKitC-1 N16R8 (16 MB flash, 8 MB PSRAM; TLS + the memory\n"
    "working set live in PSRAM, ~300 KB internal SRAM is scarce).\n"
    // ⚠ No display/input claim here: a device has EITHER the e-ink + knob OR the
    // colour touchscreen (whose panel takes the knob's pins), and this capsule is
    // static. Hardcoding one told half the fleet to look at hardware it does not
    // have. The live manifest (orch_world) and system.health answer this
    // correctly per board - and the capsule already directs the model there.
    "Peripherals: 45-LED WS2812B ring; a status display with its input device (the\n"
    "live manifest names which); I2S MEMS microphone (INMP441/ICS-43434); MAX98357A I2S speaker\n"
    "(both audio paths verified via acoustic loopback); microSD (orchestrator data\n"
    "store); Wi-Fi 2.4 GHz; BLE (Notifier mode only). No battery gauge yet\n"
    "(desk-powered). Call system.health for the LIVE status of each of these.\n";

const char kToolsGuide[] PROGMEM =
    "# Using your tools\n"
    "memory.write/search/update/pin: your long-term vector memory - save durable facts\n"
    "the owner will want later; search before claiming you don't know something.\n"
    "session.spawn/list/terminate: delegate heavy/long work to a cloud sub-agent. A\n"
    "spawn's `skill` field names a capsule from skill.list - owner-authored (SD)\n"
    "capsules are INJECTED into that sub-agent's instructions at spawn.\n"
    "web.search: live web lookups (only if a Tavily key is set).\n"
    "system.health: your own hardware/subsystem status.\n"
    "skill.list/skill.get: pull long-form docs about yourself (this guide included).\n"
    "Device state (LED theme, ring level, brightness, power profile, lights on/off,\n"
    "voice) is NOT a callable tool: change it with the device[] actions of your\n"
    "orch_turn (lights / led / config / tts). Protected settings are refused.\n"
    "reply.speak / reply.telegram: choose an output channel beyond the default reply.\n"
    "Answer simple questions immediately; reserve tool rounds for work that needs them.\n";

// The W15 deliver-pdf playbook. Lives here (flash rodata, rides OTA) so every
// board has the default; the owner overrides it live by saving an SD capsule
// with the same id (SD wins on collision) - recipe fixes never need a release.
// The catalog's [PROVIDERS & CONNECTORS] block carries only the config-gated
// one-line DISCLOSURE pointing here; this is the procedure.
static const char kDeliverPdf[] =
    "How to deliver a PDF (or any generated document) to the owner.\n"
    "The device cannot render documents itself; a sub-agent builds one in its\n"
    "provider sandbox and the device captures the file. The working recipe:\n"
    "1. Spawn ONE sub-agent on openai WITH A PROJECT:\n"
    "   session_ops: {op:\"spawn\", provider:\"openai\", project:\"<short-slug>\",\n"
    "   task:\"...\"}. The project field is what routes the captured file into\n"
    "   the device file store - no project, no capture.\n"
    "2. In the task, tell the sub EXPLICITLY to gather/prepare the content, then\n"
    "   'use Python code_interpreter to render <name>.pdf, save it as a file,\n"
    "   and output the file'. A sub not told to output a file returns prose.\n"
    "3. When the sub finishes, its result includes a note naming the saved file\n"
    "   (project/name). NO note = NO file landed - say so honestly and retry or\n"
    "   explain; NEVER invent a filename or claim a stale file as new output.\n"
    "4. Deliver with files.send {project, name}. It queues for Telegram; if it\n"
    "   reports the media queue busy, retry on your next turn.\n"
    "Provider limits: mistral's sandbox ERRORS on PDF/CSV (images/text only);\n"
    "an anthropic sub can build files in its sandbox but nothing can capture\n"
    "them back - openai is the only delivery path. Do not silently downgrade\n"
    "the owner to markdown: if PDF is unavailable, say exactly why.\n";

struct Builtin { const char* id; const char* title; const char* desc; const char* body; };
const Builtin kBuiltins[] = {
    {"architecture", "How this device is wired + how a turn runs",
     "How you run: turns, tool loop, sub-agents, memory tiers - read when unsure how your own machinery behaves.",
     kArchitecture},
    {"deep-research", "Deep research: multi-wave fan-out with cited report",
     "Multi-wave research fan-out producing a cited report - read BEFORE any deep/thorough research request.",
     kDeepResearch},
    {"hardware", "The device's hardware inventory",
     "What hardware this body has (ring, screen, mic, speaker, battery, SD) and what each can do.",
     kHardware},
    {"tools-guide", "How to use your own tools",
     "Which tool to reach for, per job - read when a task needs tools you rarely use.",
     kToolsGuide},
    {"deliver-pdf", "Deliver a PDF or generated document to the owner",
     "Build a PDF via an openai sub's code_interpreter and send it with files.send - read BEFORE promising, refusing, or downgrading a PDF/document request.",
     kDeliverPdf},
};
constexpr int kBuiltinCount = sizeof(kBuiltins) / sizeof(kBuiltins[0]);

// ---- SD capsules ------------------------------------------------------------
const char*  kSkillsDir  = "/mem/skills";
// File read cap: capsules are small by contract; the spawn injection is further
// capped at 4 KB by the portable composer.
constexpr size_t kFileCap = 8192;

// The Directive/PendingSpawn skill field is char[24] - an id longer than 23
// bytes could never round-trip through a spawn, so refuse it at save time.
constexpr size_t kIdMax = 23;

bool validId(const std::string& id) {
  if (id.empty() || id.size() > kIdMax) return false;
  for (char c : id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

std::string mdPath(const std::string& id) {
  return std::string(kSkillsDir) + "/" + id + "/SKILL.md";
}

// Read <id>/SKILL.md through a PSRAM staging buffer (never a big internal-SRAM
// block), capped at kFileCap. "" when absent/unreadable. Caller holds no lock;
// this takes memory::Lock around the SD I/O (the one mutex serializing the bus).
std::string readMd(const std::string& id) {
  if (!memory::haveSd() || !validId(id)) return std::string();
  agent::memory::Lock g;
  File f = memory::dataFs().open(mdPath(id).c_str(), FILE_READ);
  if (!f) return std::string();
  size_t n = f.size();
  if (n > kFileCap) n = kFileCap;
  char* buf = (char*)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
  if (!buf) { f.close(); return std::string(); }
  size_t got = f.read((uint8_t*)buf, n);
  f.close();
  std::string out(buf, got);
  heap_caps_free(buf);
  return out;
}

}  // namespace

bool sdAvailable() { return memory::haveSd(); }

std::vector<Capsule> list() {
  std::vector<Capsule> out;
  for (int i = 0; i < kBuiltinCount; i++)
    out.push_back(Capsule{kBuiltins[i].id, kBuiltins[i].title, kBuiltins[i].desc,
                          "", "builtin", "builtin", true});
  if (!memory::haveSd()) return out;

  // Scan /mem/skills/<id>/SKILL.md. Directory walk under the Lock; the per-file
  // header read re-takes it recursively (memory::Lock is recursive).
  std::vector<std::string> ids;
  {
    agent::memory::Lock g;
    File dir = memory::dataFs().open(kSkillsDir);
    if (dir && dir.isDirectory()) {
      File e;
      while ((e = dir.openNextFile())) {
        if (e.isDirectory()) {
          const char* full = e.name();          // may be full path or bare name
          const char* slash = strrchr(full, '/');
          std::string id = slash ? slash + 1 : full;
          if (validId(id)) ids.push_back(id);
        }
        e.close();
      }
    }
    if (dir) dir.close();
  }
  for (const auto& id : ids) {
    std::string md = readMd(id);
    if (md.empty()) continue;                   // dir without a SKILL.md
    SkillMd doc = parseSkillMd(md);
    Capsule c;
    c.id       = id;
    c.title    = doc.title.empty() ? id : doc.title;
    c.desc     = doc.desc;   // "" falls back to title in indexText
    c.version  = doc.version;
    c.source   = "sd";
    c.origin   = doc.createdBy;      // "user" | "agent" (parser clamps)
    c.approved = doc.approved;
    // SD wins on an id collision with a built-in (owner override).
    bool replaced = false;
    for (auto& b : out)
      if (b.id == id) { b = c; replaced = true; break; }
    if (!replaced) out.push_back(std::move(c));
  }
  return out;
}

std::string indexText() {
  std::vector<agent::SkillIndexEntry> es;
  for (const auto& c : list()) {
    agent::SkillIndexEntry e;
    e.id      = c.id;
    e.desc    = !c.desc.empty() ? c.desc : c.title;   // desc, else title, else id
    e.pending = !c.approved;
    es.push_back(std::move(e));
  }
  return agent::skillsIndexText(es);
}

std::string get(const std::string& id) {
  // SD first (SD wins on collision), then the built-ins.
  std::string md = readMd(id);
  if (!md.empty()) return parseSkillMd(md).body;
  for (int i = 0; i < kBuiltinCount; i++)
    if (id == kBuiltins[i].id) return std::string(kBuiltins[i].body);
  return std::string();
}

std::string spawnCapsule(const std::string& id) {
  // Only owner-authored SD capsules inject at spawn, and only when their front
  // matter says inject: spawn|both. Built-ins stay pure provider hints - the
  // pre-P2 behavior - so existing spawn flows are byte-identical.
  std::string md = readMd(id);
  if (md.empty()) return std::string();
  SkillMd doc = parseSkillMd(md);
  if (doc.inject == "context") return std::string();
  // INERT-PENDING (v4.0.0): an unapproved agent capsule never rides a spawn
  // brief - a persistent instruction blob is a prompt-injection channel until
  // the owner approves it (web or /skill approve). skill.get read-back still
  // works; injection is the gated surface.
  if (!doc.approved) return std::string();
  return doc.body;
}

std::string raw(const std::string& id) { return readMd(id); }

bool reservedId(const std::string& id) {
  for (int i = 0; i < kBuiltinCount; i++)
    if (id == kBuiltins[i].id) return true;
  return false;
}

int pendingAgentCount() {
  int n = 0;
  for (const auto& c : list())
    if (c.origin == "agent" && !c.approved) n++;
  return n;
}

// Device clock as "YYYY-MM-DD HH:MM" ("" before SNTP sync - honest absence
// beats a 1970 stamp).
static std::string nowStamp() {
  time_t t = time(nullptr);
  if (t < 1600000000) return std::string();   // clock not synced yet
  struct tm tmv;
  localtime_r(&t, &tmv);
  char buf[20];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d", tmv.tm_year + 1900,
           tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
  return std::string(buf);
}

// Write canonical text atomically (tmp + rename, writeBlobAtomic discipline).
static bool writeMd(const std::string& id, const std::string& text, std::string& err) {
  agent::memory::Lock g;
  fs::FS& fs = memory::dataFs();
  fs.mkdir(kSkillsDir);
  fs.mkdir((std::string(kSkillsDir) + "/" + id).c_str());
  std::string path = mdPath(id), tmp = path + ".tmp";
  File f = fs.open(tmp.c_str(), FILE_WRITE);
  if (!f) { err = "SD open failed"; return false; }
  size_t n = f.write((const uint8_t*)text.data(), text.size());
  f.close();
  if (n != text.size()) { fs.remove(tmp.c_str()); err = "SD write failed"; return false; }
  fs.remove(path.c_str());
  if (!fs.rename(tmp.c_str(), path.c_str())) { err = "SD rename failed"; return false; }
  return true;
}

bool save(const std::string& id, const std::string& md, std::string& err, bool byAgent) {
  if (!validId(id)) { err = "bad id (a-z 0-9 - _ , max 23 chars)"; return false; }
  if (md.empty())   { err = "empty capsule"; return false; }
  if (md.size() > kFileCap) { err = "capsule too large (8 KB max)"; return false; }
  if (!memory::haveSd()) { err = "no SD card - dynamic skills need the SD tier"; return false; }
  if (byAgent && reservedId(id)) {
    err = "'" + id + "' is a built-in skill - pick a different id";
    return false;
  }
  // Parse whatever was handed in, keep the CONTENT fields, SERVER-STAMP the
  // origin, and re-emit canonical front matter - a model-supplied
  // `approved: true` or `created_by: user` never reaches disk (the portable
  // composeSkillMd round-trip is the sanitization, host-tested).
  SkillMd doc = parseSkillMd(md);
  doc.createdBy = byAgent ? "agent" : "user";
  // Agent saves are ALWAYS pending - including re-saves of an approved capsule
  // (approve-then-mutate must not bypass review). Owner saves imply approval.
  doc.approved  = !byAgent;
  doc.createdAt = nowStamp();
  const std::string canon = composeSkillMd(doc);
  if (canon.size() > kFileCap) { err = "capsule too large (8 KB max)"; return false; }
  if (!writeMd(id, canon, err)) return false;
  alogf("skills: saved %s (%u bytes, by=%s%s)", id.c_str(), (unsigned)canon.size(),
        byAgent ? "agent" : "user", byAgent ? " PENDING" : "");
  return true;
}

bool approve(const std::string& id, std::string& err) {
  // Hold the Lock across the whole read-modify-write (it is recursive, so the
  // inner readMd/writeMd re-take it harmlessly). Without this, an agent
  // skill.save landing between our read and our write would be OVERWRITTEN by
  // the approve - an approved capsule with a body the owner never reviewed,
  // exactly the approve-then-mutate bypass the save() stamping exists to stop.
  agent::memory::Lock g;
  std::string md = readMd(id);
  if (md.empty()) { err = "no such skill on SD"; return false; }
  SkillMd doc = parseSkillMd(md);
  if (doc.approved) return true;                 // idempotent
  doc.approved = true;
  if (!writeMd(id, composeSkillMd(doc), err)) return false;
  alogf("skills: approved %s", id.c_str());
  return true;
}

bool remove(const std::string& id, std::string& err, bool byAgent) {
  if (!validId(id)) { err = "bad id"; return false; }
  if (!memory::haveSd()) { err = "no SD card"; return false; }
  // Lock spans the origin check AND the delete (recursive; same reasoning as
  // approve()) - the check must not run against a file another task replaces
  // before the delete lands.
  agent::memory::Lock g;
  if (byAgent) {
    // The model may delete ONLY what it authored; user capsules are the
    // owner's (web UI). Read the origin from disk - never trust the caller.
    std::string md = readMd(id);
    if (md.empty()) { err = "no such skill on SD"; return false; }
    if (parseSkillMd(md).createdBy != "agent") {
      err = "'" + id + "' was created by the owner - only they can delete it";
      return false;
    }
  }
  fs::FS& fs = memory::dataFs();
  if (!fs.exists(mdPath(id).c_str())) { err = "no such skill on SD"; return false; }
  fs.remove(mdPath(id).c_str());
  fs.rmdir((std::string(kSkillsDir) + "/" + id).c_str());   // best-effort
  alogf("skills: removed %s%s", id.c_str(), byAgent ? " (by agent)" : "");
  return true;
}

}  // namespace agent::skills
