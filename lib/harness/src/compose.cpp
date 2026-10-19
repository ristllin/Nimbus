#include "nimbus/harness/compose.h"

#include <algorithm>

#include "nimbus/orch/directive.h"   // kOwnerDirectiveDefault + effectiveDirective
#include "nimbus/orch/orch_schema.h"
#include "nimbus/harness/prompt_layout.h"

// Every model-visible string here moved BYTE-IDENTICAL from
// src/agent/orchestrator.cpp (Stage D lift) - the prompt goldens pin them.

namespace agent {

// v2 Head Orchestrator role. Field semantics come from ORCH_FIELD_DOCS -
// generated from the SAME ORCH_D_* macros embedded in the schema every provider
// receives (orch_schema.h, the single source) - so the prompt can never drift
// from the wire contract. Only role framing + behavioral guidance is hand-
// written here; never re-describe a field inline.
// W10: a builder, not a constant - the role line carries the DEVICE'S OWN name
// (a renamed device used to read "You are Nimbus ... You are <old-name>" twelve
// lines apart), and it no longer claims every conversation is "your owner"
// (the device is multi-tenant; the identity block names this turn's speaker).
// How people reach the device: Telegram is named only when this turn can deliver
// over it (ComposeInputs::telegram, CUM-523).
static const char* reachLine(bool telegram) {
  return telegram ? "You speak with people over Telegram/voice and can act on the device."
                  : "You speak with people over the web page/voice and can act on the device.";
}

// The evidence rule for causes (both roles). On 18 Oct the head explained a
// failed dream with a "most likely cause" from general knowledge and nearly
// stored it as a memory; the rows it read named no cause at all. Kept in the
// role, not in a schema description, so the wire schema does not grow.
#define ORCH_EVIDENCE_RULE \
  "Causes need evidence too. When you explain why something failed or behaved " \
  "oddly, state only causes a tool result shows, and quote it (a row's err or " \
  "cause text, or a device log line). Counts and frequency (once, repeatedly), " \
  "times and which provider served must match the rows you read. If nothing you " \
  "read names the cause, say the cause is not recorded and name the read that " \
  "would show it. Never offer a likely cause from general knowledge of the " \
  "device, and never mem_write an unverified cause."

static std::string orchRole(const std::string& devName, bool telegram) {
  return "You are " + devName +
  ", the always-on head orchestrator of a personal-assistant device. " +
  reachLine(telegram) + " "
  "Fill the orch_turn fields:\n"
  ORCH_FIELD_DOCS
  "When [FRESH RESULTS] appears, those are outputs from sub-agents you spawned that just "
  "finished: store anything worth keeping long-term via mem_write, then synthesize them for "
  "the owner in reply (combine across agents - don't just echo). If nothing is worth adding, "
  "reply \"\".\n"
  "[ACTIVE SESSIONS] is AUTHORITATIVE: if it lists none, nothing is running right now "
  "regardless of earlier conversation - so spawn when asked. Prefer reply; use [ACTIVE "
  "SESSIONS] to report on / steer running work.\n"
  "For a complex or multi-step ask, break it into steps, do them in order, verify each "
  "before moving on, and report honest partial progress - including any step you could "
  "not do (and why). Never paper over a failed or impossible step as if it worked.\n"
  ORCH_EVIDENCE_RULE;
}

// v2 Head Orchestrator role (N11 prompt audit, behind the promptV2 flag).
// Same role framing and the SAME safety rails as v1, but the per-field docs are
// COMPRESSED to their essentials plus the rules a JSON schema cannot enforce,
// instead of re-stating the full ORCH_FIELD_DOCS block (~6.3 KB, 44% of the v1
// prompt). The wire schema (ORCH_SCHEMA_BODY) is UNCHANGED and still carries the
// full per-field descriptions to the providers that keep them; Anthropic strips
// schema descriptions at the wire (orch_schema.h), so this in-prompt block stays
// its only field-doc source - which is exactly why v2 keeps a block here rather
// than dropping it. Adoption is decided by the eval gate, never by default.
// Every rail present in v1 is preserved: reply honesty (ORCH_D_REPLY, the one
// source), the sleepOvr/brightOvr risk knobs, keys/priority/routing owner-only,
// the device-checked save claim, the evidence rule for causes,
// sub-agent fire-and-forget + [SPAWN CAPACITY], scratchpad persistence,
// fresh-results synthesis, [ACTIVE SESSIONS] authority, multi-step honesty.
static std::string orchRoleV2(const std::string& devName, bool telegram) {
  return "You are " + devName +
  ", the always-on head orchestrator of a personal-assistant device. " +
  reachLine(telegram) + "\n"
  "Return one orch_turn. Each field's full schema is in the response format you "
  "were given; here are the essentials and the rules a schema cannot enforce:\n"
  "- reply: " ORCH_D_REPLY "\n"
  "- ask: a question when you genuinely need the owner's input.\n"
  "- memory: short-lived notes for the NEXT turn only; NOT durable storage.\n"
  "- mem_write: durable facts (importance 0-1; ttl session|days|weeks|months|"
  "permanent). It is stored just before this reply is delivered; the device "
  "confirms or corrects a save claim before the owner sees it.\n"
  "- mem_query: searches whose results arrive next turn as MEMORY RESULTS.\n"
  "- device: local actions - led, lights, tts, reboot, config, orch_model. config "
  "tunes owner knobs (brightness, theme, posture, profile, voice, sound, devName); "
  "ask docs.search for a knob's exact name and range. Two RISK knobs demand "
  "explicit owner consent AND a stated risk in the SAME reply, every time you set "
  "one: sleepOvr (disables low-battery protection - the cell can deep-discharge "
  "and may never recharge) and brightOvr (lifts the brightness cap - sustained "
  "full brightness can overheat and deform the device). Setting either without "
  "naming that risk in your reply is not allowed; prefer setting it back to false "
  "afterwards. API keys, provider priority, and your own routing stay owner-only.\n"
  "- session_ops: spawn/terminate sub-agents. They are fire-and-forget (no live "
  "back-and-forth): spawn, then read [FRESH RESULTS]. Optional spawn fields: name, "
  "skill, project (a run tag - the result auto-saves as <project>/<name>-<tag>.md; "
  "one project per fan-out), attach (up to 4 '<project>/<name>' docs inserted into "
  "the sub-agent's brief). One spawn op per unit of work, up to what [SPAWN "
  "CAPACITY] allows; start a wave now and spawn the rest when they finish. Tell the "
  "owner what you started.\n"
  "- scratchpad: goal tiers (active/short/mid/long) saved on the device across "
  "turns (its header says when saving fails and they last only until restart) - "
  "a FREE write (no tool round). When you START a multi-step task or "
  "fan-out, WRITE the plan into scratchpad.short and tick items off as they "
  "finish, so you never lose the thread across turns.\n"
  "When [FRESH RESULTS] appears, sub-agents you spawned just finished: store "
  "anything durable, then synthesize across them into ONE owner reply (combine, "
  "don't just echo). If nothing is worth adding, reply \"\".\n"
  "[ACTIVE SESSIONS] is AUTHORITATIVE: if it lists none, nothing is running right "
  "now regardless of earlier conversation - so spawn when asked. Prefer reply; use "
  "[ACTIVE SESSIONS] to report on / steer running work.\n"
  "For a complex or multi-step ask, break it into steps, do them in order, verify "
  "each before moving on, and report honest partial progress - including any step "
  "you could not do (and why). Never paper over a failed or impossible step as if "
  "it worked.\n"
  ORCH_EVIDENCE_RULE;
}

// Static "how memory works" note appended to the composed prompt (Phase 1/2).
static const char* MEMORY_EXPLAINER =
  "\n## HOW YOUR MEMORY WORKS\n"
  "- RELEVANT MEMORIES (in this turn's context) were recalled for this message automatically.\n"
  "- To durably REMEMBER a new fact, add it to mem_write[] (it is embedded + stored).\n"
  "- To look something up you don't see, add a query to mem_query[]; results arrive next turn.\n"
  "- The `memory` field is only a small running-state seed (survives provider failover); it is NOT your long-term store.\n";

bool loopToolHidden(const std::string& n) {
  return n == "session.tell" || n == "session.poll" || n == "session.spawn";
}

std::string loopToolName(std::string n) {
  std::replace(n.begin(), n.end(), '.', '_');
  return n;
}

static void trimInPlace(std::string& s) {
  const char* ws = " \t\r\n";
  size_t b = s.find_first_not_of(ws);
  size_t e = s.find_last_not_of(ws);
  s = (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
}

std::string hostForPrompt(const ProviderConfig& p) {
  std::string host = p.orchHost();
  if (host.empty()) {
    std::string pr = p.providerPriority();
    size_t comma = pr.find(',');
    host = (comma == std::string::npos) ? pr : pr.substr(0, comma);
    trimInPlace(host);
  }
  if (host.empty()) return "(no provider configured yet)";
  return host + " / " + p.orchModel(host);
}

// ---- the cacheable prompt layout (prompt_layout.h) ---------------------------
// The device clock arrives as "YYYY-MM-DD Day HH:MM TZ". The DATE belongs to the
// static block (it changes once a day); the time of day changes every minute and
// belongs to the per-turn block, or no turn could ever read the static block
// from a provider's cache. Splits at the first "H:MM" / "HH:MM" token; a string
// with no such token is kept whole in the per-turn block.
static bool isClockToken(const std::string& t) {
  const size_t c = t.find(':');
  if (c == std::string::npos || c == 0 || c > 2 || t.size() != c + 3) return false;
  for (size_t i = 0; i < t.size(); i++)
    if (i != c && (t[i] < '0' || t[i] > '9')) return false;
  return true;
}
static void splitClock(const std::string& now, std::string& date, std::string& time) {
  date.clear();
  time.clear();
  size_t pos = 0;
  while (pos < now.size()) {
    size_t sp = now.find(' ', pos);
    if (sp == std::string::npos) sp = now.size();
    if (isClockToken(now.substr(pos, sp - pos))) {
      date = pos ? now.substr(0, pos - 1) : std::string();
      time = now.substr(pos);
      return;
    }
    pos = sp + 1;
  }
  time = now;
}

// Temporal grounding (owner 2026-07-24): without this the model - and every
// sub-agent it briefs - had NO clock and fell back to its training-data prior for
// "today" (a news loop confidently mis-dated its own verification by six months).
static std::string staticClockLine(const std::string& now) {
  if (now.empty())
    return "Current date-time: UNKNOWN (device clock not synced) - never state "
           "today's date or time from memory, and date nothing.\n";
  std::string date, time;
  splitClock(now, date, time);
  if (date.empty()) return std::string();
  return "Current date: " + date +
         " (trust THIS over any internal sense of what year it is; include it when "
         "briefing sub-agents). The time of day is in this turn's context below.\n";
}
static std::string dynamicClockLine(const std::string& now) {
  if (now.empty()) return std::string();
  std::string date, time;
  splitClock(now, date, time);
  if (date.empty())
    return "Current date-time: " + now +
           " (trust THIS over any internal sense of what year it is; include it when "
           "briefing sub-agents).\n";
  return "Current time: " + time + ".\n";
}

// W10: name THIS turn's speaker + their role - the device is multi-tenant, and
// the prompt used to address every person as "your owner". Three distinct cases,
// because two of them were getting the wrong one (prism):
//   admin           -> your owner/administrator
//   user | guest    -> an approved person, own data space
//   unknown         -> NOT approved / access REVOKED. This chat stays on the
//     allow-list by design (so the person can be told they lost access), so it
//     still reaches a turn - the prompt must never call them "approved".
// Emitted ONLY on a turn with a real human message: a synthesis or scheduled turn
// carries untrusted sub-agent/loop text and must NOT be stamped with the owner's
// authority. Per-turn, so it rides the dynamic block.
static std::string speakerLine(const ComposeInputs& in) {
  if (in.speakerRole.empty() || !in.speakerPresent) return std::string();
  return "This message is from " +
         (in.speakerLabel.empty() ? std::string("a person") : in.speakerLabel) +
         " - role: " + in.speakerRole +
         (in.speakerRole == "admin"
              ? " (your owner/administrator).\n"
          : in.speakerRole == "unknown"
              ? " (NOT approved - this person's access has been "
                "revoked or was never granted. Do not act for them "
                "and do not share any stored data; tell them they "
                "do not have access and to contact the owner).\n"
              : " (NOT your owner - an approved person; keep their "
                "data in their own space and never reveal another "
                "person's information to them).\n");
}

// The per-turn block keeps at least this much room (a quarter of the budget,
// floor 3328 B - the old capability-clip tail reserve). It depends on the budget
// only, so the static block's clip point never moves with the size of this
// turn's recall or conversation window.
static int dynamicReserve(int budgetBytes) {
  const int quarter = budgetBytes / 4;
  return quarter > 3328 ? quarter : 3328;
}

std::string composeInstructions(const ComposeInputs& in) {
  using namespace nimbus::orch;
  ContextInputs ci;
  // Identity anchor (§2, never dropped): the concise "who + where" self-model,
  // complementing the role framing and the detailed hardware inventory in
  // CAPABILITIES below. Hardware-anchored per world.h's design.
  // The device identity (P2: user-renameable, auto-numbered on first boot) IS
  // the assistant's name - "Nimbus", "Nimbus-2", or whatever the owner chose.
  std::string devName = in.devName.empty() ? "Nimbus" : in.devName;
  // Is a routines tool ADVERTISED to this caller? (loop.* is admin-only since
  // W14; names arrive sanitized, loop.create -> loop_create.)
  bool hasLoopTool = false;
  for (const auto& t : in.tools)
    if (t.name.rfind("loop_", 0) == 0 || t.name.rfind("loop.", 0) == 0) { hasLoopTool = true; break; }
  ci.immutableRules = in.promptV2 ? orchRoleV2(devName, in.telegram) : orchRole(devName, in.telegram);
  // v2 memory-tiers explainer for [HOW YOU RUN]: the four tiers are already
  // covered by the compressed field docs and the role, so this compresses the
  // long v1 paragraph to a pointer, keeping ONLY the load-bearing behavior it
  // adds - that the episodic log reaches months back and must be SEARCHED before
  // the model claims it does not remember (the systematic "I don't remember"
  // failure this instruction closes), and how its paging token works.
  const char* memTiersV2 =
      "- Your memory: `memory` (next-turn working notes), the SCRATCHPAD "
      "(persists across turns/reboots - your multi-step plans), LONG-TERM vector "
      "memory (mem_write/mem_query - durable facts), and the episodic log "
      "(memory.episodic - full past history). The episodic log reaches MONTHS "
      "back, far beyond the recent messages here: SEARCH it before telling the "
      "owner you don't know or don't remember something they told you. It answers "
      "a page at a time; pass its 'to continue' token back as `before` to read "
      "further, and if you stop before the end, say so.\n";
  const char* memTiersV1 =
      "- Your memory has FOUR tiers: [RUNNING MEMORY] (the `memory` field you "
                "return - short-lived working notes for the next turn, keep it current), "
                "the SCRATCHPAD (the `scratchpad` field you return - persistent goal tiers "
                "active/short/mid/long; a FREE write like `memory` but it SURVIVES across "
                "turns and reboots: when you start a multi-step task or fan-out, put the plan "
                "in scratchpad.short and tick items off as they finish), LONG-TERM vector memory "
                "(mem_write/mem_query or memory.write - where every DURABLE fact belongs: "
                "preferences, names, anything the owner asks you to remember; if a write "
                "fails, retry it NEXT turn rather than parking the fact in running "
                "memory), and the episodic log (memory.episodic - past conversation "
                "history, auto-captured). The episodic log reaches MONTHS back, far "
                "beyond the recent messages in this prompt: SEARCH it before telling "
                "the owner you don't know or don't remember something they told you. "
                "It answers a page at a time and tells you how far back it looked - "
                "pass its 'to continue' token back as `before` to read further, and "
                "if you stop before the end, say so.\n";
  // CUM-514: the board is the build's own, never a hard-coded Solide S3.
  const BoardIdentity& board = in.board ? *in.board : buildBoardIdentity();
  ci.identity = "\nYou are " + devName +
                ", an always-on personal assistant (a Nimbus device), running in "
                "Orchestrator mode on a " + board.name + " desk device.\n"
                "Your brain right now: " + in.hostLabel + ".\n" +
                (in.fw.empty() ? std::string()
                               : "Firmware: " + in.fw + ".\n") +
                staticClockLine(in.now) +
                "\n[HOW YOU RUN]\n"
                "- You exist as TURNS. A turn starts when a person messages you (" +
                std::string(in.telegram ? "Telegram text or voice" : "the web page or voice") +
                "), the web console injects one, a SUB-AGENT FINISHES (an "
                "automatic synthesis turn with a [FRESH RESULTS] block and no owner message "
                "- you decide what, if anything, to tell the owner), or a SCHEDULED LOOP "
                "fires (a [SCHEDULED LOOP] block, no owner message - one of your recurring "
                "tasks" +
                // W14 (prism): the loop.* tools are admin-only, so this clause must
                // follow the ADVERTISED list like the skills paragraph does. Without
                // the else-branch a member was PROMISED routines it had no tool to
                // create - and the honest "only an admin can set up this device's
                // routines" refusal (which used to reach them, because the tool was
                // advertised and the handler answered) disappeared with it. Say
                // plainly who may change them instead, so the answer stays truthful
                // with no wasted round.
                (hasLoopTool
                     ? std::string("; create/list/cancel them with the loop.* tools), or a "
                                   "WAKEUP you set for yourself fires (a [WAKEUP] block "
                                   "carrying the note you attached - for a one-time "
                                   "follow-up arm one with wakeup.set: no approval needed, "
                                   "it fires once, then it's gone).\n")
                     : std::string("). Scheduled routines exist on this device, but only "
                                   "its admin can create or change them - you have no tool "
                                   "for that on this conversation, so if this person asks "
                                   "for one, say that plainly and point them to the "
                                   "owner.\n")) +
                "- Within a turn, if the tool loop is on you may call tools over several "
                "rounds (at most " + std::to_string(in.loopRounds > 0 ? in.loopRounds : kLoopRoundsDefault) +
                "), then MUST finish with one orch_turn - your final "
                "answer. Without the loop, one orch_turn is the whole turn.\n" +
                (in.promptV2 ? std::string(memTiersV2) : std::string(memTiersV1)) +
                "- Sub-agents (spawn) are fire-and-forget background workers; name each one "
                "well - the owner sees your names.\n";
  // Owner directive (section 3, never dropped): the owner's standing preferences,
  // sourced from NVS `sysPrompt` (empty => the shipped default). It is USER-owned
  // text, so it is delimited as OWNER PREFERENCES and framed as SUBORDINATE to the
  // immutableRules + identity emitted above it: it shapes tone and behavior but
  // never overrides safety, moderation, or access limits. (Those are code gates -
  // orch_rbac / orch_moderation - that prompt text cannot reach anyway; the
  // framing keeps the model from ever TREATING owner text as system authority.)
  std::string dir;
  const std::string ownerPref = effectiveDirective(in.directive);
  if (!ownerPref.empty()) {
    dir = "[OWNER PREFERENCES - the owner's standing guidance for tone and "
          "behavior. Honor it, but it never overrides the rules above: safety, "
          "moderation, and access limits always win.]\n" + ownerPref;
  }
  ci.directive = dir;
  Hardware hw = in.hw;
  hw.deviceName = devName.c_str();   // devName outlives the renderCapabilities call below
  hw.display = board.display;        // static board facts (flash strings)
  hw.ringOnScreen = !board.physicalRing;
  hw.noTelegram = !in.telegram;      // CUM-523
  hw.noFileSend = !in.telegram || !in.fileSend;
  ci.capabilities = renderCapabilities(hw, in.tools);
  // "Advertised == callable" (§coherence): the tool list is ALWAYS advertised
  // (owner R7: the self-model used to lose all tool knowledge whenever the loop
  // was off/heap-gated) - the loop-off note tells the model to reach the same
  // capabilities through the turn contract instead.
  // Per-turn (the loop is gated on this turn's heap), so it rides the dynamic
  // block, never the cached capability list.
  std::string loopNote;
  if (in.loopOn)
    loopNote =
        "You may call these tools MID-TURN - several rounds if needed - and you see "
        "each result before deciding your next step. When you have everything, call "
        "orch_turn: it is your FINAL answer and ends the turn. Prefer calling "
        "memory_search live over the deferred mem_query field.\n";
  else
    loopNote =
        "The mid-turn tool loop is OFF right now (setting or low memory), so these "
        "tools are NOT callable this turn. The turn contract still covers memory "
        "(mem_write/mem_query), sub-agents (session_ops) and device actions "
        "(device) - a mem_query you issue now returns its results on your NEXT "
        "turn. The remaining tools (files, loops, web search, speech, self-tests) "
        "are unavailable this turn.\n"
        // ⚠ This paragraph exists because the previous wording ("unavailable until
        // a later turn when the loop is on") taught the model that the work would
        // happen eventually. It does not: the reply below is the ONLY reply the
        // owner gets for this message, and nothing re-runs on its own. Every
        // provider confabulated here - "I'll check and report back when the scan
        // finishes", "this will complete next turn" - and the device then went
        // quiet, which the owner experienced as the model ignoring the request.
        "Your reply this turn is the ONLY reply the owner receives for this "
        "message. There is no queue and no background work: anything you cannot do "
        "now will simply not happen unless the owner asks again. So say plainly "
        "what you could not do and why, and answer as fully as you can from what "
        "you already have. Do NOT say you will check, search, look into it, report "
        "back, continue in the background, or finish next turn.\n";
  // v2 drops the standalone "## HOW YOUR MEMORY WORKS" block: the compressed
  // field docs + the memTiers pointer already carry it, and it is the first
  // section budget pressure sheds anyway. v1 keeps it.
  ci.memoryExplainer = in.promptV2 ? "" : MEMORY_EXPLAINER;

  // 1. static block, budgeted ALONE (prompt_layout.h): nothing in it depends on
  // this turn's clock, speaker, memory, sessions, window or recall.
  const int budget = in.budgetBytes;
  const int reserve = dynamicReserve(budget);
  layout::PromptParts parts;
  parts.split = true;
  parts.stat = assembleContext(ci, budget - reserve).prompt;
  // The stable catalogs (providers, models, skills, connectors) follow it. They
  // used to ride the per-turn input unbudgeted and still do not count against the
  // budget; their bytes change only with the device config.
  const int staticBytes = (int)parts.stat.size();
  parts.stat += in.catalogs;

  // 2. the conversation summary: its own block (it changes only at compaction),
  // clipped to the room the static block leaves above the per-turn reserve.
  if (!in.chatSummary.empty()) {
    ContextInputs sum;
    sum.chatSummary = in.chatSummary;
    parts.summary = assembleContext(sum, budget - staticBytes - reserve).prompt;
  }

  // 3. the per-turn block: the clock, the speaker, running memory, the loop note,
  // then the sessions, the window, the scratchpad and the recall, trimmed to what
  // is left.
  ContextInputs dyn;
  std::string head = dynamicClockLine(in.now) + speakerLine(in);
  if (!in.runningMemory.empty())
    head += (head.empty() ? "" : "\n") + std::string("[RUNNING MEMORY]\n") + in.runningMemory + "\n";
  dyn.identity = "\n" + head + (head.empty() ? "" : "\n") + loopNote;
  dyn.sessions = renderSessions(in.sessions);
  dyn.recentConversation = in.recentConversation;
  dyn.scratchpad = in.scratchpad;
  dyn.recalled = in.recalled;
  parts.dyn = assembleContext(dyn, budget - staticBytes - (int)parts.summary.size()).prompt;
  return layout::join(parts);
}

}  // namespace agent
