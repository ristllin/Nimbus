#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/orch/scratchpad.h"
#include "nimbus/orch/world.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

static bool has(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}

// ---- capability manifest ----------------------------------------------------

static void test_capabilities_lists_present_hardware() {
  Hardware hw;
  hw.deviceName = "Nimbus"; hw.version = "1.0";
  hw.ring = true; hw.eink = true; hw.mic = true; hw.sd = true;
  hw.wifi = true; hw.telegram = true;
  std::vector<ToolInfo> tools = {{"memory.search", "search long-term memory"}};
  std::string m = renderCapabilities(hw, tools);
  TEST_ASSERT_TRUE(has(m, "CAPABILITIES"));
  TEST_ASSERT_TRUE(has(m, "45-LED ring"));
  TEST_ASSERT_TRUE(has(m, "microphone"));
  TEST_ASSERT_TRUE(has(m, "SD card"));
  TEST_ASSERT_TRUE(has(m, "WiFi"));
  TEST_ASSERT_TRUE(has(m, "Telegram"));
  TEST_ASSERT_TRUE(has(m, "memory.search"));
  TEST_ASSERT_TRUE(has(m, "search long-term memory"));
}

static void test_capabilities_offline_and_absent_hardware() {
  Hardware hw;
  hw.mic = false; hw.sd = false; hw.wifi = false; hw.ble = false; hw.telegram = false;
  std::string m = renderCapabilities(hw, {});
  TEST_ASSERT_TRUE(has(m, "offline"));
  TEST_ASSERT_FALSE(has(m, "microphone"));
  TEST_ASSERT_FALSE(has(m, "SD card"));
  TEST_ASSERT_FALSE(has(m, "Tools you can call"));  // no tools -> no tool list
  // Honest limits are always stated (this is the real prompt path - no manifest).
  TEST_ASSERT_TRUE(has(m, "cannot set provider API keys"));
}

// ---- running-sessions digest ------------------------------------------------

static void test_sessions_empty_is_blank() {
  TEST_ASSERT_TRUE(renderSessions({}).empty());
}

static void test_sessions_digest_content() {
  std::vector<SessionInfo> s = {
    {"job0001", "openai", "gpt-5.5", "summarize logs", "running", 2, false},
    {"job0002", "anthropic", "", "review diff", "done", 4, true},
  };
  std::string d = renderSessions(s);
  TEST_ASSERT_TRUE(has(d, "RUNNING SESSIONS"));
  TEST_ASSERT_TRUE(has(d, "job0001"));
  TEST_ASSERT_TRUE(has(d, "openai/gpt-5.5"));
  TEST_ASSERT_TRUE(has(d, "summarize logs"));
  TEST_ASSERT_TRUE(has(d, "REPLY WAITING"));  // pendingReply surfaced
}

// ---- context assembler ------------------------------------------------------

static ContextInputs baseInputs(Scratchpad& sp) {
  ContextInputs in;
  in.immutableRules = "## RULES\nDo no harm.\n";
  in.identity = "\nYou are Nimbus.\n";
  in.directive = "Be concise.";
  in.capabilities = renderCapabilities(Hardware{}, {});
  sp.setActiveTask("testing");
  in.scratchpad = &sp;
  in.recalled = {"user likes teal", "project ships Friday"};
  in.memoryExplainer = "\n## MEMORY\nYou can call memory tools.\n";
  return in;
}

static void test_assemble_orders_sections() {
  Scratchpad sp;
  ContextInputs in = baseInputs(sp);
  AssembledContext c = assembleContext(in, 32768);
  TEST_ASSERT_FALSE(c.truncated);
  // priority order: rules < identity < directive < capabilities < scratchpad <
  // recall < explainer  (assert by ascending find position)
  size_t pRules = c.prompt.find("RULES");
  size_t pDir   = c.prompt.find("DIRECTIVE");
  size_t pCap   = c.prompt.find("CAPABILITIES");
  size_t pScr   = c.prompt.find("SCRATCHPAD");
  size_t pMem   = c.prompt.find("RELEVANT MEMORIES");
  TEST_ASSERT_TRUE(pRules < pDir);
  TEST_ASSERT_TRUE(pDir < pCap);
  TEST_ASSERT_TRUE(pCap < pScr);
  TEST_ASSERT_TRUE(pScr < pMem);
  TEST_ASSERT_TRUE(has(c.prompt, "user likes teal"));
  TEST_ASSERT_EQUAL_INT((int)c.prompt.size(), c.bytes);
}

static void test_assemble_directive_never_dropped() {
  Scratchpad sp;
  ContextInputs in = baseInputs(sp);
  // Absurdly tight budget: high-priority sections still present, low-priority
  // recall/explainer get dropped, and nothing crashes.
  AssembledContext c = assembleContext(in, 60);
  TEST_ASSERT_TRUE(has(c.prompt, "DIRECTIVE"));
  TEST_ASSERT_TRUE(has(c.prompt, "Be concise"));
  TEST_ASSERT_TRUE(c.truncated);
}

static void test_assemble_recall_partial_keep() {
  Scratchpad sp;
  ContextInputs in;
  in.directive = "d";
  in.recalled.clear();
  for (int i = 0; i < 50; i++) in.recalled.push_back("memory item number " + std::to_string(i));
  // Budget big enough for the header + a FEW bullets but not all 50.
  AssembledContext c = assembleContext(in, 200);
  TEST_ASSERT_TRUE(has(c.prompt, "RELEVANT MEMORIES"));
  TEST_ASSERT_TRUE(has(c.prompt, "memory item number 0"));  // kept the first ones
  TEST_ASSERT_TRUE(c.truncated);                            // but not all
  TEST_ASSERT_LESS_THAN_INT(50, (int)std::count(c.prompt.begin(), c.prompt.end(), '\n'));
}

// If the "## RELEVANT MEMORIES" header fits but not a single bullet does, the
// header must be SUPPRESSED - never inject a section that promises memories and
// then shows none (it misleads the model and wastes a tight budget).
static void test_assemble_recall_header_suppressed_when_no_bullet_fits() {
  ContextInputs in;
  in.directive = "d";                             // ~16 B directive block
  in.recalled = { std::string(500, 'x') };        // one bullet far too big to fit
  // Budget admits the directive + the ~22 B header, but not the 500 B bullet.
  AssembledContext c = assembleContext(in, 60);
  TEST_ASSERT_FALSE(has(c.prompt, "RELEVANT MEMORIES"));  // no bare header
  TEST_ASSERT_TRUE(c.truncated);
  bool droppedRecall = false;
  for (const auto& d : c.droppedSections)
    if (d.find("recall") != std::string::npos) droppedRecall = true;
  TEST_ASSERT_TRUE(droppedRecall);
}

// Context Fabric: under pressure the anchored summary CLIPS (floor 1024 B)
// before being dropped outright - a shortened summary beats none.
static void test_assemble_summary_clips_before_dropping() {
  Scratchpad sp;
  ContextInputs in = baseInputs(sp);
  // Isolate the summary/tail pair: no capabilities/recall/explainer competing for
  // the budget, so the arithmetic below is exact.
  in.capabilities.clear();
  in.recalled.clear();
  in.memoryExplainer.clear();
  in.scratchpad = nullptr;
  in.chatSummary = "\n## CONVERSATION SUMMARY\n" + std::string(6000, 's');
  in.recentConversation = "\n## RECENT CONVERSATION\n" + std::string(500, 'r');
  // Budget: room for rules/identity/directive + the tail + ~2 KB of summary -
  // NOT the full 6 KB. The old fit-or-drop lost the summary entirely.
  const int base = (int)(in.immutableRules.size() + in.identity.size() +
                         in.directive.size() + 20);
  AssembledContext c = assembleContext(in, base + (int)in.recentConversation.size() + 2000);
  TEST_ASSERT_TRUE(has(c.prompt, "CONVERSATION SUMMARY"));      // clipped, kept
  TEST_ASSERT_TRUE(has(c.prompt, "RECENT CONVERSATION"));       // tail intact
  TEST_ASSERT_TRUE(c.prompt.find(std::string(3000, 's')) == std::string::npos);  // not the full 6 KB
  TEST_ASSERT_TRUE(c.truncated);
  bool clippedMarked = false;
  for (const auto& d : c.droppedSections)
    if (d == "chatsummary(clipped)") clippedMarked = true;
  TEST_ASSERT_TRUE(clippedMarked);
}

// Context Fabric: dropped sections are NAMED in one bounded trailer line, so the
// model knows categories were withheld instead of confabulating from absence.
static void test_assemble_omitted_trailer_lists_drops() {
  Scratchpad sp;
  ContextInputs in = baseInputs(sp);
  // Make the drops deterministic: two oversized low-priority sections that
  // CANNOT fit, and a budget with room for everything else plus the ~60 B
  // trailer (the trailer is itself budget-bounded, so an absurdly tight budget
  // legitimately omits it too - that is why this is not simply "60").
  in.recalled = { std::string(4000, 'm') };
  in.memoryExplainer = "\n## MEMORY\n" + std::string(4000, 'e');
  in.scratchpad = nullptr;
  const int room = (int)(in.immutableRules.size() + in.identity.size() +
                         in.directive.size() + in.capabilities.size() + 300);
  AssembledContext tight = assembleContext(in, room);
  TEST_ASSERT_TRUE(tight.truncated);
  TEST_ASSERT_TRUE(has(tight.prompt, "## OMITTED (budget)"));
  // Assert CONTENT, not just the header (prism: the header alone is a weak
  // claim - the trailer must actually NAME what was withheld).
  TEST_ASSERT_TRUE(tight.droppedSections.size() >= 2);
  for (const auto& d : tight.droppedSections)
    TEST_ASSERT_TRUE_MESSAGE(has(tight.prompt, d.c_str()),
                             ("trailer omits a dropped section: " + d).c_str());
  AssembledContext roomy = assembleContext(in, 1 << 20);  // nothing dropped
  TEST_ASSERT_FALSE(has(roomy.prompt, "## OMITTED"));
}

static void test_assemble_skips_empty_sections() {
  ContextInputs in;
  in.directive = "only a directive";
  AssembledContext c = assembleContext(in, 32768);
  TEST_ASSERT_FALSE(c.truncated);
  TEST_ASSERT_FALSE(has(c.prompt, "CAPABILITIES"));
  TEST_ASSERT_FALSE(has(c.prompt, "SESSIONS"));
  TEST_ASSERT_FALSE(has(c.prompt, "RELEVANT MEMORIES"));
  TEST_ASSERT_TRUE(has(c.prompt, "only a directive"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_capabilities_lists_present_hardware);
  RUN_TEST(test_capabilities_offline_and_absent_hardware);
  RUN_TEST(test_sessions_empty_is_blank);
  RUN_TEST(test_sessions_digest_content);
  RUN_TEST(test_assemble_orders_sections);
  RUN_TEST(test_assemble_directive_never_dropped);
  RUN_TEST(test_assemble_recall_partial_keep);
  RUN_TEST(test_assemble_recall_header_suppressed_when_no_bullet_fits);
  RUN_TEST(test_assemble_summary_clips_before_dropping);
  RUN_TEST(test_assemble_omitted_trailer_lists_drops);
  RUN_TEST(test_assemble_skips_empty_sections);
  return UNITY_END();
}
