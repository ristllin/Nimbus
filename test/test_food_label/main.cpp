#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/orch/food_label.h"

// The deterministic nutrition-label check (food_label.h). The transcriptions are
// the five real label photos of 2 Nov 2026 (evidence/lumi-archive-2026-11-02,
// private) rewritten as the vision fallback transcribes them: a table with a
// "Per 100g / Per serving" header, kJ/kcal pairs, gram-suffixed figures and the
// net weight line. Ground truth was read off the photos themselves.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// 08:15 photo: pack, net 243 g.
static const char kT899[] =
    "Nutritional values Per 100g Per serving\n"
    "Energy kJ/kcal 565/135 1369/327\n"
    "Fat 5.0g 12g\n"
    "of which saturates 1.4g 3.4g\n"
    "Carbohydrate 8.2g 19.8g\n"
    "of which sugars 2.3g 5.7g\n"
    "Protein 13.1g 31.8g\n"
    "Salt 0.60g 1.47g\n"
    "Fibre 0.5g 1.2g\n"
    "Net 243 g";
// 13:08 photo: side, net 131 g. The label reads 41.0 g protein per serving.
static const char kT904[] =
    "Nutritional values Per 100g Per serving\n"
    "Energy kJ/kcal 750/179 982/235\n"
    "Fat 5.6g 7.3g\n"
    "of which saturates 2.2g 2.8g\n"
    "Carbohydrate 0.9g 1.2g\n"
    "of which sugars 0.0g 0.0g\n"
    "Protein 31.3g 41.0g\n"
    "Salt 0.7g 0.93g\n"
    "Fibre 0.5g 0.6g\n"
    "131g e";
// 13:08 photo: bowl, net 388 g.
static const char kT905[] =
    "Nutritional values Per 100g Per serving\n"
    "Energy kJ/kcal 600/143 2329/557\n"
    "Fat 5g 22.3g\n"
    "of which saturates 1.3g 5.2g\n"
    "Carbohydrate 11.5g 44.5g\n"
    "of which sugars 2.6g 10.1g\n"
    "Protein 10.0g 38.7g\n"
    "Salt 0.59g 2.30g\n"
    "Fibre 1.4g 5.3g\n"
    "388g e";
// 17:32 photo: masala, net 390 g.
static const char kT907[] =
    "Nutritional values Per 100g Per serving\n"
    "Energy kJ/kcal 655/157 2558/611\n"
    "Fat 5.4g 21.0g\n"
    "of which saturates 1.9g 7.5g\n"
    "Carbohydrate 12.8g 50.0g\n"
    "of which sugars 3.1g 12.0g\n"
    "Protein 14.1g 54.9g\n"
    "Salt 0.56g 2.19g\n"
    "Fibre 1.2g 4.7g\n"
    "390g e";
// 17:32 photo: waffles, net 172 g.
static const char kT908[] =
    "Nutritional values Per 100g Per serving\n"
    "Energy kJ/kcal 675/161 1159/277\n"
    "Fat 4.0g 6.9g\n"
    "of which saturates 1.2g 2.1g\n"
    "Carbohydrate 26.0g 44.7g\n"
    "of which sugars 14.7g 25.2g\n"
    "Protein 3.3g 5.7g\n"
    "Salt 0.06g 0.11g\n"
    "Fibre 4.0g 6.9g\n"
    "172g e";

static void expectRead(const char* text, double kcal100, double prot100, double kcalSrv, double protSrv,
                       double net) {
  const LabelRead r = parseLabelRead(text);
  TEST_ASSERT_TRUE(r.hasPer100);
  TEST_ASSERT_TRUE(r.hasPerServing);
  TEST_ASSERT_TRUE(r.per100.hasKcal);
  TEST_ASSERT_TRUE(r.per100.hasProtein);
  TEST_ASSERT_TRUE(r.perServing.hasKcal);
  TEST_ASSERT_TRUE(r.perServing.hasProtein);
  TEST_ASSERT_EQUAL_FLOAT(kcal100, r.per100.kcal);
  TEST_ASSERT_EQUAL_FLOAT(prot100, r.per100.protein);
  TEST_ASSERT_EQUAL_FLOAT(kcalSrv, r.perServing.kcal);
  TEST_ASSERT_EQUAL_FLOAT(protSrv, r.perServing.protein);
  TEST_ASSERT_TRUE(r.netKnown);
  TEST_ASSERT_EQUAL_FLOAT(net, r.netG);
}

// ---- parsing: the five real label shapes -------------------------------------------
static void test_parse_899() { expectRead(kT899, 135, 13.1, 327, 31.8, 243); }
static void test_parse_904() { expectRead(kT904, 179, 31.3, 235, 41.0, 131); }
static void test_parse_905() { expectRead(kT905, 143, 10.0, 557, 38.7, 388); }
static void test_parse_907() { expectRead(kT907, 157, 14.1, 611, 54.9, 390); }
static void test_parse_908() { expectRead(kT908, 161, 3.3, 277, 5.7, 172); }

// The macros come out too, so Atwater can run on a full read.
static void test_parse_macros_907() {
  const LabelRead r = parseLabelRead(kT907);
  TEST_ASSERT_EQUAL_FLOAT(5.4, r.per100.fat);
  TEST_ASSERT_EQUAL_FLOAT(12.8, r.per100.carb);
  TEST_ASSERT_EQUAL_FLOAT(21.0, r.perServing.fat);
  TEST_ASSERT_EQUAL_FLOAT(50.0, r.perServing.carb);
}

// A kJ/kcal pair is the kcal value; "[image 1/2]" is not an energy pair.
static void test_pair_vs_image_tag() {
  const LabelRead r = parseLabelRead("[image 1/2 ref=0123456789abcdef] a plate of pasta, 600/143 kcal per 100 g");
  TEST_ASSERT_TRUE(r.per100.hasKcal);
  TEST_ASSERT_EQUAL_FLOAT(143, r.per100.kcal);
  const LabelRead img = parseLabelRead("[image 1/2 ref=0123456789abcdef] (attached)");
  TEST_ASSERT_FALSE(img.hasPer100);
  TEST_ASSERT_FALSE(img.hasPerServing);
}

// ---- the checks: every correct read passes ------------------------------------------
static void test_correct_reads_pass() {
  for (const char* t : {kT899, kT904, kT905, kT907, kT908}) {
    const std::vector<LabelDoubt> d = checkLabelRead(parseLabelRead(t));
    TEST_ASSERT_TRUE_MESSAGE(d.empty(), t);
  }
}

// A swapped column fails the cross check.
static void test_swapped_columns_doubted() {
  const LabelRead r = parseLabelRead(
      "per 100 g: 235 kcal, 41.0 g protein\nper serving: 179 kcal, 31.3 g protein\nnet 131 g");
  const std::vector<LabelDoubt> d = checkLabelRead(r);
  TEST_ASSERT_FALSE(d.empty());
  bool cross = false;
  for (const LabelDoubt& x : d)
    if (x.figure.find("vs per 100 g") != std::string::npos) cross = true;
  TEST_ASSERT_TRUE(cross);
}

// ---- the store-time gate -------------------------------------------------------------
// The 2 Nov failure: the label says 41.0 g protein per serving; the row stored 4.3.
static const char kRow904Bad[] =
    "2026-11-02,snack2,\"Grilled Chicken Breast Side, full 131 g pack eaten whole "
    "[label per serving = whole pack: 235 kcal, 4.3 g protein]\",235,4.3";
static const char kRow904Good[] =
    "2026-11-02,snack2,\"Grilled Chicken Breast Side, full 131 g pack eaten whole "
    "[label per serving = whole pack: 235 kcal, 41.0 g protein]\",235,41.0";

static void test_gate_refuses_the_43_g_row() {
  const std::string refusal = labelRowGate(kRow904Bad, std::string("[image 1/1 ref=aaaa] ") + kT904);
  TEST_ASSERT_FALSE(refusal.empty());
  TEST_ASSERT_TRUE_MESSAGE(refusal.find("protein per serving vs per 100 g") != std::string::npos, refusal.c_str());
  TEST_ASSERT_TRUE_MESSAGE(refusal.find("4.3") != std::string::npos, refusal.c_str());
  TEST_ASSERT_TRUE_MESSAGE(refusal.find("41") != std::string::npos, refusal.c_str());
  TEST_ASSERT_TRUE_MESSAGE(refusal.find("Nothing was written") != std::string::npos, refusal.c_str());
}

static void test_gate_lets_the_410_g_row_through() {
  const std::string refusal = labelRowGate(kRow904Good, std::string("[image 1/1 ref=aaaa] ") + kT904);
  TEST_ASSERT_TRUE_MESSAGE(refusal.empty(), refusal.c_str());
}

// The re-read-then-store path: a row marked [unverified: ...] is let through.
static void test_gate_lets_unverified_through() {
  const std::string row =
      "2026-11-02,snack2,\"Grilled Chicken Breast Side, full 131 g pack [label per serving = whole pack: 235 kcal, "
      "4.3 g protein] [unverified: protein per serving]\",235,4.3";
  const std::string refusal = labelRowGate(row, std::string("[image 1/1 ref=aaaa] ") + kT904);
  TEST_ASSERT_TRUE_MESSAGE(refusal.empty(), refusal.c_str());
}

// A row with no label figures is not gated.
static void test_gate_ignores_plain_rows() {
  const std::string row = "2026-11-02,drink,\"Cappuccino (100 ml full milk)\",66,3.3";
  TEST_ASSERT_TRUE(labelRowGate(row, kT904).empty());
}

// The other four labels' correct rows pass against their own transcriptions.
static void test_gate_other_labels_pass() {
  const std::pair<const char*, const char*> cases[] = {
      {"2026-11-02,breakfast,\"Turkey Bacon Bean Egg Mushrooms, full 243 g pack [label per serving = whole pack: "
       "327 kcal, 31.8 g protein]\",327,31.8", kT899},
      {"2026-11-02,dinner,\"Chimichurri Beef Bowl, full 388 g pack [label per serving = whole pack: 557 kcal, 38.7 g "
       "protein]\",557,38.7", kT905},
      {"2026-11-02,lunch1,\"Chicken Tikka Masala, full 390 g pack [label per serving = whole pack: 611 kcal, 54.9 g "
       "protein]\",611,54.9", kT907},
      {"2026-11-02,snack3,\"Waffles with Compote and Custard, full 172 g pack [label per serving = whole pack: 277 "
       "kcal, 5.7 g protein]\",277,5.7", kT908},
  };
  for (const auto& c : cases) {
    const std::string refusal = labelRowGate(c.first, std::string("[image 1/1 ref=bbbb] ") + c.second);
    TEST_ASSERT_TRUE_MESSAGE(refusal.empty(), refusal.c_str());
  }
}

// A kcal that fails Atwater on its own (the 4.3 g row's 88 kcal class) is refused
// even when the columns agree with each other.
static void test_gate_atwater_only() {
  const std::string row =
      "2026-11-02,snack2,\"Grilled Chicken Breast Side, full 131 g pack [label per serving = whole pack: 50 kcal, "
      "4.3 g protein]\",50,4.3";
  const std::string refusal = labelRowGate(row, std::string("[image 1/1 ref=aaaa] ") + kT904);
  TEST_ASSERT_FALSE(refusal.empty());
  TEST_ASSERT_TRUE_MESSAGE(refusal.find("4P+4C+9F") != std::string::npos, refusal.c_str());
}

// Two labels in one turn: the row is checked against its own pack's read (net
// weight match), not the other photo's.
static void test_gate_picks_the_matching_label() {
  const std::string sources = std::string("[image 1/2 ref=aaaa] ") + kT904 + "\n[image 2/2 ref=bbbb] " + kT905;
  TEST_ASSERT_TRUE(labelRowGate(kRow904Good, sources).empty());
  TEST_ASSERT_FALSE(labelRowGate(kRow904Bad, sources).empty());
}

// ---- the second read -----------------------------------------------------------------
static void test_second_read_disagreement_surfaced() {
  const std::string second = std::string(kT904).replace(
      std::string(kT904).find("41.0g"), 4, "4.3g");
  const std::vector<FigureDisagreement> d = labelReadDisagreements(kT904, second);
  TEST_ASSERT_EQUAL(1, (int)d.size());
  TEST_ASSERT_TRUE(d[0].field.find("protein per serving") != std::string::npos);
  TEST_ASSERT_EQUAL_FLOAT(41.0, d[0].head);
  TEST_ASSERT_EQUAL_FLOAT(4.3, d[0].second);
  const std::string note = secondReadNote(kT904, second, "openai");
  TEST_ASSERT_TRUE(note.find("second read (openai)") != std::string::npos);
  TEST_ASSERT_TRUE(note.find("41") != std::string::npos && note.find("4.3") != std::string::npos);
  TEST_ASSERT_TRUE(note.find("never averaged") != std::string::npos);
}

static void test_second_read_agreement_is_quiet() {
  TEST_ASSERT_TRUE(labelReadDisagreements(kT904, kT904).empty());
  TEST_ASSERT_TRUE(secondReadNote(kT904, kT904, "openai").empty());
}

// ---- provenance ----------------------------------------------------------------------
static void test_unsupported_brand_named() {
  const std::string row =
      "2026-11-02,dinner,\"M&S Chimichurri Beef Bowl, full 388 g pack [label per serving = whole pack: 557 kcal, "
      "38.7 g protein]\",557,38.7";
  // The turn's sources hold the Frive label read and the owner's words, no M&S.
  const std::string sources = "chimichurri beef bowl frive 388 g 557 kcal 38.7 g protein per serving";
  const std::string unsup = unsupportedDetails(row, sources);
  TEST_ASSERT_TRUE_MESSAGE(unsup.find("m s") != std::string::npos, unsup.c_str());
}

static void test_supported_brand_passes() {
  const std::string row =
      "2026-11-02,dinner,\"M&S Chimichurri Beef Bowl, full 388 g pack [label per serving = whole pack: 557 kcal, "
      "38.7 g protein]\",557,38.7";
  const std::string sources = "owner said: the M&S pack you photographed, 388 g";
  TEST_ASSERT_TRUE(unsupportedDetails(row, sources).empty());
}

static void test_assumed_row_passes() {
  const std::string row =
      "2026-11-02,dinner,\"M&S Chimichurri Beef Bowl, full 388 g pack [assumed: brand M&S]\",557,38.7";
  TEST_ASSERT_TRUE(unsupportedDetails(row, "chimichurri beef bowl 388 g").empty());
}

static void test_plain_words_are_not_brands() {
  const std::string row = "2026-11-02,breakfast,\"Turkey Bacon Bean Egg Mushrooms, full 243 g pack\",327,31.8";
  TEST_ASSERT_TRUE(unsupportedDetails(row, "").empty());
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_parse_899);
  RUN_TEST(test_parse_904);
  RUN_TEST(test_parse_905);
  RUN_TEST(test_parse_907);
  RUN_TEST(test_parse_908);
  RUN_TEST(test_parse_macros_907);
  RUN_TEST(test_pair_vs_image_tag);
  RUN_TEST(test_correct_reads_pass);
  RUN_TEST(test_swapped_columns_doubted);
  RUN_TEST(test_gate_refuses_the_43_g_row);
  RUN_TEST(test_gate_lets_the_410_g_row_through);
  RUN_TEST(test_gate_lets_unverified_through);
  RUN_TEST(test_gate_ignores_plain_rows);
  RUN_TEST(test_gate_other_labels_pass);
  RUN_TEST(test_gate_atwater_only);
  RUN_TEST(test_gate_picks_the_matching_label);
  RUN_TEST(test_second_read_disagreement_surfaced);
  RUN_TEST(test_second_read_agreement_is_quiet);
  RUN_TEST(test_unsupported_brand_named);
  RUN_TEST(test_supported_brand_passes);
  RUN_TEST(test_assumed_row_passes);
  RUN_TEST(test_plain_words_are_not_brands);
  return UNITY_END();
}
