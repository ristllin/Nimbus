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
     "web.search: live web lookups (with a Tavily, Mistral or Z.ai key).\n"
     "system.health: your own hardware/subsystem status.\n"
     "skill.list/skill.get: task playbooks and knowledge capsules - read the matching one "
     "BEFORE a task it covers; once read it stays attached to the chat ([ACTIVE SKILLS]).\n"
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
     "1. Read the TO DELIVER A PDF line in [SUB-AGENT CAPABILITIES]: it names the\n"
     "   route this device has right now, or exactly why there is none.\n"
     "2. Follow that route:\n"
     "   - through an openai sub: spawn ONE sub-agent with a project:\n"
     "     session_ops: {op:\"spawn\", provider:\"openai\", skill:\"code_interpreter\",\n"
     "     project:\"<short-slug>\", task:\"...\"}. In the task, tell the sub to\n"
     "     gather/prepare the content, then 'use Python code_interpreter to render\n"
     "     <name>.pdf, save it as a file, and output the file'.\n"
     "   - through an anthropic sub: the same spawn with provider:\"anthropic\"; in\n"
     "     the task, tell it to render <name>.pdf and save it under\n"
     "     /mnt/session/outputs/.\n"
     "   - with the device's own PDF writer: call files.pdf {project, name, title,\n"
     "     text} yourself with the finished text (# headings, - bullets,\n"
     "     | table | rows |). Text and simple tables only, no charts or images.\n"
     "   A sub not told to output a file returns prose. A captured file with no\n"
     "   project lands in the \"files\" project.\n"
     "   To put a log or CSV from the file store in the task, read only its end\n"
     "   with files.read {project, name, tail: N} - never the whole file.\n"
     "3. The result carries a note naming the saved file (project/name). NO note =\n"
     "   NO file landed - say so honestly and retry or explain; NEVER invent a\n"
     "   filename or claim a stale file as new output.\n"},
    {Side::FileSend,
     "4. Deliver with files.send {project, name}. It queues for Telegram; if it\n"
     "   reports the media queue busy, retry on your next turn.\n"},
    {Side::NoFileSend,
     "4. Tell the owner the saved file's name (project/name): they open it from the\n"
     "   Files list on the device web page.\n"},
    {Side::Both,
     "Provider limits: mistral's sandbox ERRORS on PDF/CSV (images/text only), so a\n"
     "PDF never goes there. If the PDF line says it is not possible, tell the owner\n"
     "exactly why; do not silently downgrade to markdown.\n"},
};

// Logging a row (the meal-photo addendum, and any tracker): files.append writes
// only the new rows and files.read {tail} reads only the end, so a logged meal is
// one tool call instead of a whole-file read plus a whole-file rewrite (on 15 Oct
// a typical meal turn read three CSVs whole and re-saved all three). A dated CSV
// is keyed by date plus a key column (nimbus/orch/file_append.h): a correction
// REPLACES its row, so the day total (the sum of the day's rows) never counts a
// corrected figure twice - the one correction rule the tool, the total and this
// playbook share.
const Part kLogRows[] = {
    {Side::Both,
     "# Logging rows (food log, ledger, any tracker)\n"
     "A log is a text file that grows by rows: a CSV (date,meal,kcal,...) or a .log.\n"
     "1. Add rows with files.append {project, name, text}: the device writes only\n"
     "   the new rows. NEVER files.read a whole log and artifact.save it back -\n"
     "   that rewrites the whole file.\n"
     "2. To see the columns or the latest rows, read only the end:\n"
     "   files.read {project, name, tail: 5} (a CSV's header row comes with it).\n"
     "3. A new log: the first files.append carries the header row, then the row.\n"
     "4. A dated CSV (the food log, a daily ledger) is KEYED: a row is its date plus\n"
     "   its key column, so it needs a date column (a keyed write to a log without\n"
     "   one is refused). Write with files.append {project, name, text, key, sum}:\n"
     "   text = the stored header row, then the rows; key = the column that tells one\n"
     "   day's rows apart (meal in the food log); sum = the figure to total (kcal).\n"
     "   The same date and key again REPLACES that row only when it describes the\n"
     "   SAME item - the device decides by the items column, not your wording; a\n"
     "   different item under the same key is appended under a new unique key\n"
     "   (dinner-2), the stored row kept. A new key adds a row; other days are never\n"
     "   touched. Every replace keeps the replaced row as a #replaced: history row\n"
     "   and names it in the reply.\n"
     "5. Dates are the owner's local date as YYYY-MM-DD, from the Current date line.\n"
     "   What was eaten before midnight belongs to that day, even when logged later.\n"
     "6. The food log is ONE file: the ledger [TODAY'S FOOD LEDGER] names (the\n"
     "   owner's is food/food-log.csv), columns date,meal,items,kcal,protein_g.\n"
     "   Never split a day's meals across\n"
     "   files. Each eating occasion gets its own meal name for that day (breakfast,\n"
     "   lunch, dinner, else snack plus the time); a DIFFERENT item under\n"
     "   a meal name already logged is a separate occasion - its own meal name, never\n"
     "   the first row re-sent with the new item's figures. Separate items with +\n"
     "   (a field with a comma goes in double quotes).\n"
     "7. A correction (the owner fixes a figure, or a meal sits on the wrong day):\n"
     "   re-send the row with the same date and meal and the corrected figures; it\n"
     "   replaces the old row, so nothing counts twice. The files.append reply says\n"
     "   what it replaced - quote that in your reply as a replace, never as a plain\n"
     "   \"logged\". To take a row off a day, re-send it with 0 in the figures and the\n"
     "   reason in items (moved to 2026-10-25). NEVER add a second row for the same meal to correct it.\n"
     "8. Before estimating anything logged before, read the log's tail. Totals\n"
     "   come from the files tool, never from memory or the conversation; if the\n"
     "   owner's stated total and the file's disagree, say both.\n"
     "9. Never say a figure was logged, updated, corrected or removed without the\n"
     "   files.append reply that shows it (replaced or added). A replace must be said\n"
     "   as a replace in your reply (\"replaced dinner: brisket 457 kcal with the\n"
     "   mango chicken 445 kcal\") - a reply that hides a replace behind \"logged\" is\n"
     "   corrected by the device. A refused files.append wrote nothing: fix what it\n"
     "   names and send again. If it says the log already holds 2\n"
     "   or more rows for that date and meal: one meal logged more than once, send\n"
     "   the same call with replace: \"all\" (the others are kept as #superseded:\n"
     "   rows, not counted); separate items, log the new one under its own meal name.\n"
     "10. A meal photo: estimate it, then one files.append to the food log, then\n"
     "   reply with the row you wrote. Several rows for one meal go in ONE call.\n"
     "11. If a reply says the stored last row was cut mid-write, log that row again\n"
     "   in full. 12. A log with no header row: fix it with files.edit.\n"},
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
      "Build a PDF the way the TO DELIVER A PDF line says (an openai or anthropic sub, or the "
      "device's own writer) and deliver it - read BEFORE promising, refusing, or downgrading a "
      "PDF/document request."},
     NIMBUS_PARTS(kDeliverPdf)},
    {{"log-rows", "Log a row to a CSV or log file (food log, ledger)",
      "Add rows to a log with files.append and read its end with files.read tail - read "
      "BEFORE logging or correcting a meal, a weight, a ledger total or any tracker row."},
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
