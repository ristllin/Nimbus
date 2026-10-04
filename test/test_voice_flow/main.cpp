// Host tests for the hold-to-talk flow (lib/core voice_flow, CUM-456).
//
// The owner's report: on RELEASE the ring stayed on the listening color for ~30 s
// (the speech-to-text network call ran first, offline), then the device said
// "Didn't catch that" although the real failure was the network. These tests pin
// the CLASS, not the instance:
//   - release leaves the recording state and shows processing with ZERO network
//     calls made first, whatever the network does afterwards (seam test);
//   - no link / unreachable -> the network line, never the no-speech line, for
//     every failure kind (property over the whole taxonomy);
//   - the state machine counts transitions and fails on oscillation (absence of
//     thrash, AGENTS.md section 3);
//   - every user-facing string is pinned and fits the ring-center layout;
//   - the transcript's turn starts promptly wherever the tg_poll loop is in its
//     Telegram poll cycle (CUM-462, composed with test/support/tg_poll_sim.h).

#include <unity.h>

#include <cstring>
#include <string>
#include <vector>

#include "../support/tg_poll_sim.h"
#include "nimbus/net/tg_poll_sched.h"
#include "nimbus/orch/voice_route.h"
#include "nimbus/voice_flow.h"

using namespace nimbus::voice;
using solide::ring::RGB;

void setUp() {}
void tearDown() {}

// ---- a recording fake of the device seam --------------------------------------

struct FakePort : Port {
  uint32_t clock = 1000;
  bool link = true;
  SttResult stt;
  uint32_t sttCostMs = 1500;    // how long the network call "takes"
  bool sendOk = true;
  const Flow* flow = nullptr;   // to inspect the state from inside transcribe()
  std::vector<std::string> log;
  std::vector<uint32_t> showTimes;
  int shows = 0, transcribes = 0, sends = 0;
  Phase phaseAtTranscribe = Phase::Idle;
  std::string lastShownTitle;
  std::string sentText;
  std::vector<nimbus::sfx::Ev> sounds;

  uint32_t now() override { return clock; }
  void show(const Flow& f) override {
    ++shows;
    showTimes.push_back(clock);
    lastShownTitle = f.line().title;
    log.push_back("show:" + f.line().title);
  }
  bool linkUp() override { log.push_back("link"); return link; }
  SttResult transcribe() override {
    ++transcribes;
    phaseAtTranscribe = flow ? flow->phase() : Phase::Idle;
    log.push_back("transcribe");
    clock += sttCostMs;
    return stt;
  }
  bool sendTurn(const std::string& t) override {
    ++sends;
    sentText = t;
    log.push_back("send");
    return sendOk;
  }
  void sound(nimbus::sfx::Ev e) override { sounds.push_back(e); }

  int indexOf(const std::string& entry) const {
    for (size_t i = 0; i < log.size(); ++i)
      if (log[i] == entry) return int(i);
    return -1;
  }
};

static SttResult okText(const std::string& t) {
  SttResult r;
  r.kind = SttResult::Kind::Ok;
  r.provider = "mistral";
  r.text = t;
  return r;
}

static void startRecording(Flow& f, FakePort& p) {
  p.flow = &f;
  TEST_ASSERT_TRUE(f.press(p.clock));
  TEST_ASSERT_EQUAL(int(Phase::Recording), int(f.phase()));
}

// ---- seam: release shows processing before ANY network I/O ---------------------

static void test_release_shows_processing_before_any_network_call() {
  Flow f;
  FakePort p;
  p.stt = okText("what is on my calendar");
  startRecording(f, p);
  const Outcome o = afterRelease(f, p);
  TEST_ASSERT_EQUAL(int(Outcome::None), int(o));
  const int shown = p.indexOf("show:Transcribing");
  const int net = p.indexOf("transcribe");
  TEST_ASSERT_TRUE_MESSAGE(shown >= 0, "processing was never shown");
  TEST_ASSERT_TRUE_MESSAGE(net >= 0, "online path never transcribed");
  TEST_ASSERT_TRUE_MESSAGE(shown < net, "the network call ran before processing was shown");
  // At the moment the network call started, recording was already over.
  TEST_ASSERT_EQUAL(int(Phase::Transcribing), int(p.phaseAtTranscribe));
  TEST_ASSERT_EQUAL(1, p.transcribes);
  TEST_ASSERT_EQUAL(1, p.sends);
  TEST_ASSERT_EQUAL_STRING("what is on my calendar", p.sentText.c_str());
  TEST_ASSERT_EQUAL(int(Phase::Thinking), int(f.phase()));
  TEST_ASSERT_EQUAL_STRING("Thinking", f.line().title.c_str());
  TEST_ASSERT_EQUAL_STRING("You: what is on my calendar", f.line().detail.c_str());
}

// The release -> visible change latency is independent of the network: a 30 s
// transcription (the owner's offline case) must not delay the first repaint.
static void test_release_repaint_latency_is_independent_of_network_time() {
  for (uint32_t cost : {0u, 1500u, 30000u, 60000u}) {
    Flow f;
    FakePort p;
    p.stt = okText("hello");
    p.sttCostMs = cost;
    startRecording(f, p);
    const uint32_t releasedAt = p.clock;
    afterRelease(f, p);
    TEST_ASSERT_TRUE(p.showTimes.size() >= 1);
    TEST_ASSERT_UINT32_WITHIN_MESSAGE(0, releasedAt, p.showTimes[0],
                                      "first repaint waited on the network");
    TEST_ASSERT_TRUE_MESSAGE(p.showTimes[0] - releasedAt < 200, "release repaint >= 200 ms");
  }
}

static void test_offline_fails_fast_with_the_network_line_and_no_network_call() {
  Flow f;
  FakePort p;
  p.link = false;
  p.stt = okText("");   // would be "empty" if it were ever called - it must not be
  startRecording(f, p);
  const uint32_t releasedAt = p.clock;
  const Outcome o = afterRelease(f, p);
  TEST_ASSERT_EQUAL(int(Outcome::NoNetwork), int(o));
  TEST_ASSERT_EQUAL_MESSAGE(0, p.transcribes, "offline must not attempt the network call");
  TEST_ASSERT_EQUAL(0, p.sends);
  TEST_ASSERT_EQUAL(int(Phase::Notice), int(f.phase()));
  TEST_ASSERT_EQUAL_STRING("No network", f.line().title.c_str());
  TEST_ASSERT_EQUAL_STRING("No network. Check Wi-Fi and try again.", sentence(f.line()).c_str());
  TEST_ASSERT_EQUAL_MESSAGE(0, int(p.clock - releasedAt), "offline error took time");
  TEST_ASSERT_EQUAL(int(Cue::Offline), int(f.cue()));
  // Processing was shown first, then the notice: exactly two renders.
  TEST_ASSERT_EQUAL(2, p.shows);
  TEST_ASSERT_EQUAL_STRING("show:Transcribing", p.log[0].c_str());
  TEST_ASSERT_EQUAL_STRING("show:No network", p.log.back().c_str());
  // No "capture confirmed" sound when the send cannot happen; the error sound only.
  TEST_ASSERT_EQUAL(1, int(p.sounds.size()));
  TEST_ASSERT_EQUAL(int(nimbus::sfx::Ev::Error), int(p.sounds[0]));
}

// Wi-Fi joined but the host unreachable (captive portal, no internet): the
// transport reports "connect failed" -> still the network line, never no-speech.
static void test_unreachable_host_is_the_network_line_not_no_speech() {
  Flow f;
  FakePort p;
  int http = -1;
  p.stt.kind = sttKindForError("connect failed", &http);
  p.stt.provider = "mistral";
  startRecording(f, p);
  TEST_ASSERT_EQUAL(int(Outcome::NoNetwork), int(afterRelease(f, p)));
  TEST_ASSERT_EQUAL(1, p.transcribes);
  TEST_ASSERT_EQUAL_STRING("No network", f.line().title.c_str());
}

static void test_send_failure_is_busy_not_silence() {
  Flow f;
  FakePort p;
  p.stt = okText("remind me at five");
  p.sendOk = false;
  startRecording(f, p);
  TEST_ASSERT_EQUAL(int(Outcome::Busy), int(afterRelease(f, p)));
  TEST_ASSERT_EQUAL(int(Phase::Notice), int(f.phase()));
  TEST_ASSERT_EQUAL_STRING("Busy", f.line().title.c_str());
}

static void test_release_when_not_recording_does_nothing() {
  Flow f;
  FakePort p;
  p.flow = &f;
  TEST_ASSERT_EQUAL(int(Outcome::None), int(afterRelease(f, p)));
  TEST_ASSERT_EQUAL(0, p.shows);
  TEST_ASSERT_EQUAL(0, p.transcribes);
  TEST_ASSERT_EQUAL(0u, f.transitions());
}

// ---- the taxonomy, as a property over every failure kind -----------------------

static const SttResult::Kind kFailKinds[] = {
    SttResult::Kind::NoNetwork, SttResult::Kind::Http,     SttResult::Kind::Refused,
    SttResult::Kind::BadReply,  SttResult::Kind::Busy,     SttResult::Kind::NoAudio,
};

static void test_no_failure_kind_ever_reads_as_no_speech() {
  const std::string noSpeech = sentence(lineFor(Outcome::EmptyTranscript));
  for (SttResult::Kind k : kFailKinds) {
    for (const char* text : {"", "   ", "hello"}) {
      SttResult r;
      r.kind = k;
      r.provider = "mistral";
      r.text = text;   // a failure never becomes "empty" even with an empty body
      const Outcome o = classify(r);
      TEST_ASSERT_TRUE_MESSAGE(o != Outcome::EmptyTranscript, "a failure read as no-speech");
      TEST_ASSERT_TRUE_MESSAGE(o != Outcome::None, "a failure read as success");
      TEST_ASSERT_TRUE(sentence(lineFor(o, r)) != noSpeech);
      // ... and the same holds end to end through the release path.
      Flow f;
      FakePort p;
      p.stt = r;
      startRecording(f, p);
      TEST_ASSERT_TRUE(afterRelease(f, p) != Outcome::EmptyTranscript);
      TEST_ASSERT_TRUE(f.line().title != "Didn't catch that");
    }
  }
}

static void test_empty_transcript_only_from_a_reachable_provider() {
  for (const char* text : {"", " ", "\n", " \t "}) {
    TEST_ASSERT_EQUAL(int(Outcome::EmptyTranscript), int(classify(okText(text))));
  }
  TEST_ASSERT_EQUAL(int(Outcome::None), int(classify(okText("hi"))));
  Flow f;
  FakePort p;
  p.stt = okText("  ");
  startRecording(f, p);
  TEST_ASSERT_EQUAL(int(Outcome::EmptyTranscript), int(afterRelease(f, p)));
  TEST_ASSERT_EQUAL_STRING("Didn't catch that. Hold the mic button and speak.",
                           sentence(f.line()).c_str());
  TEST_ASSERT_EQUAL(0, p.sends);
  TEST_ASSERT_EQUAL(int(Cue::None), int(f.cue()));   // calm: no alarm for silence
}

static void test_transport_error_mapping() {
  int http = -1;
  TEST_ASSERT_EQUAL(int(SttResult::Kind::NoNetwork), int(sttKindForError(kErrConnectFailed, &http)));
  // The uploader's wire words (shared constants: producer and classifier agree).
  TEST_ASSERT_EQUAL_STRING("connect failed", kErrConnectFailed);
  TEST_ASSERT_EQUAL_STRING("tls arbiter busy", kErrSlotBusy);
  TEST_ASSERT_EQUAL_STRING("file open failed", kErrFileOpen);
  TEST_ASSERT_EQUAL(0, http);
  TEST_ASSERT_EQUAL(int(SttResult::Kind::Busy), int(sttKindForError("tls arbiter busy", &http)));
  TEST_ASSERT_EQUAL(int(SttResult::Kind::NoAudio), int(sttKindForError("file open failed", &http)));
  TEST_ASSERT_EQUAL(int(SttResult::Kind::Http), int(sttKindForError("HTTP 401", &http)));
  TEST_ASSERT_EQUAL(401, http);
  TEST_ASSERT_EQUAL(int(SttResult::Kind::Http), int(sttKindForError("HTTP 0", &http)));
  TEST_ASSERT_EQUAL(0, http);
  TEST_ASSERT_EQUAL(int(SttResult::Kind::Http), int(sttKindForError("something odd", &http)));
  TEST_ASSERT_EQUAL(0, http);
  TEST_ASSERT_EQUAL(int(SttResult::Kind::Http), int(sttKindForError("HTTP 503", nullptr)));
}

static void test_http_error_names_the_provider_and_status() {
  struct { const char* slug; const char* title; } provs[] = {
      {"mistral", "Mistral error"}, {"openai", "OpenAI error"}, {"cumulo", "Cumulo error"},
      {"", "Voice error"}};
  for (const auto& pv : provs) {
    SttResult r;
    r.kind = SttResult::Kind::Http;
    r.provider = pv.slug;
    r.http = 500;
    const Line l = lineFor(Outcome::SttHttp, r);
    TEST_ASSERT_EQUAL_STRING(pv.title, l.title.c_str());
    TEST_ASSERT_EQUAL_STRING("Speech-to-text HTTP 500. Try again.", l.detail.c_str());
  }
  SttResult r;
  r.kind = SttResult::Kind::Http;
  r.provider = "mistral";
  r.http = 401;
  TEST_ASSERT_EQUAL_STRING("Key rejected (HTTP 401). Check it in the web app.",
                           lineFor(Outcome::SttHttp, r).detail.c_str());
  r.http = 403;
  TEST_ASSERT_EQUAL_STRING("Key rejected (HTTP 403). Check it in the web app.",
                           lineFor(Outcome::SttHttp, r).detail.c_str());
  r.http = 429;
  TEST_ASSERT_EQUAL_STRING("Too many requests (HTTP 429). Wait a minute.",
                           lineFor(Outcome::SttHttp, r).detail.c_str());
  r.http = 0;
  TEST_ASSERT_EQUAL_STRING("No answer from speech-to-text. Try again.",
                           lineFor(Outcome::SttHttp, r).detail.c_str());
}

// Every remaining string, pinned exactly.
static void test_every_line_is_pinned() {
  struct { Outcome o; const char* s; } want[] = {
      {Outcome::NoNetwork, "No network. Check Wi-Fi and try again."},
      {Outcome::SttBadReply, "Voice error. Unreadable speech-to-text reply. Try again."},
      {Outcome::Busy, "Busy. Another request is running. Try again in a moment."},
      {Outcome::NoAudio, "No audio. The mic recorded nothing. Try again."},
      {Outcome::EmptyTranscript, "Didn't catch that. Hold the mic button and speak."},
      {Outcome::TurnError, "No answer. The assistant could not finish. Try again."},
      {Outcome::NoReply, "No reply. Nothing came back. Try again."},
      {Outcome::SttRefused, "Voice unavailable. Try again soon."},
  };
  for (const auto& w : want)
    TEST_ASSERT_EQUAL_STRING(w.s, sentence(lineFor(w.o)).c_str());
  SttResult refused;
  refused.kind = SttResult::Kind::Refused;
  refused.refusal = nimbus::orch::voiceRefusalStatus("funding_cap_reached");
  TEST_ASSERT_EQUAL_STRING("Voice unavailable. Voice is out of credit for now. Try again later.",
                           sentence(lineFor(Outcome::SttRefused, refused)).c_str());
  TEST_ASSERT_TRUE(lineFor(Outcome::None).title.empty());
  // Phase lines.
  Flow f;
  TEST_ASSERT_TRUE(f.line().title.empty());
  f.press(0);
  TEST_ASSERT_EQUAL_STRING("Listening. Release to send.", sentence(f.line()).c_str());
  f.release(1);
  TEST_ASSERT_EQUAL_STRING("Transcribing. One moment.", sentence(f.line()).c_str());
  TEST_ASSERT_EQUAL_STRING("Mistral", providerName("mistral").c_str());
  TEST_ASSERT_EQUAL_STRING("OpenAI", providerName("openai").c_str());
  TEST_ASSERT_EQUAL_STRING("Cumulo", providerName("cumulo").c_str());
  TEST_ASSERT_EQUAL_STRING("Speech-to-text", providerName("zai").c_str());
}

// Copy style (AGENTS.md section 6) + the ring-center layout, for EVERY outcome and
// every detail variant: printable ASCII, no em dash, no " - ", no "!", a title that
// fits, a detail that wraps into the lines the ring center has without truncating.
static void assertCopy(const Line& l) {
  const std::string all = sentence(l);
  for (char c : all) TEST_ASSERT_TRUE_MESSAGE(c >= 0x20 && c < 0x7F, all.c_str());
  TEST_ASSERT_TRUE_MESSAGE(all.find(" - ") == std::string::npos, all.c_str());
  TEST_ASSERT_TRUE_MESSAGE(all.find('!') == std::string::npos, all.c_str());
  TEST_ASSERT_FALSE_MESSAGE(l.title.empty(), all.c_str());
  TEST_ASSERT_TRUE_MESSAGE(l.title.size() <= kTitleMaxChars, l.title.c_str());
  TEST_ASSERT_TRUE_MESSAGE(all.back() == '.', all.c_str());
  const auto lines = wrap(l.detail, kTitleMaxChars, kDetailMaxLines);
  TEST_ASSERT_TRUE_MESSAGE(!lines.empty(), all.c_str());
  TEST_ASSERT_TRUE_MESSAGE(lines.back().size() < 3 ||
                               lines.back().compare(lines.back().size() - 3, 3, "...") != 0,
                           (std::string("detail truncated: ") + all).c_str());
  for (const auto& ln : lines) TEST_ASSERT_TRUE(ln.size() <= kTitleMaxChars);
}

static void test_copy_style_and_fit_for_every_outcome() {
  const char* slugs[] = {"mistral", "openai", "cumulo", "", "zai"};
  const int codes[] = {0, 400, 401, 403, 404, 413, 429, 500, 502, 503};
  const char* refusals[] = {"funding_cap_reached", "rate_limited", "audio_duration_unknown",
                            "unsupported_media_type", "", "something_new"};
  for (int oi = 1; oi < kOutcomeCount; ++oi) {
    const Outcome o = Outcome(oi);
    for (const char* slug : slugs) {
      for (int code : codes) {
        for (const char* rc : refusals) {
          SttResult r;
          r.provider = slug;
          r.http = code;
          r.refusal = nimbus::orch::voiceRefusalStatus(rc);
          assertCopy(lineFor(o, r));
        }
      }
    }
  }
  Flow f;
  f.press(0);
  assertCopy(f.line());
  f.release(0);
  assertCopy(f.line());
}

static void test_cue_and_sound_per_outcome() {
  for (int oi = 0; oi < kOutcomeCount; ++oi) {
    const Outcome o = Outcome(oi);
    nimbus::sfx::Ev e = nimbus::sfx::Ev::Boot;
    const bool loud = sfxFor(o, e);
    if (o == Outcome::None || o == Outcome::EmptyTranscript) {
      TEST_ASSERT_EQUAL(int(Cue::None), int(cueFor(o)));
      TEST_ASSERT_FALSE(loud);
    } else if (o == Outcome::NoNetwork) {
      TEST_ASSERT_EQUAL(int(Cue::Offline), int(cueFor(o)));
      TEST_ASSERT_TRUE(loud);
      TEST_ASSERT_EQUAL(int(nimbus::sfx::Ev::Error), int(e));
    } else {
      TEST_ASSERT_EQUAL(int(Cue::Alert), int(cueFor(o)));
      TEST_ASSERT_TRUE(loud);
      TEST_ASSERT_EQUAL(int(nimbus::sfx::Ev::Error), int(e));
    }
  }
}

// ---- state machine: transitions counted, no oscillation --------------------------

static void test_happy_path_is_exactly_four_transitions() {
  Flow f;
  TEST_ASSERT_TRUE(f.press(0));
  TEST_ASSERT_TRUE(f.release(1000));
  TEST_ASSERT_TRUE(f.transcribed("hi", 2500));
  // A storm of ticks while the turn runs changes nothing.
  for (uint32_t t = 2500; t < 2500 + 50000; t += 33) TEST_ASSERT_FALSE(f.tick(true, t));
  f.replyLanded(52000);   // a reply mid-turn: processing continues until the turn ends
  TEST_ASSERT_EQUAL(int(Phase::Thinking), int(f.phase()));
  TEST_ASSERT_TRUE(f.turnEnded(true, 52010));
  TEST_ASSERT_EQUAL(int(Phase::Idle), int(f.phase()));
  TEST_ASSERT_EQUAL(4u, f.transitions());
  // Duplicate and late events are ignored - no extra transitions.
  TEST_ASSERT_FALSE(f.release(52100));
  TEST_ASSERT_FALSE(f.transcribed("late", 52100));
  TEST_ASSERT_FALSE(f.turnEnded(false, 52100));
  f.replyLanded(52100);   // a late reply after the end: no-op
  TEST_ASSERT_FALSE(f.dismiss(52100));
  for (uint32_t t = 52100; t < 300000; t += 500) TEST_ASSERT_FALSE(f.tick(false, t));
  TEST_ASSERT_EQUAL(4u, f.transitions());
  TEST_ASSERT_TRUE(f.replyShown());
}

static void test_busy_flow_ignores_a_second_press() {
  Flow f;
  f.press(0);
  TEST_ASSERT_FALSE(f.press(10));   // re-entry while held
  f.release(20);
  TEST_ASSERT_FALSE(f.press(30));   // while transcribing
  f.transcribed("x", 40);
  TEST_ASSERT_FALSE(f.press(50));   // while thinking
  TEST_ASSERT_EQUAL(3u, f.transitions());
  TEST_ASSERT_TRUE(f.busy());
  TEST_ASSERT_FALSE(f.canPress());
}

static void test_notice_holds_then_expires_once_and_can_retry() {
  Flow f;
  f.press(0);
  f.release(100);
  TEST_ASSERT_TRUE(f.fail(Outcome::NoNetwork, lineFor(Outcome::NoNetwork), 100));
  TEST_ASSERT_TRUE(f.ownsRing());
  for (uint32_t t = 100; t < 100 + kNoticeHoldMs; t += 33) TEST_ASSERT_FALSE(f.tick(false, t));
  TEST_ASSERT_TRUE(f.tick(false, 100 + kNoticeHoldMs));
  TEST_ASSERT_EQUAL(int(Phase::Idle), int(f.phase()));
  TEST_ASSERT_FALSE(f.ownsRing());
  TEST_ASSERT_EQUAL(4u, f.transitions());
  // Retry straight from a notice (the owner holds the mic again).
  Flow g;
  g.press(0);
  g.release(1);
  g.fail(Outcome::SttHttp, lineFor(Outcome::SttHttp), 2);
  TEST_ASSERT_TRUE(g.canPress());
  TEST_ASSERT_TRUE(g.press(3));
  TEST_ASSERT_EQUAL(int(Outcome::None), int(g.outcome()));
  // A touch dismisses.
  Flow h;
  h.press(0);
  h.release(1);
  h.fail(Outcome::Busy, lineFor(Outcome::Busy), 2);
  TEST_ASSERT_TRUE(h.dismiss(3));
  TEST_ASSERT_FALSE(h.dismiss(4));
}

static void test_thinking_backstops() {
  // No turn ever runs: NoReply exactly at kQuietMs, not a moment before.
  Flow f;
  f.press(0);
  f.release(0);
  f.transcribed("x", 1000);
  TEST_ASSERT_FALSE(f.tick(false, 1000 + kQuietMs - 1));
  TEST_ASSERT_TRUE(f.tick(false, 1000 + kQuietMs));
  TEST_ASSERT_EQUAL(int(Outcome::NoReply), int(f.outcome()));
  TEST_ASSERT_EQUAL_STRING("No reply", f.line().title.c_str());
  // A running turn (even a long one) is never called "no reply" before the ceiling.
  Flow g;
  g.press(0);
  g.release(0);
  g.transcribed("x", 0);
  for (uint32_t t = 0; t < kMaxThinkMs; t += 1000) TEST_ASSERT_FALSE(g.tick(true, t));
  TEST_ASSERT_TRUE(g.tick(true, kMaxThinkMs));
  TEST_ASSERT_EQUAL(int(Outcome::NoReply), int(g.outcome()));
  // Queued behind another turn, then ours ends quietly after a shown reply -> Idle.
  Flow h;
  h.press(0);
  h.release(0);
  h.transcribed("x", 0);
  TEST_ASSERT_FALSE(h.tick(true, 20000));
  h.replyLanded(21000);
  TEST_ASSERT_TRUE(h.tick(false, 21000 + kQuietMs));
  TEST_ASSERT_EQUAL(int(Phase::Idle), int(h.phase()));
  TEST_ASSERT_EQUAL(int(Outcome::None), int(h.outcome()));
}

static void test_turn_end_and_reply_outcomes() {
  Flow f;
  f.press(0);
  f.release(0);
  f.transcribed("x", 0);
  TEST_ASSERT_TRUE(f.turnEnded(false, 10));
  TEST_ASSERT_EQUAL(int(Outcome::TurnError), int(f.outcome()));
  TEST_ASSERT_EQUAL(int(Cue::Alert), int(f.cue()));
  // A reply on its own never ends processing (a stray sub-agent result or a mid-turn
  // notice is not the answer): only the message's own end does.
  Flow g;
  g.press(0);
  g.release(0);
  g.transcribed("x", 0);
  g.replyLanded(10);
  TEST_ASSERT_EQUAL(int(Phase::Thinking), int(g.phase()));
  TEST_ASSERT_TRUE(g.replyShown());
  TEST_ASSERT_TRUE(g.turnEnded(true, 20));
  TEST_ASSERT_EQUAL(int(Phase::Idle), int(g.phase()));
}

// Fuzz the event stream: every transition must follow an allowed edge, Recording
// is entered only by an accepted press, and nothing ever moves without an event.
static bool allowedEdge(Phase a, Phase b) {
  switch (a) {
    case Phase::Idle:         return b == Phase::Recording;
    case Phase::Recording:    return b == Phase::Transcribing;
    case Phase::Transcribing: return b == Phase::Thinking || b == Phase::Notice;
    case Phase::Thinking:     return b == Phase::Idle || b == Phase::Notice;
    case Phase::Notice:       return b == Phase::Idle || b == Phase::Recording;
  }
  return false;
}

struct Lcg {
  uint32_t seed = 0x5eed1234u;
  uint32_t next() { seed = seed * 1664525u + 1013904223u; return seed >> 8; }
};

// One random event; returns whether the flow reported a transition.
static bool randomEvent(Flow& f, Lcg& rnd, uint32_t now, uint32_t& presses) {
  const bool flag = rnd.next() % 2;
  switch (rnd.next() % 9) {
    case 0: { const bool c = f.press(now); presses += c; return c; }
    case 1: return f.release(now);
    case 2: return f.transcribed("x", now);
    case 3: return f.fail(Outcome(1 + rnd.next() % (kOutcomeCount - 1)), lineFor(Outcome::Busy), now);
    case 4: f.replyLanded(now); return false;
    case 5: return f.turnEnded(flag, now);
    case 6: return f.tick(flag, now);
    case 7: return f.dismiss(now);
    default: return false;   // no event
  }
}

static void test_random_event_storm_never_takes_an_illegal_edge() {
  Lcg rnd;
  Flow f;
  uint32_t now = 0, presses = 0, recordingEntries = 0;
  for (int i = 0; i < 20000; ++i) {
    now += rnd.next() % 5000;
    const Phase before = f.phase();
    const uint32_t tBefore = f.transitions();
    const bool changed = randomEvent(f, rnd, now, presses);
    TEST_ASSERT_EQUAL(changed ? tBefore + 1 : tBefore, f.transitions());
    if (!changed) TEST_ASSERT_EQUAL(int(before), int(f.phase()));
    if (changed) TEST_ASSERT_TRUE_MESSAGE(allowedEdge(before, f.phase()), "illegal voice-flow edge");
    if (changed && f.phase() == Phase::Recording) ++recordingEntries;
    // The ring is owned exactly while a cue is live, and Idle never owns it.
    if (f.phase() == Phase::Idle) TEST_ASSERT_FALSE(f.ownsRing());
  }
  TEST_ASSERT_EQUAL(presses, recordingEntries);
  TEST_ASSERT_TRUE(presses > 10);
}

// Render thrash: the release path repaints once per state change, never more.
static void test_release_path_renders_once_per_transition() {
  struct Case { bool link; SttResult r; bool send; };
  SttResult http;
  http.kind = SttResult::Kind::Http;
  http.http = 500;
  const Case cases[] = {{true, okText("hi"), true},  {false, okText("hi"), true},
                        {true, okText(""), true},    {true, http, true},
                        {true, okText("hi"), false}};
  for (const Case& c : cases) {
    Flow f;
    FakePort p;
    p.link = c.link;
    p.stt = c.r;
    p.sendOk = c.send;
    startRecording(f, p);
    const uint32_t t0 = f.transitions();
    afterRelease(f, p);
    TEST_ASSERT_EQUAL_MESSAGE(int(f.transitions() - t0), p.shows,
                              "renders != state changes in the release path");
  }
}

// ---- wrap + ring frames --------------------------------------------------------------

static void test_wrap() {
  auto w = wrap("Check Wi-Fi and try again.", 17, 4);
  TEST_ASSERT_EQUAL(2, int(w.size()));
  TEST_ASSERT_EQUAL_STRING("Check Wi-Fi and", w[0].c_str());
  TEST_ASSERT_EQUAL_STRING("try again.", w[1].c_str());
  w = wrap("abcdefghijklmnopqrstuvwxyz", 10, 4);   // hard split
  TEST_ASSERT_EQUAL(3, int(w.size()));
  TEST_ASSERT_EQUAL_STRING("abcdefghij", w[0].c_str());
  TEST_ASSERT_EQUAL_STRING("uvwxyz", w[2].c_str());
  w = wrap("one two three four five six seven eight", 9, 2);   // overflow
  TEST_ASSERT_EQUAL(2, int(w.size()));
  TEST_ASSERT_EQUAL_STRING("three...", w[1].c_str());
  for (const auto& ln : w) TEST_ASSERT_TRUE(ln.size() <= 9);
  TEST_ASSERT_EQUAL(0, int(wrap("", 17, 4).size()));
  TEST_ASSERT_EQUAL(0, int(wrap("   ", 17, 4).size()));
  TEST_ASSERT_EQUAL(0, int(wrap("x", 0, 4).size()));
}

static int brightest(const RGB* f, int n) {
  int best = 0;
  for (int i = 1; i < n; ++i)
    if (f[i].r + f[i].g + f[i].b > f[best].r + f[best].g + f[best].b) best = i;
  return best;
}

static void test_cue_frames() {
  const int n = 45;
  const RGB accent{255, 128, 0}, alert{255, 0, 0};
  RGB a[n], b[n];
  // Listening + Alert: steady, whole ring, their color.
  cueFrame(Cue::Listening, 0, accent, alert, a, n);
  cueFrame(Cue::Listening, 5000, accent, alert, b, n);
  TEST_ASSERT_EQUAL_MEMORY(a, b, sizeof a);
  TEST_ASSERT_EQUAL(255, a[17].r);
  TEST_ASSERT_EQUAL(128, a[17].g);
  cueFrame(Cue::Alert, 123, accent, alert, a, n);
  for (int i = 0; i < n; ++i) TEST_ASSERT_EQUAL(0, a[i].g);
  TEST_ASSERT_EQUAL(255, a[0].r);
  // Processing: a comet whose head MOVES, with one brightest LED and a lit floor.
  cueFrame(Cue::Processing, 0, accent, alert, a, n);
  cueFrame(Cue::Processing, kSpinMs / 3, accent, alert, b, n);
  TEST_ASSERT_TRUE_MESSAGE(brightest(a, n) != brightest(b, n), "processing ring does not move");
  int maxCount = 0;
  for (int i = 0; i < n; ++i)
    if (a[i].r == 255) ++maxCount;
  TEST_ASSERT_EQUAL(1, maxCount);
  for (int i = 0; i < n; ++i) TEST_ASSERT_TRUE_MESSAGE(a[i].r > 0, "processing floor went dark");
  cueFrame(Cue::Processing, kSpinMs, accent, alert, b, n);   // one full sweep
  TEST_ASSERT_EQUAL_MEMORY(a, b, sizeof a);
  // Offline: breathes (brightness changes over time) in the alert color.
  cueFrame(Cue::Offline, 0, accent, alert, a, n);
  cueFrame(Cue::Offline, kOfflineBreatheMs / 2, accent, alert, b, n);
  TEST_ASSERT_TRUE(a[0].r != b[0].r);
  TEST_ASSERT_EQUAL(0, a[0].g);
  // None: dark.
  cueFrame(Cue::None, 0, accent, alert, a, n);
  for (int i = 0; i < n; ++i) TEST_ASSERT_EQUAL(0, a[i].r + a[i].g + a[i].b);
  cueFrame(Cue::Alert, 0, accent, alert, nullptr, n);   // tolerated, no crash
}

static void test_flow_cue_per_phase() {
  Flow f;
  TEST_ASSERT_EQUAL(int(Cue::None), int(f.cue()));
  f.press(0);
  TEST_ASSERT_EQUAL(int(Cue::Listening), int(f.cue()));
  f.release(0);
  TEST_ASSERT_EQUAL(int(Cue::Processing), int(f.cue()));
  f.transcribed("x", 0);
  TEST_ASSERT_EQUAL(int(Cue::Processing), int(f.cue()));
  f.fail(Outcome::EmptyTranscript, lineFor(Outcome::EmptyTranscript), 0);   // calm notice
  TEST_ASSERT_EQUAL(int(Phase::Notice), int(f.phase()));
  TEST_ASSERT_FALSE(f.ownsRing());
  TEST_ASSERT_FALSE(f.fail(Outcome::None, Line{}, 1));
}

static void test_named_refusals_and_names() {
  for (const char* c : {"funding_cap_reached", "rate_limited", "audio_duration_unknown",
                        "unsupported_media_type"})
    TEST_ASSERT_TRUE_MESSAGE(nimbus::orch::voiceRefusalKnown(c), c);
  // A provider's own error code (a rejected key) is NOT a named refusal: it is shown
  // as its HTTP status, never reworded as "unavailable".
  for (const char* c : {"", "invalid_api_key", "Unauthorized", "rate_limit_exceeded"})
    TEST_ASSERT_FALSE_MESSAGE(nimbus::orch::voiceRefusalKnown(c), c);
  // Known <=> it has its own line: one table backs both, so they cannot drift.
  const std::string generic = nimbus::orch::voiceRefusalStatus("");
  for (const char* c : {"funding_cap_reached", "rate_limited", "audio_duration_unknown",
                        "unsupported_media_type", "x", ""})
    TEST_ASSERT_EQUAL(nimbus::orch::voiceRefusalKnown(c),
                      nimbus::orch::voiceRefusalStatus(c) != generic);
  TEST_ASSERT_EQUAL_STRING("transcribing", phaseName(Phase::Transcribing));
  TEST_ASSERT_EQUAL_STRING("notice", phaseName(Phase::Notice));
  TEST_ASSERT_EQUAL_STRING("no_network", outcomeName(Outcome::NoNetwork));
  TEST_ASSERT_EQUAL_STRING("empty_transcript", outcomeName(Outcome::EmptyTranscript));
  for (int oi = 0; oi < kOutcomeCount; ++oi)
    TEST_ASSERT_TRUE(std::strcmp(outcomeName(Outcome(oi)), "unknown") != 0);
  for (int pi = 0; pi <= int(Phase::Notice); ++pi)
    TEST_ASSERT_TRUE(std::strcmp(phaseName(Phase(pi)), "unknown") != 0);
}

static void test_blocked_lines_are_pinned() {
  TEST_ASSERT_EQUAL_STRING("Updating firmware. Try again after the restart.",
                           blockedLine(Block::Updating).c_str());
  TEST_ASSERT_EQUAL_STRING("Voice needs a speech-to-text key. Set one in the web app.",
                           blockedLine(Block::NoKey).c_str());
  for (Block b : {Block::Updating, Block::NoKey}) {
    const std::string l = blockedLine(b);
    TEST_ASSERT_TRUE(l.find(" - ") == std::string::npos);
    for (char c : l) TEST_ASSERT_TRUE(c >= 0x20 && c < 0x7F);
  }
  // A keyless refusal reads through the SttRefused line, never as no-speech.
  SttResult r;
  r.kind = SttResult::Kind::Refused;
  r.refusal = blockedLine(Block::NoKey);
  TEST_ASSERT_EQUAL(int(Outcome::SttRefused), int(classify(r)));
  assertCopy(lineFor(Outcome::SttRefused, r));
}

static void test_tone_per_phase_and_outcome() {
  Flow f;
  TEST_ASSERT_EQUAL(int(Tone::Accent), int(f.tone()));
  f.press(0);
  TEST_ASSERT_EQUAL(int(Tone::Accent), int(f.tone()));
  f.release(0);
  TEST_ASSERT_EQUAL(int(Tone::Accent), int(f.tone()));
  f.fail(Outcome::NoNetwork, lineFor(Outcome::NoNetwork), 0);
  TEST_ASSERT_EQUAL(int(Tone::Alert), int(f.tone()));
  Flow g;
  g.press(0);
  g.release(0);
  g.fail(Outcome::EmptyTranscript, lineFor(Outcome::EmptyTranscript), 0);
  TEST_ASSERT_EQUAL(int(Tone::Calm), int(g.tone()));
}

// The LED look per cue, and its agreement with the panel frame's COLOR: every
// alert cue is the alert color on both, every accent cue the accent on both.
static void test_led_cue_matches_the_panel_color() {
  const RGB accent{240, 120, 40}, alert{240, 40, 60};
  for (Cue c : {Cue::None, Cue::Listening, Cue::Processing, Cue::Alert, Cue::Offline}) {
    const LedCue l = ledCueFor(c);
    TEST_ASSERT_EQUAL(c == Cue::None, l.motion == LedMotion::Off);
    TEST_ASSERT_EQUAL(c == Cue::Alert || c == Cue::Offline, l.alert);
    if (c == Cue::None) continue;
    RGB frame[45];
    cueFrame(c, kOfflineBreatheMs / 2, accent, alert, frame, 45);
    const RGB want = l.alert ? alert : accent;
    const int b = brightest(frame, 45);
    TEST_ASSERT_EQUAL(want.r, frame[b].r);
    TEST_ASSERT_EQUAL(want.g, frame[b].g);
  }
  TEST_ASSERT_EQUAL(int(LedMotion::Pulse), int(ledCueFor(Cue::Listening).motion));
  TEST_ASSERT_EQUAL(int(LedMotion::Spinner), int(ledCueFor(Cue::Processing).motion));
  TEST_ASSERT_EQUAL(int(LedMotion::Solid), int(ledCueFor(Cue::Alert).motion));
  TEST_ASSERT_EQUAL(int(LedMotion::Pulse), int(ledCueFor(Cue::Offline).motion));
}

// The device loop's pass (Flow::step), the ONLINE path end to end on the host:
// transcript sent -> processing held while the turn runs -> ends on the turn's own
// end, with every ordering of reply and end the two tasks can produce.
static Flow thinkingFlow() {
  Flow f;
  f.press(0);
  f.release(1000);
  f.transcribed("what time is it", 2000);
  return f;
}

static void test_step_online_reply_then_end() {
  Flow f = thinkingFlow();
  Flow::TurnSignals s;
  s.turnInFlight = true;
  for (uint32_t t = 2000; t < 20000; t += 33) TEST_ASSERT_FALSE(f.step(s, t));
  s.replyLanded = true;   // a mid-turn delivery (e.g. a fallback notice)
  TEST_ASSERT_FALSE(f.step(s, 20000));
  TEST_ASSERT_EQUAL(int(Phase::Thinking), int(f.phase()));
  s.replyLanded = false;
  s.turnInFlight = false;
  s.turnEnded = true;
  s.turnOk = true;
  TEST_ASSERT_TRUE(f.step(s, 21000));
  TEST_ASSERT_EQUAL(int(Phase::Idle), int(f.phase()));
  TEST_ASSERT_EQUAL(4u, f.transitions());
  s.turnEnded = false;   // later passes: nothing moves
  for (uint32_t t = 21000; t < 400000; t += 1000) TEST_ASSERT_FALSE(f.step(s, t));
  TEST_ASSERT_EQUAL(4u, f.transitions());
}

static void test_step_reply_and_end_in_one_pass() {
  Flow f = thinkingFlow();
  Flow::TurnSignals s;
  s.replyLanded = true;
  s.turnEnded = true;
  s.turnOk = true;
  TEST_ASSERT_TRUE(f.step(s, 5000));
  TEST_ASSERT_EQUAL(int(Phase::Idle), int(f.phase()));
  TEST_ASSERT_EQUAL(int(Outcome::None), int(f.outcome()));
  Flow g = thinkingFlow();
  s.turnOk = false;   // the engine's honest failure reply + a failed end
  TEST_ASSERT_TRUE(g.step(s, 5000));
  TEST_ASSERT_EQUAL(int(Outcome::TurnError), int(g.outcome()));
  TEST_ASSERT_EQUAL(int(Cue::Alert), int(g.cue()));
}

static void test_step_stray_reply_waits_for_our_message() {
  // A sub-agent result lands on the voice chat while OUR message is still queued
  // (no turn running): it is shown, but processing waits for our message.
  Flow f = thinkingFlow();
  Flow::TurnSignals s;
  s.replyLanded = true;
  TEST_ASSERT_FALSE(f.step(s, 3000));
  TEST_ASSERT_EQUAL(int(Phase::Thinking), int(f.phase()));
  // Our message then ends with an honest early refusal ("No AI provider is set up
  // yet", no turn ran): not ok -> the turn-error cue, never a silent idle.
  s.replyLanded = true;
  s.turnEnded = true;
  s.turnOk = false;
  TEST_ASSERT_TRUE(f.step(s, 3100));
  TEST_ASSERT_EQUAL(int(Outcome::TurnError), int(f.outcome()));
}

static void test_step_queued_turn_is_not_no_reply() {
  // Queued behind another chat's turn for 50 s: still processing, no false alarm.
  Flow f = thinkingFlow();
  Flow::TurnSignals s;
  s.turnInFlight = true;
  uint32_t lastBusy = 0;
  for (uint32_t t = 2000; t < 52000; t += 100) {
    TEST_ASSERT_FALSE(f.step(s, t));
    lastBusy = t;
  }
  TEST_ASSERT_EQUAL(int(Phase::Thinking), int(f.phase()));
  // It never runs and nothing is in flight: the quiet backstop, counted from the
  // last pass a turn was running, fires once.
  s.turnInFlight = false;
  TEST_ASSERT_FALSE(f.step(s, lastBusy + kQuietMs - 1));
  TEST_ASSERT_TRUE(f.step(s, lastBusy + kQuietMs));
  TEST_ASSERT_EQUAL(int(Outcome::NoReply), int(f.outcome()));
  TEST_ASSERT_FALSE(f.step(s, lastBusy + kQuietMs + 1));
  TEST_ASSERT_TRUE(f.step(s, lastBusy + kQuietMs + kNoticeHoldMs));
  TEST_ASSERT_EQUAL(int(Phase::Idle), int(f.phase()));
}

// ---- composed with the tg_poll loop (CUM-462) -----------------------------------

// sendTurn hands the transcript to the tg_poll loop (injectMessage), which spent
// almost all its time blocked in the Telegram long-poll and never looked at it: the
// owner saw "Thinking" and the turn could start ~30 s later. Release at every point
// of an idle device's poll cycle: the turn starts within one pause slice of the
// transcript, the flow holds Thinking (no transition) until it does, and ends on our
// own turn's end - never the quiet backstop.
static void test_voice_turn_starts_promptly_wherever_the_poll_cycle_is() {
  const tgsim::Config c;
  for (uint32_t offset = 0; offset < c.longPollS * 1000 + c.pauseMs + 500; offset += 101) {
    tgsim::Sim sim(c);
    sim.runUntil(100000);   // an idle device: poll socket open, long-poll cycling
    Flow f;
    FakePort p;
    p.stt = okText("turn on the lights");
    p.clock = sim.now() + offset;
    startRecording(f, p);
    TEST_ASSERT_EQUAL(int(Outcome::None), int(afterRelease(f, p)));
    const uint32_t sentAt = p.clock;   // sendTurn ran here, after speech-to-text
    sim.injectLocal(sentAt);
    sim.runUntil(sentAt + 2 * c.longPollS * 1000);
    TEST_ASSERT_EQUAL(1, int(sim.local().size()));
    const uint32_t startedAt = sim.local()[0].startedAt;
    TEST_ASSERT_TRUE_MESSAGE(startedAt - sentAt <= nimbus::net::kTgIdleSliceMs,
                             "the voice turn waited on the Telegram long-poll");
    const uint32_t before = f.transitions();
    Flow::TurnSignals s;
    for (uint32_t t = sentAt; t < startedAt; t += 50) TEST_ASSERT_FALSE(f.step(s, t));
    s.turnInFlight = true;
    TEST_ASSERT_FALSE(f.step(s, startedAt));
    TEST_ASSERT_EQUAL(before, f.transitions());
    s.turnInFlight = false;
    s.turnEnded = true;
    s.turnOk = true;
    TEST_ASSERT_TRUE(f.step(s, startedAt + c.turnMs));
    TEST_ASSERT_EQUAL(int(Phase::Idle), int(f.phase()));
    TEST_ASSERT_EQUAL(int(Outcome::None), int(f.outcome()));
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_release_shows_processing_before_any_network_call);
  RUN_TEST(test_release_repaint_latency_is_independent_of_network_time);
  RUN_TEST(test_offline_fails_fast_with_the_network_line_and_no_network_call);
  RUN_TEST(test_unreachable_host_is_the_network_line_not_no_speech);
  RUN_TEST(test_send_failure_is_busy_not_silence);
  RUN_TEST(test_release_when_not_recording_does_nothing);
  RUN_TEST(test_no_failure_kind_ever_reads_as_no_speech);
  RUN_TEST(test_empty_transcript_only_from_a_reachable_provider);
  RUN_TEST(test_transport_error_mapping);
  RUN_TEST(test_http_error_names_the_provider_and_status);
  RUN_TEST(test_every_line_is_pinned);
  RUN_TEST(test_copy_style_and_fit_for_every_outcome);
  RUN_TEST(test_cue_and_sound_per_outcome);
  RUN_TEST(test_happy_path_is_exactly_four_transitions);
  RUN_TEST(test_busy_flow_ignores_a_second_press);
  RUN_TEST(test_notice_holds_then_expires_once_and_can_retry);
  RUN_TEST(test_thinking_backstops);
  RUN_TEST(test_turn_end_and_reply_outcomes);
  RUN_TEST(test_random_event_storm_never_takes_an_illegal_edge);
  RUN_TEST(test_release_path_renders_once_per_transition);
  RUN_TEST(test_wrap);
  RUN_TEST(test_cue_frames);
  RUN_TEST(test_flow_cue_per_phase);
  RUN_TEST(test_named_refusals_and_names);
  RUN_TEST(test_tone_per_phase_and_outcome);
  RUN_TEST(test_blocked_lines_are_pinned);
  RUN_TEST(test_led_cue_matches_the_panel_color);
  RUN_TEST(test_step_online_reply_then_end);
  RUN_TEST(test_step_reply_and_end_in_one_pass);
  RUN_TEST(test_step_stray_reply_waits_for_our_message);
  RUN_TEST(test_step_queued_turn_is_not_no_reply);
  RUN_TEST(test_voice_turn_starts_promptly_wherever_the_poll_cycle_is);
  return UNITY_END();
}
