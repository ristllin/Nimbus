#include <unity.h>

#include <cstring>
#include <regex>
#include <string>
#include <vector>

#include "nimbus/exchange_trace.h"

using namespace nimbus::heapwatch;

void setUp() {}
void tearDown() {}

// A scripted exchange: each phase succeeds unless told to fail; the body sink takes
// whole slices until `bodyFailAt` bytes have gone out, then takes `partial` bytes of
// the failing slice.
struct FakeIo {
  bool connectOk = true, headOk = true, headersOk = true, bodyParses = true;
  int status = 200;
  size_t bodyFailAt = SIZE_MAX;
  size_t partial = 0;
  std::vector<size_t> slices;   // the size of every write the transport made
  int samples = 0;

  HeapSample sample() { ++samples; return {30000u - (uint32_t)samples, 15000u}; }
  bool connect() { return connectOk; }
  bool writeHead() { return headOk; }
  size_t writeBody(size_t off, size_t want) {
    slices.push_back(want);
    if (off + want > bodyFailAt) return partial;
    return want;
  }
  int readStatus() { return status; }
  bool readHeaders(int) { return headersOk; }
  bool readBody() { return bodyParses; }
};

struct Run {
  int code = -1;
  int finishes = 0;
  ExchangeTrace t;
};

static Run runOne(FakeIo& io, uint32_t body) {
  Run r;
  r.t.host = "api.anthropic.com";
  r.t.path = "/v1/messages";
  r.t.body = body;
  r.t.pre = {24760, 11764};
  r.code = tracedExchange(io, r.t, [&](ExchangeTrace& t) {
    ++r.finishes;
    t.done = {24600, 11764};
  });
  return r;
}

// ---- one line per exit stage ----------------------------------------------------

static void test_every_request_stage_finishes_exactly_once() {
  struct Case { const char* name; Stage want; int wantCode; void (*arm)(FakeIo&); };
  const Case cases[] = {
      {"connect", Stage::Connect, 0, [](FakeIo& io) { io.connectOk = false; }},
      {"head", Stage::Head, 0, [](FakeIo& io) { io.headOk = false; }},
      {"read", Stage::Read, 0, [](FakeIo& io) { io.status = 0; }},
      {"headers", Stage::Headers, 200, [](FakeIo& io) { io.headersOk = false; }},
      {"parse", Stage::Parse, 200, [](FakeIo& io) { io.bodyParses = false; }},
      {"ok", Stage::None, 200, [](FakeIo&) {}},
  };
  for (const Case& c : cases) {
    FakeIo io;
    c.arm(io);
    const Run r = runOne(io, 71508);
    TEST_ASSERT_EQUAL_MESSAGE(1, r.finishes, c.name);
    TEST_ASSERT_EQUAL_MESSAGE((int)c.want, (int)r.t.fail, c.name);
    TEST_ASSERT_EQUAL_MESSAGE(c.wantCode, r.code, c.name);
    const std::string line = r.t.line();
    TEST_ASSERT_NOT_NULL_MESSAGE(std::strstr(line.c_str(), (std::string(" fail=") + stageName(c.want)).c_str()), c.name);
    TEST_ASSERT_NOT_NULL(std::strstr(line.c_str(), " af=0 min=- at=0 ms=0"));
  }
}

// Property over every slice index of a field-sized body (71,508 B = 18 slices): a
// failure in slice k finishes once, names body@<bytes out>, and the error says
// "after N of M bytes" with N the exact count, never the old "0/M bytes written".
static void test_body_failure_at_every_slice_is_named_with_its_byte_count() {
  const uint32_t body = 71508;
  const size_t nSlices = (body + kBodySliceBytes - 1) / kBodySliceBytes;
  for (size_t k = 0; k < nSlices; ++k) {
    for (size_t partial : {(size_t)0, (size_t)100}) {
      FakeIo io;
      io.bodyFailAt = k * kBodySliceBytes;
      io.partial = partial;
      const Run r = runOne(io, body);
      TEST_ASSERT_EQUAL(1, r.finishes);
      TEST_ASSERT_EQUAL(0, r.code);
      TEST_ASSERT_EQUAL((int)Stage::Body, (int)r.t.fail);
      const uint32_t sent = (uint32_t)(k * kBodySliceBytes + partial);
      TEST_ASSERT_EQUAL_UINT32(sent, r.t.bodySent);
      TEST_ASSERT_EQUAL((int)k + 1, (int)io.slices.size());   // stops at the failing slice
      for (size_t w : io.slices) TEST_ASSERT_TRUE(w <= kBodySliceBytes);
      char want[64];
      snprintf(want, sizeof(want), " fail=body@%u ", (unsigned)sent);
      TEST_ASSERT_NOT_NULL(std::strstr(r.t.line().c_str(), want));
      const std::string err = stageError(r.t, nullptr, nullptr, false);
      snprintf(want, sizeof(want), "after %u of %u bytes", (unsigned)sent, (unsigned)body);
      TEST_ASSERT_NOT_NULL(std::strstr(err.c_str(), want));
      TEST_ASSERT_NOT_NULL(std::strstr(err.c_str(), "body truncated"));   // "unsent" marker kept
      TEST_ASSERT_NULL(std::strstr(err.c_str(), "bytes written"));
      TEST_ASSERT_NULL(std::strstr(err.c_str(), "low memory"));   // no evidence, not starved
    }
  }
}

// A healthy body goes out as whole 4,096 B slices plus one tail, covering it exactly.
static void test_slices_cover_the_body_exactly() {
  for (uint32_t body : {1u, 4095u, 4096u, 4097u, 64339u, 71508u}) {
    FakeIo io;
    const Run r = runOne(io, body);
    TEST_ASSERT_EQUAL((int)Stage::None, (int)r.t.fail);
    size_t sum = 0;
    for (size_t i = 0; i < io.slices.size(); ++i) {
      sum += io.slices[i];
      if (i + 1 < io.slices.size()) TEST_ASSERT_EQUAL((int)kBodySliceBytes, (int)io.slices[i]);
    }
    TEST_ASSERT_EQUAL_UINT32(body, (uint32_t)sum);
    TEST_ASSERT_EQUAL_UINT32(body, r.t.bodySent);
  }
  FakeIo io;   // a body-less request writes no slice and takes no post-write sample
  const Run r = runOne(io, 0);
  TEST_ASSERT_EQUAL(0, (int)io.slices.size());
  TEST_ASSERT_EQUAL_UINT32(0, r.t.wrote.free);
}

// Counter-test for the analyzers: a healthy line keeps today's fields in today's
// order, so the scripts that parse "heaptrace <host><path> body= pre= conn= wrote=
// done=" still match it; the new fields only append.
static void test_healthy_line_keeps_the_field_order() {
  FakeIo io;
  Run r = runOne(io, 71508);
  r.t.minFree = 18000;
  r.t.atMs = 42651042;
  r.t.durMs = 1712;
  const std::string line = r.t.line();
  const std::regex today(
      "^transport: heaptrace (\\S+) body=(\\d+) pre=(\\d+)/(\\d+) conn=(\\d+)/(\\d+) "
      "wrote=(\\d+)/(\\d+) done=(\\d+)/(\\d+)");
  std::smatch m;
  TEST_ASSERT_TRUE_MESSAGE(std::regex_search(line, m, today), line.c_str());
  TEST_ASSERT_EQUAL_STRING("api.anthropic.com/v1/messages", m[1].str().c_str());
  TEST_ASSERT_EQUAL_STRING("71508", m[2].str().c_str());
  TEST_ASSERT_EQUAL_STRING("24760", m[3].str().c_str());
  TEST_ASSERT_EQUAL_STRING("11764", m[4].str().c_str());
  const std::string tail = line.substr((size_t)m.length(0));
  TEST_ASSERT_EQUAL_STRING(" fail=none af=0 min=18000 at=42651042 ms=1712", tail.c_str());
}

// ---- error texts and evidence ---------------------------------------------------

static void test_errors_keep_the_markers_and_name_evidence() {
  ExchangeTrace t;
  t.body = 71508;
  FailRecord last{};
  last.size = 1600;
  last.caps = (1u << 3) | (1u << 11);

  t.fail = Stage::Connect;
  TEST_ASSERT_EQUAL_STRING("connect failed", stageError(t, nullptr, nullptr, false).c_str());
  TEST_ASSERT_EQUAL_STRING("connect failed: low memory", stageError(t, nullptr, nullptr, true).c_str());
  t.af = 2;
  TEST_ASSERT_EQUAL_STRING("connect failed: low memory (alloc failed 1600 B DMA|INTERNAL)",
                           stageError(t, nullptr, &last, false).c_str());
  t.af = 0;

  t.fail = Stage::Head;
  TEST_ASSERT_EQUAL_STRING("request head write failed", stageError(t, nullptr, nullptr, false).c_str());

  t.fail = Stage::Body;
  t.bodySent = 8192;
  t.af = 1;
  TEST_ASSERT_EQUAL_STRING(
      "request body truncated: write failed after 8192 of 71508 bytes: low memory (alloc failed "
      "1600 B DMA|INTERNAL)",
      stageError(t, nullptr, &last, false).c_str());
  t.af = 0;

  t.fail = Stage::Read;   // a missing response was never tagged low memory without evidence
  TEST_ASSERT_EQUAL_STRING("no response", stageError(t, nullptr, nullptr, true).c_str());
  t.af = 1;
  TEST_ASSERT_EQUAL_STRING("no response: low memory (alloc failed)",
                           stageError(t, nullptr, nullptr, false).c_str());
  t.af = 0;

  t.fail = Stage::Headers;   // the status line came: a cut response, not a missing one
  TEST_ASSERT_EQUAL_STRING("response headers cut short", stageError(t, nullptr, nullptr, true).c_str());

  t.fail = Stage::Parse;
  TEST_ASSERT_EQUAL_STRING("response parse failed: IncompleteInput",
                           stageError(t, "IncompleteInput", nullptr, false).c_str());
  TEST_ASSERT_EQUAL_STRING("response parse failed: NoMemory (low memory)",
                           stageError(t, "NoMemory", nullptr, true).c_str());
  t.fail = Stage::None;
  TEST_ASSERT_EQUAL_STRING("", stageError(t, nullptr, nullptr, true).c_str());
}

// The forensic predicate: failures, failed allocations and near-floor samples are
// durable evidence; a healthy exchange is not, and the unset post-write sample of a
// body-less request never counts as a reading.
static void test_abnormal_predicate() {
  ExchangeTrace t;
  t.pre = {24000, 11000};
  t.conn = {23000, 10000};
  t.done = {24000, 11000};
  TEST_ASSERT_FALSE(t.abnormal());   // wrote stays (0,0): not a reading
  t.minFree = 12288;
  TEST_ASSERT_FALSE(t.abnormal());
  t.minFree = 12287;
  TEST_ASSERT_TRUE(t.abnormal());
  t.minFree = UINT32_MAX;
  t.conn = {23000, 7156};
  TEST_ASSERT_TRUE(t.abnormal());
  t.conn = {23000, 10000};
  t.af = 1;
  TEST_ASSERT_TRUE(t.abnormal());
  t.af = 0;
  t.fail = Stage::Parse;
  TEST_ASSERT_TRUE(t.abnormal());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_every_request_stage_finishes_exactly_once);
  RUN_TEST(test_body_failure_at_every_slice_is_named_with_its_byte_count);
  RUN_TEST(test_slices_cover_the_body_exactly);
  RUN_TEST(test_healthy_line_keeps_the_field_order);
  RUN_TEST(test_errors_keep_the_markers_and_name_evidence);
  RUN_TEST(test_abnormal_predicate);
  return UNITY_END();
}
