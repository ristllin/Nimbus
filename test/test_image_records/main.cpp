#include <unity.h>

#include <cstring>
#include <string>
#include <vector>

#include "../support/fake_http.h"
#include "../support/fake_platform.h"
#include "nimbus/harness/vision_caption.h"
#include "nimbus/orch/blob_store.h"
#include "nimbus/orch/episodic.h"
#include "nimbus/orch/media_label.h"
#include "nimbus/orch/provider_park.h"
#include "nimbus/orch/psram_alloc.h"
#include "nimbus/orch/row_overlay.h"
#include "nimbus/orch/turn_images.h"

// A photo's record in memory (R1) and the re-read after a park ends (L19-5), host
// level (Virtual Nimbus has no photo intake, so the plan's nimbusd scenario is this
// suite, step for step):
//  - a photo read during a scripted 429 is stored with its typed reason and ref and
//    queued; while the provider is parked nothing is sent; when the park's re-check
//    comes, exactly one re-read runs, the row text is replaced, and the next read of
//    the chat's rows (the next turn's history) sees the description;
//  - counter-tests: a photo over the size budget is not queued; a failed re-read keeps
//    the pending state, waits longer each time and gives up, never looping;
//  - the overlay: newest replacement wins, bounded, replayed from its durable lines.

using harness_test::FakeHttpTransport;
using harness_test::LogCapture;
namespace vision = agent::vision;
namespace orch = nimbus::orch;

namespace {

const char* kQuota429 =
    "{\"error\":{\"message\":\"You exceeded your current quota.\",\"type\":\"insufficient_quota\","
    "\"code\":\"insufficient_quota\"}}";
const std::string kRef = "0123456789abcdef";

std::string chatOk(const std::string& t) {
  return "{\"choices\":[{\"message\":{\"content\":\"" + t + "\"}}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}";
}

struct Rig {
  FakeHttpTransport http;
  uint32_t now = 1000;
  orch::InMemoryEpisodicStore store;
  orch::RowTextOverlay overlay;
  const uint8_t photo[4] = {0xFF, 0xD8, 0xFF, 0xD9};

  Rig() { overlay.bind(&store); }
  vision::CaptionDeps deps() {
    vision::CaptionDeps d;
    d.http = &http;
    d.key = [](const std::string& p) { return p == "openai" ? std::string("key-openai") : std::string(); };
    d.priority = [] { return std::string("openai"); };
    d.nowMs = [this] { return now; };
    return d;
  }
  vision::CaptionResult read(vision::CaptionKind k) {
    vision::CaptionRequest in;
    in.bytes = photo;
    in.len = sizeof photo;
    in.kind = k;
    return vision::caption(deps(), in);
  }
  bool providerOpen() const {
    return orch::providerPark()
        .available("openai", orch::PaidSeam::Vision, orch::parkKeyFingerprint("key-openai"), now)
        .open;
  }
  void storeRow(const std::string& text) {
    orch::EpisodicMessage m;
    m.id = "m00000042";
    m.sessionId = "4848";
    m.kind = orch::MsgKind::Image;
    m.role = "user";
    m.text = text;
    m.blobPath = orch::blobPath("/mem/blobs", kRef, "jpg");
    overlay.addMessage(m);
  }
  std::string rowText() const {
    orch::MsgQuery q;
    q.sessionId = "4848";
    q.haveKind = true;
    q.kind = orch::MsgKind::Image;
    const auto rows = overlay.query(q);
    return rows.empty() ? std::string() : rows.front().text;
  }
  // The device's replaceMediaRowText, on the host store: the newest photo row of
  // the chat whose stored copy is `ref`.
  bool replace(const std::string& ref, const std::string& text) {
    orch::MsgQuery q;
    q.sessionId = "4848";
    q.haveKind = true;
    q.kind = orch::MsgKind::Image;
    for (const auto& m : store.query(q))
      if (orch::blobHashOf(m.blobPath) == ref) {
        overlay.put(m.id, text);
        return true;
      }
    return false;
  }
};

vision::PendingRecord recordFor(size_t len) {
  vision::PendingRecord r;
  r.chatId = "4848";
  r.ref = orch::workingCopy(kRef);
  r.mime = "image/jpeg";
  r.tag = orch::workingCopy("[image 1/1 ref=" + kRef + "]");
  r.ext = "jpg";
  r.len = len;
  return r;
}

}  // namespace

void setUp() {
  LogCapture::install();
  orch::providerPark().clear();
  vision::clearLastError();
  vision::recordQueue().clear();
}
void tearDown() { agent::hlog::setSink(nullptr); }

// The L19-5 scenario, step for step.
static void test_reread_once_after_the_park_ends_replaces_the_row() {
  Rig rig;
  // 1) a photo during a scripted captioner 429: stored with the reason and the ref.
  rig.http.script.push_back({"api.openai.com", "", 429, kQuota429});
  const vision::CaptionResult first = rig.read(vision::CaptionKind::Fallback);
  TEST_ASSERT_EQUAL((int)vision::ReadFail::Billing, (int)first.fail);
  orch::MediaLabel l;
  l.k = l.n = 1;
  l.ref = kRef;
  l.cls = orch::MediaClass::NotRead;
  l.detail = first.words;
  rig.storeRow(orch::mediaSegment(l));
  TEST_ASSERT_EQUAL_STRING(("[image 1/1 ref=" + kRef + "] (not read: openai out of credit (HTTP 429))").c_str(),
                           rig.rowText().c_str());
  TEST_ASSERT_TRUE(vision::recordQueue().push(recordFor(sizeof rig.photo)));
  // 2) while the provider is parked: nothing is due, nothing is sent.
  for (int i = 0; i < 5; ++i) {
    rig.now += 60000;
    TEST_ASSERT_NULL(vision::recordQueue().due(rig.now, rig.providerOpen()));
  }
  TEST_ASSERT_EQUAL(1u, rig.http.seen.size());
  // 3) the park's re-check comes: exactly one re-read, the row text replaced.
  rig.now += orch::ProviderPark::kRecheckMs;
  const vision::PendingRecord* due = vision::recordQueue().due(rig.now, rig.providerOpen());
  TEST_ASSERT_NOT_NULL(due);
  const vision::PendingRecord rec = *due;
  rig.http.script.push_back({"api.openai.com", "", 200, chatOk("A jar label: batch 4417, best before 03/27.")});
  const vision::CaptionResult again = rig.read(vision::CaptionKind::Memory);
  TEST_ASSERT_EQUAL_STRING("A jar label: batch 4417, best before 03/27.", again.text.c_str());
  orch::MediaLabel done;
  done.cls = orch::MediaClass::Transcribed;
  done.detail = again.text;
  done.by = again.provider;
  TEST_ASSERT_TRUE(rig.replace(orch::plainCopy(rec.ref), orch::plainCopy(rec.tag) + orch::mediaSuffix(done)));
  vision::recordQueue().settle(rec.ref, true, rig.now);
  // 4) the next turn's read of the chat's rows sees the description.
  TEST_ASSERT_EQUAL_STRING(("[image 1/1 ref=" + kRef + "] (transcribed by openai) A jar label: batch 4417, best "
                            "before 03/27.").c_str(),
                           rig.rowText().c_str());
  TEST_ASSERT_EQUAL(2u, rig.http.seen.size());   // one refusal, exactly one re-read
  TEST_ASSERT_NULL(vision::recordQueue().due(rig.now + 3600000u, true));
  TEST_ASSERT_EQUAL(0u, vision::recordQueue().size());
}

// Counter-test: a photo over the size budget is not queued (nor one without a ref).
static void test_over_budget_photo_is_not_queued() {
  TEST_ASSERT_FALSE(vision::recordQueue().push(recordFor(orch::kImagePartBytes + 1)));
  vision::PendingRecord noRef = recordFor(100);
  noRef.ref = "";
  TEST_ASSERT_FALSE(vision::recordQueue().push(noRef));
  TEST_ASSERT_TRUE(vision::recordQueue().push(recordFor(orch::kImagePartBytes)));
  TEST_ASSERT_FALSE(vision::recordQueue().push(recordFor(100)));   // the same photo is queued once
  TEST_ASSERT_EQUAL(1u, vision::recordQueue().size());
}

// Counter-test: a failed re-read keeps the pending state, waits 10, 20 minutes, and
// gives up after kRecordTries attempts: never a loop.
static void test_failed_reread_waits_and_gives_up() {
  auto& q = vision::recordQueue();
  TEST_ASSERT_TRUE(q.push(recordFor(100)));
  uint32_t now = 5000;
  int attempts = 0;
  for (int minute = 0; minute < 24 * 60; ++minute, now += 60000) {
    const vision::PendingRecord* due = q.due(now, true);
    if (!due) continue;
    ++attempts;
    q.settle(due->ref, false, now);
  }
  TEST_ASSERT_EQUAL((int)vision::RecordQueue::kRecordTries, attempts);
  TEST_ASSERT_EQUAL(0u, q.size());
}

// Newest first, bounded: the oldest is dropped when the queue is full.
static void test_queue_newest_first_and_bounded() {
  auto& q = vision::recordQueue();
  for (size_t i = 0; i < vision::RecordQueue::kMax + 3; ++i) {
    vision::PendingRecord r = recordFor(100);
    char ref[17];
    snprintf(ref, sizeof ref, "%016zx", i + 1);
    r.ref = ref;
    TEST_ASSERT_TRUE(q.push(r));
  }
  TEST_ASSERT_EQUAL(vision::RecordQueue::kMax, q.size());
  char newest[17];
  snprintf(newest, sizeof newest, "%016zx", vision::RecordQueue::kMax + 3);
  TEST_ASSERT_EQUAL_STRING(newest, q.due(0, true)->ref.c_str());
  TEST_ASSERT_NULL(q.due(0, false));   // no provider open: nothing is due
}

// The overlay: the newest replacement of a row wins, every read path returns it, the
// durable lines replay it, and it is bounded.
static void test_overlay_replays_and_is_bounded() {
  orch::InMemoryEpisodicStore store;
  orch::RowTextOverlay ov;
  ov.bind(&store);
  orch::EpisodicMessage m;
  m.id = "m1";
  m.sessionId = "c";
  m.kind = orch::MsgKind::Image;
  m.text = "old";
  ov.addMessage(m);
  ov.put("m1", "first");
  ov.put("m1", "second \"quoted\"\nline");
  orch::MsgQuery q;
  q.sessionId = "c";
  q.haveKind = true;
  q.kind = orch::MsgKind::Image;
  TEST_ASSERT_EQUAL_STRING("second \"quoted\"\nline", ov.query(q).front().text.c_str());
  TEST_ASSERT_EQUAL_STRING("old", store.query(q).front().text.c_str());   // the store keeps its row
  const std::string lines = orch::RowTextOverlay::line("m1", "first") + "\n" +
                            orch::RowTextOverlay::line("m1", "second") + "\nnot json\n";
  orch::RowTextOverlay replay;
  replay.bind(&store);
  TEST_ASSERT_EQUAL(2u, replay.load(lines));
  TEST_ASSERT_EQUAL_STRING("second", replay.query(q).front().text.c_str());
  TEST_ASSERT_EQUAL(1u, replay.size());
  for (size_t i = 0; i < orch::RowTextOverlay::kMax + 5; ++i) replay.put("x" + std::to_string(i), "t");
  TEST_ASSERT_EQUAL(orch::RowTextOverlay::kMax, replay.size());
  TEST_ASSERT_FALSE(replay.has("m1"));   // the oldest went first
  orch::RowTextOverlay again;
  TEST_ASSERT_EQUAL(orch::RowTextOverlay::kMax, again.load(replay.dump()));
}

// The queue and the overlay outlive a turn: their strings, even a 16-character ref or
// a 20-character row id, go through the working-set allocator (PSRAM on the device),
// never the internal heap where small long-lived pieces cut the largest free block.
static size_t g_workingAllocs = 0;
static void* countingAlloc(size_t n) {
  ++g_workingAllocs;
  return malloc(n);
}
static void test_queue_and_overlay_strings_use_the_working_allocator() {
  orch::setWorkingAllocators(countingAlloc, free);
  g_workingAllocs = 0;
  {
    vision::RecordQueue q;
    vision::PendingRecord r = recordFor(100);
    r.context = "a context longer than any short string buffer";
    const size_t before = g_workingAllocs;
    TEST_ASSERT_TRUE(q.push(r));
    TEST_ASSERT_TRUE(g_workingAllocs > before);   // the item and its long strings
    orch::RowTextOverlay ov;
    const size_t mid = g_workingAllocs;
    ov.put("m-1729650000123-abcd", "(transcribed by anthropic claude-haiku-5-5) a label");
    TEST_ASSERT_TRUE(g_workingAllocs >= mid + 2);   // the id and the text
    TEST_ASSERT_TRUE(ov.has("m-1729650000123-abcd"));
  }
  orch::setWorkingAllocators(nullptr, nullptr);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_reread_once_after_the_park_ends_replaces_the_row);
  RUN_TEST(test_over_budget_photo_is_not_queued);
  RUN_TEST(test_failed_reread_waits_and_gives_up);
  RUN_TEST(test_queue_newest_first_and_bounded);
  RUN_TEST(test_overlay_replays_and_is_bounded);
  RUN_TEST(test_queue_and_overlay_strings_use_the_working_allocator);
  return UNITY_END();
}
