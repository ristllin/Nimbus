#include "nimbus/orch/world.h"

#include <algorithm>
#include <cstring>

#include "nimbus/mem_cap.h"

namespace nimbus {
namespace orch {

namespace {

// Tool-manifest ranking. A real device registers far more tool description than
// the system-prompt budget can hold, so the manifest must degrade - and WHICH
// tools degrade decides whether the model can still plan a turn. Rank 0 is the
// set a turn is actually built out of: fan out, save, deliver. Ranks rise toward
// the administrative surface, which a turn can discover through docs.search.
//
// Names arrive here in the provider-sanitized form ('.' -> '_', see
// loopToolName), so match on that and normalize defensively.
int toolRank(const std::string& rawName) {
  std::string n = rawName;
  std::replace(n.begin(), n.end(), '.', '_');
  // The verbs a multi-step turn cannot be planned without.
  if (n == "session_spawn" || n == "artifact_save" || n == "files_send") return 0;
  // Reading back what was produced, and the device's own documentation.
  if (n == "files_list" || n == "files_read" || n == "files_search" ||
      n == "results_get" || n == "results_list" ||
      n == "docs_search" || n == "docs_read" || n == "docs_list") return 1;
  // Core memory + knowing what is running / what state the device is in.
  if (n == "memory_write" || n == "memory_search" || n == "memory_scratchpad" ||
      n == "session_list" || n == "session_terminate" || n == "device_status") return 2;
  // Everything else keeps a gloss below this line.
  if (n.rfind("tenant_", 0) == 0) return 6;   // admin-only, rarely part of a turn
  if (n.rfind("skill_", 0) == 0) return 5;
  if (n.rfind("loop_", 0) == 0) return 5;
  return 4;
}

// Three tiers, sized against MEASURED budget headroom, not intuition. On a
// 128K-context head the derived system-prompt budget is 20971 B; with the ~9.9 KB
// undroppable prefix a real board leaves exactly 11071 B for this section (binary-
// searched against assembleContext). A flat full-description render of the live
// 42-tool registry is 12551 B - 1480 B over - which is why the tail was being
// clipped. These caps bring it to ~10 KB with headroom, so nothing clips at all.
constexpr int kToolRankVerb = 0;   // full description: spawn / save / send
constexpr int kToolRankCore = 2;   // capped: read-back, docs, memory, status
constexpr int kToolRankAux  = 4;   // short gloss
// rank > kToolRankAux (skill_*, loop_*, tenant_*) becomes a name-only roster line:
// administrative surface a turn rarely plans around, and docs.search has the detail.
constexpr int kToolCoreBytes = 200;
constexpr int kToolAuxBytes  = 88;

// First clause of a description, UTF-8-safely capped. Descriptions carry em-dashes
// and other multi-byte punctuation, so a raw substr can split a sequence and emit
// a broken glyph into the prompt.
//
// ⚠ prism finding (PoC-confirmed): find_last_of matches BYTES, so passing the
// 3-byte em-dash "-" made it also match stray \xE2/\x80/\x94 CONTINUATION bytes
// inside other multi-byte characters (an ellipsis "…" is \xE2\x80\xA6), and the
// substr then split that character - invalid UTF-8 emitted into the prompt.
// Single-byte separators go through find_last_of; the em-dash is a full-sequence
// rfind. Every candidate cut point now lands on a character boundary: the
// initial utf8CapLen keep, a single-byte '.'/';'/' ', or the first byte of a
// whole em-dash match - so the result is valid UTF-8 by construction. (A
// utf8CapLen re-pass would NOT verify this: len <= maxBytes returns len
// untouched, so it cannot catch a dangling lead byte - don't add one as a
// "guarantee".)
std::string glossOf(const std::string& desc, int maxBytes) {
  int keep = nimbus::utf8CapLen(desc.c_str(), (int)desc.size(), maxBytes);
  if (keep >= (int)desc.size()) return desc;
  // Prefer a clause boundary so the gloss reads as a sentence, not a cut.
  std::string s = desc.substr(0, (size_t)keep);
  size_t cut = s.find_last_of(".;");        // single-byte separators only
  const size_t dash = s.rfind("\xE2\x80\x94");   // "-" as a whole sequence
  if (dash != std::string::npos && (cut == std::string::npos || dash > cut)) cut = dash;
  if (cut != std::string::npos && cut > (size_t)(maxBytes / 3)) {
    s = s.substr(0, cut);
  } else {
    const size_t sp = s.find_last_of(' ');
    if (sp != std::string::npos && sp > (size_t)(maxBytes / 3)) s = s.substr(0, sp);
  }
  s += "...";
  return s;
}

}  // namespace

std::string renderCapabilities(const Hardware& hw, const std::vector<ToolInfo>& tools) {
  std::string out = "\n## CAPABILITIES\n";
  out += "You are running on the ";
  out += hw.deviceName;
  out += " desk device (ESP32-S3). Hardware present: ";
  // (The `hw.version` clause was removed: the identity block already carries the
  // firmware version, and no caller ever populated the field - a dead branch.)
  std::vector<std::string> parts;
  if (hw.ring)     parts.push_back(std::to_string(hw.ledCount) + "-LED ring");
  if (hw.touch)    parts.push_back("2.8\" color touchscreen");
  if (hw.mic)      parts.push_back("microphone");
  if (hw.speaker)  parts.push_back("speaker");
  if (hw.battery)  parts.push_back("battery");
  if (hw.sd)       parts.push_back("SD card");
  for (size_t i = 0; i < parts.size(); i++) {
    if (i) out += ", ";
    out += parts[i];
  }
  out += ".\nConnectivity: ";
  std::vector<const char*> conn;
  if (hw.wifi)     conn.push_back("WiFi");
  if (hw.ble)      conn.push_back("BLE");
  if (hw.telegram) conn.push_back("Telegram");
  if (conn.empty()) {
    out += "offline";
  } else {
    for (size_t i = 0; i < conn.size(); i++) { if (i) out += ", "; out += conn[i]; }
  }
  out += ".\n";

  // What you can ACTUALLY do with each surface - plain language so the model forms a
  // correct self-model instead of inventing capabilities (e.g. arbitrary ring effects,
  // charge sensing, live voice) and failing at the wire. This is the human-readable
  // view of the device-action contract (ORCH_D_DEVICE); keep the two in sync.
  if (hw.ring)
    out += "- The LED ring is a STATUS ring (colour + motion), NOT a display - it cannot "
           "show text or images. Drive it with a `led` action (mode solid/spinner/pulse/"
           "flash + an RGB colour, or mode rainbow for a self-animating hue wheel), turn it "
           "off or back to full with `lights`, or recolour it with `config.theme`. Those are "
           "the only ring effects that exist.\n";
  if (hw.touch)
    out += "- The colour touchscreen is a status screen the firmware owns; you do not render "
           "arbitrary content to it. The owner navigates it by tapping.\n";
  if (hw.files)
    out += "- A durable PRIVATE artifact store lives on the SD card (/mem/files/<project>/"
           "<name>). Save reports/documents there with artifact.save; browse with files.list; "
           "deliver one to the owner with files.send. Files persist across reboots and are "
           "never auto-deleted.\n";
  if (hw.speaker || hw.telegram) {
    if (hw.voiceReplies)
      out += "- YOU choose the reply channel per situation (owner rule): short status or a "
             "heads-up while the owner is nearby -> speak it aloud on the device speaker "
             "(reply.speak, or the tts device action); longer content, lists, code -> "
             "Telegram text; a question you need answered -> ask. For a spoken Telegram "
             "message use reply.telegram with voice true. Never send the SAME content as "
             "both text and audio. There is no live phone call.\n";
    else
      out += "- Reply in TEXT (Telegram / on-screen) by default: spoken replies are switched "
             "OFF, so tts and reply.speak will not play. If the owner asks you to speak or to "
             "turn on voice replies, enable it IN THIS SAME TURN with the device.control tool "
             "({\"action\":{\"type\":\"config\",\"ttsOn\":true}}) and then call reply.speak in "
             "the same turn (owner-request only - never enable voice unasked).\n";
  }
  if (hw.battery)
    out += "- Battery percentage is INFERRED from voltage; there is NO charge-detect hardware, "
           "so you cannot truly know whether it is plugged in or charging - never claim to.\n";
  // Sub-agent completion model (owner-observed self-model gap): make the automatic
  // synthesis turn explicit so the model stops mis-describing how its own jobs work.
  out += "- Sub-agents you spawn run in the BACKGROUND and are fire-and-forget - you cannot "
         "talk to a running one. Individual results are NOT shown to the owner: each is "
         "saved to the device (the results ring + its project doc) as it finishes, and once "
         "they have ALL finished you are AUTOMATICALLY given one turn (a [FRESH RESULTS] "
         "block, with no new owner message) whose reply is the ONE report the owner sees - "
         "make it the complete synthesis, and say in it which sub-agents failed, if any. "
         "You never need the owner to poke you to follow up on a sub-agent.\n";
  // Honesty self-model (harness eval 2026-08 + v4.0.0 skills authoring): the
  // capability description must match the REAL rails, or the model either
  // refuses a capability it has or narrates actions it did not take.
  // W14: this paragraph ASSERTS a capability, so it must follow the same scope
  // as the tool list - a member/guest turn (where skill.save is neither
  // advertised nor callable) was still told "You CAN save reusable skills".
  {
    bool canSaveSkills = false;
    for (const auto& t : tools)
      if (t.name == "skill.save" || t.name == "skill_save") { canSaveSkills = true; break; }
    if (canSaveSkills)
      out += "- You CAN save reusable skills with skill.save (admin conversations only) - but "
             "approval is ASYNCHRONOUS: a skill you save stays INACTIVE until the owner "
             "approves it later, so treat saving as an investment for FUTURE tasks, never a "
             "step of the current one. Only claim a skill was saved when the tool result "
             "confirms it, and always say it awaits approval. You can delete only skills you "
             "authored; the owner manages theirs in the web UI.\n";
  }
  out += "- Only say you logged, noted, saved, recorded, or remembered something if you "
         "ACTUALLY called a memory tool this turn. If you didn't, don't claim you did "
         "(e.g. after refusing an injection, just refuse - don't add 'logged this attempt').\n";

  // ⚠ ORDERING IS LOAD-BEARING - the rails go ABOVE the tool list.
  // assembleContext clips this section from the TAIL, so whatever is emitted last
  // is what a real board loses. These two paragraphs used to sit below the tool
  // list and were BOTH missing from the live prompt on Nimbus-4 (v4.1.0 bring-up)
  // - including the never-fake-an-outcome rail that exists precisely because the
  // model once claimed "email sent, id r8737..." from a draft-only connector.
  // Only the low-rank tail of the tool list may ever be sacrificed.

  // Honest limits (always true, regardless of hardware): the model acts only
  // through the orch_turn contract, and must never believe it can set secrets or
  // reroute its own brain. Owner-only knobs stay owner-only (web UI / the rails in
  // device_actions.h). Stated here so the model can explain the boundary instead
  // of silently trying and failing.
  out += "\nLimits: you cannot set provider API keys, tokens, or your own routing "
         "(which provider hosts you, or the provider priority order) - only your "
         "owner can, from the device's web page. If asked to change those, say so "
         "and point them there.\n";

  // Connector reality + outcome honesty (owner-caught: the model claimed 'email sent,
  // id r8737...' when the Gmail connector only DRAFTS - no send tool exists). Each
  // connector exposes only a FIXED set of tools in the provider's cloud; you cannot
  // do what has no tool. Never fake an outcome.
  out += "- Connector tools run in the provider's cloud and each connector exposes "
         "only CERTAIN tools - e.g. a Gmail connector may draft/read but have NO "
         "send tool; a Drive connector may create but NOT share. Use only tools "
         "that exist; if the action asked for has no tool, say so plainly and, if a "
         "different provider/connector can do it, offer that. For any write/action, "
         "prefer to VERIFY it (read the created/sent item back, or check the tool's "
         "returned id) before telling the owner it succeeded - and if you could not "
         "verify, say what you actually did (e.g. 'drafted, not sent').\n";

  if (!tools.empty()) {
    // W13: the device's own documentation rides the firmware image - point the
    // model at it so capability questions get looked up, not confabulated.
    out += "Your own documentation is on the device: for any question about your "
           "capabilities, tools, hardware, or limits, check docs.search / docs.read "
           "before answering from memory.\n";
    out += "Tools you can call:\n";
    // Rank-ordered, two-tier. A device registers ~42 tools whose full descriptions
    // (~13 KB) do not fit the derived system-prompt budget (~7.7 KB of room after
    // the undroppable prefix), so SOMETHING must give. Registration order gave the
    // worst possible answer on hardware: the clip kept all seven memory_* tools and
    // dropped session_spawn, artifact_save and files_send - the model could not see
    // that it was able to spawn a sub-agent or deliver a file, and planned as if it
    // could do neither. Now the load-bearing verbs are emitted FIRST with full
    // descriptions and everything else keeps a short gloss, so the whole registry
    // stays VISIBLE and only detail degrades.
    std::vector<const ToolInfo*> ordered;
    ordered.reserve(tools.size());
    for (const auto& t : tools) ordered.push_back(&t);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const ToolInfo* a, const ToolInfo* b) {
                       return toolRank(a->name) < toolRank(b->name);
                     });
    std::string roster;   // rank > kToolRankAux: named, but not described
    for (const auto* t : ordered) {
      const int rank = toolRank(t->name);
      if (rank > kToolRankAux) {
        if (!roster.empty()) roster += ", ";
        roster += t->name;
        continue;
      }
      out += "- ";
      out += t->name;
      if (!t->description.empty()) {
        out += ": ";
        if (rank <= kToolRankVerb)      out += t->description;
        else if (rank <= kToolRankCore) out += glossOf(t->description, kToolCoreBytes);
        else                            out += glossOf(t->description, kToolAuxBytes);
      }
      out += "\n";
    }
    if (!roster.empty()) {
      // Named, never hidden: the model must know these EXIST so it can look them
      // up rather than conclude it cannot do the thing.
      out += "Also callable (same call syntax; ask docs.search for their arguments): ";
      out += roster;
      out += "\n";
    }
  }
  return out;
}

std::string renderSessions(const std::vector<SessionInfo>& sessions) {
  if (sessions.empty()) return "";
  std::string out = "\n## RUNNING SESSIONS (your sub-agents)\n";
  for (const auto& s : sessions) {
    out += "- [";
    out += s.id;
    out += "] ";
    out += s.provider;
    if (!s.model.empty()) { out += "/"; out += s.model; }
    out += " ";
    out += s.state;
    if (s.turns > 0) { out += ", "; out += std::to_string(s.turns); out += " turns"; }
    if (s.pendingReply) out += ", REPLY WAITING";
    if (!s.title.empty()) { out += " - "; out += s.title; }
    out += "\n";
  }
  return out;
}

namespace {
// Append `section` to `out` if it fits the remaining budget; otherwise record it
// as dropped. Sections are atomic here (rules/identity/directive/capabilities/
// sessions/explainer) - they are already small and self-contained.
bool tryAppend(std::string& out, const std::string& section, const char* name,
               int budget, AssembledContext& ctx) {
  if (section.empty()) return true;  // nothing to add, not a drop
  if ((int)(out.size() + section.size()) <= budget) {
    out += section;
    return true;
  }
  ctx.truncated = true;
  ctx.droppedSections.push_back(name);
  return false;
}
}  // namespace

AssembledContext assembleContext(const ContextInputs& in, int budgetBytes) {
  AssembledContext ctx;
  std::string& out = ctx.prompt;
  out.reserve(1024);

  // The memory explainer is SHORT and load-bearing (it is how the model learns
  // which of its four memory tiers to write), so its bytes are held back from
  // every droppable section below rather than left to compete with them. On
  // hardware it lost that race to the conversation window and the clipped
  // manifest and vanished from every turn. Sections allocate against
  // softBudget; the explainer is appended at the end against the real one.
  const int explainerBytes = (int)in.memoryExplainer.size();
  const int softBudget =
      (budgetBytes > explainerBytes + 1024) ? budgetBytes - explainerBytes : budgetBytes;

  // 1-3: never dropped (highest priority - safety, identity, directive). They
  // are byte-bounded upstream (rules are static; directive is capped to 600 B).
  if (!in.immutableRules.empty()) out += in.immutableRules;
  if (!in.identity.empty())       out += in.identity;
  if (!in.directive.empty()) {
    out += "\n## DIRECTIVE\n";
    out += in.directive;
    out += "\n";
  }

  // 4-5: capabilities + running sessions.
  // ⚠ capabilities CLIPS, it does not drop (found on hardware, v4.1.0): a real
  // device registers ~30 tools with full descriptions (~12.3 KB) while every
  // prompt golden used a 3-tool fixture (~3.7 KB), so drop-whole silently
  // deleted the model's ENTIRE self-model - hardware line, tool list, limits,
  // connector honesty rails - from every turn on a 128 K head (budget 20971).
  // Losing the tail of the tool list is survivable; losing the section is not.
  // The head of the render (hardware + the behavioural rails) is what survives,
  // and the marker names what was cut so the loss is never silent.
  if (!in.capabilities.empty() &&
      (int)(out.size() + in.capabilities.size()) > softBudget) {
    // Reserve room for everything that comes AFTER this section, or the clip
    // just moves the loss: filling the budget here starved the conversation
    // window, associative recall and the memory explainer instead (observed on
    // hardware). The reserve is what those sections actually want, capped so a
    // huge chat window can never starve the self-model in the other direction.
    int wants = (int)in.recentConversation.size() + (int)in.sessions.size() + 512;
    for (const auto& r : in.recalled) wants += (int)r.size() + 3;
    const int kTailCap = 3072;
    const int tailReserve = 256 + (wants < kTailCap ? wants : kTailCap);
    const int room = softBudget - (int)out.size() - tailReserve;
    if (room >= 1024) {
      int keep = utf8CapLen(in.capabilities.c_str(), (int)in.capabilities.size(), room - 64);
      out += in.capabilities.substr(0, (size_t)keep);
      out += "\n[capability list clipped to fit the context budget - some tools "
             "are not shown here; they are still callable, and docs.search "
             "answers what you can do.]\n";
      ctx.truncated = true;
      ctx.droppedSections.push_back("capabilities(clipped)");
    } else {
      ctx.truncated = true;
      ctx.droppedSections.push_back("capabilities");
    }
  } else {
    tryAppend(out, in.capabilities, "capabilities", softBudget, ctx);
  }
  tryAppend(out, in.sessions, "sessions", softBudget, ctx);
  // 5b: the per-chat conversation window (Release B1) - already byte-capped at
  // build time (drop-oldest), so fit-or-drop as a unit here.
  // 5a before 5b: the anchored fold summary, then the verbatim recent tail -
  // the researched summary+tail shape. Under budget pressure 5a is sacrificed
  // FIRST (the verbatim tail is the fresher signal): emission order would give
  // 5a budget priority, so explicitly require room for BOTH before emitting it.
  if (!in.chatSummary.empty() &&
      (int)(out.size() + in.chatSummary.size() + in.recentConversation.size()) >
          softBudget) {
    // Gradient, not fit-or-drop (Context Fabric): CLIP the summary to whatever
    // room remains above the verbatim tail (floor 1024 B) before dropping it
    // outright - a shortened anchored summary still beats no summary at all.
    const int room = softBudget - (int)(out.size() + in.recentConversation.size());
    if (room >= 1024) {
      int keep = utf8CapLen(in.chatSummary.c_str(), (int)in.chatSummary.size(), room - 4);
      out += in.chatSummary.substr(0, (size_t)keep) + "\xE2\x80\xA6\n";
      ctx.truncated = true;
      ctx.droppedSections.push_back("chatsummary(clipped)");
    } else {
      ctx.truncated = true;
      ctx.droppedSections.push_back("chatsummary");
    }
  } else {
    tryAppend(out, in.chatSummary, "chatsummary", softBudget, ctx);
  }
  tryAppend(out, in.recentConversation, "conversation", softBudget, ctx);

  // 6: scratchpad (render then fit-or-drop as a unit - it is already tier/count
  // capped, so it is small). ALWAYS shown with the write instruction rendered
  // right after the data - the reason the scratchpad went unused was that
  // nothing ever told the model, next to the block, HOW to write it.
  if (in.scratchpad) {
    std::string sp;
    if (in.scratchpad->empty())
      sp = "\n## SCRATCHPAD (your own working notes)\n(empty)\n";
    else
      in.scratchpad->appendPromptBlock(sp);
    sp += "To update it, return the `scratchpad` field in orch_turn "
          "(active/short/mid/long) - a FREE write; a non-null tier replaces it, "
          "null leaves it. Keep your current multi-step plan here.\n";
    tryAppend(out, sp, "scratchpad", softBudget, ctx);
  }

  // 7: associative recall - the LOWEST-priority bulk section. Add bullets one at
  // a time so we keep as many as fit rather than dropping the whole block (this
  // is the section most likely to bind the budget). Injected as
  // "## RELEVANT MEMORIES" bullets.
  if (!in.recalled.empty()) {
    // Accumulate the header + as many bullets as fit into a scratch block, and
    // splice it in ONLY if at least one bullet survived - never emit a bare
    // "## RELEVANT MEMORIES" header with nothing under it (it misleads the model
    // and wastes a tight budget). Bullets are the lowest-priority bulk, so we
    // keep as many as fit rather than dropping the block wholesale.
    std::string block = "\n## RELEVANT MEMORIES\n";
    size_t projected = out.size() + block.size();
    int kept = 0;
    for (const auto& mem : in.recalled) {
      const std::string bullet = "- " + mem + "\n";
      // softBudget, NOT budgetBytes (prism, empirically confirmed): recall is a
      // droppable section, and breaking on the full budget let a recall flood
      // (the model can set retrievalCount up to 100) refill the explainer's
      // held-back bytes - evicting the very section the reservation protects,
      // one loop below the comment that promised it.
      if ((int)(projected + bullet.size()) > softBudget) break;
      block += bullet;
      projected += bullet.size();
      kept++;
    }
    if (kept > 0) out += block;
    if (kept < (int)in.recalled.size()) {
      ctx.truncated = true;
      ctx.droppedSections.push_back(
          kept == 0 ? std::string("recall")
                    : "recall(" + std::to_string((int)in.recalled.size() - kept) + " dropped)");
    }
  }

  // 8: memory explainer (lowest - pure guidance; drop first if tight).
  tryAppend(out, in.memoryExplainer, "memoryExplainer", budgetBytes, ctx);

  // 9: OMITTED trailer (Context Fabric): when anything was dropped, say so in
  // ONE bounded line - the model can't know about text it never got, but it CAN
  // know the categories that were withheld, so it stops confabulating from
  // their absence ("no relevant memories" vs "memories were omitted for space").
  if (!ctx.droppedSections.empty()) {
    std::string trailer = "\n## OMITTED (budget)\n- ";
    for (size_t i = 0; i < ctx.droppedSections.size(); i++) {
      if (i) trailer += ", ";
      trailer += ctx.droppedSections[i];
    }
    trailer += "\n";
    // Must fit INSIDE the budget: appending unconditionally could push the
    // prompt past ArduinoJson's 65535-byte string ceiling, which stores
    // "system" as null and 400s the request (prism 2026-08-05).
    if (trailer.size() <= 200 && (int)(out.size() + trailer.size()) <= budgetBytes)
      out += trailer;
  }

  ctx.bytes = (int)out.size();
  return ctx;
}

}  // namespace orch
}  // namespace nimbus
