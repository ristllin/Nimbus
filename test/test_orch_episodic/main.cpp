#include <unity.h>

#include <string>

#include "nimbus/orch/episodic.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

static EpisodicMessage msg(const std::string& id, const std::string& sess, uint32_t ts,
                           const std::string& role, MsgKind kind, const std::string& text) {
  EpisodicMessage m;
  m.id = id; m.sessionId = sess; m.tsHours = ts; m.role = role; m.kind = kind; m.text = text;
  return m;
}

// ---- kind name mapping ------------------------------------------------------
static void test_kind_names_roundtrip() {
  TEST_ASSERT_EQUAL_STRING("transcript", kindName(MsgKind::Transcript));
  TEST_ASSERT_EQUAL_STRING("image", kindName(MsgKind::Image));
  MsgKind k;
  TEST_ASSERT_TRUE(kindFromName("audio", k));
  TEST_ASSERT_TRUE(k == MsgKind::Audio);
  TEST_ASSERT_FALSE(kindFromName("bogus", k));
}

// ---- sessions ---------------------------------------------------------------
static void test_sessions_upsert_and_status_filter() {
  InMemoryEpisodicStore st;
  st.addSession({"s1", 10, "openai", "task A", "active"});
  st.addSession({"s2", 11, "anthropic", "task B", "active"});
  st.setSessionStatus("s1", "completed");
  TEST_ASSERT_EQUAL_INT(2, (int)st.sessions().size());
  TEST_ASSERT_EQUAL_INT(1, (int)st.sessions("active").size());
  TEST_ASSERT_EQUAL_STRING("s2", st.sessions("active")[0].id.c_str());
  // upsert replaces in place (no dup)
  st.addSession({"s1", 10, "openai", "task A v2", "active"});
  TEST_ASSERT_EQUAL_INT(2, (int)st.sessions().size());
}

// ---- message queries: session / kind / time / text -------------------------
static void test_query_by_session_and_kind() {
  InMemoryEpisodicStore st;
  st.addMessage(msg("m1", "s1", 1, "user", MsgKind::Message, "hello"));
  st.addMessage(msg("m2", "s1", 2, "assistant", MsgKind::LlmResponse, "hi there"));
  st.addMessage(msg("m3", "s2", 3, "user", MsgKind::Message, "other session"));
  st.addMessage(msg("m4", "s1", 4, "tool", MsgKind::Transcript, "spoken words"));

  MsgQuery q; q.sessionId = "s1";
  TEST_ASSERT_EQUAL_INT(3, (int)st.query(q).size());

  MsgQuery qk; qk.haveKind = true; qk.kind = MsgKind::Transcript;
  auto r = st.query(qk);
  TEST_ASSERT_EQUAL_INT(1, (int)r.size());
  TEST_ASSERT_EQUAL_STRING("m4", r[0].id.c_str());
}

static void test_query_time_window_and_recency_order() {
  InMemoryEpisodicStore st;
  for (uint32_t t = 1; t <= 5; t++)
    st.addMessage(msg("m" + std::to_string(t), "s1", t, "user", MsgKind::Message, "x"));
  MsgQuery q; q.sinceHours = 2; q.beforeHours = 5;  // t in [2,5) -> 2,3,4
  auto r = st.query(q);
  TEST_ASSERT_EQUAL_INT(3, (int)r.size());
  // newest-first
  TEST_ASSERT_EQUAL_STRING("m4", r[0].id.c_str());
  TEST_ASSERT_EQUAL_STRING("m2", r[2].id.c_str());
}

static void test_query_full_text_and_limit() {
  InMemoryEpisodicStore st;
  st.addMessage(msg("m1", "s1", 1, "user", MsgKind::Message, "the ship date is Friday"));
  st.addMessage(msg("m2", "s1", 2, "user", MsgKind::Message, "unrelated chatter"));
  st.addMessage(msg("m3", "s1", 3, "user", MsgKind::Message, "shipping now"));

  MsgQuery q; q.textContains = "ship";
  auto r = st.query(q);
  TEST_ASSERT_EQUAL_INT(2, (int)r.size());  // m1 + m3

  MsgQuery lim; lim.limit = 1;
  TEST_ASSERT_EQUAL_INT(1, (int)st.query(lim).size());
  TEST_ASSERT_EQUAL_STRING("m3", st.query(lim)[0].id.c_str());  // most recent
}

// ---- ring cap: a host-less device can't OOM -------------------------------
static void test_ring_cap_drops_oldest() {
  InMemoryEpisodicStore st(3);  // tiny cap
  for (int i = 1; i <= 5; i++)
    st.addMessage(msg("m" + std::to_string(i), "s1", i, "user", MsgKind::Message, "x"));
  TEST_ASSERT_EQUAL_INT(3, st.messageCount());  // only last 3 kept
  MsgQuery q; q.limit = 10;
  auto r = st.query(q);
  TEST_ASSERT_EQUAL_STRING("m5", r[0].id.c_str());   // newest
  TEST_ASSERT_EQUAL_STRING("m3", r[2].id.c_str());   // oldest surviving
}

// ---- blob rows are first-class (media referenced by path) ------------------
static void test_blob_referenced_row() {
  InMemoryEpisodicStore st;
  EpisodicMessage m = msg("v1", "s1", 1, "user", MsgKind::Audio, "");
  m.blobPath = "/sd/mem/blobs/abc123.wav";
  st.addMessage(m);
  MsgQuery q; q.haveKind = true; q.kind = MsgKind::Audio;
  auto r = st.query(q);
  TEST_ASSERT_EQUAL_INT(1, (int)r.size());
  TEST_ASSERT_EQUAL_STRING("/sd/mem/blobs/abc123.wav", r[0].blobPath.c_str());
}

// ---- binary persistence round-trip (device LittleFS blob) ------------------
static void test_serialize_roundtrip() {
  InMemoryEpisodicStore a;
  EpisodicSession s; s.id = "chat1"; s.startedHours = 100; s.provider = "openai";
  s.title = "the codename talk"; s.status = "active";
  a.addSession(s);
  a.addMessage(msg("m1", "chat1", 101, "user", MsgKind::Message, "what is the codename?"));
  EpisodicMessage im = msg("m2", "chat1", 102, "assistant", MsgKind::Image, "here you go");
  im.blobPath = "/data/blobs/x.png"; im.tags = "reply,media";
  a.addMessage(im);
  a.addMessage(msg("m3", "chat1", 103, "assistant", MsgKind::Message, "BLUEBIRD"));

  std::string blob = a.serialize();
  InMemoryEpisodicStore b;
  TEST_ASSERT_TRUE(b.deserialize(blob));
  TEST_ASSERT_EQUAL_INT(3, b.messageCount());
  TEST_ASSERT_EQUAL_INT(1, (int)b.sessions().size());
  TEST_ASSERT_EQUAL_STRING("the codename talk", b.sessions()[0].title.c_str());
  MsgQuery q; q.limit = 10;
  auto r = b.query(q);                                   // newest-first
  TEST_ASSERT_EQUAL_STRING("BLUEBIRD", r[0].text.c_str());
  // media row survives with its kind + blob path + tags
  MsgQuery iq; iq.haveKind = true; iq.kind = MsgKind::Image;
  auto ir = b.query(iq);
  TEST_ASSERT_EQUAL_INT(1, (int)ir.size());
  TEST_ASSERT_EQUAL_STRING("/data/blobs/x.png", ir[0].blobPath.c_str());
  TEST_ASSERT_EQUAL_STRING("reply,media", ir[0].tags.c_str());
}

// ---- deserialize is tolerant of garbage / truncation -----------------------
static void test_deserialize_rejects_garbage() {
  InMemoryEpisodicStore a;
  a.addMessage(msg("m1", "s", 1, "user", MsgKind::Message, "keep me"));
  TEST_ASSERT_FALSE(a.deserialize("not an episodic blob"));  // bad magic -> false
  TEST_ASSERT_EQUAL_INT(0, a.messageCount());               // ...and cleared, not corrupt
  // truncated: valid magic but cut mid-record -> false, keeps what parsed
  std::string good = InMemoryEpisodicStore().serialize();   // "EP01" + zero counts
  TEST_ASSERT_TRUE(a.deserialize(good));
  TEST_ASSERT_EQUAL_INT(0, a.messageCount());
}

// A LittleFS blob can be cut mid-write by power loss and is read at device boot.
// deserialize must reject EVERY mid-record truncation cleanly - each getStr/getU32
// bounds-checks, so no offset may over-read or crash. (The sibling test above only
// covered bad-magic + a zero-count blob; this pins the actual truncation path.)
static void test_deserialize_survives_every_truncation() {
  InMemoryEpisodicStore a;
  a.addSession({"s1", 100, "openai", "a title", "active"});
  a.addMessage(msg("m1", "s1", 101, "user", MsgKind::Message, "hello world"));
  EpisodicMessage im = msg("m2", "s1", 102, "assistant", MsgKind::Image, "img");
  im.blobPath = "/data/blobs/x.png"; im.tags = "media";
  a.addMessage(im);
  const std::string full = a.serialize();

  // Every prefix short of the whole blob truncates a record -> must return false
  // and never read out of bounds (serialize emits no trailing padding, so only the
  // complete blob parses; a wrong bound would fault here under a sanitizer). Starts
  // at 4 to keep the "EP01" magic. NB: deserialize's contract is "keep what parsed",
  // so a rejected blob may leave a partial store - we only assert it is never
  // mistaken for VALID and never fabricates more records than the source held.
  for (size_t cut = 4; cut < full.size(); ++cut) {
    InMemoryEpisodicStore b;
    TEST_ASSERT_FALSE(b.deserialize(full.substr(0, cut)));
    TEST_ASSERT_TRUE(b.messageCount() <= 2);
  }
  // The intact blob still round-trips.
  InMemoryEpisodicStore ok;
  TEST_ASSERT_TRUE(ok.deserialize(full));
  TEST_ASSERT_EQUAL_INT(2, ok.messageCount());
}


// v3.7.0 media: a photo is stored as an Image row whose TEXT is its description.
// The conversation window must include those kinds - filtering to Message alone
// made every picture invisible to the model one turn later - while still keeping
// the trace kinds out, because re-feeding tool output would double the context.
static void test_kind_set_includes_media_but_not_trace() {
  InMemoryEpisodicStore st;
  auto add = [&](const char* id, MsgKind k, const char* text) {
    EpisodicMessage m; m.id = id; m.sessionId = "c1"; m.kind = k; m.text = text;
    m.role = "user"; st.addMessage(m);
  };
  add("1", MsgKind::Message,     "hello");
  add("2", MsgKind::Image,       "a tabby cat on a windowsill");
  add("3", MsgKind::File,        "budget.csv");
  add("4", MsgKind::ToolOutput,  "{\"rows\":42}");
  add("5", MsgKind::LlmResponse, "internal reasoning");

  MsgQuery q;
  q.sessionId = "c1";
  q.haveKind = true; q.kind = MsgKind::Message;
  q.alsoKinds = {MsgKind::Image, MsgKind::File};
  auto rows = st.query(q);
  TEST_ASSERT_EQUAL_INT(3, (int)rows.size());
  bool sawImage = false, sawTrace = false;
  for (const auto& r : rows) {
    if (r.kind == MsgKind::Image) sawImage = true;
    if (r.kind == MsgKind::ToolOutput || r.kind == MsgKind::LlmResponse) sawTrace = true;
  }
  TEST_ASSERT_TRUE(sawImage);
  TEST_ASSERT_FALSE(sawTrace);

  // A single-kind query is unchanged - alsoKinds is additive, never a widening
  // of every existing caller.
  MsgQuery only;
  only.sessionId = "c1";
  only.haveKind = true; only.kind = MsgKind::Message;
  TEST_ASSERT_EQUAL_INT(1, (int)st.query(only).size());

  // And no kind filter at all still returns everything.
  MsgQuery all; all.sessionId = "c1";
  TEST_ASSERT_EQUAL_INT(5, (int)st.query(all).size());
}


static void test_text_match_score() {
  // all-of-terms gate: a missing term scores 0
  TEST_ASSERT_EQUAL_INT(0, textMatchScore("the bilge pump serial", "bilge diesel"));
  // present terms score their summed frequency, case-insensitive
  TEST_ASSERT_EQUAL_INT(3, textMatchScore("Pump the pump, pump it", "pump"));
  TEST_ASSERT_EQUAL_INT(2, textMatchScore("Bilge PUMP and bilge tank", "bilge"));
  // multi-term: sum across terms, order-independent
  TEST_ASSERT_EQUAL_INT(3, textMatchScore("bilge pump; the pump runs", "pump bilge"));
  // empty needle scores 0 (nothing to rank)
  TEST_ASSERT_EQUAL_INT(0, textMatchScore("anything", ""));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_kind_set_includes_media_but_not_trace);
  RUN_TEST(test_kind_names_roundtrip);
  RUN_TEST(test_sessions_upsert_and_status_filter);
  RUN_TEST(test_query_by_session_and_kind);
  RUN_TEST(test_query_time_window_and_recency_order);
  RUN_TEST(test_query_full_text_and_limit);
  RUN_TEST(test_text_match_score);
  RUN_TEST(test_ring_cap_drops_oldest);
  RUN_TEST(test_blob_referenced_row);
  RUN_TEST(test_serialize_roundtrip);
  RUN_TEST(test_deserialize_rejects_garbage);
  RUN_TEST(test_deserialize_survives_every_truncation);
  return UNITY_END();
}
