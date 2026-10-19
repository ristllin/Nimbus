#include <unity.h>

#include "../support/utf8_check.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "nimbus/mem_cap.h"
#include "nimbus/orch/failure_rows.h"

// test_orch_failure_rows - the rows a failed turn leaves are always written, always
// within their ceilings, and always carry the first cause.
//
// The device used to build the ev:turnend row and then DROP it whenever it ran over
// 256 B (orchestrator.cpp, `if (js.length() <= ORCH_TRACE_END_MAX)`). The property
// below fuzzes the widest inputs (64 B model ids, 3 billing hosts with 47-character
// model keys and 10-digit counts, 2 KB invalid-UTF-8 errors) and requires the row to be
// produced, parse back, and fit; the counter-test shows the old builder drops them.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

static uint32_t g_seed = 0xF00Du;
static uint32_t rnd() {
  g_seed ^= g_seed << 13;
  g_seed ^= g_seed >> 17;
  g_seed ^= g_seed << 5;
  return g_seed;
}

static bool validUtf8(const std::string& s) { return testutf8::valid(s); }

static std::string randText(size_t n, bool hostile) {
  static const char* kOk = "abcdefghijklmnopqrstuvwxyz0123456789-._:/\" ";
  static const char* kBad[] = {"\xFF", "\xC3", "\xE2\x82", "\xED\xA0\x80", "\n", "\x01", "\xC3\xA9",
                               "\xF0\x9F\x98\x80", "\\", "\""};
  std::string s;
  while (s.size() < n) {
    if (hostile && rnd() % 3 == 0) s += kBad[rnd() % 10];
    else s += kOk[rnd() % std::strlen(kOk)];
  }
  return s;
}

static std::string modelKey47() {
  static const char* kSafe = "abcdefghijklmnopqrstuvwxyz0123456789-.";
  std::string s;
  const size_t n = 1 + rnd() % 47;
  while (s.size() < n) s += kSafe[rnd() % std::strlen(kSafe)];
  return s;
}

struct Fuzzed {
  TurnEndRowIn in;
  FailureChain chain;
  TurnSpend spend;
};

static void fuzz(Fuzzed& f) {
  static const char* kHosts[] = {"anthropic", "openai", "mistral", "cumulo", "zai", "custom"};
  f.chain.clear();
  f.spend.clear();
  f.in = TurnEndRowIn();
  f.in.ok = rnd() % 4 == 0;
  f.in.src = rnd() % 2 ? "dream" : "synthesis";
  f.in.host = kHosts[rnd() % 6];
  f.in.model = randText(rnd() % 65, rnd() % 2);
  f.in.fallback = rnd() % 2;
  f.in.tools = (int)(rnd() % 1000);
  f.in.in = rnd() % 2 ? 4294967295u : rnd();
  f.in.out = rnd() % 2 ? 4294967295u : rnd();
  f.in.cacheR = rnd() % 2 ? 4294967295u : 0;
  f.in.cacheW = rnd() % 2 ? 4294967295u : 0;
  f.in.reply = rnd();
  const int hosts = (int)(rnd() % 4);
  for (int h = 0; h < hosts; ++h) {
    nimbus::orch::TokenUsage u;
    u.promptTokens = 4294967295u;
    u.completionTokens = 4294967295u;
    u.cacheReadTokens = 4294967295u;
    u.cacheWriteTokens = 4294967295u;
    u.responses = 65535;
    u.servedModel = modelKey47();
    f.spend.add(kHosts[h], u.servedModel.c_str(), u);
  }
  f.in.spend = &f.spend;
  const int steps = (int)(rnd() % 4);
  for (int s = 0; s < steps; ++s)
    for (int a = 0; a < 2; ++a) f.chain.note(kHosts[s], randText(1 + rnd() % 2048, true));
  f.in.chain = rnd() % 5 ? &f.chain : nullptr;
  f.in.error = randText(rnd() % 2048, true);
}

static void test_turnend_row_always_produced_within_256_and_round_trips() {
  g_seed = 0xF00Du;
  for (int c = 0; c < 3000; ++c) {
    Fuzzed f;
    fuzz(f);
    const std::string js = turnEndRowJson(f.in, kTurnEndRowMax);
    TEST_ASSERT_FALSE(js.empty());
    TEST_ASSERT_TRUE_MESSAGE(js.size() <= kTurnEndRowMax, js.c_str());
    TEST_ASSERT_TRUE(validUtf8(js));
    JsonDocument d;
    TEST_ASSERT_TRUE_MESSAGE(deserializeJson(d, js) == DeserializationError::Ok, js.c_str());
    TEST_ASSERT_EQUAL_UINT32(f.in.in, d["in"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(f.in.out, d["out"].as<uint32_t>());
    TEST_ASSERT_EQUAL(f.in.ok, d["ok"].as<bool>());
    TEST_ASSERT_EQUAL_STRING(f.in.host.c_str(), d["host"] | "");
    // A failed turn's err names the first failing host whenever a chain exists.
    if (!f.in.ok && f.in.chain && !f.chain.empty()) {
      const char* err = d["err"] | "";
      TEST_ASSERT_EQUAL_INT_MESSAGE(0, std::strncmp(err, f.chain.first()->host.c_str(),
                                                    f.chain.first()->host.size()),
                                    js.c_str());
    }
  }
}

// Every cap from the bare minimum up: produced, within the cap, parseable.
static void test_turnend_row_fits_every_cap() {
  g_seed = 0xBEEFu;
  for (int c = 0; c < 200; ++c) {
    Fuzzed f;
    fuzz(f);
    for (size_t cap = kTurnEndRowMin; cap <= 512; cap += 8) {
      const std::string js = turnEndRowJson(f.in, cap);
      TEST_ASSERT_TRUE(js.size() <= cap);
      JsonDocument d;
      TEST_ASSERT_TRUE(deserializeJson(d, js) == DeserializationError::Ok);
    }
  }
}

// Counter-test: the pre-T builder (err capped at 96 B, then the whole row DROPPED when
// over 256 B) loses rows on these inputs; the new one never does.
static std::string oldRow(const TurnEndRowIn& r) {
  std::string err = r.error;
  err = err.substr(0, (size_t)nimbus::utf8CapLen(err.c_str(), (int)err.size(), 96));
  JsonDocument d;
  d["host"] = r.host;
  d["model"] = r.model;
  if (r.fallback) d["fallback"] = true;
  d["tools"] = r.tools;
  d["ok"] = r.ok;
  d["in"] = r.in;
  d["out"] = r.out;
  d["reply"] = (uint32_t)r.reply;
  if (!err.empty()) d["err"] = err;
  std::string js;
  serializeJson(d, js);
  return js.size() <= kTurnEndRowMax ? js : std::string();   // the silent drop
}

static void test_counter_old_builder_drops_rows() {
  g_seed = 0xF00Du;
  int dropped = 0;
  for (int c = 0; c < 500; ++c) {
    Fuzzed f;
    fuzz(f);
    f.in.ok = false;
    f.in.model = std::string(64, 'm');
    f.in.error = std::string(300, 'e') + "\"quoted\\\"";   // escapes push the JSON over
    if (oldRow(f.in).empty()) dropped++;
    TEST_ASSERT_FALSE(turnEndRowJson(f.in).empty());
  }
  TEST_ASSERT_TRUE_MESSAGE(dropped > 0, "the old builder never dropped a row: the test lost its teeth");
}

// The switched turn: a via array with both billing hosts, in the order they billed.
static void test_turnend_row_via_for_a_switched_turn() {
  TurnSpend sp;
  nimbus::orch::TokenUsage a, m;
  a.promptTokens = 900; a.completionTokens = 200; a.cacheWriteTokens = 9703; a.responses = 1;
  m.promptTokens = 1500; m.completionTokens = 80; m.responses = 1;
  sp.add("anthropic", "claude-haiku-5-5", a);
  sp.add("mistral", "mistral-small-latest", m);
  TurnEndRowIn r;
  r.ok = true;
  r.src = "owner";
  r.host = "mistral";
  r.model = "mistral-small";
  r.fallback = true;
  r.tools = 1;
  const nimbus::orch::TokenUsage t = sp.total();
  r.in = t.promptTokens; r.out = t.completionTokens; r.cacheW = t.cacheWriteTokens;
  r.reply = 120;
  r.spend = &sp;
  const std::string js = turnEndRowJson(r);
  TEST_ASSERT_EQUAL_STRING(
      "{\"host\":\"mistral\",\"model\":\"mistral-small\",\"fallback\":true,\"src\":\"owner\","
      "\"tools\":1,\"ok\":true,\"in\":2400,\"out\":280,\"cw\":9703,\"reply\":120,"
      "\"via\":[[\"anthropic\",\"claude-haiku-5-5\",900,200,0,9703],"
      "[\"mistral\",\"mistral-small\",1500,80,0,0]]}",
      js.c_str());
  // One billing host: no via.
  TurnSpend one;
  one.add("anthropic", "claude-haiku-5-5", a);
  r.spend = &one;
  TEST_ASSERT_NULL(std::strstr(turnEndRowJson(r).c_str(), "\"via\""));
}


static ErrorEventIn ev(const std::string& src, const std::string& from, const std::string& to,
                       const FailureChain* chain, const std::string& error, int calls) {
  ErrorEventIn e;
  e.src = src;
  e.requestedHost = from;
  e.finalHost = to;
  e.chain = chain;
  e.error = error;
  e.toolCalls = calls;
  return e;
}

static void test_error_event_text() {
  FailureChain ch;
  ch.note("anthropic", "messages HTTP 400: The request body is not valid JSON: invalid unicode "
                       "code point at line 1 column 55142");
  ch.note("anthropic", "messages HTTP 400: same");
  ch.note("openai", "resp HTTP 400: Invalid body");
  ch.note("mistral", "chat HTTP 400");
  const std::string t = errorEventText(ev("dream", "anthropic", "mistral", &ch, "chat HTTP 400", 4));
  TEST_ASSERT_TRUE(t.size() <= kErrorEventMax);
  TEST_ASSERT_EQUAL_INT(0, t.compare(0, 41, "Turn FAILED (dream) on anthropic -> mistr"));
  TEST_ASSERT_NOT_NULL(std::strstr(t.c_str(), ": anthropic (x2): messages HTTP 400: The request body"));
  TEST_ASSERT_NOT_NULL(std::strstr(t.c_str(), "; 4 tool calls ran"));
  TEST_ASSERT_NULL(std::strstr(t.c_str(), "round"));
  // No chain: the last error; nothing at all: said plainly.
  TEST_ASSERT_EQUAL_STRING("Turn FAILED (owner) on openai: resp HTTP 500; no tool ran",
                           errorEventText(ev("owner", "openai", "openai", nullptr, "resp HTTP 500", 0)).c_str());
  TEST_ASSERT_EQUAL_STRING("Turn FAILED on openai: cause not recorded; 1 tool call ran",
                           errorEventText(ev("", "openai", "openai", nullptr, "", 1)).c_str());
  // Hostile input stays within the cap and valid.
  g_seed = 7;
  for (int c = 0; c < 500; ++c) {
    FailureChain h;
    h.note(randText(1 + rnd() % 40, true), randText(rnd() % 2048, true));
    const std::string x = errorEventText(ev(randText(rnd() % 30, true), randText(rnd() % 30, true),
                                            randText(rnd() % 30, true), &h, randText(100, true),
                                            (int)rnd()));
    TEST_ASSERT_TRUE(x.size() <= kErrorEventMax);
    TEST_ASSERT_TRUE(validUtf8(x));
    TEST_ASSERT_EQUAL_INT(0, x.compare(0, 11, "Turn FAILED"));
  }
}

static void test_dream_row_text() {
  TEST_ASSERT_EQUAL_STRING(
      "Nightly dream FAILED (anthropic (x2): messages HTTP 400: bad body > mistral 400) - spent "
      "15438 tokens; maintenance pruned=0 deduped=0",
      dreamRowText(false, "anthropic (x2): messages HTTP 400: bad body > mistral 400", 15438, 0, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("Nightly dream ok - spent 900 tokens; maintenance pruned=3 deduped=1",
                           dreamRowText(true, "", 900, 3, 1).c_str());
  TEST_ASSERT_EQUAL_STRING(
      "Nightly dream FAILED (cause not recorded) - spent 0 tokens; maintenance pruned=0 deduped=0",
      dreamRowText(false, "", 0, 0, 0).c_str());
  const std::string longCause(2000, 'x');
  const std::string t = dreamRowText(false, longCause, 4294967295u, -5, 2147483647);
  TEST_ASSERT_TRUE(t.size() <= kErrorEventMax);
  TEST_ASSERT_NOT_NULL(std::strstr(t.c_str(), "spent 4294967295 tokens"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_turnend_row_always_produced_within_256_and_round_trips);
  RUN_TEST(test_turnend_row_fits_every_cap);
  RUN_TEST(test_counter_old_builder_drops_rows);
  RUN_TEST(test_turnend_row_via_for_a_switched_turn);
  RUN_TEST(test_error_event_text);
  RUN_TEST(test_dream_row_text);
  return UNITY_END();
}
