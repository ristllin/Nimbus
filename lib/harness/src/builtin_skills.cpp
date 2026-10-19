#include "nimbus/harness/builtin_skills.h"

#include <cstdint>
#include <cstring>

#include "nimbus/harness/board_identity.h"   // CUM-514: the build's own 'hardware' capsule

namespace agent {

namespace {

// Which turns a body part is served on (CUM-523).
// Telegram/NoTelegram: text that names reply.telegram (or Telegram as a channel);
// FileSend/NoFileSend: text that names files.send (CUM-523: a public-mode guest
// turn has the first and not the second).
enum class Side : uint8_t { Both, Telegram, NoTelegram, FileSend, NoFileSend };
struct Part {
  Side side;
  const char* text;
};

// Kept deliberately concise (distilled from docs/ + AGENTS.md); flash rodata, so
// they cost flash, not RAM. Update these when the architecture they describe changes.
const Part kDeepResearch[] = {
    {Side::Both,
     "# Deep research (multi-stage fan-out playbook)\n"
     "A head-side playbook for thorough, cited research using waves of sub-agents and\n"
     "the durable file store. Requires the SD card: if files are unavailable, say so\n"
     "honestly and OFFER the degraded mode instead (one wave of <=6 sub-agents,\n"
     "synthesized straight into your reply, nothing persisted).\n\n"
     "## Setup\n"
     "1. Pick ONE project tag for the whole run: dr-<topic-slug>-<MMDDHHMM> (<=24\n"
     "   chars, e.g. dr-mkt-08061530). Every spawn in this run carries it in\n"
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
     "   "},
    {Side::FileSend, "files.send for Telegram"},
    {Side::NoFileSend, "the Files list on the device web page"},
    {Side::Both,
     ", or a Gmail DRAFT via a Mistral sub-agent (email can\n"
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
     "  round up. A failed wave is stated, not papered over.\n"},
};

const Part kArchitecture[] = {
    {Side::Both,
     "# Device architecture (how you run)\n"
     "You are the head orchestrator on a Nimbus desk device (ESP32-S3) with two modes\n"
     "(the 'hardware' capsule names this board):\n"
     "- NOTIFIER: a BLE-driven status light for AI coding sessions (you are inactive).\n"
     "- ORCHESTRATOR (current): a hosted-LLM agent reachable over "},
    {Side::Telegram, "Telegram, "},
    {Side::Both,
     "the device\n"
     "  microphone (hold-to-talk), the web UI, and the serial console.\n\n"
     "A turn = one user message -> you produce ONE orch_turn (reply/ask + optional\n"
     "device actions + memory/session ops). With the tool loop ON (the default) you may\n"
     "first call tools across several rounds, THEN finish by emitting orch_turn.\n\n"
     "Your reply routes back to the channel the message arrived on (see [CHANNEL] each\n"
     "turn). To reach a DIFFERENT channel, use reply.speak (device speaker)"},
    {Side::Telegram, " or\nreply.telegram"},
    {Side::Both,
     ". Sub-work you delegate via session.spawn runs on a cloud provider\n"
     "(OpenAI/Anthropic/Mistral) as a fire-and-forget job; poll session.list for status.\n\n"
     "Memory: a vector store (memory.write/search) + a scratchpad + episodic history,\n"
     "backed by the SD card when present (degraded but functional on internal flash).\n"
     "You may NEVER set provider keys, the orchestrator host, or provider priority -\n"
     "those are human-only. You MAY retune the sub-session provider preference.\n"},
};

const Part kToolsGuide[] = {
    {Side::Both,
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
     "reply.speak"},
    {Side::Telegram, " / reply.telegram"},
    {Side::Both,
     ": choose an output channel beyond the default reply.\n"
     "Answer simple questions immediately; reserve tool rounds for work that needs them.\n"},
};

// The W15 deliver-pdf playbook. Lives here (flash rodata, rides OTA) so every
// board has the default; the owner overrides it live by saving an SD capsule
// with the same id (SD wins on collision) - recipe fixes never need a release.
// The catalog's [PROVIDERS & CONNECTORS] block carries only the config-gated
// one-line DISCLOSURE pointing here; this is the procedure.
const Part kDeliverPdf[] = {
    {Side::Both,
     "How to deliver a PDF (or any generated document) to the owner.\n"
     "The device cannot render documents itself; a sub-agent builds one in its\n"
     "provider sandbox and the device captures the file. The working recipe:\n"
     "1. Spawn ONE sub-agent on openai WITH A PROJECT:\n"
     "   session_ops: {op:\"spawn\", provider:\"openai\", project:\"<short-slug>\",\n"
     "   task:\"...\"}. The project field is what routes the captured file into\n"
     "   the device file store - no project, no capture.\n"
     "   To put a log or CSV from the file store in the task, read only its end\n"
     "   with files.read {project, name, tail: N} - never the whole file.\n"
     "2. In the task, tell the sub EXPLICITLY to gather/prepare the content, then\n"
     "   'use Python code_interpreter to render <name>.pdf, save it as a file,\n"
     "   and output the file'. A sub not told to output a file returns prose.\n"
     "3. When the sub finishes, its result includes a note naming the saved file\n"
     "   (project/name). NO note = NO file landed - say so honestly and retry or\n"
     "   explain; NEVER invent a filename or claim a stale file as new output.\n"},
    {Side::FileSend,
     "4. Deliver with files.send {project, name}. It queues for Telegram; if it\n"
     "   reports the media queue busy, retry on your next turn.\n"},
    {Side::NoFileSend,
     "4. Tell the owner the saved file's name (project/name): they open it from the\n"
     "   Files list on the device web page.\n"},
    {Side::Both,
     "Provider limits: mistral's sandbox ERRORS on PDF/CSV (images/text only);\n"
     "an anthropic sub can build files in its sandbox but nothing can capture\n"
     "them back - openai is the only delivery path. Do not silently downgrade\n"
     "the owner to markdown: if PDF is unavailable, say exactly why.\n"},
};

// Logging a row (the meal-photo addendum, and any tracker): files.append writes
// only the new rows and files.read {tail} reads only the end, so a logged meal is
// one tool call instead of a whole-file read plus a whole-file rewrite (on 15 Oct
// a typical meal turn read three CSVs whole and re-saved all three).
const Part kLogRows[] = {
    {Side::Both,
     "# Logging rows (food log, ledger, any tracker)\n"
     "A log is a text file that grows by rows: a CSV (date,meal,kcal,...) or a .log.\n"
     "1. Add rows with files.append {project, name, text}: only the new rows are\n"
     "   written. NEVER files.read a whole log and artifact.save it back to add a\n"
     "   row - that reads and rewrites the whole file, and every byte rides your turn.\n"
     "2. You do not need to read the log first when you know its columns. To see\n"
     "   the columns or the latest rows, read only the end:\n"
     "   files.read {project, name, tail: 5} (a CSV's header row comes with it).\n"
     "3. A new log: the first files.append carries the header row, then the row.\n"
     "4. A running total (a daily ledger): append a NEW row with the updated totals;\n"
     "   the latest row for a date is the current one. Never rewrite earlier rows.\n"
     "5. A meal photo: estimate it, then one files.append to the food log (and one\n"
     "   to the ledger if you keep one), then reply with the row you added. Several\n"
     "   rows for one meal go in ONE call, one row per line.\n"
     "6. If files.append says the last row was incomplete, the file was cut\n"
     "   mid-write: say so, and check the tail before adding that row again.\n"},
};

// ONE table: the index entry and the body together, so they cannot drift.
struct Entry {
  BuiltinSkill meta;
  const Part* parts;   // nullptr = this build's board text ('hardware', CUM-514)
  size_t count;
};
#define NIMBUS_PARTS(a) a, sizeof(a) / sizeof(a[0])
const Entry kEntries[] = {
    {{"architecture", "How this device is wired + how a turn runs",
      "How you run: turns, tool loop, sub-agents, memory tiers - read when unsure how your own "
      "machinery behaves."},
     NIMBUS_PARTS(kArchitecture)},
    {{"deep-research", "Deep research: multi-wave fan-out with cited report",
      "Multi-wave research fan-out producing a cited report - read BEFORE any deep/thorough "
      "research request."},
     NIMBUS_PARTS(kDeepResearch)},
    {{"hardware", "The device's hardware inventory",
      "What hardware this body has (ring, screen, mic, speaker, battery, SD) and what each can "
      "do."},
     nullptr, 0},
    {{"tools-guide", "How to use your own tools",
      "Which tool to reach for, per job - read when a task needs tools you rarely use."},
     NIMBUS_PARTS(kToolsGuide)},
    {{"deliver-pdf", "Deliver a PDF or generated document to the owner",
      "Build a PDF via an openai sub's code_interpreter and deliver it to the owner - read "
      "BEFORE promising, refusing, or downgrading a PDF/document request."},
     NIMBUS_PARTS(kDeliverPdf)},
    {{"log-rows", "Log a row to a CSV or log file (food log, ledger)",
      "Add rows to a log with files.append and read its end with files.read tail - read "
      "BEFORE logging a meal, a weight, a ledger total or any tracker row."},
     NIMBUS_PARTS(kLogRows)},
};
#undef NIMBUS_PARTS
constexpr size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

const Entry* findEntry(const std::string& id) {
  for (const Entry& e : kEntries)
    if (id == e.meta.id) return &e;
  return nullptr;
}

}  // namespace

size_t builtinSkillCount() { return kEntryCount; }

const BuiltinSkill& builtinSkill(size_t i) { return kEntries[i < kEntryCount ? i : 0].meta; }

bool isBuiltinSkill(const std::string& id) { return findEntry(id) != nullptr; }

std::string builtinSkillBody(const std::string& id, bool telegram, bool fileSend) {
  const Entry* e = findEntry(id);
  if (!e) return std::string();
  if (!e->parts) return std::string(buildBoardIdentity().hardware);
  auto shown = [telegram, fileSend](Side s) {
    switch (s) {
      case Side::Telegram: return telegram;
      case Side::NoTelegram: return !telegram;
      case Side::FileSend: return fileSend;
      case Side::NoFileSend: return !fileSend;
      case Side::Both: return true;
    }
    return true;
  };
  // One exact allocation (served during a turn, under the heap floor): size first.
  size_t total = 0;
  for (size_t i = 0; i < e->count; ++i)
    if (shown(e->parts[i].side)) total += std::strlen(e->parts[i].text);
  std::string out;
  out.reserve(total);
  for (size_t i = 0; i < e->count; ++i)
    if (shown(e->parts[i].side)) out += e->parts[i].text;
  return out;
}

std::string builtinSkillBody(const std::string& id, bool telegram) {
  return builtinSkillBody(id, telegram, telegram);
}

}  // namespace agent
