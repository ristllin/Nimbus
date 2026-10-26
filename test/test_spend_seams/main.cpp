#include <unity.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "nimbus/orch/spend_ceiling.h"

// The seam table (lane DEF): every paid seam consults the paid-call gate before its
// first request, so the monthly spend ceiling has no way around it. Each row names the
// function every request of the seam passes through and the consult it must contain;
// a new seam, or a refactor that drops a consult, fails here. The counter-test removes
// one consult from a copy of its function and proves the checker catches it.
//
// The meter table: a seam the gate pauses but nobody counts could still overrun the
// ceiling, so every gated seam kind also has a meter row (the function that reports
// what the call cost, and the call it must contain), and every seam kind has one.
//
// Device seams outside lane DEF's files (embeddings, the fallback captioner, speech,
// the download scan, and image generation besides this lane's own gate) consult the
// per-provider park of lane K2, which answers "paused" while the ceiling freezes paid
// calls (the ceiling store's freeze listener sets the park's pause switch). Those rows
// are enforced whenever the park is in the tree, and the counter-test covers them too.

void setUp() {}
void tearDown() {}

namespace {

std::string slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool exists(const std::string& path) { return std::ifstream(path).good(); }

// The end of the literal or comment that starts at `i` (one past it), or `i` when none
// starts there: braces inside a string, a raw string or a comment are not code.
size_t skipNonCode(const std::string& s, size_t i) {
  if (s.compare(i, 2, "//") == 0) {
    const size_t e = s.find('\n', i);
    return e == std::string::npos ? s.size() : e;
  }
  if (s.compare(i, 2, "/*") == 0) {
    const size_t e = s.find("*/", i + 2);
    return e == std::string::npos ? s.size() : e + 2;
  }
  if (s.compare(i, 2, "R\"") == 0 && (i == 0 || !isalnum((unsigned char)s[i - 1]))) {
    const size_t paren = s.find('(', i + 2);
    if (paren == std::string::npos) return s.size();
    const std::string close = ")" + s.substr(i + 2, paren - i - 2) + "\"";
    const size_t e = s.find(close, paren);
    return e == std::string::npos ? s.size() : e + close.size();
  }
  if (s[i] == '"' || s[i] == '\'') {
    const char q = s[i];
    for (size_t j = i + 1; j < s.size(); ++j) {
      if (s[j] == '\\') { ++j; continue; }
      if (s[j] == q) return j + 1;
    }
    return s.size();
  }
  return i;
}

// The body of the function whose definition starts at `signature` (the first '{'
// after it to its matching '}', braces in literals and comments skipped), or "" when
// the signature is absent.
std::string functionBody(const std::string& src, const std::string& signature) {
  const size_t at = src.find(signature);
  if (at == std::string::npos) return std::string();
  const size_t open = src.find('{', at + signature.size());
  if (open == std::string::npos) return std::string();
  int depth = 0;
  for (size_t i = open; i < src.size(); ++i) {
    const size_t past = skipNonCode(src, i);
    if (past != i) {
      i = past - 1;
      continue;
    }
    if (src[i] == '{') ++depth;
    if (src[i] == '}' && --depth == 0) return src.substr(open, i - open + 1);
  }
  return std::string();
}

struct SeamRow {
  const char* seam;        // what the owner would call it
  const char* file;
  const char* signature;   // the choke point every request of the seam passes through
  const char* consult;     // the gate call its body must contain
  bool parkSeam;           // consults lane K2's park (ignored until the park is in the tree)
  nimbus::orch::SpendSeam kind;   // the meter rows of this kind must exist
};

// The contract. Head turns, routines, wakeups and the dream are refused at the turn
// core before recall's embed; every harness provider request (head rounds, single
// shots, sub-agent dispatch, the fold) at wire::exchange; vision on the head rides the
// head turn and its requests.
const SeamRow kSeams[] = {
    {"head turn", "lib/harness/src/engine.cpp", "bool TurnEngine::runTurn(const std::string& inputs, const std::string& chatId,\n                         const std::string& userText, bool telegram)",
     "paidCallAllowed(nimbus::orch::SpendSeam::Head)", false, nimbus::orch::SpendSeam::Head},
    {"routine", "lib/harness/src/engine.cpp", "nimbus::orch::FireOutcome TurnEngine::injectScheduledTurn(",
     "nimbus::orch::SpendSeam::Routine", false, nimbus::orch::SpendSeam::Routine},
    {"wakeup", "lib/harness/src/engine.cpp", "nimbus::orch::FireOutcome TurnEngine::injectScheduledTurn(",
     "nimbus::orch::SpendSeam::Wakeup", false, nimbus::orch::SpendSeam::Wakeup},
    {"dream", "lib/harness/src/engine.cpp", "nimbus::orch::FireOutcome TurnEngine::injectScheduledTurn(",
     "nimbus::orch::SpendSeam::Dream", false, nimbus::orch::SpendSeam::Dream},
    {"scheduled turn gate", "lib/harness/src/engine.cpp", "nimbus::orch::FireOutcome TurnEngine::injectScheduledTurn(",
     "paidCallAllowed(seam)", false, nimbus::orch::SpendSeam::Routine},
    {"fold", "lib/harness/src/engine.cpp", "TurnEngine::FoldResult TurnEngine::runFold(",
     "paidCallAllowed(orch::SpendSeam::Fold)", false, nimbus::orch::SpendSeam::Fold},
    {"every provider request", "lib/harness/src/providers/wire.h", "inline int exchange(",
     "paidCallAllowed(nimbus::orch::SpendSeam::Wire)", false, nimbus::orch::SpendSeam::Wire},
    {"web search", "lib/harness/src/websearch.cpp", "void registerWebSearchTool(",
     "paidCallAllowed(nimbus::orch::SpendSeam::Search)", false, nimbus::orch::SpendSeam::Search},
    {"image generation", "src/agent/adapters/image_gen.cpp", "uint8_t* generateToBuffer(",
     "paidCallAllowed(nimbus::orch::SpendSeam::ImageGen)", false, nimbus::orch::SpendSeam::ImageGen},
    {"image generation (park)", "src/agent/adapters/image_gen.cpp", "uint8_t* generateToBuffer(",
     "gate::check(\"openai\", nimbus::orch::PaidSeam::ImageGen", true, nimbus::orch::SpendSeam::ImageGen},
    {"embeddings", "src/agent/adapters/embeddings.cpp", "std::vector<int8_t> embedWith(const String& text,",
     "gate::check(provider.c_str(), nimbus::orch::PaidSeam::Embeddings", true, nimbus::orch::SpendSeam::Embed},
    // Lane E: the fallback captioner (and image.look) is the portable caption walk, one
    // request per provider it asks; the device adapter only supplies keys and transport.
    {"vision (fallback captioner)", "lib/harness/src/vision_caption.cpp",
     "CaptionResult caption(const CaptionDeps& d, const CaptionRequest& in)",
     "refusedUpFront(d, in, res)", false, nimbus::orch::SpendSeam::VisionFallback},
    {"vision: the ceiling", "lib/harness/src/vision_caption.cpp",
     "bool refusedUpFront(const CaptionDeps& d, const CaptionRequest& in, CaptionResult& res)",
     "paidCallAllowed(orch::SpendSeam::VisionFallback)", false, nimbus::orch::SpendSeam::VisionFallback},
    {"vision: the provider pick", "lib/harness/src/vision_caption.cpp",
     "CaptionResult tryProvider(const CaptionDeps& d, const std::string& p, uint32_t now, const CaptionRequest& in)",
     "providerPark().available(p.c_str(), orch::PaidSeam::Vision", true, nimbus::orch::SpendSeam::VisionFallback},
    {"speech to text", "src/agent/adapters/audio_stt.cpp", "static nimbus::voice::SttResult transcribeCommon(",
     "closedByPark(prov, out)", true, nimbus::orch::SpendSeam::Stt},
    {"speech to text: the park", "src/agent/adapters/audio_stt.cpp", "static bool closedByPark(",
     "gate::check(prov.slug.c_str(), nimbus::orch::PaidSeam::Stt", true, nimbus::orch::SpendSeam::Stt},
    {"text to speech", "src/agent/adapters/audio_tts.cpp", "size_t synthesizeToFile(",
     "gate::check(prov.slug.c_str(), nimbus::orch::PaidSeam::Tts", true, nimbus::orch::SpendSeam::Tts},
    {"url_fetch scan", "src/agent/adapters/url_fetch.cpp", "int scanVerdict(", "resolveScan(p, reason)", true,
     nimbus::orch::SpendSeam::UrlFetch},
    // Lane QW4 (CUM-777): the scan consults the ceiling itself, not only the park.
    {"url_fetch scan: the ceiling", "src/agent/adapters/url_fetch.cpp", "int scanVerdict(",
     "paidCallAllowed(nimbus::orch::SpendSeam::UrlFetch)", false, nimbus::orch::SpendSeam::UrlFetch},
    {"url_fetch scan: the provider pick", "src/agent/adapters/url_fetch.cpp", "bool resolveScan(",
     "nimbus::orch::PaidSeam::UrlFetch", true, nimbus::orch::SpendSeam::UrlFetch},
    {"moderation", "src/agent/adapters/moderation.cpp", "ClassifierVerdict classify(const std::string& text,",
     "paidCallAllowed(nimbus::orch::SpendSeam::Moderation)", false, nimbus::orch::SpendSeam::Moderation},
    {"moderation (park)", "src/agent/adapters/moderation.cpp", "static ClassifierVerdict classifyMistral(",
     "providerPark().pausedBy(\"mistral\")", true, nimbus::orch::SpendSeam::Moderation},
    {"each loop round (park)", "lib/harness/src/providers/loop_common.cpp", "bool runFabricLoop(",
     "orch::kPauseProviderLimit", true, nimbus::orch::SpendSeam::Wire},
    {"hosted embeddings", "nimbusd/src/rig.h", "std::vector<int8_t> embedWith(const std::string& text,",
     "paidCallAllowed(orch::SpendSeam::Embed)", false, nimbus::orch::SpendSeam::Embed},
};

bool parkInTree() { return exists("src/agent/provider_gate.h"); }

// The meter contract: where each kind of paid call reports what it cost. A turn's
// requests (head, routine, wakeup, dream, images handed to the head) are metered once by
// the turn's spend record and round by round while it runs; everything else at the
// answer that billed it.
struct MeterRow {
  const char* what;
  nimbus::orch::SpendSeam kind;
  const char* file;
  const char* signature;   // the function that sees the billed answer
  const char* meter;       // the report its body must contain
};
using S = nimbus::orch::SpendSeam;
const char* const kRunTurn =
    "bool TurnEngine::runTurn(const std::string& inputs, const std::string& chatId,\n"
    "                         const std::string& userText, bool telegram)";
const MeterRow kMeters[] = {
    {"turn spend", S::Head, "lib/harness/src/engine.cpp", kRunTurn, "meterSpend(tf.spend"},
    {"images on the head", S::VisionNative, "lib/harness/src/engine.cpp", kRunTurn, "meterSpend(tf.spend"},
    {"routine turn", S::Routine, "lib/harness/src/engine.cpp", "nimbus::orch::FireOutcome TurnEngine::injectScheduledTurn(", "runTurn("},
    {"wakeup turn", S::Wakeup, "lib/harness/src/engine.cpp", "nimbus::orch::FireOutcome TurnEngine::injectScheduledTurn(", "runTurn("},
    {"dream turn", S::Dream, "lib/harness/src/engine.cpp", "nimbus::orch::FireOutcome TurnEngine::injectScheduledTurn(", "runTurn("},
    {"fold", S::Fold, "lib/harness/src/engine.cpp", "TurnEngine::FoldResult TurnEngine::runFold(", "meterSpend(foldSpend"},
    {"each loop round", S::Wire, "lib/harness/src/providers/loop_common.cpp", "", "notePendingUsage("},
    {"sub-agent tokens (OpenAI, Anthropic)", S::Sub, "lib/harness/src/jobs.cpp", "int JobEngine::pump()", "d_.recordSpawnUsage("},
    {"OpenAI sub usage", S::Sub, "lib/harness/src/providers/openai.cpp", "FabricErr oaiPoll(", "env.cacheReadTokens"},
    {"OpenAI sub searches", S::Sub, "lib/harness/src/providers/openai.cpp", "FabricErr oaiPoll(", "meterSubSearchRuns("},
    {"Anthropic sub usage", S::Sub, "lib/harness/src/providers/anthropic.cpp", "static FabricErr antSessionUsage(", "anthropicSessionUsage("},
    {"Anthropic sub searches", S::Sub, "lib/harness/src/providers/anthropic.cpp", "static FabricErr antSessionUsage(", "meterSubSearchRuns("},
    {"Mistral sub", S::Sub, "lib/harness/src/providers/mistral.cpp", "FabricErr mistralDispatch(", "meterSubAnswer("},
    {"Z.ai and Cumulo sub", S::Sub, "lib/harness/src/providers/openai_compat.cpp", "FabricErr openaiCompatDispatch(", "meterSubAnswer("},
    {"custom endpoint sub", S::Sub, "lib/harness/src/providers/custom.cpp", "FabricErr customDispatch(", "meterSubAnswer("},
    {"web search", S::Search, "lib/harness/src/websearch.cpp", "void registerWebSearchTool(", "deps.recordCall()"},
    {"web search call priced", S::Search, "src/agent/store.cpp", "void recordProviderCall(const String& provider)", "agent::spend::commit("},
    {"embeddings", S::Embed, "src/agent/adapters/embeddings.cpp", "std::vector<int8_t> embedWith(const String& text,", "SpendSeam::Embed"},
    {"hosted embeddings", S::Embed, "nimbusd/src/rig.h", "std::vector<int8_t> embedWith(const std::string& text,", "SpendSeam::Embed"},
    // Lane E: what the caption step billed rides the next turn's spend record (lane T),
    // which the ledger and the ceiling both count once.
    {"fallback captioner", S::VisionFallback, "src/agent/adapters/image_vision.cpp", "CaptionResult read(", "addVisionSpend("},
    {"speech to text", S::Stt, "src/agent/adapters/audio_stt.cpp", "static nimbus::voice::SttResult transcribeCommon(", "meterTranscription("},
    {"text to speech", S::Tts, "src/agent/adapters/audio_tts.cpp", "size_t synthesizeToFile(", "meterSpeech("},
    {"image generation", S::ImageGen, "src/agent/adapters/image_gen.cpp", "uint8_t* generateToBuffer(", "meterImage("},
    {"url_fetch scan", S::UrlFetch, "src/agent/adapters/url_fetch.cpp", "int scanVerdict(", "SpendSeam::UrlFetch"},
    {"moderation", S::Moderation, "src/agent/adapters/moderation.cpp", "static ClassifierVerdict classifyMistral(", "SpendSeam::Moderation"},
    // Lane QW4 (CUM-776): a request that left and got no usable answer is counted at an
    // estimate: every provider request (the wire), the caption step, the download scan.
    {"a sent request with no answer", S::Wire, "lib/harness/src/providers/wire.h", "inline int exchange(",
     "meterUnanswered(pd, host, req)"},
    {"a caption with no answer", S::VisionFallback, "lib/harness/src/vision_caption.cpp",
     "CaptionResult askOne(", "sentRequestEstimate("},
    {"a scan with no answer", S::UrlFetch, "src/agent/adapters/url_fetch.cpp", "int scanVerdict(",
     "meterUnansweredScan(p, body.length())"},
};

// The two sinks the meter reports reach (the device's ceiling, the hosted one's).
const MeterRow kSinks[] = {
    {"device sink", S::Head, "src/agent/spend_ceiling_store.cpp", "void install()", "installSeamMeter("},
    {"hosted sink", S::Head, "nimbusd/src/spend_ceiling_vn.h", "void install()", "installSeamMeter("},
    // The freeze rides K2's park: the device store hands every change to the pause switch.
    {"freeze to the park", S::Head, "src/agent/spend_ceiling_store.cpp", "void install()",
     "installFreezeListener(&freezeToPark)"},
    {"the park's pause switch", S::Head, "src/agent/spend_ceiling_store.cpp", "void freezeToPark(bool isFrozen)",
     "pause(nimbus::orch::kPauseAllProviders, nimbus::orch::kPauseSpendCeiling)"},
    {"the park's resume", S::Head, "src/agent/spend_ceiling_store.cpp", "void freezeToPark(bool isFrozen)",
     "resume(nimbus::orch::kPauseAllProviders, nimbus::orch::kPauseSpendCeiling)"},
    // Lane DEF2: each provider's own monthly limit pauses that provider alone.
    {"a provider's limit to the park", S::Head, "src/agent/spend_ceiling_store.cpp",
     "void applyProviderFreezes(const ProviderFreezes& want)",
     "pause(want.provider[i], nimbus::orch::kPauseProviderLimit)"},
    {"every report hands it over", S::Head, "src/agent/spend_ceiling_store.cpp",
     "void report(bool global, const ProviderFreezes& providers)", "applyProviderFreezes(providers)"},
};

// "" when the meter row holds, else why not ("" signature: anywhere in the file).
std::string checkMeter(const MeterRow& m, const std::string& src) {
  const std::string body = *m.signature ? functionBody(src, m.signature) : src;
  if (body.empty()) return std::string(m.what) + ": function not found in " + m.file;
  if (body.find(m.meter) == std::string::npos)
    return std::string(m.what) + ": " + m.file + " does not report its spend (" + m.meter + ")";
  return std::string();
}

// The checker: "" when the row holds, else why it does not.
std::string checkRow(const SeamRow& r, const std::string& src) {
  const std::string body = functionBody(src, r.signature);
  if (body.empty()) return std::string(r.seam) + ": choke point not found in " + r.file;
  if (body.find(r.consult) == std::string::npos)
    return std::string(r.seam) + ": " + r.file + " does not consult the gate (" + r.consult + ")";
  return std::string();
}

}  // namespace

static void test_every_paid_seam_consults_the_gate() {
  int ignored = 0;
  for (const SeamRow& r : kSeams) {
    if (r.parkSeam && !parkInTree()) {
      ++ignored;
      continue;
    }
    const std::string why = checkRow(r, slurp(r.file));
    TEST_ASSERT_TRUE_MESSAGE(why.empty(), why.c_str());
  }
  if (ignored) TEST_IGNORE_MESSAGE("park seams (embeddings, captioner, speech, scan) wait for lane K2's park");
}

// Counter-test: drop one consult from a copy of its function and the checker fails.
static void test_removing_a_consult_fails_the_checker() {
  for (const SeamRow& r : kSeams) {
    if (r.parkSeam && !parkInTree()) continue;
    std::string src = slurp(r.file);
    const std::string body = functionBody(src, r.signature);
    TEST_ASSERT_FALSE_MESSAGE(body.empty(), r.seam);
    const size_t at = src.find(body);
    std::string cut = body;
    size_t p;
    while ((p = cut.find(r.consult)) != std::string::npos) cut.erase(p, std::string(r.consult).size());
    src.replace(at, body.size(), cut);
    TEST_ASSERT_FALSE_MESSAGE(checkRow(r, src).empty(), r.seam);
  }
}

// Every seam of the gate's own table has a row above (a new SpendSeam with no row in
// the contract fails), except the ones a row covers by another name.
static void test_the_table_covers_every_seam_kind() {
  std::string all;
  for (const SeamRow& r : kSeams) all += std::string(r.consult) + "\n" + r.seam + "\n";
  for (int s = 0; s < nimbus::orch::kSpendSeamCount; ++s) {
    const auto seam = (nimbus::orch::SpendSeam)s;
    switch (seam) {
      case nimbus::orch::SpendSeam::Sub:            // rides wire::exchange (the provider-files rule)
      case nimbus::orch::SpendSeam::VisionNative:   // rides the head turn and its requests
      case nimbus::orch::SpendSeam::Embed:          // the park rows
      case nimbus::orch::SpendSeam::VisionFallback:
      case nimbus::orch::SpendSeam::Stt:
      case nimbus::orch::SpendSeam::Tts:
      case nimbus::orch::SpendSeam::UrlFetch:
        continue;
      default:
        break;
    }
    std::string token = "SpendSeam::";
    const char* slug = nimbus::orch::spendSeamSlug(seam);
    // The enumerator name: the slug with its dashes dropped and each word capitalized.
    bool up = true;
    for (const char* c = slug; *c; ++c) {
      if (*c == '-') { up = true; continue; }
      token += up ? (char)toupper(*c) : *c;
      up = false;
    }
    TEST_ASSERT_TRUE_MESSAGE(all.find(token) != std::string::npos, token.c_str());
  }
}

// Sub-agent dispatch, polls, single shots and loop rounds of every provider: the
// provider files dial only through wire::exchange (gated above). A provider file that
// called the transport itself would bypass the ceiling; the checker proves it would
// be caught.
static std::string withoutComments(const std::string& src) {
  std::string out;
  out.reserve(src.size());
  for (size_t i = 0; i < src.size(); ++i) {
    if (src.compare(i, 2, "//") == 0) {
      while (i < src.size() && src[i] != '\n') ++i;
    } else if (src.compare(i, 2, "/*") == 0) {
      const size_t end = src.find("*/", i + 2);
      i = end == std::string::npos ? src.size() : end + 1;
      continue;
    }
    if (i < src.size()) out += src[i];
  }
  return out;
}

static std::string dialsAroundTheWire(const std::string& src) {
  const std::string code = withoutComments(src);
  for (const char* t : {"execJson(", "execStream(", "WiFiClient"})
    if (code.find(t) != std::string::npos) return t;
  return std::string();
}

static void test_provider_files_dial_only_through_the_gated_wire() {
  int files = 0;
  for (const auto& e : std::filesystem::directory_iterator("lib/harness/src/providers")) {
    const std::string path = e.path().string();
    if (e.path().filename() == "wire.h") continue;
    const std::string ext = e.path().extension().string();
    if (ext != ".cpp" && ext != ".h") continue;
    ++files;
    const std::string why = dialsAroundTheWire(slurp(path));
    TEST_ASSERT_TRUE_MESSAGE(why.empty(), (path + " calls " + why + " outside wire::exchange").c_str());
  }
  TEST_ASSERT_TRUE(files >= 6);   // anthropic, openai, mistral, custom, openai_compat, loop_common, ...
  // The five sub-agent dispatchers are in those files (the rule covers them).
  for (const char* f : {"anthropic.cpp", "openai.cpp", "mistral.cpp", "custom.cpp", "openai_compat.cpp"})
    TEST_ASSERT_TRUE_MESSAGE(slurp(std::string("lib/harness/src/providers/") + f).find("Dispatch(") != std::string::npos, f);
  // Counter-test: a direct transport call is caught.
  TEST_ASSERT_FALSE(dialsAroundTheWire("int x = pd.http->execJson(req, doc, filter, err);").empty());
}

// Every gated seam kind is metered, every meter row holds, and both sinks are installed.
static void test_every_gated_seam_is_metered() {
  for (const SeamRow& r : kSeams) {
    bool metered = false;
    for (const MeterRow& m : kMeters) metered = metered || m.kind == r.kind;
    TEST_ASSERT_TRUE_MESSAGE(metered, (std::string(r.seam) + ": gated but never metered").c_str());
  }
  for (const MeterRow& m : kMeters) {
    const std::string why = checkMeter(m, slurp(m.file));
    TEST_ASSERT_TRUE_MESSAGE(why.empty(), why.c_str());
  }
  for (const MeterRow& m : kSinks) {
    const std::string why = checkMeter(m, slurp(m.file));
    TEST_ASSERT_TRUE_MESSAGE(why.empty(), why.c_str());
  }
}

// Every seam kind has a meter row (a new SpendSeam with none fails), and dropping a
// meter from a copy of its function is caught (the counter-test).
static void test_every_seam_kind_has_a_meter_and_removing_one_fails() {
  for (int k = 0; k < nimbus::orch::kSpendSeamCount; ++k) {
    bool has = false;
    for (const MeterRow& m : kMeters) has = has || (int)m.kind == k;
    TEST_ASSERT_TRUE_MESSAGE(has, nimbus::orch::spendSeamSlug((nimbus::orch::SpendSeam)k));
  }
  for (const MeterRow& m : kMeters) {
    std::string src = slurp(m.file);
    const std::string body = *m.signature ? functionBody(src, m.signature) : src;
    TEST_ASSERT_FALSE_MESSAGE(body.empty(), m.what);
    std::string cut = body;
    size_t p;
    while ((p = cut.find(m.meter)) != std::string::npos) cut.erase(p, std::string(m.meter).size());
    src.replace(src.find(body), body.size(), cut);
    TEST_ASSERT_FALSE_MESSAGE(checkMeter(m, src).empty(), m.what);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_paid_seam_consults_the_gate);
  RUN_TEST(test_removing_a_consult_fails_the_checker);
  RUN_TEST(test_the_table_covers_every_seam_kind);
  RUN_TEST(test_provider_files_dial_only_through_the_gated_wire);
  RUN_TEST(test_every_gated_seam_is_metered);
  RUN_TEST(test_every_seam_kind_has_a_meter_and_removing_one_fails);
  return UNITY_END();
}
