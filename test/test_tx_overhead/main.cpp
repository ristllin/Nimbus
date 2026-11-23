// test_tx_overhead - text added around messages cannot pass unseen (PRD section 2 and
// section 11, rule 3 against prompt-patching; layer L1; lane RB-B0b writes it). Two
// properties. Bytes: the rendered body minus the stored text it shows minus the system text
// minus the tools is at most 96 B per owner message and 64 B per photo label, plus a fixed
// framing allowance per tool call, result and extra text block and one 512 B envelope
// (model, max_tokens, thinking, tool_choice, cache marks) that does not grow with the
// conversation. Text: every text block is stored text, so a RUNNING MEMORY, a ledger block
// or a reminder written around messages fails however much framing slack is left. Wire
// level per dialect (RB-W1a, RB-W2, RB-W3, RB-W4); real turns through the engine (RB-E1a).
// Both checkers are counter-tested first.
//
// Owner: the battery (code-owner review to edit). Lanes may add tests.
#include <unity.h>

#include <string>
#include <vector>

#include "../support/tx_battery/fixtures.h"
#include "../support/tx_battery/pending.h"
#include "../support/tx_battery/props.h"
#include "../support/tx_battery/render.h"
#include "../support/tx_battery/toy_render.h"

namespace tx = nimbus::orch::tx;
namespace plain = agent::plain;
namespace sp = harness_test::scripted;

void setUp() {}
void tearDown() {}

namespace {

// ---- counter-tests ---------------------------------------------------------------------
txb::OverheadInput day(size_t owners, size_t tools) {
  txb::OverheadInput in;
  in.itemBytes = 40000;
  in.systemBytes = 7000;
  in.toolsBytes = 9000;
  in.ownerMessages = owners;
  in.stampBytes = owners * 22;   // '[2026-11-17 Tue 13:02]', the plain form
  in.calls = in.results = owners * tools;
  in.textBlocks = owners;
  return in;
}

void test_checker_allows_framing_and_rejects_an_injected_block() {
  txb::OverheadInput in = day(20, 2);
  long over = 0;
  std::string why;
  const long bound = txb::overheadBound(in);
  TEST_ASSERT_EQUAL(20 * 96 + 40 * 96 + 40 * 80 + 20 * 48 + 512, (int)bound);
  in.bodyBytes = 40000 + 7000 + 9000 + (size_t)bound;
  TEST_ASSERT_TRUE_MESSAGE(txb::overheadWithin(in, over, why), why.c_str());
  in.bodyBytes += 1;
  TEST_ASSERT_FALSE(txb::overheadWithin(in, over, why));
  // Text added around every owner message, beyond each block's framing, is caught.
  for (size_t owners : {2, 6, 30, 150}) {
    in = day(owners, 1);
    in.bodyBytes = in.itemBytes + in.systemBytes + in.toolsBytes + (size_t)txb::overheadBound(in) - 512 + owners * 300;
    why.clear();
    TEST_ASSERT_FALSE_MESSAGE(txb::overheadWithin(in, over, why), "a 300 B block per message passed");
  }
  // Photos earn their 104 B label (lane E2-FIX4's true line), not more.
  // The night form earns its own 51 B, the no-clock form its 14 B: exactly, nothing looser.
  in = day(1, 0);
  in.stampBytes = 51;
  in.bodyBytes = in.itemBytes + in.systemBytes + in.toolsBytes + 74 + 51 + 48 + 512;
  TEST_ASSERT_TRUE_MESSAGE(txb::overheadWithin(in, over, why), why.c_str());
  in.bodyBytes += 1;
  TEST_ASSERT_FALSE(txb::overheadWithin(in, over, why));
  in = day(1, 0);
  in.photoLabels = 4;
  in.bodyBytes = in.itemBytes + in.systemBytes + in.toolsBytes + 96 + 48 + 4 * txb::kOverheadPerPhotoLabel + 512;
  why.clear();
  TEST_ASSERT_TRUE_MESSAGE(txb::overheadWithin(in, over, why), why.c_str());
  in.bodyBytes += 1;
  TEST_ASSERT_FALSE(txb::overheadWithin(in, over, why));
}

void test_json_string_bytes_count_escapes() {
  TEST_ASSERT_EQUAL(2, (int)txb::jsonStringBytes(""));
  TEST_ASSERT_EQUAL(7, (int)txb::jsonStringBytes("hello"));
  TEST_ASSERT_EQUAL(6, (int)txb::jsonStringBytes("a\nb"));
  TEST_ASSERT_EQUAL(6, (int)txb::jsonStringBytes("a\"b"));
}

// Text written around messages that no stored item holds is caught, whatever the slack.
void test_checker_finds_text_no_item_holds() {
  txb::Convo c;
  for (int i = 0; i < 3; ++i) c.ownerTurn("question " + std::to_string(i), "answer " + std::to_string(i));
  c.openTurn();
  c.owner("photo time", {"pab12cd34"});
  txb::CoverInput in;
  in.items = &c.items;
  in.bulk = &c.bulk;
  in.openTurn = c.turn;
  sp::RequestView v = txb::toyRender(in, txb::Mutation::None, 1);
  v.messages.back().blocks.push_back(sp::detail::textBlock("[photo pab12cd34 Tue 17 Nov 13:02: shown earlier in this conversation; not attached to this request]"));
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(txb::noUnstoredText(v, c.items, why), why.c_str());
  const char* injected[] = {"RUNNING MEMORY: the owner likes skyr.", "Remember to log every meal.",
                            "[device] recent actions: L00A3 skyr"};
  for (const char* text : injected) {
    sp::RequestView bad = v;
    bad.messages.back().blocks.back().text += std::string(" ") + text;
    why.clear();
    TEST_ASSERT_FALSE_MESSAGE(txb::noUnstoredText(bad, c.items, why), text);
    bad = v;
    bad.messages.front().blocks.push_back(sp::detail::textBlock(text));
    why.clear();
    TEST_ASSERT_FALSE_MESSAGE(txb::noUnstoredText(bad, c.items, why), text);
  }
}

// The stamps are taken out exactly as RB-REC5 makes them (ruling 68): the night form and the
// no-clock form pass, any other time label (the old form, a second stamp) is caught.
void test_checker_takes_out_each_stamp_form_exactly() {
  const uint64_t night = txb::kT0 + 11 * 3600 + 28 * 60;   // Wed 18 Nov 00:30, before the 04:00 day start
  for (uint64_t t : {txb::kT0, night, (uint64_t)0}) {
    tx::Item u;
    u.kind = tx::ItemKind::User;
    u.q = 1;
    u.t = t;
    u.x = "when is it?";
    const std::string stamp = txb::stampOf(u, 240);
    TEST_ASSERT_TRUE(!stamp.empty());
    sp::RequestView v;
    v.parsed = true;
    v.messages.push_back(sp::Message{"user", {sp::detail::textBlock(stamp + " " + u.x)}});
    std::string why;
    TEST_ASSERT_TRUE_MESSAGE(txb::noUnstoredText(v, {u}, why), (stamp + ": " + why).c_str());
    for (const char* extra : {"[Tue 17 Nov 2026 13:02]", "[2026-11-17 Tue 13:02]", "(day 2026-11-17 until 04:00)"}) {
      sp::RequestView bad = v;
      bad.messages[0].blocks[0].text = std::string(extra) + " " + bad.messages[0].blocks[0].text;
      TEST_ASSERT_FALSE_MESSAGE(txb::noUnstoredText(bad, {u}, why), extra);
    }
  }
}

// ---- wire level ------------------------------------------------------------------------
size_t base64Bytes(size_t n) { return 4 * ((n + 2) / 3); }

// shift: seconds added to every stored time (11 h 28 m puts the day's turns between midnight
// and the 04:00 day start: every owner stamp takes the night form).
constexpr uint64_t kNightShiftS = 11 * 3600 + 28 * 60;   // the day's turns at 00:30 to 00:50
// dayStartMin: the turn's day start, on the wire's TurnCtx and on the checkers alike (ruling 79: a wire
// that stamps with a fixed 04:00 fails at any other day start).
void wireOverhead(plain::Dialect d, uint64_t shift = 0, uint16_t dayStartMin = 240) {
  txb::RenderFixture fx(d);
  fx.turn.dayStartMin = dayStartMin;
  const size_t photo = base64Bytes(fx.photoBytes.begin()->second.size());
  for (txb::Snapshot s : txb::dayOfFourTurns(d)) {
    for (tx::Item& it : s.c.items)
      if (it.t) it.t += shift;
    txb::Rendered r;
    TXB_OK(txb::renderItems(fx, s.c, 0, r, s.openTurn));
    txb::CoverInput cov;
    cov.items = &s.c.items;
    cov.bulk = &s.c.bulk;
    cov.openTurn = s.openTurn;
    cov.dayStartMin = dayStartMin;
    txb::OverheadInput in;
    txb::renderedText(s.c, cov, in);
    // A file reference carries no pixels in the body; an inline photo carries its base64.
    if (r.body.find("file_pab12cd34") == std::string::npos) in.itemBytes += txb::detail::countImages(r.view) * photo;
    in.bodyBytes = r.body.size();
    in.systemBytes = txb::systemJsonBytes(r.view);
    in.toolsBytes = txb::toolsJsonBytes(r.view);
    long over = 0;
    std::string why;
    TEST_ASSERT_TRUE_MESSAGE(txb::overheadWithin(in, over, why), (s.name + ": " + why).c_str());
    TEST_ASSERT_TRUE_MESSAGE(txb::noUnstoredText(r.view, s.c.items, why, dayStartMin), (s.name + ": " + why).c_str());
  }
}
// The night window at the default day start, and at two others (00:00: every stamp plain; 05:00).
void wireOverheadAtNight(plain::Dialect d) {
  for (uint16_t ds : {(uint16_t)240, (uint16_t)0, (uint16_t)300}) wireOverhead(d, kNightShiftS, ds);
}

void test_wire_overhead_anthropic() {
  TXB_WAITS_FOR("PENDING RB-W1a", "RB-T1");
  wireOverhead(plain::Dialect::AnthropicMessages);
}
void test_wire_overhead_responses() {
  TXB_WAITS_FOR("PENDING RB-W2", "RB-T1");
  wireOverhead(plain::Dialect::OpenAIResponses);
}
void test_wire_overhead_chat() {
  TXB_WAITS_FOR("PENDING RB-W3", "RB-T1");
  wireOverhead(plain::Dialect::ChatCompletions);
}
void test_wire_overhead_flat() {
  TXB_WAITS_FOR("PENDING RB-W4", "RB-T1");
  wireOverhead(plain::Dialect::Flat);
}

// Owner messages inside the night window (ruling 68): the same day of turns at 00:30 to 00:50.
void test_wire_overhead_night_anthropic() {
  TXB_WAITS_FOR("PENDING RB-W1a", "RB-T1");
  wireOverheadAtNight(plain::Dialect::AnthropicMessages);
}
void test_wire_overhead_night_responses() {
  TXB_WAITS_FOR("PENDING RB-W2", "RB-T1");
  wireOverheadAtNight(plain::Dialect::OpenAIResponses);
}
void test_wire_overhead_night_chat() {
  TXB_WAITS_FOR("PENDING RB-W3", "RB-T1");
  wireOverheadAtNight(plain::Dialect::ChatCompletions);
}
void test_wire_overhead_night_flat() {
  TXB_WAITS_FOR("PENDING RB-W4", "RB-T1");
  wireOverheadAtNight(plain::Dialect::Flat);
}

// ---- engine level ----------------------------------------------------------------------
// Twelve real turns (tools, outside content, gaps): the last request's framing is bounded
// and every text in it is stored text.
void engineFraming(uint64_t start) {
  txb::Battery b;
  b.epoch = start;
  b.world.files["notes/boat.md"] = "Nereid II, 9.5 m";
  std::vector<txb::TurnScript> turns;
  for (int i = 0; i < 12; ++i) {
    const std::string k = std::to_string(i);
    turns.push_back(i % 3 == 0   ? txb::withTool(60, "boat " + k, "read_file", R"({"path":"notes/boat.md"})", "Nereid " + k)
                    : i % 3 == 1 ? txb::withTool(400, "search " + k, "web_search", "{\"q\":\"s" + k + "\"}", "found " + k)
                                 : txb::chat(30, "chat " + k, "reply " + k));
  }
  turns.push_back(txb::chat(60, "last one", "bye"));
  TXB_OK(txb::runTurns(b, turns));
  txb::Convo c;
  TXB_OK(b.storedBeforeLastFinal(c.items));
  const txb::CoverInput cov = b.coverOf(c.items);
  txb::OverheadInput in;
  txb::renderedText(c, cov, in);
  const sp::Recorded& last = b.model.last();
  in.bodyBytes = last.body.size();
  in.systemBytes = txb::systemJsonBytes(last.view);
  in.toolsBytes = txb::toolsJsonBytes(last.view);
  long over = 0;
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(txb::overheadWithin(in, over, why), why.c_str());
  TEST_ASSERT_TRUE_MESSAGE(txb::noUnstoredText(last.view, c.items, why), why.c_str());
}

void test_engine_requests_stay_within_the_framing() {
  TXB_WAITS_FOR("PENDING RB-E1a");
  engineFraming(txb::kT0);
}

// The same turns starting at 23:40: most owner messages fall between midnight and the day start.
void test_engine_requests_stay_within_the_framing_at_night() {
  TXB_WAITS_FOR("PENDING RB-E1a");
  engineFraming(txb::kT0 + 10 * 3600 + 38 * 60);
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_checker_allows_framing_and_rejects_an_injected_block);
  RUN_TEST(test_json_string_bytes_count_escapes);
  RUN_TEST(test_checker_finds_text_no_item_holds);
  RUN_TEST(test_wire_overhead_anthropic);
  RUN_TEST(test_wire_overhead_responses);
  RUN_TEST(test_wire_overhead_chat);
  RUN_TEST(test_wire_overhead_flat);
  RUN_TEST(test_checker_takes_out_each_stamp_form_exactly);
  RUN_TEST(test_wire_overhead_night_anthropic);
  RUN_TEST(test_wire_overhead_night_responses);
  RUN_TEST(test_wire_overhead_night_chat);
  RUN_TEST(test_wire_overhead_night_flat);
  RUN_TEST(test_engine_requests_stay_within_the_framing);
  RUN_TEST(test_engine_requests_stay_within_the_framing_at_night);
  return UNITY_END();
}
