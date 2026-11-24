// test_wire_shape - the documented-shape checkers of the Responses and Chat dialects (router
// design section 11, layer L2; lane E2-R3b), counter-tested before they judge anything:
//   - each checker accepts one body of the documented shape and refuses every planted fault: a
//     call with no result, a result with no call, a wrong id form, a forbidden or unknown field,
//     a wrong role or order, empty text, a refused image format, bad bytes;
//   - every committed route golden request (test/support/goldens/wire/*/*.req) passes the checker
//     of its signature's dialect, and every Messages golden is refused by both new checkers (a
//     body of the wrong dialect never slips through). The Messages checker itself lives in
//     test_plain_wire_anthropic.
#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../support/wire_shape/chat_shape.h"
#include "../support/wire_shape/responses_shape.h"

using wire_shape::ChatFlavor;
using wire_shape::Request;

void setUp() {}
void tearDown() {}

namespace {

Request req(const char* path, const std::string& body, bool key = true) {
  Request r;
  r.path = path;
  r.body = body;
  r.headers = {{"Content-Type", "application/json"}};
  if (key) r.headers.push_back({"Authorization", "Bearer fixture-key-not-a-secret"});
  return r;
}

struct Fault {
  const char* from;
  const char* to;
  const char* what;
};

using Check = bool (*)(const std::string&, std::string&);

std::string patched(const std::string& good, const Fault& f) {
  std::string body = good;
  const size_t at = body.find(f.from);
  TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos, (std::string("anchor not found: ") + f.what).c_str());
  return body.replace(at, std::strlen(f.from), f.to);
}

// The good body passes; each fault applied to it is refused with a reason.
void eachFaultRefused(const std::string& good, const std::vector<Fault>& faults, Check check) {
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(check(good, why), ("the good body is refused: " + why).c_str());
  for (const Fault& f : faults) {
    why.clear();
    TEST_ASSERT_FALSE_MESSAGE(check(patched(good, f), why), (std::string("accepted a body with ") + f.what).c_str());
    TEST_ASSERT_FALSE_MESSAGE(why.empty(), f.what);
    if (std::getenv("WIRE_SHAPE_VERBOSE")) std::printf("  %-55s -> %s\n", f.what, why.c_str());
  }
}

// Each variation of the good body is still the documented shape.
void eachVariantAccepted(const std::string& good, const std::vector<Fault>& variants, Check check) {
  for (const Fault& v : variants) {
    std::string why;
    TEST_ASSERT_TRUE_MESSAGE(check(patched(good, v), why), (std::string(v.what) + ": " + why).c_str());
  }
}

// ---- Responses ----------------------------------------------------------------------------------
const std::string kResponsesGood =
    R"({"model":"gpt-6-luna","instructions":"You are a test assistant.","max_output_tokens":4096,)"
    R"("reasoning":{"effort":"low"},"store":false,"include":["reasoning.encrypted_content"],"prompt_cache_key":"k1",)"
    R"("tools":[{"type":"function","name":"read_note","description":"Read one note.","parameters":{"type":"object","properties":{"path":{"type":"string"}}}}],)"
    R"("tool_choice":"auto","input":[)"
    R"({"role":"developer","content":"Pinned: three beds."},)"
    R"({"role":"user","content":[{"type":"input_text","text":"What is in beds.md?"},{"type":"input_image","image_url":"data:image/jpeg;base64,AAAA","detail":"auto"}]},)"
    R"({"type":"reasoning","id":"rs_01","encrypted_content":"gAAAA","summary":[]},)"
    R"({"type":"function_call","call_id":"call_01A","name":"read_note","arguments":"{\"path\":\"beds.md\"}"},)"
    R"({"type":"function_call_output","call_id":"call_01A","output":"bed 1: beans"},)"
    R"({"type":"message","role":"assistant","content":[{"type":"output_text","text":"Beans in bed 1."}]},)"
    R"({"role":"user","content":"Thanks."}]})";

bool responsesCheck(const std::string& body, std::string& why) {
  return wire_shape::responsesRequest(req("/v1/responses", body), why);
}

void test_responses_checker_accepts_the_documented_shape_and_refuses_each_fault() {
  eachFaultRefused(
      kResponsesGood,
      {
          {R"({"type":"function_call_output","call_id":"call_01A","output":"bed 1: beans"},)", "", "a call with no result"},
          {R"("call_id":"call_01A","output")", R"("call_id":"call_09Z","output")", "a result that answers no call"},
          {R"("call_id":"call_01A","name")", R"("call_id":"fc_01A","name")", "the item id (fc_) where the call id belongs"},
          {R"("call_id":"call_01A","name")", R"("call_id":"call 01A","name")", "a call id with a space"},
          {R"({"type":"function_call","call_id")", R"({"type":"function_call","id":"fc_01","call_id")", "an item id on a rendered call"},
          {R"("max_output_tokens":4096,)", R"("max_output_tokens":4096,"max_tokens":4096,)", "the Chat cap name (forbidden)"},
          {R"("store":false,)", R"("store":false,"temperature":0.2,)", "an unknown top-level field"},
          {R"("instructions")", R"("messages":[],"instructions")", "a Chat messages array"},
          {R"("max_output_tokens":4096)", R"("max_output_tokens":8)", "max_output_tokens under 16"},
          {R"("arguments":"{\"path\":\"beds.md\"}")", R"("arguments":{"path":"beds.md"})", "arguments as an object"},
          {R"("arguments":"{\"path\":\"beds.md\"}")", R"("arguments":"[1]")", "arguments not holding an object"},
          {R"("store":false,)", R"("store":true,)", "reasoning replayed with store on"},
          {R"("include":["reasoning.encrypted_content"],)", "", "reasoning replayed without the include"},
          {R"("encrypted_content":"gAAAA",)", "", "reasoning without encrypted_content"},
          {R"({"type":"function_call","call_id":"call_01A","name":"read_note","arguments":"{\"path\":\"beds.md\"}"},{"type":"function_call_output","call_id":"call_01A","output":"bed 1: beans"},{"type":"message","role":"assistant","content":[{"type":"output_text","text":"Beans in bed 1."}]},)",
           "", "a reasoning item with nothing after it"},
          {R"({"type":"function_call_output","call_id":"call_01A","output":"bed 1: beans"},)",
           R"({"role":"user","content":"wait"},{"type":"function_call_output","call_id":"call_01A","output":"bed 1: beans"},)",
           "a user message between a call and its result"},
          {R"({"role":"user","content":"Thanks."})", R"({"role":"tool","content":"Thanks."})", "a Chat role"},
          {R"({"type":"input_text","text":"What is in beds.md?"})", R"({"type":"input_text","text":"  "})", "whitespace-only text"},
          {R"({"type":"input_text","text":"What is in beds.md?"})", R"({"type":"text","text":"x"})", "a Chat text part"},
          {R"("output_text","text":"Beans in bed 1.")", R"("input_text","text":"Beans in bed 1.")", "input_text from the assistant"},
          {R"(data:image/jpeg;base64,AAAA)", R"(data:image/tiff;base64,AAAA)", "an image format OpenAI refuses"},
          {R"(data:image/jpeg;base64,AAAA)", R"(data:image/jpeg;base64,A@A)", "image data that is not base64"},
          {R"("detail":"auto")", R"("detail":"max")", "an unknown image detail"},
          {R"({"type":"function","name":"read_note")", R"({"type":"function","function":{},"name":"read_note")", "a nested Chat-style tool"},
          {R"("tool_choice":"auto")", R"("tool_choice":"any")", "an Anthropic tool_choice value"},
          {R"("tool_choice":"auto")", R"("tool_choice":{"type":"function","name":"write_note"})", "tool_choice naming no tool"},
          {R"("effort":"low")", R"("effort":"extreme")", "an unknown effort"},
          {R"("prompt_cache_key":"k1",)", R"("prompt_cache_key":"k1","text":{"format":{"type":"json_schema","schema":{"type":"object"}}},)",
           "a json_schema format with no name"},
          {R"("input":[{"role":"developer","content":"Pinned: three beds."},{"role":"user","content":[)",
           R"("input":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"hi"}]},{"role":"user","content":[)",
           "the input opening on the assistant"},
          {R"("text":"What is in beds.md?")", "\"text\":\"What \x01is\"", "a raw control byte"},
          {R"("text":"What is in beds.md?")", "\"text\":\"What \xC3is\"", "invalid UTF-8"},
      },
      responsesCheck);
  eachVariantAccepted(
      kResponsesGood,
      {
          {R"({"role":"developer","content":"Pinned: three beds."},)", "", "no developer message"},
          {R"({"type":"reasoning","id":"rs_01","encrypted_content":"gAAAA","summary":[]},)", "", "no reasoning item"},
          {R"("tool_choice":"auto")", R"("tool_choice":"none")", "tool_choice none"},
          {R"("prompt_cache_key":"k1",)", R"("prompt_cache_key":"k1","text":{"format":{"type":"json_schema","name":"lines","schema":{"type":"object"},"strict":true}},)",
           "a named json_schema format"},
          {R"("output":"bed 1: beans")", R"("output":[{"type":"input_image","image_url":"data:image/png;base64,AAAA"}])", "an image in a tool result"},
      },
      responsesCheck);
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(responsesCheck(R"({"model":"gpt-6-luna","input":[{"role":"user","content":"hi"}]})", why), why.c_str());
  // Headers: the path, Bearer only, no Anthropic header, no key on cleartext.
  Request r = req("/v1/chat/completions", kResponsesGood);
  TEST_ASSERT_FALSE(wire_shape::responsesRequest(r, why));
  r = req("/v1/responses", kResponsesGood, false);
  TEST_ASSERT_FALSE(wire_shape::responsesRequest(r, why));
  r = req("/v1/responses", kResponsesGood);
  r.headers.push_back({"anthropic-version", "2023-06-01"});
  TEST_ASSERT_FALSE(wire_shape::responsesRequest(r, why));
  r = req("/v1/responses", kResponsesGood);
  r.tls = false;
  TEST_ASSERT_FALSE(wire_shape::responsesRequest(r, why));
}

// ---- Chat ---------------------------------------------------------------------------------------
const std::string kMistralGood =
    R"({"model":"mistral-large-4-0","max_tokens":4096,"reasoning_effort":"high","prompt_cache_key":"k1","parallel_tool_calls":true,)"
    R"("tools":[{"type":"function","function":{"name":"read_note","description":"Read one note.","parameters":{"type":"object"}}}],)"
    R"("tool_choice":"auto","messages":[)"
    R"({"role":"system","content":"You are a test assistant."},)"
    R"({"role":"user","content":[{"type":"text","text":"What is in beds.md?"},{"type":"image_url","image_url":"data:image/webp;base64,AAAA"}]},)"
    R"({"role":"assistant","content":[{"type":"thinking","thinking":[{"type":"text","text":"look it up"}]},{"type":"text","text":"Reading."}],)"
    R"("tool_calls":[{"id":"D681PevKs","type":"function","function":{"name":"read_note","arguments":"{\"path\":\"beds.md\"}"}}]},)"
    R"({"role":"tool","name":"read_note","tool_call_id":"D681PevKs","content":"bed 1: beans"},)"
    R"({"role":"user","content":"Thanks."}]})";

const std::string kZaiGood =
    R"({"model":"glm-5.3","max_tokens":4096,"reasoning_effort":"low","thinking":{"type":"enabled","clear_thinking":false},)"
    R"("tools":[{"type":"function","function":{"name":"read_note","description":"Read one note.","parameters":{"type":"object"}}}],)"
    R"("tool_choice":"auto","response_format":{"type":"json_object"},"messages":[)"
    R"({"role":"system","content":"You are a test assistant."},)"
    R"({"role":"user","content":"What is in beds.md?"},)"
    R"({"role":"assistant","content":"","reasoning_content":"look it up",)"
    R"("tool_calls":[{"id":"call_-7a1b","type":"function","function":{"name":"read_note","arguments":"{\"path\":\"beds.md\"}"}}]},)"
    R"({"role":"tool","tool_call_id":"call_-7a1b","content":"bed 1: beans"}]})";

const std::string kCustomTextMode =
    R"({"model":"llama-4","max_tokens":2048,"messages":[{"role":"system","content":"s"},{"role":"user","content":"hi"},)"
    R"({"role":"assistant","content":"hello"},{"role":"user","content":"again"}]})";

bool mistralCheck(const std::string& b, std::string& why) {
  return wire_shape::chatRequest(req("/v1/chat/completions", b), ChatFlavor::Mistral, why);
}
bool zaiCheck(const std::string& b, std::string& why) {
  return wire_shape::chatRequest(req("/api/paas/v4/chat/completions", b), ChatFlavor::Zai, why);
}
bool customCheck(const std::string& b, std::string& why) {
  return wire_shape::chatRequest(req("/v1/chat/completions", b, false), ChatFlavor::Custom, why);
}

void test_chat_checker_mistral_accepts_the_documented_shape_and_refuses_each_fault() {
  eachFaultRefused(
      kMistralGood,
      {
          {R"({"role":"tool","name":"read_note","tool_call_id":"D681PevKs","content":"bed 1: beans"},)", "", "a call with no result"},
          {R"("tool_call_id":"D681PevKs")", R"("tool_call_id":"Q000000Zz")", "a result that answers no call"},
          {R"("id":"D681PevKs")", R"("id":"toolu_01A")", "a foreign id form (Mistral needs 9 alphanumerics)"},
          {R"("id":"D681PevKs")", R"("id":"D681Pe-Ks")", "a 9-character id with a dash"},
          {R"("max_tokens":4096,)", R"("max_tokens":4096,"max_output_tokens":4096,)", "the Responses cap name (forbidden)"},
          {R"("max_tokens":4096,)", R"("max_tokens":4096,"thinking":{"type":"enabled"},)", "the Z.ai thinking field"},
          {R"("reasoning_effort":"high")", R"("reasoning_effort":"max")", "an effort Mistral does not document"},
          {R"({"role":"user","content":"Thanks."})", R"({"role":"system","content":"late"})", "a system message after the start"},
          {R"(,{"role":"user","content":"Thanks."})", R"(,{"role":"assistant","content":"done"})", "ending on the assistant"},
          {R"({"role":"user","content":"Thanks."})", R"({"role":"user","content":""})", "empty text"},
          {R"({"type":"text","text":"Reading."})", R"({"type":"text","text":" "})", "whitespace-only assistant text"},
          {R"(,{"type":"text","text":"Reading."})", "", "an assistant content of thinking alone"},
          {R"("arguments":"{\"path\":\"beds.md\"}")", R"("arguments":{"path":"beds.md"})", "arguments as an object"},
          {R"("tool_choice":"auto")", R"("tool_choice":{"type":"function","function":{"name":"write_note"}})", "tool_choice naming no tool"},
          {R"("tool_choice":"auto")", R"("tool_choice":"tool")", "an Anthropic tool_choice value"},
          {R"({"type":"function","function":{"name":"read_note")", R"({"type":"function","name":"read_note","function":{"name":"read_note")",
           "a flat Responses-style tool"},
          {R"(data:image/webp;base64,AAAA)", R"(data:image/tiff;base64,AAAA)", "an image format Mistral refuses"},
          {R"("role":"tool","name")", R"("role":"function","name")", "an unknown role"},
          {R"({"role":"user","content":[{"type":"text")", R"({"role":"assistant","content":"hi"},{"role":"user","content":[{"type":"text")",
           "the conversation opening on the assistant"},
          {R"("parallel_tool_calls":true,"tools":[{"type":"function","function":{"name":"read_note","description":"Read one note.","parameters":{"type":"object"}}}],"tool_choice":"auto",)",
           "", "tool_calls in text mode (no tools)"},
      },
      mistralCheck);
  eachVariantAccepted(
      kMistralGood,
      {
          {R"(,{"role":"user","content":"Thanks."})", R"(,{"role":"assistant","content":"Be","prefix":true})", "a prefix assistant last"},
          {R"("image_url":"data:image/webp;base64,AAAA")", R"("image_url":{"url":"https://example.com/a.png"})", "an image URL object"},
          {R"("tool_choice":"auto")", R"("tool_choice":"any")", "tool_choice any"},
          {R"("tool_choice":"auto")", R"("tool_choice":{"type":"function","function":{"name":"read_note"}})", "a named tool_choice"},
      },
      mistralCheck);
}

void test_chat_checker_zai_accepts_the_documented_shape_and_refuses_each_fault() {
  eachFaultRefused(
      kZaiGood,
      {
          {R"(,{"role":"tool","tool_call_id":"call_-7a1b","content":"bed 1: beans"})", "", "a call with no result (ends on the assistant)"},
          {R"("tool_call_id":"call_-7a1b")", R"("tool_call_id":"call_x")", "a result that answers no call"},
          {R"("id":"call_-7a1b")", R"("id":"call -7a1b")", "a call id with a space"},
          {R"({"role":"tool","tool_call_id")", R"({"role":"tool","name":"read_note","tool_call_id")", "the Mistral name field on a tool message"},
          {R"("max_tokens":4096)", R"("max_tokens":200000)", "max_tokens over 131,072"},
          {R"("max_tokens":4096,)", R"("max_tokens":4096,"parallel_tool_calls":false,)", "parallel_tool_calls (not documented)"},
          {R"("max_tokens":4096,)", R"("max_tokens":4096,"prompt_cache_key":"k",)", "the Mistral cache key (forbidden)"},
          {R"("response_format":{"type":"json_object"})", R"("response_format":{"type":"json_schema","json_schema":{"name":"x","schema":{"type":"object"}}})",
           "json_schema (not documented on Z.ai)"},
          {R"("tool_choice":"auto")", R"("tool_choice":"none")", "tool_choice none (not recorded)"},
          {R"("tool_choice":"auto")", R"("tool_choice":{"type":"function","function":{"name":"read_note"}})", "a named tool_choice"},
          {R"("reasoning_effort":"low")", R"("reasoning_effort":"medium")", "an effort Z.ai does not document"},
          {R"("thinking":{"type":"enabled")", R"("thinking":{"type":"adaptive")", "an Anthropic thinking type"},
          {R"("content":"","reasoning_content")", R"("content":[{"type":"thinking","thinking":[{"type":"text","text":"x"}]}],"reasoning_content")",
           "Mistral thinking chunks on Z.ai"},
          {R"("role":"user","content":"What is in beds.md?")",
           R"("role":"user","content":[{"type":"image_url","image_url":{"url":"data:image/webp;base64,AAAA"}}])", "a WebP image (not listed)"},
          {R"({"role":"system","content":"You are a test assistant."},{"role":"user","content":"What is in beds.md?"},{"role":"assistant")",
           R"({"role":"system","content":"You are a test assistant."},{"role":"assistant")", "no user message first"},
      },
      zaiCheck);
  std::string why;
  TEST_ASSERT_FALSE(zaiCheck(R"({"model":"glm-5.3","messages":[{"role":"system","content":"s"}]})", why));
  TEST_ASSERT_TRUE_MESSAGE(zaiCheck(R"({"model":"glm-5.3","messages":[{"role":"user","content":[{"type":"image_url","image_url":{"url":"data:image/jpg;base64,AAAA"}}]}]})", why),
                           why.c_str());
}

void test_chat_checker_custom_text_mode_and_its_faults() {
  eachFaultRefused(kCustomTextMode,
                   {
                       {R"({"role":"assistant","content":"hello"})",
                        R"({"role":"assistant","content":"","tool_calls":[{"id":"c1","type":"function","function":{"name":"f","arguments":"{}"}}]},{"role":"tool","tool_call_id":"c1","content":"x"})",
                        "a tool call in text mode"},
                       {R"("max_tokens":2048,)", R"("max_tokens":2048,"tool_choice":"auto",)", "tool_choice without tools"},
                       {R"("max_tokens":2048,)", R"("max_tokens":2048,"reasoning_content":"x",)", "an unknown top-level field"},
                       {R"("max_tokens":2048)", R"("max_tokens":0)", "max_tokens 0"},
                       {R"("model":"llama-4")", R"("model":"")", "no model"},
                   },
                   customCheck);
  // A key is optional on a custom endpoint but never on cleartext; Mistral and Z.ai need one.
  std::string why;
  Request r = req("/v1/chat/completions", kCustomTextMode, false);
  r.tls = false;
  TEST_ASSERT_TRUE_MESSAGE(wire_shape::chatRequest(r, ChatFlavor::Custom, why), why.c_str());
  r = req("/v1/chat/completions", kCustomTextMode);
  r.tls = false;
  TEST_ASSERT_FALSE(wire_shape::chatRequest(r, ChatFlavor::Custom, why));
  TEST_ASSERT_FALSE(wire_shape::chatRequest(req("/v1/chat/completions", kMistralGood, false), ChatFlavor::Mistral, why));
  TEST_ASSERT_FALSE(wire_shape::chatRequest(req("/v1/responses", kMistralGood), ChatFlavor::Mistral, why));
}

// ---- the committed goldens ----------------------------------------------------------------------
const char* kGoldens = "test/support/goldens/wire/";

bool readFile(const std::string& path, std::string& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  char buf[8192];
  size_t n;
  out.clear();
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  std::fclose(f);
  return true;
}

// A golden request: "POST <url>", "header <k>: <v>" lines, "splices <n>", "body <len>", the body.
bool parseGolden(const std::string& g, Request& r) {
  size_t at = 0;
  while (at < g.size()) {
    const size_t eol = g.find('\n', at);
    if (eol == std::string::npos) return false;
    const std::string line = g.substr(at, eol - at);
    at = eol + 1;
    if (line.rfind("POST ", 0) == 0) {
      const std::string url = line.substr(5);
      r.tls = url.rfind("https://", 0) == 0;
      const size_t host = url.find("://");
      const size_t path = url.find('/', host == std::string::npos ? 0 : host + 3);
      if (path == std::string::npos) return false;
      r.path = url.substr(path);
    } else if (line.rfind("header ", 0) == 0) {
      const size_t colon = line.find(": ");
      if (colon == std::string::npos) return false;
      r.headers.push_back({line.substr(7, colon - 7), line.substr(colon + 2)});
    } else if (line.rfind("body ", 0) == 0) {
      const size_t len = std::strtoul(line.c_str() + 5, nullptr, 10);
      if (at + len > g.size()) return false;
      r.body = g.substr(at, len);
      return true;
    }
  }
  return false;
}

// Signatures are <dialect>-<slot>-<route>; the slot picks the Chat flavor.
void test_every_golden_request_passes_its_dialect_checker_and_messages_bodies_are_refused() {
  std::string manifest;
  TEST_ASSERT_TRUE_MESSAGE(readFile(std::string(kGoldens) + "MANIFEST", manifest), "no golden MANIFEST");
  int checked = 0, refused = 0;
  size_t at = 0;
  while (at < manifest.size()) {
    size_t eol = manifest.find('\n', at);
    if (eol == std::string::npos) eol = manifest.size();
    const std::string name = manifest.substr(at, eol - at);
    at = eol + 1;
    if (name.empty() || name[0] == '#' || name.size() < 5 || name.compare(name.size() - 4, 4, ".req") != 0) continue;
    std::string g;
    Request r;
    TEST_ASSERT_TRUE_MESSAGE(readFile(kGoldens + name, g) && parseGolden(g, r), ("unreadable golden " + name).c_str());
    const std::string sig = name.substr(0, name.find('/'));
    const std::string dialect = sig.substr(0, sig.find('-'));
    std::string why;
    if (dialect == "messages") {
      TEST_ASSERT_FALSE_MESSAGE(wire_shape::responsesRequest(r, why), ("Responses checker accepted " + name).c_str());
      for (ChatFlavor f : {ChatFlavor::Mistral, ChatFlavor::Zai, ChatFlavor::Custom})
        TEST_ASSERT_FALSE_MESSAGE(wire_shape::chatRequest(r, f, why), ("Chat checker accepted " + name).c_str());
      ++refused;
      continue;
    }
    bool ok = false;
    if (dialect == "responses") ok = wire_shape::responsesRequest(r, why);
    else if (dialect == "chat") {
      const ChatFlavor f = sig.find("-mistral-") != std::string::npos ? ChatFlavor::Mistral
                           : sig.find("-zai-") != std::string::npos   ? ChatFlavor::Zai
                                                                      : ChatFlavor::Custom;
      ok = wire_shape::chatRequest(r, f, why);
    } else {
      TEST_FAIL_MESSAGE(("a golden of an unknown dialect: " + name).c_str());
    }
    TEST_ASSERT_TRUE_MESSAGE(ok, (name + ": " + why).c_str());
    ++checked;
  }
  TEST_ASSERT_TRUE_MESSAGE(refused > 0, "no Messages golden found to counter-test with");
  std::printf("wire_shape: %d Responses/Chat goldens checked, %d Messages goldens refused by both\n", checked, refused);
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_responses_checker_accepts_the_documented_shape_and_refuses_each_fault);
  RUN_TEST(test_chat_checker_mistral_accepts_the_documented_shape_and_refuses_each_fault);
  RUN_TEST(test_chat_checker_zai_accepts_the_documented_shape_and_refuses_each_fault);
  RUN_TEST(test_chat_checker_custom_text_mode_and_its_faults);
  RUN_TEST(test_every_golden_request_passes_its_dialect_checker_and_messages_bodies_are_refused);
  return UNITY_END();
}
