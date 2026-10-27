// Class guards from the unit L bench finding (program-2026-10-18, lane M).
//
// The console DREAM drill ran the nightly dream - a full model turn - on the Arduino
// loop task, whose 8 KB stack cannot hold one: the device panicked with a stack-canary
// fault 0.1 s after "dream: start", so "DREAM fired" never printed. The same run logged
// "File system is not mounted" every second or two: the sound-effects SD sync used the
// raw Arduino `SD` (SPI) global on an SDMMC board, where the card is mounted through
// SD_MMC and `SD` is never begun. These pin the CLASS over the firmware sources
// (comments stripped, so prose that NAMES a call never counts; whitespace squeezed out,
// so a call split across lines still counts):
//   - no console hook runs a turn or a night inline; the dream fires only from the
//     scheduler's fire hook and from the drill pump, and the pump runs only from the
//     turn task's pollJobs;
//   - the raw `SD` global does file work only inside the sound-effects SD tier, whose
//     player and sync both gate on cardIsRawSd(); nothing routes data to it;
//   - the one atomic file writer refuses an unmounted file area before it opens a file.
// The night's own behaviour on a store that is not mounted or fails is host-tested in
// test/test_harness_dream_night.

#include <unity.h>

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

void setUp() {}
void tearDown() {}

static std::string readFile(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path.c_str());
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

// Index just past the literal that opens at `i`: a "..." string or a '...' character.
static size_t skipLiteral(const std::string& s, size_t i) {
  const char q = s[i];
  for (++i; i < s.size(); ++i) {
    if (s[i] == '\\') ++i;
    else if (s[i] == q) return i + 1;
  }
  return s.size();
}

// True when the ' at `i` is a C++14 digit separator (24'576): its token starts with a digit.
static bool isDigitSeparator(const std::string& s, size_t i) {
  size_t t = i;
  while (t > 0 && (std::isalnum((unsigned char)s[t - 1]) || s[t - 1] == '\'')) --t;
  return t < i && std::isdigit((unsigned char)s[t]);
}

// C++ source with every comment replaced by a space; literals are kept whole.
static std::string stripComments(const std::string& src) {
  std::string out;
  size_t i = 0;
  while (i < src.size()) {
    const size_t at = src.find_first_of("\"'/", i);
    if (at == std::string::npos) { out.append(src, i, std::string::npos); break; }
    out.append(src, i, at - i);
    if (src[at] == '\'' && isDigitSeparator(src, at)) {
      out += '\'';
      i = at + 1;
    } else if (src[at] != '/') {
      i = skipLiteral(src, at);
      out.append(src, at, i - at);
    } else if (src.compare(at, 2, "//") == 0) {
      out += ' ';
      i = src.find('\n', at);
      if (i == std::string::npos) break;
    } else if (src.compare(at, 2, "/*") == 0) {
      out += ' ';
      const size_t end = src.find("*/", at + 2);
      i = end == std::string::npos ? src.size() : end + 2;
    } else {
      out += '/';
      i = at + 1;
    }
  }
  return out;
}

static std::string squeeze(const std::string& code) {
  std::string out;
  out.reserve(code.size());
  for (char c : code)
    if (!std::isspace((unsigned char)c)) out += c;
  return out;
}

static size_t countOf(const std::string& hay, const std::string& needle) {
  size_t n = 0;
  for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) ++n;
  return n;
}

struct Source {
  std::string path;
  std::string code;   // comments stripped, whitespace squeezed out
};

// Every firmware source under src/ (bring-up sketches are bench diagnostics).
static const std::vector<Source>& firmware() {
  static std::vector<Source> all;
  if (!all.empty()) return all;
  for (const auto& e : std::filesystem::recursive_directory_iterator("src")) {
    if (!e.is_regular_file()) continue;
    const std::string path = e.path().generic_string();
    const std::string ext = e.path().extension().string();
    if ((ext != ".cpp" && ext != ".h") || path.rfind("src/bringup/", 0) == 0) continue;
    all.push_back({path, squeeze(stripComments(readFile(path)))});
  }
  TEST_ASSERT_TRUE(all.size() > 50);
  return all;
}

static const std::string& codeOf(const std::string& path) {
  for (const Source& s : firmware())
    if (s.path == path) return s.code;
  TEST_FAIL_MESSAGE(path.c_str());
  static const std::string none;
  return none;
}

// The squeezed code between `from` and `to` in `path` (both must exist, in order).
static std::string between(const std::string& path, const std::string& from, const std::string& to) {
  const std::string& c = codeOf(path);
  const size_t a = c.find(from);
  TEST_ASSERT_TRUE_MESSAGE(a != std::string::npos, from.c_str());
  const size_t b = c.find(to, a);
  TEST_ASSERT_TRUE_MESSAGE(b != std::string::npos, to.c_str());
  return c.substr(a, b - a);
}

// Calls that run a model turn (or a whole night) on the caller's task.
static const char* const kTurnRunners[] = {
    "dream::fire(", "runNight(", "injectScheduledTurn(", "handleMessage(", "runTurn(",
};

static void test_no_console_hook_runs_a_turn_inline() {
  // The console hooks run on the loop task (8 KB stack). TURN already queues to the
  // turn task; DREAM ran the night inline and panicked the device.
  const std::string block = between("src/main.cpp", "tc::Hooksh;", "tc::begin(h);");
  for (const char* runner : kTurnRunners)
    TEST_ASSERT_EQUAL_MESSAGE(0, countOf(block, runner), runner);
  TEST_ASSERT_EQUAL(1, countOf(block, "agent::dream::requestDrill("));
  // TURN queues to the turn task through injectLocalTurn (main.cpp: telegram::injectMessage,
  // counted so an inline model-list refresh steps aside for it; HFM round 3).
  TEST_ASSERT_EQUAL(1, countOf(block, "injectLocalTurn(\"serial\""));
  const std::string wrap = between("src/main.cpp", "staticboolinjectLocalTurn(", "staticboolownerMessageWaiting(");
  TEST_ASSERT_EQUAL(1, countOf(wrap, "agent::telegram::injectMessage(chat,text)"));
  for (const char* runner : kTurnRunners) TEST_ASSERT_EQUAL_MESSAGE(0, countOf(wrap, runner), runner);
}

// Storm run 3 (24 Oct), the pong question: the console's liveness PING must answer
// on the CONSOLE seam only. The boot pump (tc::pumpBoot) and dispatch's PING verb
// write Serial and nothing else - never the Telegram send queue, never a captured
// row - and the boot pump's answer is MARKED "(boot console)" so no transcript can
// mistake it for the assistant's word (the storm legs' probes ask the model to
// answer "pong"; run 3's 'pong' rows were those answers, not this echo). This pin
// keeps the pump provably unable to leak into any owner-facing channel.
static void test_console_ping_answers_only_on_the_console_seam() {
  // The boot pump: Serial in, the marked PONG out, the line freed. No owner-facing
  // call may appear inside it.
  const std::string pump =
      between("src/test_console.cpp", "voidpumpBoot(){", "voidonRender(");
  TEST_ASSERT_TRUE(pump.size() > 60);
  TEST_ASSERT_EQUAL(1, countOf(pump, "Serial.printf(\"PONG(bootconsole)\\n\");"));
  for (const char* leak : {"telegram::send(", "captureMessage(", "handleMessage(", "injectMessage("})
    TEST_ASSERT_EQUAL_MESSAGE(0, countOf(pump, leak), leak);
  // dispatch's PING verb: the reply goes to Serial through reply(), and the verb
  // consumes the line before anything owner-facing could see it.
  const std::string ping = between("src/test_console.cpp", "if(line==\"PING\"){",
                                   "if(line.startsWith(\"SFXVOL\"))");
  TEST_ASSERT_TRUE(ping.size() > 10);
  TEST_ASSERT_EQUAL(1, countOf(ping, "reply(\"PONG\");"));
  for (const char* leak : {"telegram::send(", "captureMessage(", "handleMessage(", "injectMessage("})
    TEST_ASSERT_EQUAL_MESSAGE(0, countOf(ping, leak), leak);
}

static void test_the_dream_fires_only_from_the_scheduler_and_the_drill_pump() {
  size_t fires = 0, pumps = 0, reconciles = 0;
  for (const Source& s : firmware()) {
    const size_t f = countOf(s.code, "dream::fire(") + countOf(s.code, "=fire(req,");
    const size_t p = countOf(s.code, "dream::pumpDrill(");
    const size_t rb = countOf(s.code, "dream::pumpBootReconcile(");
    if (f) TEST_ASSERT_TRUE_MESSAGE(s.path == "src/main.cpp" || s.path == "src/agent/dream_subsystem.cpp",
                                    s.path.c_str());
    if (p || rb) TEST_ASSERT_EQUAL_STRING("src/agent/orchestrator.cpp", s.path.c_str());
    fires += f;
    pumps += p;
    reconciles += rb;
  }
  TEST_ASSERT_EQUAL(2, fires);   // the loops fire hook (main.cpp) and pumpDrill
  TEST_ASSERT_EQUAL(1, pumps);
  TEST_ASSERT_EQUAL(1, reconciles);   // the boot reconcile: once, on the turn task
  // main.cpp's one fire is the scheduler hook, and the pump sits in pollJobs (tg_poll).
  TEST_ASSERT_EQUAL(1, countOf(codeOf("src/main.cpp"), "if(agent::dream::isReserved(r.id))returnagent::dream::fire(r);"));
  const std::string pollJobs =
      between("src/agent/orchestrator.cpp", "intpollJobs(){", "loops::checkDue(");
  TEST_ASSERT_EQUAL(1, countOf(pollJobs, "dream::pumpDrill();"));
  // The boot reconcile runs before the drill pump and the tick, so the retry it
  // schedules can fire the same pass and a night interrupted by a restart is
  // reconciled before anything else fires the dream.
  TEST_ASSERT_EQUAL(1, countOf(pollJobs, "dream::pumpBootReconcile();"));
  TEST_ASSERT_TRUE(pollJobs.find("dream::pumpBootReconcile();") < pollJobs.find("dream::pumpDrill();"));
}

// File work (or a card probe) through the raw Arduino SD global ("SD." directly, not
// "SD_MMC." and not a member such as "x.SD.").
static size_t rawSdCalls(const std::string& code) {
  size_t n = 0;
  for (const char* op : {"open(", "mkdir(", "remove(", "rename(", "exists(", "rmdir(", "cardType("}) {
    const std::string needle = std::string("SD.") + op;
    for (size_t p = code.find(needle); p != std::string::npos; p = code.find(needle, p + 1)) {
      const char before = p ? code[p - 1] : ' ';
      if (!std::isalnum((unsigned char)before) && before != '_' && before != '.') ++n;
    }
  }
  return n;
}

static void test_raw_sd_file_work_stays_in_the_gated_sound_tier() {
  for (const Source& s : firmware()) {
    const bool soundTier = s.path == "src/sfx/sfx_sync.cpp" || s.path == "src/sfx/sound_fx.cpp";
    if (!soundTier) TEST_ASSERT_EQUAL_MESSAGE(0, rawSdCalls(s.code), s.path.c_str());
    TEST_ASSERT_EQUAL_MESSAGE(0, countOf(s.code, "setDataFs(SD)"), s.path.c_str());
  }
  // Both entry gates of the sound tier ask whether the mounted card is the raw SD global.
  TEST_ASSERT_TRUE(between("src/sfx/sfx_sync.cpp", "boolgateOpen(){", "}").find("::sfx::cardIsRawSd()") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(between("src/sfx/sound_fx.cpp", "boolsdUsable(){", "}").find("cardIsRawSd()") !=
                   std::string::npos);
  TEST_ASSERT_EQUAL(1, countOf(codeOf("src/sfx/sound_fx.cpp"),
                               "boolcardIsRawSd(){return&solide::storage::activeFs()==static_cast<fs::FS*>(&SD);}"));
}

static void test_the_atomic_writer_refuses_an_unmounted_file_area_first() {
  const std::string body = between("src/agent/fs_atomic.cpp", "boolwriteFileAtomic(", "LittleFS.rename(");
  const size_t mounted = body.find("if(!flashMounted()){");
  const size_t open = body.find("LittleFS.open(");
  TEST_ASSERT_TRUE(mounted != std::string::npos);
  TEST_ASSERT_TRUE(open != std::string::npos);
  TEST_ASSERT_TRUE(mounted < open);
  // The dream glue never opens its state file on an unmounted file area either.
  TEST_ASSERT_EQUAL(1, countOf(codeOf("src/agent/dream_subsystem.cpp"),
                               "flashMounted()?LittleFS.open(kStatePath,FILE_READ):File()"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_no_console_hook_runs_a_turn_inline);
  RUN_TEST(test_console_ping_answers_only_on_the_console_seam);
  RUN_TEST(test_the_dream_fires_only_from_the_scheduler_and_the_drill_pump);
  RUN_TEST(test_raw_sd_file_work_stays_in_the_gated_sound_tier);
  RUN_TEST(test_the_atomic_writer_refuses_an_unmounted_file_area_first);
  return UNITY_END();
}
