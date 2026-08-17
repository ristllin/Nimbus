#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/orch/budget.h"
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
  hw.ring = true; hw.touch = true; hw.eink = false; hw.encoder = false;
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
  return UNITY_END();
}
