#include <unity.h>

#include <string>

#include "nimbus/tg_html.h"

// tg_html - Telegram HTML converter (v4.1.1). The contract under test:
// escape-first (no injection), closed-pairs-only (never unbalanced tags),
// verbatim code spans, and the plain-text fallback's precondition - the
// converter must never make a message WORSE than plain.

using nimbus::tgHtml;

void setUp() {}
void tearDown() {}

static void test_escapes_html_and_never_injects() {
  // A model reply (or recalled memory!) containing markup must arrive as text.
  TEST_ASSERT_EQUAL_STRING("&lt;script&gt;alert(1)&lt;/script&gt; &amp; more",
                           tgHtml("<script>alert(1)</script> & more").c_str());
}

static void test_bold_code_heading_link_convert() {
  TEST_ASSERT_EQUAL_STRING("<b>PDF delivered!</b>", tgHtml("**PDF delivered!**").c_str());
  TEST_ASSERT_EQUAL_STRING("see <code>files.send</code> now",
                           tgHtml("see `files.send` now").c_str());
  TEST_ASSERT_EQUAL_STRING("<b>Top stories</b>\nbody",
                           tgHtml("## Top stories\nbody").c_str());
  TEST_ASSERT_EQUAL_STRING("<a href=\"https://x.com/a\">link</a>",
                           tgHtml("[link](https://x.com/a)").c_str());
}

static void test_unclosed_markers_stay_literal() {
  // Unbalanced tags 400 the Telegram API - an unclosed pair must NOT convert.
  TEST_ASSERT_EQUAL_STRING("**half open", tgHtml("**half open").c_str());
  TEST_ASSERT_EQUAL_STRING("`no close", tgHtml("`no close").c_str());
  TEST_ASSERT_EQUAL_STRING("a ` b\nc ` d", tgHtml("a ` b\nc ` d").c_str());  // cross-line: literal
  TEST_ASSERT_EQUAL_STRING("[text](notaurl)", tgHtml("[text](notaurl)").c_str());
}

static void test_code_spans_are_verbatim() {
  // Markdown INSIDE code must not convert; markup inside code must escape.
  TEST_ASSERT_EQUAL_STRING("<code>**not bold**</code>", tgHtml("`**not bold**`").c_str());
  const std::string pre = tgHtml("```\nif (a < b) **x**;\n```");
  TEST_ASSERT_EQUAL_STRING("<pre>if (a &lt; b) **x**;</pre>", pre.c_str());
}

static void test_math_and_globs_survive() {
  // Single * is deliberately untouched; backtick-free technical text unchanged.
  TEST_ASSERT_EQUAL_STRING("2*3*4 = 24 and *.cpp files",
                           tgHtml("2*3*4 = 24 and *.cpp files").c_str());
}

static void test_utf8_passes_through() {
  TEST_ASSERT_EQUAL_STRING("✅ <b>готово</b> - done",
                           tgHtml("✅ **готово** - done").c_str());
}

static void test_a_real_reply_shape() {
  const std::string in =
      "✅ **AI Research Weekly PDF - delivered!**\n\n"
      "Saved as `ai-research-pdf/ai-research-weekly.pdf` (6.4 KB).\n"
      "### Next steps\n"
      "Reply if you want changes.";
  const std::string out = tgHtml(in);
  TEST_ASSERT_TRUE(out.find("<b>AI Research Weekly PDF - delivered!</b>") != std::string::npos);
  TEST_ASSERT_TRUE(out.find("<code>ai-research-pdf/ai-research-weekly.pdf</code>") != std::string::npos);
  TEST_ASSERT_TRUE(out.find("<b>Next steps</b>") != std::string::npos);
  TEST_ASSERT_TRUE(out.find("**") == std::string::npos);   // no leftover markers
  TEST_ASSERT_TRUE(out.find('`') == std::string::npos);
}

// Balanced-tags invariant, fuzzed lightly: whatever the input, every opened
// tag is closed (the property that keeps the HTML send from 400ing).
static void test_tags_always_balanced() {
  const char* nasty[] = {
      "** ` [ ( ``` #", "```", "**a`b**`c", "[x](https://a\").com)",
      "`a**b`c**d", "# \n## x\n**y", "***bold?***", "``", "**``**",
  };
  for (const char* s : nasty) {
    const std::string h = tgHtml(s);
    for (const char* tag : {"b", "code", "pre", "a"}) {
      size_t opens = 0, closes = 0, p = 0;
      const std::string o = std::string("<") + tag, c = std::string("</") + tag + ">";
      while ((p = h.find(o, p)) != std::string::npos) {
        if (h[p + o.size()] == '>' || h[p + o.size()] == ' ') opens++;
        p++;
      }
      p = 0;
      while ((p = h.find(c, p)) != std::string::npos) { closes++; p++; }
      TEST_ASSERT_EQUAL_MESSAGE(opens, closes, s);
    }
  }
}


// ⚠ prism (silent reply loss): a line-start ``` with CONTENT on the same line is
// NOT a fence - treating it as one swallowed everything up to the NEXT fence
// into <pre>, dropping the in-between bytes, and the emitted HTML was balanced
// so the plain-text fallback never fired. Only ```[bare-lang]\n opens a block.
static void test_inline_triple_backtick_is_not_a_fence() {
  const std::string in =
    "```json {\"a\":1}``` and more\ntext\n```\nreal\n```";
  const std::string out = tgHtml(in);
  // Nothing may vanish: the "and more" and "text" bytes survive.
  TEST_ASSERT_TRUE(out.find("and more") != std::string::npos);
  TEST_ASSERT_TRUE(out.find("text") != std::string::npos);
  // The REAL fence at the end still converts.
  TEST_ASSERT_TRUE(out.find("<pre>real</pre>") != std::string::npos);
  // A legitimate language tag still opens a fence.
  TEST_ASSERT_EQUAL_STRING("<pre>x=1</pre>", tgHtml("```python\nx=1\n```").c_str());
}

// CUM-995 (lane MD51): escaping cases from REAL replies. The 10 Nov food-log
// reply (evidence/lumi-v500-issue-20261110T172807Z/telegram-message-as-received.txt,
// the clean reply half) is numbers, commas and slashes - it must pass through
// byte-identical (no marker in it may convert or vanish).
static void test_real_10nov_food_log_reply_passes_through() {
  const std::string in =
      "Logged: snack-1723, Frive Firecracker Chicken Thighs (whole 388 g pack, "
      "label per-serving) = 563 kcal / 39.3 g protein.\n\n"
      "Today: 2,385 kcal / 106.4 g protein (5 rows). Left: 15 kcal and 43.6 g "
      "protein. Undo by re-sending the row with 0 kcal.";
  TEST_ASSERT_EQUAL_STRING(in.c_str(), tgHtml(in).c_str());
}

// CUM-995 (lane MD51): a real reply carrying *, _, [ ] and code (corpus shapes:
// 2026-10-30T19:30-120's `nutrition/meal_log.csv` + numbered list + **bold**,
// 2026-10-28T18:10-82's *speculative decoding*). The contract: **bold** and
// `code` convert; single * and intra-word _ stay literal; only an http(s)
// [text](url) converts.
static void test_real_reply_with_markers_and_code() {
  const std::string in =
      "The log is saved in `nutrition/meal_log.csv`.\n\n"
      "**Today's total**: **1,547 kcal, 176 g protein**.\n\n"
      "The check-in covered *speculative decoding*:\n"
      "- Memory-bound decoding, rejection sampling r<=p/q\n\n"
      "1. Apply the addenda rule to branded items\n"
      "2. Use the science skill for portions\n\n"
      "See [the docs](https://docs.cumulo-nimbus.ai/) and [not a link](ftp://x).";
  const std::string out = tgHtml(in);
  TEST_ASSERT_TRUE(out.find("<code>nutrition/meal_log.csv</code>") != std::string::npos);
  TEST_ASSERT_TRUE(out.find("<b>1,547 kcal, 176 g protein</b>") != std::string::npos);
  TEST_ASSERT_TRUE(out.find("*speculative decoding*") != std::string::npos);   // single * literal
  TEST_ASSERT_TRUE(out.find("r&lt;=p/q") != std::string::npos);                 // < escaped, text intact
  TEST_ASSERT_TRUE(out.find("<a href=\"https://docs.cumulo-nimbus.ai/\">the docs</a>") != std::string::npos);
  TEST_ASSERT_TRUE(out.find("[not a link](ftp://x)") != std::string::npos);     // non-http stays literal
  // No raw markers leaked into tags: every ** pair converted, none left dangling.
  TEST_ASSERT_TRUE(out.find("**") == std::string::npos);
  // And the whole thing stays balanced (the property that avoids the 400).
  for (const char* tag : {"b", "code", "pre", "a"}) {
    size_t opens = 0, closes = 0, p = 0;
    const std::string o = std::string("<") + tag, c = std::string("</") + tag + ">";
    while ((p = out.find(o, p)) != std::string::npos) {
      if (out[p + o.size()] == '>' || out[p + o.size()] == ' ') opens++;
      p++;
    }
    p = 0;
    while ((p = out.find(c, p)) != std::string::npos) { closes++; p++; }
    TEST_ASSERT_EQUAL(opens, closes);
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_escapes_html_and_never_injects);
  RUN_TEST(test_bold_code_heading_link_convert);
  RUN_TEST(test_unclosed_markers_stay_literal);
  RUN_TEST(test_code_spans_are_verbatim);
  RUN_TEST(test_math_and_globs_survive);
  RUN_TEST(test_utf8_passes_through);
  RUN_TEST(test_a_real_reply_shape);
  RUN_TEST(test_tags_always_balanced);
  RUN_TEST(test_inline_triple_backtick_is_not_a_fence);
  RUN_TEST(test_real_10nov_food_log_reply_passes_through);
  RUN_TEST(test_real_reply_with_markers_and_code);
  return UNITY_END();
}
