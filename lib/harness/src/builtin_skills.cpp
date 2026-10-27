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
     "   the new rows. NEVER files.read a whole log and artifact.save it back to add\n"
     "   or fix a row - that rewrites the whole file, and every byte rides your turn.\n"
     "2. You do not need to read the log first when you know its columns. To see\n"
     "   the columns or the latest rows, read only the end:\n"
     "   files.read {project, name, tail: 5} (a CSV's header row comes with it).\n"
     "3. A new log: the first files.append carries the header row, then the row.\n"
     "4. A dated CSV (the food log, a daily ledger) is KEYED: a row is its date plus\n"
     "   its key column, so it needs a date column (date, YYYY-MM-DD; a keyed write\n"
     "   to a log without one is refused). Write it with\n"
     "   files.append {project, name, text, key, sum}:\n"
     "   text = the stored header row, then the rows; key = the column that tells\n"
     "   one day's rows apart (meal in the food log; date in a ledger that keeps one\n"
     "   row per day); sum = the figure to total (kcal). The same date and key again\n"
     "   REPLACES that row; a new key adds a row; other days are never touched.\n"
     "5. Dates are the owner's local date as YYYY-MM-DD, from the Current date line.\n"
     "   What was eaten before midnight belongs to that day, even when logged later.\n"
     "6. The food log is ONE file: the ledger [TODAY'S FOOD LEDGER] names (the\n"
     "   owner's is food/food-log.csv), columns date,meal,items,kcal,protein_g; the\n"
     "   food-log playbook has the food rules. Never split a day's meals across\n"
     "   files. Each eating occasion gets its own meal\n"
     "   name for that day: breakfast, lunch, dinner, and for anything else snack\n"
     "   plus the time eaten (snack-1126). An item added to a meal already logged:\n"
     "   re-send that meal's row with the combined figures. Separate items with +\n"
     "   (a field that holds a comma goes in double quotes).\n"
     "7. A correction (the owner fixes a figure, or a meal sits on the wrong day):\n"
     "   re-send the row with the same date and meal and the corrected figures; it\n"
     "   replaces the old row, so nothing counts twice. To take a row off a day,\n"
     "   re-send it with 0 in the figures and the reason in items (moved to\n"
     "   2026-10-25). NEVER add a second row for the same meal to correct it.\n"
     "8. Before estimating anything you logged before (an earlier meal, a figure\n"
     "   the owner corrects, a day total), read the log's tail first. Totals come\n"
     "   from the files tool, never from memory or the conversation: a keyed\n"
     "   files.append reply states each day's total (today's kcal (2026-10-25,\n"
     "   GMT): 976 from 2 rows), and files.read {project, name, tail: 5, sum:\n"
     "   \"kcal\", date: \"2026-10-24\"} gives any day's. If the owner's stated total\n"
     "   and the file's disagree, say both.\n"
     "9. Never say a figure was logged, updated or corrected without the\n"
     "   files.append reply that shows it (replaced or added). A refused\n"
     "   files.append wrote nothing: fix what it names and send again, or tell\n"
     "   the owner. If it says the log already holds 2 or more rows for that date\n"
     "   and meal: when they are one meal logged more than once (an older log's\n"
     "   corrections), send the same call with replace: \"all\" (the others are kept\n"
     "   as #superseded: rows, not counted); when they are separate items, log the\n"
     "   new one under its own meal name.\n"
     "10. A meal photo: estimate it, then one files.append to the food log (and one\n"
     "   to the ledger if you keep one), then reply with the row you wrote. Several\n"
     "   rows for one meal go in ONE call, one row per line.\n"
     "11. If a reply says the stored last row was cut mid-write, say so and log\n"
     "   that row again in full.\n"},
};

// The food playbooks (lane CTX). On 26 Oct the head never opened a playbook, logged
// meals to long-term memory, guessed three file names and rebuilt the day's total
// from history five different ways in an hour. The device now shows today's ledger
// in every turn that has one ([TODAY'S FOOD LEDGER], food_ledger.h) and attaches a
// playbook to every food turn without a skill read (day_context.cpp): food-log when
// the owner reports food, food-coach when they ask, both when the words do not say.
// The coach carries the food-coach eval's findings (tests/evals/food_coach): a plan
// built with oil, rice and potatoes the owner never named, and a number asked for
// three times.
const Part kFoodLog[] = {
    {Side::Both,
     "# Food log: logging a meal in today's ledger\n"
     "[TODAY'S FOOD LEDGER] in this turn's context is the device's record of today's\n"
     "meals: its rows and totals are the truth for today.\n"
     "1. A meal is logged only when the owner says it was eaten (I had, I ate,\n"
     "   breakfast was, a photo of their plate). A question, a request for a plan or\n"
     "   for amounts (how much, what should I eat, plan my dinner) and a complaint\n"
     "   about a plan are not meals: no files.append, and no word about logging.\n"
     "2. Meals go ONLY to the ledger file it names, never to mem_write, memory.write,\n"
     "   the scratchpad or standing rules. Long-term memory holds lasting facts\n"
     "   (targets, preferences, calibrations), not what was eaten today.\n"
     "3. Write the row as soon as the meal has an estimate (the owner's description,\n"
     "   a photo you estimated, a figure they correct): one files.append\n"
     "   {project, name, text, key: \"meal\", sum: \"kcal\"}; text = the ledger's header\n"
     "   row, then one row per meal, dated with the ledger's date; items joined with +\n"
     "   (a field never holds a bare comma). Then give the row you wrote (meal, kcal,\n"
     "   protein) and today's total as the tool states it.\n"
     "4. One row per eating occasion: breakfast, lunch, dinner; anything else snack\n"
     "   plus the time eaten (snack-1530). An item added to a meal: re-send that meal's\n"
     "   row with the combined items and figures (it replaces the row). A correction:\n"
     "   the same date and meal with the corrected figures. To take a row off: 0 kcal\n"
     "   and the reason in items. Never a second row for one meal. A meal from another\n"
     "   day goes under that day's date only when the owner names the day.\n"
     "5. Totals come from [TODAY'S FOOD LEDGER] or the files.append reply. Never add up\n"
     "   a day from the conversation, a summary, memory or a standing rule. Never say\n"
     "   a meal was logged without the files.append reply that shows it.\n"
     "6. Estimates are estimates: give the figure with its main assumption. When an\n"
     "   amount is missing, log your best estimate and ask one short question; replace\n"
     "   the row when the owner answers.\n"
     "7. A photo nobody could read: say so in the reply, with the reason given. When\n"
     "   the owner's words describe the meal, estimate from them and say the estimate\n"
     "   comes from their description; otherwise ask for a short description and log\n"
     "   nothing yet. Never describe what only the photo could show.\n"},
};

// The food coach (lane CTX, the food-coach eval's findings): attached to a food turn
// that asks for an answer or a plan. It carries no logging procedure: a Mistral head
// given the logging steps on such a turn logged the requested plan as a meal eaten.
const Part kFoodCoach[] = {
    {Side::Both,
     "# Food coach: answers and plans\n"
     "1. Totals come from [TODAY'S FOOD LEDGER] in this turn's context, never from the\n"
     "   conversation, a summary, memory or a standing rule. Asked for the list by\n"
     "   meal, list the ledger's rows with their kcal and its total.\n"
     "2. Only meals the owner says they ate go in the ledger (with files.append, never\n"
     "   memory). A question, a plan or an amount asked for is not a meal: write no\n"
     "   row and say nothing about logging.\n"
     "3. Answer what was asked, first: the reply's first sentence gives the number\n"
     "   asked for (grams, slices, kcal, yes or no), not a heading, a list, an apology\n"
     "   or an agreement (the one exception is the [THIS MESSAGE] line's own). Then\n"
     "   each food's amount with its kcal, and the total. Take the owner's goal as\n"
     "   stated (\"max out\" means use up what is left); never argue with it or open\n"
     "   with the day's totals.\n"
     "4. When the owner has named no food at all in this chat, ask one short question\n"
     "   about what they have: no plan, no amounts and no food.portions call yet.\n"
     "5. Amounts and plans come from food.portions, never from your own arithmetic:\n"
     "   left_kcal = what is left (the owner's figure, or their target minus today's\n"
     "   ledger total), left_protein likewise, and one entry per food the owner says\n"
     "   they have, in this chat (the ledger lists what was eaten, not what is in the\n"
     "   kitchen), the ones the owner fixed included: its kcal and protein per 100 g,\n"
     "   fixed_g for every amount the owner fixed, otherwise amount_g (a sensible\n"
     "   portion; the tool keeps the proportions and scales them), and unit_g for\n"
     "   slices or pieces. When the owner\n"
     "   fixes some amounts and leaves the rest to you (\"assume 100 g beef, you tell me\n"
     "   the rest\"), the fixed foods still go in the call as fixed_g: the kcal left\n"
     "   covers them too. Answer with exactly the amounts it returns, one line per\n"
     "   food with its kcal, and its Total line; add nothing on top. Nothing the owner\n"
     "   did not name (cooking oil, rice, potatoes, a side) goes in; at most one\n"
     "   optional extra may be offered after the total.\n"},
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
    {{"food-log", "Today's food ledger: logging and correcting meals",
      "How a meal eaten is logged and corrected in today's food ledger - attached "
      "automatically when the owner reports food."},
     NIMBUS_PARTS(kFoodLog)},
    {{"food-coach", "Food answers and plans from today's ledger",
      "Answer-first amounts and plans from the foods the owner has - attached "
      "automatically when the owner asks about food."},
     NIMBUS_PARTS(kFoodCoach)},
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
