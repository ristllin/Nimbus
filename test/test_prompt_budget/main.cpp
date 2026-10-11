#include <unity.h>

#include <string>
#include <vector>

#include "../support/code_snippet.h"   // CUM-485
#include "nimbus/harness/dream.h"       // buildConversationWindow (the device's window)
#include "nimbus/orch/budget.h"
#include "nimbus/orch/scratchpad.h"
#include "nimbus/orch/world.h"

// Prompt-budget headroom guard.
//
// ⚠ Found on HARDWARE, not here (v4.1.0 bring-up): on a real board the composed
// system prompt reported "## OMITTED (budget) - capabilities" - the WHOLE
// capability manifest (hardware self-model, the tool list, the limits and
// connector-honesty rails, the docs pointer) silently vanished from every turn.
// Nothing caught it because every existing prompt golden uses a 3-4 tool canned
// fixture (~3.7 KB of capabilities) while a real device registers ~25 tools with
// full descriptions (~12.9 KB), and assembleContext drops that section WHOLE
// rather than clipping it. v4.1 also grew the never-dropped prefix by ~1 KB,
// which is what pushed it over.
//
// This test reproduces the DEVICE's shape - a realistic tool count with
// realistic description lengths - and asserts the manifest still fits the
// budget the device actually derives, with margin. If a future change bloats a
// tool description or the immutable rules, this goes red instead of the model
// silently losing its own self-model in the field.

using nimbus::orch::ContextInputs;
using nimbus::orch::Hardware;
using nimbus::orch::ToolInfo;

void setUp() {}
void tearDown() {}

// The auto budget a 128K-context head derives for the system prompt - the value
// measured live on Nimbus-4 (log line: "budget: ctx=128K sys=20971").
static int autoSystemBudget() {
  nimbus::orch::BudgetOverrides none;
  return nimbus::orch::deriveBudget(128000, none).systemPromptBytes;
}

// The REAL registry, read off Nimbus-4 (`tools/list` over /mcp, v4.1.0): all 42
// tools, in the registration order the renderer used to emit. Kept verbatim -
// a shorter or reordered stand-in is exactly how this bug hid.
static const char* kDeviceTools[] = {
    "artifact_save",  "device_control", "device_selftest", "device_status",
    "docs_list",      "docs_read",      "docs_search",     "files_list",
    "files_read",     "files_search",   "files_send",      "files_share",
    "files_stat",     "image_generate", "loop_cancel",     "loop_create",
    "loop_list",      "memory_config",  "memory_episodic", "memory_pin",
    "memory_scratchpad", "memory_search", "memory_update", "memory_write",
    "ota_status",     "reply_speak",    "reply_telegram",  "results_get",
    "results_list",   "session_list",   "session_poll",    "session_spawn",
    "session_tell",   "session_terminate", "skill_delete", "skill_get",
    "skill_list",     "skill_save",     "system_health",   "tenant_list",
    "tenant_set_quota", "tenant_set_role",
};

// The verbs a multi-step turn cannot be planned without.
//
// ⚠ What actually happened on Nimbus-4, stated precisely: the tail clip hid
// artifact_save and files_send (the DELIVERY verbs) while keeping all seven
// memory_* tools, so the model offered to describe a file instead of sending
// one. Spawning was NOT lost - it rides the `session_ops` RESPONSE FIELD, which
// lives in the undroppable prefix, not the tool manifest. session_spawn is kept
// in this list because it is a registered MCP tool that would rank first if it
// were ever surfaced to the manifest; do not read it as part of the field bug.
static const char* kLoadBearing[] = {"session_spawn", "artifact_save", "files_send"};

// A device-shaped tool list: the real registry, with descriptions at the length
// the real registrations use (309 B measured mean on Nimbus-4).
static std::vector<ToolInfo> deviceShapedTools() {
  const std::string typical(309, 'x');
  const std::string longest(784, 'y');   // device.status after the v4.1 trim
  std::vector<ToolInfo> v;
  for (const char* n : kDeviceTools)
    v.push_back(ToolInfo{n, std::string(n) == "device_status" ? longest : typical});
  return v;
}

static Hardware deviceHw() {
  Hardware hw;
  hw.deviceName = "Nimbus-4";
  hw.ring = true; hw.touch = true;
  hw.mic = hw.speaker = hw.battery = hw.sd = true;
  hw.wifi = hw.telegram = hw.files = true;
  hw.ledCount = 45;
  return hw;
}

// ⚠ CALIBRATION NOTE: the first version of this test PASSED while the device
// still dropped the section, because its synthetic descriptions were smaller
// than the real registrations. The numbers below are MEASURED on Nimbus-4
// (v4.1.0, mistral head): budget 20971, capability manifest ~12.3 KB,
// undroppable prefix ~9.9 KB - i.e. the manifest genuinely does NOT fit, and
// the contract is that it CLIPS rather than vanishes.
static void test_oversize_manifest_clips_and_keeps_its_head() {
  const int budget = autoSystemBudget();
  ContextInputs in;
  in.immutableRules = std::string(9900, 'r');   // device-sized undroppable prefix
  in.capabilities = nimbus::orch::renderCapabilities(deviceHw(), deviceShapedTools());
  // Pad the manifest to the size a real 30-tool device produces, so this test
  // exercises the OVER-budget path the hardware actually hits.
  while ((int)in.capabilities.size() < 12300) in.capabilities += "- padding tool: description\n";

  const auto asm1 = nimbus::orch::assembleContext(in, budget);
  TEST_ASSERT_TRUE_MESSAGE((int)asm1.prompt.size() <= budget, "clip must respect the budget");
  // The section must NOT have vanished: its HEAD (hardware + rails) survives.
  TEST_ASSERT_TRUE_MESSAGE(asm1.prompt.find("Hardware present:") != std::string::npos,
                           "the hardware self-model was dropped, not clipped");
  TEST_ASSERT_TRUE(asm1.prompt.find("45-LED ring") != std::string::npos);
  // ...and the loss is announced, never silent.
  TEST_ASSERT_TRUE(asm1.prompt.find("capability list clipped") != std::string::npos);
  bool clipped = false;
  for (const auto& d : asm1.droppedSections) if (d == "capabilities(clipped)") clipped = true;
  TEST_ASSERT_TRUE_MESSAGE(clipped, "the clip must be reported in droppedSections");
  // The old failure mode - a bare "capabilities" drop - must never come back.
  for (const auto& d : asm1.droppedSections)
    TEST_ASSERT_TRUE_MESSAGE(d != "capabilities", "capabilities was DROPPED WHOLE (the v4.1 hardware bug)");
}

// A manifest that fits is emitted untouched (no gratuitous clipping).
static void test_fitting_manifest_is_untouched() {
  ContextInputs in;
  in.immutableRules = std::string(2000, 'r');
  in.capabilities = nimbus::orch::renderCapabilities(deviceHw(), deviceShapedTools());
  const auto a = nimbus::orch::assembleContext(in, autoSystemBudget());
  TEST_ASSERT_TRUE(a.prompt.find("capability list clipped") == std::string::npos);
  TEST_ASSERT_TRUE(a.droppedSections.empty());
}

// The manifest carries the things the model cannot work without; if any of them
// is missing the section is not doing its job even when it fits.
static void test_manifest_carries_the_load_bearing_parts() {
  const std::string caps =
      nimbus::orch::renderCapabilities(deviceHw(), deviceShapedTools());
  TEST_ASSERT_TRUE(caps.find("Hardware present:") != std::string::npos);
  TEST_ASSERT_TRUE(caps.find("45-LED ring") != std::string::npos);
  TEST_ASSERT_TRUE(caps.find("2.8\" color touchscreen") != std::string::npos);
  TEST_ASSERT_TRUE(caps.find("Tools you can call:") != std::string::npos);
  TEST_ASSERT_TRUE(caps.find("docs.search / docs.read") != std::string::npos);
  TEST_ASSERT_TRUE(caps.find("you cannot set provider API keys") != std::string::npos);
}

// ⚠ THE REGRESSION THIS FILE EXISTS FOR (round 2).
// Round 1 stopped the section vanishing WHOLE, but the surviving clip still ate
// the TAIL - and on Nimbus-4 the tail held artifact_save and files_send plus
// BOTH honesty rails (including the never-fake-an-outcome one), while all seven
// memory_* tools survived. Asked to send a report, the model offered to describe
// a file rather than deliver one. A manifest that "fits" is not the contract;
// the contract is that the turn-critical verbs and the rails are ALWAYS there.
static void test_loadbearing_tools_and_rails_survive_a_real_device_manifest() {
  ContextInputs in;
  in.immutableRules = std::string(9900, 'r');   // device-sized undroppable prefix
  in.capabilities = nimbus::orch::renderCapabilities(deviceHw(), deviceShapedTools());
  const auto a = nimbus::orch::assembleContext(in, autoSystemBudget());

  for (const char* t : kLoadBearing) {
    std::string msg = std::string("load-bearing tool missing from the prompt: ") + t;
    TEST_ASSERT_TRUE_MESSAGE(a.prompt.find(t) != std::string::npos, msg.c_str());
  }
  // The rails moved ABOVE the tool list precisely so a clip can never reach them.
  TEST_ASSERT_TRUE_MESSAGE(
      a.prompt.find("you cannot set provider API keys") != std::string::npos,
      "the owner-only-knobs rail was clipped away");
  TEST_ASSERT_TRUE_MESSAGE(
      a.prompt.find("each connector exposes") != std::string::npos,
      "the never-fake-an-outcome rail was clipped away");
  TEST_ASSERT_TRUE_MESSAGE(
      a.prompt.find("docs.search / docs.read") != std::string::npos,
      "the docs pointer was clipped away");
}

// No tool may become INVISIBLE. Detail may degrade to a gloss, but a name the
// model never sees is a capability it cannot plan with - and the tool-loop will
// happily accept a call to it, so the prompt and the wire would disagree.
static void test_every_registered_tool_is_still_named() {
  ContextInputs in;
  in.immutableRules = std::string(9900, 'r');
  in.capabilities = nimbus::orch::renderCapabilities(deviceHw(), deviceShapedTools());
  const auto a = nimbus::orch::assembleContext(in, autoSystemBudget());
  for (const char* t : kDeviceTools) {
    std::string msg = std::string("tool invisible in the manifest: ") + t;
    TEST_ASSERT_TRUE_MESSAGE(a.prompt.find(t) != std::string::npos, msg.c_str());
  }
}

// Rank order is the mechanism. The turn-critical verbs get a described bullet and
// are emitted FIRST; the administrative surface degrades to the name-only roster
// at the very end. So if byte pressure ever returns, tenant_*/skill_*/loop_* are
// what give - never spawn/save/send.
static void test_loadbearing_tools_precede_the_admin_surface() {
  const std::string caps =
      nimbus::orch::renderCapabilities(deviceHw(), deviceShapedTools());
  const size_t spawn  = caps.find("- session_spawn:");
  const size_t send   = caps.find("- files_send:");
  const size_t save   = caps.find("- artifact_save:");
  const size_t roster = caps.find("Also callable");
  TEST_ASSERT_TRUE_MESSAGE(spawn != std::string::npos, "session_spawn lost its bullet");
  TEST_ASSERT_TRUE_MESSAGE(send  != std::string::npos, "files_send lost its bullet");
  TEST_ASSERT_TRUE_MESSAGE(save  != std::string::npos, "artifact_save lost its bullet");
  TEST_ASSERT_TRUE_MESSAGE(roster != std::string::npos, "the roster line is missing");
  TEST_ASSERT_TRUE_MESSAGE(spawn < roster, "session_spawn must precede the roster");
  TEST_ASSERT_TRUE_MESSAGE(send  < roster, "files_send must precede the roster");
  TEST_ASSERT_TRUE_MESSAGE(save  < roster, "artifact_save must precede the roster");
  // The admin surface is named there, not described - and never as a bullet.
  TEST_ASSERT_TRUE(caps.find("tenant_set_role", roster) != std::string::npos);
  TEST_ASSERT_TRUE_MESSAGE(caps.find("- tenant_set_role:") == std::string::npos,
                           "admin tools should be rostered, not described");
}

// Ordering must actually REORDER. In registration order device_control,
// image_generate and memory_config all precede session_spawn; rank has to invert
// that. ⚠ Mutation-checked: deleting the stable_sort MUST turn this red. The
// first version of the ordering test did not - the roster is emitted after the
// loop whatever the order, and the retiering made the manifest fit, so the
// assertion held either way. A test that cannot fail is not a test.
static void test_rank_ordering_actually_reorders() {
  const std::string caps =
      nimbus::orch::renderCapabilities(deviceHw(), deviceShapedTools());
  const size_t spawn = caps.find("- session_spawn:");
  TEST_ASSERT_TRUE(spawn != std::string::npos);
  for (const char* later : {"- memory_config:", "- device_control:",
                            "- image_generate:", "- memory_episodic:"}) {
    const size_t at = caps.find(later);
    std::string msg = std::string("session_spawn must be ordered before ") + later;
    TEST_ASSERT_TRUE(at != std::string::npos);
    TEST_ASSERT_TRUE_MESSAGE(spawn < at, msg.c_str());
  }
}

// The ordering earns its keep only under byte pressure, so squeeze the prefix
// until the manifest genuinely must clip and assert WHAT survives. Registration
// order fails this: session_spawn sits 32nd and is cut, which is precisely the
// state Nimbus-4 shipped in.
static void test_under_pressure_the_verbs_survive_and_admin_is_sacrificed() {
  ContextInputs in;
  in.immutableRules = std::string(15000, 'r');   // hard squeeze
  in.capabilities = nimbus::orch::renderCapabilities(deviceHw(), deviceShapedTools());
  const auto a = nimbus::orch::assembleContext(in, autoSystemBudget());
  for (const char* t : kLoadBearing) {
    std::string msg = std::string("clipped under pressure: ") + t;
    TEST_ASSERT_TRUE_MESSAGE(a.prompt.find(t) != std::string::npos, msg.c_str());
  }
  // ...and the thing that gave way is the administrative roster, as designed.
  TEST_ASSERT_TRUE_MESSAGE(a.prompt.find("tenant_set_role") == std::string::npos,
                           "the admin roster should be the first thing sacrificed");
  // ⚠ prism: BOTH honesty rails must survive the squeeze too - this is the
  // assertion that pins "rails above the tool list". Moving them back below
  // (reverting the ordering half of 366ce80) turns exactly this red; without it
  // all eight prior tests stayed green under that mutation.
  TEST_ASSERT_TRUE_MESSAGE(
      a.prompt.find("you cannot set provider API keys") != std::string::npos,
      "the owner-only-knobs rail was clipped under pressure");
  TEST_ASSERT_TRUE_MESSAGE(
      a.prompt.find("each connector exposes") != std::string::npos,
      "the never-fake-an-outcome rail was clipped under pressure");
}

// ⚠ prism finding (PoC-confirmed before the fix): glossOf's clause-boundary
// search used find_last_of(".;-") - and find_last_of matches BYTES, so the
// 3-byte em-dash also matched stray \xE2/\x80/\x94 continuation bytes inside
// OTHER multi-byte characters. An ellipsis ("…" = \xE2\x80\xA6) near the cut
// made it slice mid-character and emit invalid UTF-8 into the composed prompt.
// renderCapabilities is the public seam, so feed descriptions through it and
// validate every byte of the output.
static bool validUtf8(const std::string& s) {
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = (unsigned char)s[i];
    const int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2
                : (c >> 3) == 30 ? 3 : -1;
    if (n < 0) return false;
    for (int k = 1; k <= n; k++)
      if (i + k >= s.size() || ((unsigned char)s[i + k] >> 6) != 2) return false;
    i += (size_t)n + 1;
  }
  return true;
}

static void test_glossed_manifest_is_valid_utf8() {
  // Adversarial descriptions: multi-byte punctuation straddling every plausible
  // gloss cut point (glosses cap near 88/200 bytes - walk the ellipsis through
  // both windows).
  std::vector<ToolInfo> tools;
  for (int pad = 80; pad <= 96; pad++)
    tools.push_back(ToolInfo{"aux_tool_" + std::to_string(pad),
                             std::string((size_t)pad, 'x') +
                                 "\xE2\x80\xA6 more text follows here to force a byte cut"});
  for (int pad = 192; pad <= 208; pad++)
    tools.push_back(ToolInfo{"docs_probe_" + std::to_string(pad),
                             std::string((size_t)pad, 'x') +
                                 "\xE2\x80\x94 em-dash straddles the core gloss window"});
  const std::string caps = nimbus::orch::renderCapabilities(deviceHw(), tools);
  TEST_ASSERT_TRUE_MESSAGE(validUtf8(caps),
                           "glossed manifest emitted invalid UTF-8 (mid-character cut)");
}


// ⚠ prism (empirically confirmed): the recall-bullet loop broke on budgetBytes
// while every other droppable section honored softBudget - so a recall FLOOD
// (the model can set retrievalCount up to 100) refilled the memoryExplainer's
// held-back bytes and evicted it, one loop below the comment promising the
// reservation. Reverting the one-token fix (softBudget -> budgetBytes there)
// must turn this red.
// ⚠ Calibrated so the mutation is DETERMINISTIC (the first version passed under
// the mutation: with the caps clip disengaged, both paths left slack). The caps
// pad engages the clip (out fills to ~budget − tailReserve), the flood then fills
// toward the boundary in 161-byte bullets, and the explainer is 600 B - larger
// than one bullet, so the mutated path (bullets run to budgetBytes) NEVER leaves
// room for it, while the fixed path (bullets stop at softBudget = budget − 600)
// ALWAYS does.
static void test_recall_flood_cannot_evict_the_memory_explainer() {
  ContextInputs in;
  in.immutableRules = std::string(9900, 'r');
  in.capabilities = nimbus::orch::renderCapabilities(deviceHw(), deviceShapedTools());
  while ((int)in.capabilities.size() < 12300) in.capabilities += "- padding tool: description\n";
  in.memoryExplainer = "## HOW YOUR MEMORY WORKS\n" + std::string(570, 'e');
  for (int i = 0; i < 100; i++)
    in.recalled.push_back("recalled memory " + std::to_string(i) + ": " +
                          std::string(140, 'm'));
  const auto a = nimbus::orch::assembleContext(in, autoSystemBudget());
  TEST_ASSERT_TRUE_MESSAGE(
      a.prompt.find("HOW YOUR MEMORY WORKS") != std::string::npos,
      "a recall flood evicted the memory explainer (the softBudget reservation failed)");
  for (const auto& d : a.droppedSections)
    TEST_ASSERT_TRUE_MESSAGE(d != "memoryExplainer",
                             "memoryExplainer must never be the section that gives way");
}

// ---- CUM-485: the conversation window is a gradient ---------------------------

static const char* kConvHeader =
    "\n## RECENT CONVERSATION (this chat, oldest first; the current message is NOT "
    "here - it is in [USER])\n";

static nimbus::orch::EpisodicMessage row(const char* role, const std::string& text) {
  nimbus::orch::EpisodicMessage m;
  m.role = role;
  m.text = text;
  return m;
}

// On a 128K head the device-shaped prompt leaves well under 1 KB for the window,
// and the window was fit-or-drop - so the WHOLE conversation vanished every turn.
// It must now keep the newest rows that fit (oldest cut first), stay inside the
// budget, say what it cut, and never touch the self-model above it.
static void test_conversation_trims_oldest_rows_instead_of_vanishing() {
  const int budget = autoSystemBudget();   // 128K head
  std::vector<nimbus::orch::EpisodicMessage> rows;   // newest-first, as queried
  rows.push_back(row("assistant", "the newest reply"));
  rows.push_back(row("user", "the newest question"));
  for (int i = 0; i < 10; ++i) rows.push_back(row(i % 2 ? "user" : "assistant",
                                                   "older row " + std::to_string(i) + " " + std::string(250, 'o')));
  ContextInputs in;
  in.immutableRules = std::string(9900, 'r');
  in.capabilities = nimbus::orch::renderCapabilities(deviceHw(), deviceShapedTools());
  in.recentConversation = std::string(kConvHeader) +
                          agent::dream::buildConversationWindow(rows, nimbus::orch::deriveBudget(128000, {}).recentConvBytes);
  const auto a = nimbus::orch::assembleContext(in, budget);
  TEST_ASSERT_TRUE((int)a.prompt.size() <= budget);
  TEST_ASSERT_TRUE_MESSAGE(a.prompt.find("## RECENT CONVERSATION") != std::string::npos,
                           "the window vanished whole (the pre-CUM-485 fit-or-drop)");
  TEST_ASSERT_TRUE(a.prompt.find("- assistant: the newest reply\n") != std::string::npos);
  TEST_ASSERT_TRUE(a.prompt.find("older row 9 ") == std::string::npos);   // oldest cut
  TEST_ASSERT_TRUE(a.prompt.find(nimbus::orch::kConvOmittedNote) != std::string::npos);
  bool clipped = false;
  for (const auto& d : a.droppedSections) clipped |= (d == "conversation(clipped)");
  TEST_ASSERT_TRUE(clipped);
  TEST_ASSERT_TRUE(a.prompt.find("Hardware present:") != std::string::npos);   // self-model intact
}

// prism: trimming the window must not evict the scratchpad. The old fit-or-drop
// dropped an oversize window and the scratchpad (the owner's persistent plan)
// still fit; the gradient holds the scratchpad's bytes back when it cuts rows.
static void test_trimmed_window_leaves_room_for_the_scratchpad() {
  nimbus::orch::Scratchpad sp;
  sp.setActiveTask("finish the carbon-stock report");
  ContextInputs in;
  in.immutableRules = std::string(9900, 'r');
  in.capabilities = nimbus::orch::renderCapabilities(deviceHw(), deviceShapedTools());
  in.scratchpad = &sp;
  std::vector<nimbus::orch::EpisodicMessage> rows;
  for (int i = 0; i < 12; ++i) rows.push_back(row(i % 2 ? "user" : "assistant", "row " + std::to_string(i) + " " + std::string(200, 'w')));
  in.recentConversation = std::string(kConvHeader) + agent::dream::buildConversationWindow(rows, 8192);
  const auto a = nimbus::orch::assembleContext(in, autoSystemBudget());
  TEST_ASSERT_TRUE((int)a.prompt.size() <= autoSystemBudget());
  TEST_ASSERT_TRUE_MESSAGE(a.prompt.find("finish the carbon-stock report") != std::string::npos,
                           "the trimmed window evicted the scratchpad");
  for (const auto& d : a.droppedSections) TEST_ASSERT_TRUE_MESSAGE(d != "scratchpad", "scratchpad dropped");
}

// A window that fits is emitted byte-for-byte; one whose NEWEST row alone cannot
// fit keeps that row's head; with no useful room left it is dropped (reported).
static void test_conversation_fits_untouched_or_drops_honestly() {
  ContextInputs in;
  in.immutableRules = std::string(2000, 'r');
  in.recentConversation = std::string(kConvHeader) + "- user: hi\n- assistant: hello\n";
  auto a = nimbus::orch::assembleContext(in, 32768);
  TEST_ASSERT_TRUE(a.prompt.find(in.recentConversation) != std::string::npos);
  TEST_ASSERT_TRUE(a.droppedSections.empty());

  // Newest row alone too big: its head survives (clipped, "..."), never mid-char.
  in.recentConversation = std::string(kConvHeader) + "- user: hi\n- assistant: " + std::string(3000, 'z') + "\n";
  a = nimbus::orch::assembleContext(in, 2000 + 1500);
  TEST_ASSERT_TRUE((int)a.prompt.size() <= 2000 + 1500);
  TEST_ASSERT_TRUE(a.prompt.find("- assistant: zzzz") != std::string::npos);
  TEST_ASSERT_TRUE(a.prompt.find("z...\n") != std::string::npos);
  TEST_ASSERT_TRUE(a.prompt.find("- user: hi") == std::string::npos);   // older row cut
  // Too little room for even a useful head: dropped, and reported.
  a = nimbus::orch::assembleContext(in, 2000 + 200);
  TEST_ASSERT_TRUE(a.prompt.find("RECENT CONVERSATION") == std::string::npos);
  bool dropped = false;
  for (const auto& d : a.droppedSections) dropped |= (d == "conversation");
  TEST_ASSERT_TRUE(dropped);
}

// The DoD case at prompt level: on the head bench unit B used (gpt-5.6, 272K) and
// on the 200K anchor, the previous reply's 1.2 KB code block reaches the
// assembled system prompt whole, behind a device-shaped prefix and manifest.
static void test_prev_reply_code_survives_assembly_on_200k_and_272k_heads() {
  const std::string code = harness_test::kPrevReplyCodeSnippet;
  for (uint32_t ctx : {200000u, 272000u}) {
    const auto b = nimbus::orch::deriveBudget(ctx, {});
    std::vector<nimbus::orch::EpisodicMessage> rows = {row("assistant", code), row("user", "snippet?")};
    for (int i = 0; i < 10; ++i) rows.push_back(row(i % 2 ? "user" : "assistant", std::string(600, 'h')));
    ContextInputs in;
    in.immutableRules = std::string(9900, 'r');
    in.capabilities = nimbus::orch::renderCapabilities(deviceHw(), deviceShapedTools());
    in.recentConversation = std::string(kConvHeader) + agent::dream::buildConversationWindow(rows, b.recentConvBytes);
    const auto a = nimbus::orch::assembleContext(in, b.systemPromptBytes);
    TEST_ASSERT_TRUE((int)a.prompt.size() <= b.systemPromptBytes);
    TEST_ASSERT_TRUE_MESSAGE(a.prompt.find("stock_by_core = (") != std::string::npos, std::to_string(ctx).c_str());
    TEST_ASSERT_TRUE(a.prompt.find("\n  print(stock_by_core)\n") != std::string::npos);
    TEST_ASSERT_TRUE(a.prompt.find("one row per core.") != std::string::npos);
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_oversize_manifest_clips_and_keeps_its_head);
  RUN_TEST(test_fitting_manifest_is_untouched);
  RUN_TEST(test_manifest_carries_the_load_bearing_parts);
  RUN_TEST(test_loadbearing_tools_and_rails_survive_a_real_device_manifest);
  RUN_TEST(test_every_registered_tool_is_still_named);
  RUN_TEST(test_loadbearing_tools_precede_the_admin_surface);
  RUN_TEST(test_rank_ordering_actually_reorders);
  RUN_TEST(test_under_pressure_the_verbs_survive_and_admin_is_sacrificed);
  RUN_TEST(test_glossed_manifest_is_valid_utf8);
  RUN_TEST(test_recall_flood_cannot_evict_the_memory_explainer);
  RUN_TEST(test_conversation_trims_oldest_rows_instead_of_vanishing);
  RUN_TEST(test_conversation_fits_untouched_or_drops_honestly);
  RUN_TEST(test_trimmed_window_leaves_room_for_the_scratchpad);
  RUN_TEST(test_prev_reply_code_survives_assembly_on_200k_and_272k_heads);
  return UNITY_END();
}
