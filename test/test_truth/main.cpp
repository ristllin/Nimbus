// test_truth - the truth check (lane RB-G0, PRD section 8 rail 8, ruling 9), on a scripted
// provider. Properties, each over the class, not one instance:
//   - the probe prompt is RB-M0c's byte for byte, and the device's measured table equals
//     tests/evals/measured/probe.json (ruling 9's rule);
//   - the probe's user message is the bytes RB-M0c measured, for every measured row;
//   - a true statement about an effect in the ring is never a hit: the whole ring reaches the
//     probe, at every ring size and position;
//   - a reply that reports a success the ring does not hold is held once; what returns from the
//     one retry is delivered unchanged, whatever it says; a failed or empty retry delivers the
//     first reply;
//   - the retry item holds facts only: with every effect's own text taken out, only the fixed
//     frame is left;
//   - the receipt line follows only when the probe says the reply reports success, and
//     deterministically where there is no probe answer; never for a read, never when a later
//     attempt of the same kind landed;
//   - a head without a passing measured row is receipt-only; a tainted turn never probes;
//   - the probe fails open at 8 s, on errors and on malformed answers, each counted;
//   - the request never carries a photo;
//   - no word filter: an acknowledgement is probed like any reply;
//   - the stored modes move only as PRD 8.8 says, and truth_state.json round-trips.
#include <unity.h>

#include <ArduinoJson.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <new>
#include <stdexcept>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "nimbus/orch/head_qual.h"
#include "nimbus/orch/truth.h"

namespace truth = nimbus::orch::truth;
namespace fx = nimbus::orch::fx;
namespace hq = nimbus::orch::headqual;
using truth::Truth;
using truth::Verdict;

void setUp() {}
void tearDown() {}

namespace {

std::string readFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

uint32_t crc32Of(const std::string& s) {
  uint32_t c = 0xFFFFFFFFu;
  for (unsigned char b : s) {
    c ^= b;
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

// The text between "<tag>\n" and "\n</tag>" of a rendered user message.
std::string section(const std::string& user, const std::string& tag) {
  const std::string open = "<" + tag + ">\n", close = "\n</" + tag + ">";
  const size_t a = user.find(open);
  if (a == std::string::npos) return "";
  const size_t b = user.find(close, a + open.size());
  return b == std::string::npos ? "" : user.substr(a + open.size(), b - a - open.size());
}

// The user text of a probe request body.
std::string userOf(const truth::ProbeRequest& r) {
  JsonDocument d;
  if (deserializeJson(d, r.body)) return "";
  return d["messages"][0]["content"].as<std::string>();
}

// A scripted provider: answers from a queue (No when it runs out) and records every request.
struct Scripted {
  std::vector<truth::ProbeResult> answers;
  std::vector<truth::ProbeRequest> seen;
  size_t next = 0;
  truth::ProbeFn fn() {
    return [this](const truth::ProbeRequest& r) {
      seen.push_back(r);
      if (next < answers.size()) return answers[next++];
      return truth::ProbeResult{Verdict::No, 500};
    };
  }
};

// A scripted provider that plays a perfect probe for one claimed receipt: yes unless that
// receipt is listed (as succeeded) in the request's effect sections.
struct Oracle {
  std::string claimed;
  std::vector<truth::ProbeRequest> seen;
  truth::ProbeFn fn() {
    return [this](const truth::ProbeRequest& r) {
      seen.push_back(r);
      const std::string u = userOf(r);
      const std::string now = section(u, "effects_this_turn"), rec = section(u, "recent_effects");
      const bool backed = now.find(" ok: " + claimed) != std::string::npos ||
                          rec.find(": " + claimed) != std::string::npos;
      return truth::ProbeResult{backed ? Verdict::No : Verdict::Yes, 600};
    };
  }
};

// A retry round that records its item and answers with a fixed text and the effects the
// round itself had.
struct Retry {
  std::string answer;
  bool ok = true;
  std::vector<truth::TurnEffect> roundFx;
  std::vector<std::string> items;
  truth::RetryFn fn() {
    return [this](const std::string& item, std::string& reply, std::vector<truth::TurnEffect>& more) {
      items.push_back(item);
      reply = answer;
      more = roundFx;
      return ok;
    };
  }
};

fx::RingEntry entry(int64_t epoch, const char* tool, const std::string& receipt) {
  fx::RingEntry e;
  e.epoch = epoch;
  snprintf(e.tool, sizeof(e.tool), "%s", tool);
  snprintf(e.receipt, sizeof(e.receipt), "%s", receipt.c_str());
  e.stored = true;
  return e;
}

constexpr int64_t kNow = 1763470920;   // Tue 18 Nov 2025 13:02 UTC

truth::Turn enforceTurn(const std::string& reply) {
  truth::Turn t;
  t.host = "anthropic";
  t.mode = Truth::Enforce;
  t.ringKnown = true;
  t.owner = "Log 150 g skyr for breakfast";
  t.reply = reply;
  t.nowEpoch = kNow;
  return t;
}

truth::TurnEffect eff(const char* tool, bool ok, const std::string& text, truth::Kind k) {
  truth::TurnEffect e;
  e.tool = tool;
  e.ok = ok;
  e.receipt = text;
  e.kind = k;
  return e;
}

}  // namespace

// ---- the measured probe ------------------------------------------------------------------

void test_the_prompt_is_the_measured_one_byte_for_byte() {
  const std::string want = readFile("tests/evals/probe/prompt.txt");
  TEST_ASSERT_EQUAL_size_t(1296, want.size());
  TEST_ASSERT_EQUAL_size_t(want.size(), truth::kProbePromptLen);
  TEST_ASSERT_TRUE(want == std::string(truth::kProbePrompt, truth::kProbePromptLen));
}

void test_the_measured_table_is_probe_json() {
  JsonDocument d;
  TEST_ASSERT_FALSE(deserializeJson(d, readFile("tests/evals/measured/probe.json")));
  JsonObject models = d["models"].as<JsonObject>();
  TEST_ASSERT_TRUE(models.size() >= 4);
  size_t n = 0;
  const truth::MeasuredProbe* rows = truth::measuredProbes(n);
  std::set<std::string> compiled;
  for (size_t i = 0; i < n; ++i) {
    compiled.insert(rows[i].model);
    TEST_ASSERT_TRUE_MESSAGE(models[rows[i].model].is<JsonObject>(), rows[i].model);
  }
  for (JsonPair kv : models) {
    const bool passes = kv.value()["ruling9"]["qualifies"] | false;
    TEST_ASSERT_TRUE_MESSAGE(compiled.count(kv.key().c_str()) == 1, kv.key().c_str());
    TEST_ASSERT_EQUAL_MESSAGE(passes, truth::probePasses(kv.key().c_str()), kv.key().c_str());
  }
  // Ruling 9: claude-haiku-5-5 qualifies; the partial and unmeasured models do not.
  TEST_ASSERT_TRUE(truth::probePasses("claude-haiku-5-5"));
  TEST_ASSERT_FALSE(truth::probePasses("gpt-5.6-luna"));
  TEST_ASSERT_FALSE(truth::probePasses("glm-5.3-flash"));
  TEST_ASSERT_FALSE(truth::probePasses("mistral-small-latest"));
  TEST_ASSERT_FALSE(truth::probePasses(""));
  TEST_ASSERT_FALSE(truth::probePasses(nullptr));
  TEST_ASSERT_FALSE(truth::probePasses("claude-haiku-5-5x"));
}

// Every head_qual row: enforce or shadow only when its host's probe model passes and the wire
// exists; the head row's own mode otherwise caps it.
void test_a_head_without_a_passing_row_is_receipt_only() {
  size_t n = 0;
  const hq::Row* rows = hq::rows(n);
  TEST_ASSERT_TRUE(n > 0);
  for (size_t i = 0; i < n; ++i) {
    const hq::Row& r = rows[i];
    const char* model = r.family[0] == '*' ? "some-model" : r.family;
    for (hq::Line line : {hq::Line::E1, hq::Line::E2}) {
      const Truth got = truth::startMode(r.host, model, line);
      const bool probe = truth::probePasses(truth::probeModelFor(r.host)) && truth::probeWire(r.host);
      const Truth want = probe ? hq::lookup(r.host, model, line).truth : Truth::ReceiptOnly;
      TEST_ASSERT_EQUAL_MESSAGE((int)want, (int)got, r.host);
    }
  }
  TEST_ASSERT_EQUAL((int)Truth::Enforce, (int)truth::startMode("anthropic", "claude-sonnet-5-5", hq::Line::E1));
  TEST_ASSERT_EQUAL((int)Truth::Enforce, (int)truth::startMode("anthropic", "claude-haiku-5-5", hq::Line::E1));
  TEST_ASSERT_EQUAL((int)Truth::ReceiptOnly, (int)truth::startMode("openai", "gpt-5.6-luna", hq::Line::E1));
  TEST_ASSERT_EQUAL((int)Truth::ReceiptOnly, (int)truth::startMode("zai", "glm-5.3", hq::Line::E1));
  TEST_ASSERT_EQUAL((int)Truth::ReceiptOnly, (int)truth::startMode("mistral", "mistral-large", hq::Line::E1));
  TEST_ASSERT_EQUAL((int)Truth::ReceiptOnly, (int)truth::startMode("cumulo", "x", hq::Line::E1));
  TEST_ASSERT_EQUAL((int)Truth::ReceiptOnly, (int)truth::startMode("custom", "x", hq::Line::E1));
  TEST_ASSERT_EQUAL((int)Truth::ReceiptOnly, (int)truth::startMode("nowhere", "x", hq::Line::E1));
  TEST_ASSERT_EQUAL((int)Truth::ReceiptOnly, (int)truth::startMode(nullptr, nullptr, hq::Line::E1));
  // The rule itself, over every combination: only a wire and a passing model keep the row's mode.
  for (Truth row : {Truth::ReceiptOnly, Truth::Enforce, Truth::Shadow})
    for (bool wire : {false, true})
      for (bool passes : {false, true})
        TEST_ASSERT_EQUAL((int)(wire && passes ? row : Truth::ReceiptOnly), (int)truth::startModeFrom(row, wire, passes));
  TEST_ASSERT_EQUAL_STRING("claude-haiku-5-5", truth::probeModelFor("anthropic"));
  TEST_ASSERT_EQUAL_STRING("", truth::probeModelFor("custom"));
  TEST_ASSERT_EQUAL_STRING("", truth::probeModelFor(nullptr));
}

// A receipt-only turn never asks the probe, whatever the reply says.
void test_receipt_only_never_probes() {
  for (bool failed : {false, true}) {
    truth::Turn t = enforceTurn("Logged: 150 g skyr.");
    t.mode = Truth::ReceiptOnly;
    if (failed) t.now.push_back(eff("ledger_write", false, "card write failed", truth::Kind::Ledger));
    Scripted s;
    s.answers = {{Verdict::Yes, 100}};
    Retry r;
    truth::Counters c;
    const truth::Outcome o = truth::check(t, s.fn(), r.fn(), c);
    TEST_ASSERT_EQUAL_size_t(0, s.seen.size());
    TEST_ASSERT_EQUAL_size_t(0, r.items.size());
    TEST_ASSERT_TRUE(o.reply == t.reply);
    TEST_ASSERT_FALSE(o.held);
    TEST_ASSERT_EQUAL(0, o.probes);
    TEST_ASSERT_EQUAL(failed, !o.status.empty());
    TEST_ASSERT_EQUAL_UINT32(failed ? 1 : 0, c.receiptsFixed);
    TEST_ASSERT_EQUAL_UINT32(0, c.receiptsProbed);
  }
}

// ---- the request ------------------------------------------------------------------------

// Every measured row (claim corpus, past references, near misses, acknowledgements, stress),
// with and without the recent effects, renders to the bytes RB-M0c sent.
static void renderRow(const std::string& set, const std::string& id, JsonVariant row, JsonVariant ring,
                      std::map<std::string, std::string>& got) {
  std::vector<truth::TurnEffect> now;
  for (JsonVariant e : row["this_turn"].as<JsonArray>())
    now.push_back(eff(e["tool"] | "", e["ok"] | false, e["receipt"] | "", truth::Kind::Other));
  std::vector<truth::RecentLine> recent;
  for (JsonVariant e : ring.as<JsonArray>()) recent.push_back({e["at"] | "", e["tool"] | "", e["receipt"] | ""});
  const std::string owner = row["owner"] | "", reply = row["reply"] | "";
  const std::string with = truth::renderUser(owner, reply, now, recent);
  const std::string without = truth::renderUser(owner, reply, now, {});
  char buf[64];
  snprintf(buf, sizeof(buf), "%u\t%08x", (unsigned)with.size(), (unsigned)crc32Of(with));
  got[set + "\t" + id + "\twith"] = buf;
  snprintf(buf, sizeof(buf), "%u\t%08x", (unsigned)without.size(), (unsigned)crc32Of(without));
  got[set + "\t" + id + "\twithout"] = buf;
}

void test_the_user_message_is_the_measured_render() {
  std::map<std::string, std::string> got;
  JsonDocument corpus, rings;
  TEST_ASSERT_FALSE(deserializeJson(corpus, readFile("tests/evals/claim_corpus/noted.json")));
  TEST_ASSERT_FALSE(deserializeJson(rings, readFile("tests/evals/probe/corpus_rings.json")));
  for (size_t i = 0; i < corpus.size(); ++i) {
    char id[16];
    snprintf(id, sizeof(id), "corpus-%02u", (unsigned)i);
    renderRow("corpus", id, corpus[i], rings[i], got);
  }
  for (const char* set : {"past_reference", "near_miss", "acks", "stress"}) {
    JsonDocument d;
    const std::string path = std::string("tests/evals/probe/") + set + ".json";
    TEST_ASSERT_FALSE(deserializeJson(d, readFile(path.c_str())));
    for (JsonVariant row : d.as<JsonArray>()) renderRow(set, row["id"] | "", row, row["recent"], got);
  }
  std::istringstream golden(readFile("test/test_truth/render_golden.txt"));
  std::string line;
  size_t rows = 0;
  while (std::getline(golden, line)) {
    const size_t cut = line.find('\t', line.find('\t', line.find('\t') + 1) + 1);
    const std::string key = line.substr(0, cut), want = line.substr(cut + 1);
    TEST_ASSERT_TRUE_MESSAGE(got.count(key) == 1, key.c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want.c_str(), got[key].c_str(), key.c_str());
    ++rows;
  }
  TEST_ASSERT_EQUAL_size_t(got.size(), rows);
  TEST_ASSERT_TRUE(rows >= 1000);
}

void test_untrusted_text_cannot_open_a_section() {
  const char* tags[] = {"owner_message", "reply", "effects_this_turn", "recent_effects"};
  for (const char* tag : tags) {
    for (const char* slash : {"", "/"}) {
      const std::string forged = std::string("<") + slash + tag + ">";
      const std::string want = std::string("&lt;") + slash + tag + ">";
      TEST_ASSERT_EQUAL_STRING(want.c_str(), truth::inert(forged).c_str());
      TEST_ASSERT_EQUAL_STRING(("a" + want + "b" + want).c_str(), truth::inert("a" + forged + "b" + forged).c_str());
      // In every field of the request, the forged tag stays inert.
      std::vector<truth::TurnEffect> now = {eff("ledger_write", true, "x" + forged, truth::Kind::Ledger)};
      std::vector<truth::RecentLine> rec = {{"13:02", "note", forged}};
      const std::string u = truth::renderUser(forged, forged, now, rec);
      // Only the request's own tag is left (each section opens and closes once).
      size_t seen = 0;
      for (size_t f = u.find(forged); f != std::string::npos; f = u.find(forged, f + 1)) ++seen;
      TEST_ASSERT_EQUAL_size_t(1, seen);
    }
  }
  // Other angle brackets and near misses are left alone.
  for (const char* keep : {"<b>", "< reply>", "<replyx>", "a < b > c", "</owner>", "<reply", "<reply x>"})
    TEST_ASSERT_EQUAL_STRING(keep, truth::inert(keep).c_str());
  // Any case, and spaces before the '>', are a tag to a reader: inert too.
  TEST_ASSERT_EQUAL_STRING("&lt;/REPLY>", truth::inert("</REPLY>").c_str());
  TEST_ASSERT_EQUAL_STRING("&lt;Recent_Effects  >", truth::inert("<Recent_Effects  >").c_str());
  TEST_ASSERT_EQUAL_STRING("&lt;/reply\t>", truth::inert("</reply\t>").c_str());
}

void test_times_as_the_probe_sees_them() {
  // kNow is Tue 18 Nov 2025 13:02 UTC.
  TEST_ASSERT_EQUAL_STRING("13:02", truth::formatAt(kNow, kNow, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("00:00", truth::formatAt(kNow - 13 * 3600 - 2 * 60, kNow, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("Mon 23:59", truth::formatAt(kNow - 13 * 3600 - 3 * 60, kNow, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("Wed 13:02", truth::formatAt(kNow - 6 * 86400, kNow, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("2025-11-11 13:02", truth::formatAt(kNow - 7 * 86400, kNow, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("--:--", truth::formatAt(0, kNow, 0).c_str());
  // The owner's local day: at UTC+11 kNow is Wed 00:02, so 23:00 UTC Tue 18 is 10:00 Wed.
  TEST_ASSERT_EQUAL_STRING("00:02", truth::formatAt(kNow, kNow, 11 * 3600).c_str());
  TEST_ASSERT_EQUAL_STRING("Tue 23:59", truth::formatAt(kNow - 3 * 60, kNow, 11 * 3600).c_str());
  TEST_ASSERT_EQUAL_STRING("08:02", truth::formatAt(kNow, kNow, -5 * 3600).c_str());
  // The device's clock is not set (now before 2023): every entry carries its full date.
  TEST_ASSERT_EQUAL_STRING("2025-11-18 13:02", truth::formatAt(kNow, 0, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("2025-11-17 13:02", truth::formatAt(kNow - 86400, 1000, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("--:--", truth::formatAt(0, 0, 0).c_str());
  // A clock that ran ahead of the entry's time still reads as today.
  TEST_ASSERT_EQUAL_STRING("13:07", truth::formatAt(kNow + 300, kNow, 0).c_str());
  std::vector<fx::RingEntry> ring = {entry(kNow - 86400, "ledger_write", "added L1"), entry(kNow, "note", "k")};
  const std::vector<truth::RecentLine> v = truth::recentLines(ring, kNow, 0);
  TEST_ASSERT_EQUAL_size_t(2, v.size());
  TEST_ASSERT_EQUAL_STRING("Mon 13:02", v[0].at.c_str());
  TEST_ASSERT_EQUAL_STRING("ledger_write", v[0].tool.c_str());
  TEST_ASSERT_EQUAL_STRING("added L1", v[0].receipt.c_str());
  TEST_ASSERT_EQUAL_STRING("13:02", v[1].at.c_str());
}

// The request: the measured settings, one text user message, never an image, for any owner
// message (a photo reaches the check only as its stub line).
void test_the_request_never_carries_a_photo() {
  truth::ProbeRequest r;
  TEST_ASSERT_FALSE(truth::buildRequest("openai", "x", r));
  TEST_ASSERT_FALSE(truth::buildRequest("custom", "x", r));
  TEST_ASSERT_FALSE(truth::buildRequest(nullptr, "x", r));
  TEST_ASSERT_TRUE(r.body.empty());
  const std::string user = truth::renderUser("[photo p1a2b3c4: label] what is in this?", "It is skyr.", {}, {});
  TEST_ASSERT_TRUE(truth::buildRequest("anthropic", user, r));
  TEST_ASSERT_EQUAL_STRING("claude-haiku-5-5", r.model.c_str());
  TEST_ASSERT_EQUAL_STRING("api.anthropic.com", r.host.c_str());
  TEST_ASSERT_EQUAL_STRING("/v1/messages", r.path.c_str());
  TEST_ASSERT_EQUAL_UINT32(8000, r.timeoutMs);
  JsonDocument d;
  TEST_ASSERT_FALSE(deserializeJson(d, r.body));
  JsonObject o = d.as<JsonObject>();
  TEST_ASSERT_EQUAL_size_t(5, o.size());
  TEST_ASSERT_EQUAL_STRING("claude-haiku-5-5", o["model"] | "");
  TEST_ASSERT_EQUAL(1024, o["max_tokens"] | 0);
  TEST_ASSERT_TRUE(std::string(truth::kProbePrompt) == (o["system"] | ""));
  TEST_ASSERT_EQUAL_STRING("low", o["output_config"]["effort"] | "");
  TEST_ASSERT_EQUAL_size_t(1, o["output_config"].size());
  JsonArray msgs = o["messages"].as<JsonArray>();
  TEST_ASSERT_EQUAL_size_t(1, msgs.size());
  TEST_ASSERT_EQUAL_size_t(2, msgs[0].size());
  TEST_ASSERT_EQUAL_STRING("user", msgs[0]["role"] | "");
  TEST_ASSERT_TRUE(msgs[0]["content"].is<const char*>());   // a string: no content blocks at all
  TEST_ASSERT_TRUE(user == msgs[0]["content"].as<std::string>());
  for (const char* banned : {"\"image\"", "base64", "\"source\"", "media_type"})
    TEST_ASSERT_TRUE(r.body.find(banned) == std::string::npos);
}

void test_owner_and_reply_are_bounded() {
  const std::string big(40000, 'a');
  const std::string u = truth::renderUser(big, big, {}, {});
  TEST_ASSERT_EQUAL_size_t(truth::kOwnerMax, section(u, "owner_message").size());
  TEST_ASSERT_TRUE(section(u, "reply").size() <= truth::kReplyMax + 8);
  // A long reply keeps its start and its end: "saved to notes/x.md" usually closes it.
  const std::string longReply = "Here is the plan. " + std::string(40000, 'p') + " I saved it to notes/plan.md.";
  const std::string r = section(truth::renderUser("o", longReply, {}, {}), "reply");
  TEST_ASSERT_TRUE(r.find("Here is the plan.") == 0);
  TEST_ASSERT_TRUE(r.find("I saved it to notes/plan.md.") != std::string::npos);
  // This turn's receipts reach the probe whole up to kNowReceiptMax (a ledger day of rows).
  const std::string rows = "added L1 a, L2 b, L3 c, L4 d, L5 e, L6 f, L7 g, L8 h, L9 i, L10 j, L11 k, L12 l, "
                           "L13 m, L14 n, L15 o, L16 p, L17 q, L18 r, L19 s, L20 t, L21 u, L22 v, L23 w, L24 x, "
                           "L25 y, L26 z, L27 skyr 150 g";
  TEST_ASSERT_TRUE(rows.size() > fx::kReceiptMax);
  const std::string n = section(truth::renderUser("o", "r", {eff("ledger_write", true, rows, truth::Kind::Ledger)}, {}),
                                "effects_this_turn");
  TEST_ASSERT_TRUE(n.find("skyr 150 g") != std::string::npos);
  // Clipped on a UTF-8 boundary.
  std::string euro;
  for (int i = 0; i < 6000; ++i) euro += "\xe2\x82\xac";
  const std::string o = section(truth::renderUser(euro, "r", {}, {}), "owner_message");
  TEST_ASSERT_TRUE(o.size() <= truth::kOwnerMax && o.size() % 3 == 0);
}

void test_answers_are_read_as_measured() {
  bool lenient = true;
  TEST_ASSERT_EQUAL((int)Verdict::Yes, (int)truth::parseAnswer("{\"claims_unbacked_action\": true}", &lenient));
  TEST_ASSERT_FALSE(lenient);
  TEST_ASSERT_EQUAL((int)Verdict::No, (int)truth::parseAnswer(" {\"claims_unbacked_action\":false}\n", &lenient));
  TEST_ASSERT_FALSE(lenient);
  TEST_ASSERT_EQUAL((int)Verdict::Yes,
                    (int)truth::parseAnswer("```json\n{\"claims_unbacked_action\": true}\n```", &lenient));
  TEST_ASSERT_FALSE(lenient);
  TEST_ASSERT_EQUAL((int)Verdict::Yes,
                    (int)truth::parseAnswer("The reply says saved. {\"claims_unbacked_action\" : true} ok", &lenient));
  TEST_ASSERT_TRUE(lenient);
  TEST_ASSERT_EQUAL((int)Verdict::No, (int)truth::parseAnswer(
                                          "{\"claims_unbacked_action\": false} {\"claims_unbacked_action\": true}"));
  for (const char* bad : {"", "true", "yes", "{\"claims_unbacked_action\": \"true\"}", "{\"claims_unbacked_action\": 1}",
                          "{\"claims_unbacked_action\": true", "{\"claims_unbacked_action\": true, \"x\": 1}",
                          "{\"claim\": true}", "{'claims_unbacked_action': true}"})
    TEST_ASSERT_EQUAL_MESSAGE((int)Verdict::Malformed, (int)truth::parseAnswer(bad), bad);
  // The Anthropic response: text blocks only (thinking is skipped); any non-200 is an error.
  const char* ok200 =
      "{\"content\":[{\"type\":\"thinking\",\"thinking\":\"{\\\"claims_unbacked_action\\\": false}\"},"
      "{\"type\":\"text\",\"text\":\"{\\\"claims_unbacked_action\\\": true}\"}]}";
  TEST_ASSERT_EQUAL((int)Verdict::Yes, (int)truth::parseResponse(200, ok200));
  TEST_ASSERT_EQUAL((int)Verdict::Error, (int)truth::parseResponse(529, ok200));
  TEST_ASSERT_EQUAL((int)Verdict::Error, (int)truth::parseResponse(0, ""));
  TEST_ASSERT_EQUAL((int)Verdict::Error, (int)truth::parseResponse(200, "not json"));
  TEST_ASSERT_EQUAL((int)Verdict::Malformed, (int)truth::parseResponse(200, "{\"content\":[]}"));
  TEST_ASSERT_EQUAL((int)Verdict::Malformed,
                    (int)truth::parseResponse(200, "{\"content\":[{\"type\":\"text\",\"text\":\"I think so\"}]}"));
}

// ---- the check --------------------------------------------------------------------------

// A true statement about any effect in the ring, at any ring size and position, with this
// turn's effects or none: the whole ring reaches the probe, so a perfect probe never hits.
void test_a_true_statement_about_an_effect_in_the_ring_is_never_a_hit() {
  std::mt19937 rng(7);
  for (size_t size = 1; size <= fx::kRingEntries; ++size) {
    for (size_t pos : {(size_t)0, size / 2, size - 1}) {
      truth::Turn t = enforceTurn("");
      for (size_t i = 0; i < size; ++i) {
        const std::string rec = "added L" + std::to_string(1000 + i) + " item " + std::to_string(rng() % 997);
        t.recent.push_back(entry(kNow - (int64_t)(size - i) * 3000, i % 2 ? "ledger_write" : "files_write", rec));
      }
      if (size % 3 == 0) t.now.push_back(eff("note_save", true, "saved key diet", truth::Kind::Memory));
      Oracle o;
      o.claimed = t.recent[pos].receipt;
      t.reply = "Yes: " + o.claimed + ", as I logged it earlier.";
      Retry r;
      truth::Counters c;
      const truth::Outcome out = truth::check(t, o.fn(), r.fn(), c);
      TEST_ASSERT_EQUAL_size_t(1, o.seen.size());
      TEST_ASSERT_FALSE(out.held);
      TEST_ASSERT_TRUE(out.reply == t.reply);
      TEST_ASSERT_TRUE(out.status.empty());
      TEST_ASSERT_EQUAL_size_t(0, r.items.size());
      TEST_ASSERT_EQUAL_UINT32(0, c.hits);
      // Every entry of the ring is in the request, oldest first.
      const std::string rec = section(userOf(o.seen[0]), "recent_effects");
      size_t at = 0;
      for (const auto& e : t.recent) {
        const size_t f = rec.find(e.receipt, at);
        TEST_ASSERT_TRUE(f != std::string::npos);
        at = f;
      }
    }
  }
}

// A reply that reports a success the device has no effect for: held once, one facts-only
// round, and whatever that round returns is what is delivered.
void test_an_unbacked_success_is_held_once_and_the_retry_is_delivered_unchanged() {
  const std::vector<std::string> retries = {
      "Sorry, I have not logged it yet. Shall I?",
      "Logged: 150 g skyr.",   // still claims it: delivered as is (one hold a turn)
      "<b>Done</b> \xf0\x9f\x91\x8d (Correction: none)",
      std::string("multi\nline\n\n reply \t with tabs"),
      std::string(5000, 'z'),
  };
  for (const std::string& answer : retries) {
    truth::Turn t = enforceTurn("Logged: 150 g skyr for breakfast.");
    t.recent.push_back(entry(kNow - 3600, "ledger_write", "added L00A2 lunch: soup 210 kcal"));
    Oracle o;
    o.claimed = "added L00A3 breakfast: skyr 150 g";
    Retry r;
    r.answer = answer;
    truth::Counters c;
    const truth::Outcome out = truth::check(t, o.fn(), r.fn(), c);
    TEST_ASSERT_TRUE(out.held);
    TEST_ASSERT_TRUE(out.retried);
    TEST_ASSERT_EQUAL_size_t(1, r.items.size());
    TEST_ASSERT_EQUAL_size_t(1, o.seen.size());   // no failed write: the retry is not probed
    TEST_ASSERT_TRUE(out.reply == answer);
    TEST_ASSERT_TRUE(out.status.empty());
    TEST_ASSERT_EQUAL((int)Verdict::Yes, (int)out.first);
    TEST_ASSERT_EQUAL_UINT32(1, c.holds);
    TEST_ASSERT_EQUAL_UINT32(1, c.hits);
    TEST_ASSERT_EQUAL_UINT32(1, c.retryReplies);
  }
}

// A round that fails, or comes back empty, never costs the owner his answer.
void test_a_failed_or_empty_retry_delivers_the_first_reply() {
  for (int mode = 0; mode < 3; ++mode) {
    truth::Turn t = enforceTurn("Saved your target: 2,300 kcal.");
    Scripted s;
    s.answers = {{Verdict::Yes, 700}};
    Retry r;
    r.ok = mode != 0;
    r.answer = mode == 1 ? "" : " \n\t ";
    truth::Counters c;
    const truth::Outcome out = truth::check(t, s.fn(), r.fn(), c);
    TEST_ASSERT_TRUE(out.held);
    TEST_ASSERT_FALSE(out.retried);
    TEST_ASSERT_TRUE(out.reply == t.reply);
    TEST_ASSERT_EQUAL_UINT32(1, c.retryFailed);
    TEST_ASSERT_EQUAL_UINT32(0, c.retryReplies);
  }
  // No retry function at all: nothing can be held; the reply is delivered, a failed write
  // still earns its line.
  truth::Turn t = enforceTurn("Saved.");
  t.now.push_back(eff("note_save", false, "card busy", truth::Kind::Memory));
  Scripted s;
  s.answers = {{Verdict::Yes, 700}};
  truth::Counters c;
  const truth::Outcome out = truth::check(t, s.fn(), truth::RetryFn(), c);
  TEST_ASSERT_TRUE(out.reply == t.reply);
  TEST_ASSERT_FALSE(out.held);
  TEST_ASSERT_EQUAL_UINT32(0, c.holds);
  TEST_ASSERT_EQUAL_UINT32(0, c.retryFailed);
  TEST_ASSERT_EQUAL_UINT32(1, c.receiptsProbed);
  TEST_ASSERT_FALSE(out.status.empty());
}

// The round after a hold may itself write: its effects join this turn's before the receipt
// question, so a write that landed in the round clears the line and one that failed earns it.
void test_the_retry_rounds_own_effects_count() {
  {
    truth::Turn t = enforceTurn("Logged: 150 g skyr.");
    t.now.push_back(eff("ledger_write", false, "busy", truth::Kind::Ledger));
    Scripted s;
    s.answers = {{Verdict::Yes, 300}, {Verdict::Yes, 300}};
    Retry r;
    r.answer = "Done now: skyr 150 g is logged (L00A4).";
    r.roundFx = {eff("ledger_write", true, "added L00A4 breakfast: skyr 150 g", truth::Kind::Ledger)};
    truth::Counters c;
    const truth::Outcome out = truth::check(t, s.fn(), r.fn(), c);
    TEST_ASSERT_TRUE(out.reply == r.answer);
    TEST_ASSERT_TRUE(out.status.empty());
    TEST_ASSERT_EQUAL(1, out.probes);   // nothing failed any more: no receipt question
  }
  {
    truth::Turn t = enforceTurn("Logged: 150 g skyr.");
    Scripted s;
    s.answers = {{Verdict::Yes, 300}, {Verdict::Yes, 300}};
    Retry r;
    r.answer = "Logged it now (L00A4).";
    r.roundFx = {eff("ledger_write", false, "the card write failed", truth::Kind::Ledger)};
    truth::Counters c;
    const truth::Outcome out = truth::check(t, s.fn(), r.fn(), c);
    TEST_ASSERT_TRUE(out.reply == r.answer);
    TEST_ASSERT_EQUAL(2, out.probes);
    TEST_ASSERT_EQUAL_STRING("Note: the ledger change in this reply did not go through (the card write failed)",
                             out.status.c_str());
    // The receipt question sees the round's effect.
    TEST_ASSERT_TRUE(section(userOf(s.seen[1]), "effects_this_turn").find("ledger_write FAILED: the card write failed") !=
                     std::string::npos);
  }
}

// A probe or a round that throws is a probe with no answer or a failed round: the owner still
// gets his reply.
void test_a_throwing_probe_or_retry_fails_open() {
  truth::Turn t = enforceTurn("Logged.");
  truth::Counters c;
  truth::ProbeFn boom = [](const truth::ProbeRequest&) -> truth::ProbeResult { throw std::runtime_error("x"); };
  truth::Outcome out = truth::check(t, boom, Retry().fn(), c);
  TEST_ASSERT_TRUE(out.reply == t.reply);
  TEST_ASSERT_EQUAL_UINT32(1, c.failError);
  Scripted s;
  s.answers = {{Verdict::Yes, 300}};
  truth::RetryFn bad = [](const std::string&, std::string&, std::vector<truth::TurnEffect>&) -> bool {
    throw std::bad_alloc();
  };
  out = truth::check(t, s.fn(), bad, c);
  TEST_ASSERT_TRUE(out.held);
  TEST_ASSERT_TRUE(out.reply == t.reply);
  TEST_ASSERT_EQUAL_UINT32(1, c.retryFailed);
  truth::HeadState h;
  truth::shadowProbe(t, boom, h);
  TEST_ASSERT_EQUAL_UINT32(1, h.c.failError);
}

// The item says what the device did and nothing else: take every effect's own words out and
// only the fixed frame is left, whatever the effects hold.
void test_the_retry_item_holds_facts_only() {
  std::mt19937 rng(11);
  const char* tools[] = {"ledger_write", "files_write", "note_save", "routine_create", "send_message"};
  for (int round = 0; round < 200; ++round) {
    std::vector<truth::TurnEffect> now;
    std::vector<truth::RecentLine> rec;
    const size_t nn = rng() % 5, nr = rng() % 20;
    for (size_t i = 0; i < nn; ++i)
      now.push_back(eff(tools[rng() % 5], rng() % 2, "text " + std::to_string(rng()), truth::Kind::Other));
    for (size_t i = 0; i < nr; ++i)
      rec.push_back({std::to_string(10 + i) + ":0" + std::to_string(i % 10), tools[rng() % 5],
                     "receipt " + std::to_string(rng())});
    const std::string item = truth::factsItem(now, rec);
    TEST_ASSERT_TRUE(item.size() <= truth::kItemMax);
    std::string rest = item;
    auto cut = [&rest](const std::string& frag) {
      const size_t f = rest.find(frag);
      TEST_ASSERT_TRUE_MESSAGE(f != std::string::npos, frag.c_str());
      rest.erase(f, frag.size());
    };
    for (const auto& e : now) cut(e.tool + (e.ok ? " ok: " : " FAILED: ") + e.receipt);
    const size_t shown = nr < truth::kItemRecentMax ? nr : truth::kItemRecentMax;
    for (size_t i = nr - shown; i < nr; ++i) cut(rec[i].at + " " + rec[i].tool + ": " + rec[i].receipt);
    // What is left: the frame, the separators, "none" for an empty list.
    std::string frame = "[device] effects this turn: ";
    frame += nn ? std::string((nn - 1) * 2, ' ') : "none";
    frame += ". Recent: ";
    frame += shown ? std::string((shown - 1) * 2, ' ') : "none";
    frame += ".";
    for (char& ch : rest)
      if (ch == ';') ch = ' ';
    TEST_ASSERT_EQUAL_STRING(frame.c_str(), rest.c_str());
  }
  TEST_ASSERT_EQUAL_STRING("[device] effects this turn: none. Recent: none.", truth::factsItem({}, {}).c_str());
}

void test_the_retry_item_is_bounded_and_keeps_this_turn_whole() {
  std::vector<truth::TurnEffect> now;
  for (int i = 0; i < 6; ++i) now.push_back(eff("ledger_write", true, std::string(150, 'n'), truth::Kind::Ledger));
  std::vector<truth::RecentLine> rec;
  for (int i = 0; i < 50; ++i) rec.push_back({"12:00", "files_write", std::string(150, 'a' + i % 26)});
  const std::string item = truth::factsItem(now, rec);
  TEST_ASSERT_TRUE(item.size() <= truth::kItemMax);
  size_t count = 0;
  for (size_t f = item.find(std::string(150, 'n')); f != std::string::npos; f = item.find(std::string(150, 'n'), f + 1))
    ++count;
  TEST_ASSERT_EQUAL_size_t(6, count);
  // The newest recent effect is the one kept first.
  TEST_ASSERT_TRUE(item.find(rec.back().receipt) != std::string::npos);
  // This turn's effects are never dropped, even past the bound.
  std::vector<truth::TurnEffect> many;
  for (int i = 0; i < 32; ++i) many.push_back(eff("t", false, std::string(150, 'f'), truth::Kind::File));
  const std::string big = truth::factsItem(many, rec);
  TEST_ASSERT_TRUE(big.find("Recent: none.") != std::string::npos);
  // Effect text arrives as one line: a newline in a tool's error does not break the item.
  const std::string one = truth::factsItem({eff("files_write", false, "a\nb\rc", truth::Kind::File)}, {});
  TEST_ASSERT_TRUE(one.find('\n') == std::string::npos && one.find('\r') == std::string::npos);
}

// The held turn's item is the facts item of this turn and the chat's ring.
void test_the_hold_sends_the_facts_item_of_this_turn() {
  truth::Turn t = enforceTurn("Logged.");
  t.recent.push_back(entry(kNow - 600, "ledger_write", "added L00A1 skyr 150 g"));
  t.now.push_back(eff("ledger_write", false, "the card write failed", truth::Kind::Ledger));
  Scripted s;
  s.answers = {{Verdict::Yes, 400}, {Verdict::No, 400}};
  Retry r;
  r.answer = "The ledger write failed; nothing was logged.";
  truth::Counters c;
  truth::check(t, s.fn(), r.fn(), c);
  TEST_ASSERT_EQUAL_size_t(1, r.items.size());
  const std::string want =
      truth::factsItem(t.now, truth::recentLines(t.recent, t.nowEpoch, t.utcOffsetSec));
  TEST_ASSERT_EQUAL_STRING(want.c_str(), r.items[0].c_str());
  TEST_ASSERT_EQUAL_STRING(
      "[device] effects this turn: ledger_write FAILED: the card write failed. Recent: 12:52 ledger_write: added "
      "L00A1 skyr 150 g.",
      r.items[0].c_str());
}

// ---- the receipt line -------------------------------------------------------------------

void test_the_receipt_names_each_kind_whose_last_attempt_failed() {
  using K = truth::Kind;
  TEST_ASSERT_EQUAL_STRING("", truth::receiptLine({}).c_str());
  TEST_ASSERT_EQUAL_STRING("Note: the ledger change in this reply did not go through (the card write failed)",
                           truth::receiptLine({eff("ledger_write", false, "the card write failed", K::Ledger)}).c_str());
  // A later attempt of the same kind that landed: no line.
  TEST_ASSERT_EQUAL_STRING("", truth::receiptLine({eff("ledger_write", false, "busy", K::Ledger),
                                                   eff("ledger_write", true, "added L1", K::Ledger)})
                                   .c_str());
  // A landed attempt followed by a failed one: the line.
  TEST_ASSERT_FALSE(truth::receiptLine({eff("ledger_write", true, "added L1", K::Ledger),
                                        eff("ledger_write", false, "busy", K::Ledger)})
                        .empty());
  // A failed read never earns one.
  TEST_ASSERT_EQUAL_STRING("", truth::receiptLine({eff("files_read", false, "no such file", K::Read)}).c_str());
  TEST_ASSERT_FALSE(truth::writeFailed({eff("files_read", false, "no such file", K::Read)}));
  // Two kinds: one line naming both, in the order they failed.
  TEST_ASSERT_EQUAL_STRING(
      "Note: the file change and the reminder change in this reply did not go through (disk full; clock not set)",
      truth::receiptLine({eff("files_write", false, "disk full", K::File),
                          eff("routine_create", false, "clock not set", K::Routine)})
          .c_str());
  // Three kinds: "a, b and c".
  TEST_ASSERT_EQUAL_STRING(
      "Note: the ledger change, the file change and the message in this reply did not go through (x; y; z)",
      truth::receiptLine({eff("ledger_write", false, "x", K::Ledger), eff("files_write", false, "y", K::File),
                          eff("send_message", false, "z", K::Send)})
          .c_str());
  // The error is one line and bounded.
  const std::string l = truth::receiptLine({eff("x", false, std::string(400, 'e') + "\nsecond", K::Other)});
  TEST_ASSERT_TRUE(l.find('\n') == std::string::npos);
  TEST_ASSERT_TRUE(l.size() <= 64 + truth::kErrorMax);
  for (K k : {K::Ledger, K::File, K::Memory, K::Routine, K::Setting, K::Send, K::Other})
    TEST_ASSERT_TRUE(std::string(truth::kindNoun(k)).size() > 0);
  TEST_ASSERT_EQUAL_STRING("", truth::kindNoun(K::Read));
}

// Enforce: the receipt follows only when a probe says the delivered reply reports success.
void test_the_receipt_follows_only_a_probe_yes_in_enforce() {
  struct Case {
    Verdict first, second;
    bool held, status;
    uint8_t probes;
  } cases[] = {
      {Verdict::No, Verdict::No, false, false, 1},          // an honest report of the failure
      {Verdict::Yes, Verdict::Yes, true, true, 2},          // the retry still reports success
      {Verdict::Yes, Verdict::No, true, false, 2},          // the retry reports the failure
      {Verdict::Yes, Verdict::Timeout, true, true, 2},      // no answer on the retry: deterministic
      {Verdict::Yes, Verdict::Malformed, true, true, 2},
      {Verdict::Timeout, Verdict::No, false, true, 1},      // no answer at all: deterministic
      {Verdict::Error, Verdict::No, false, true, 1},
      {Verdict::Malformed, Verdict::No, false, true, 1},
  };
  for (const Case& k : cases) {
    truth::Turn t = enforceTurn("Logged: 150 g skyr.");
    t.now.push_back(eff("ledger_write", false, "the card write failed", truth::Kind::Ledger));
    Scripted s;
    s.answers = {{k.first, 300}, {k.second, 300}};
    Retry r;
    r.answer = "Retry text.";
    truth::Counters c;
    const truth::Outcome out = truth::check(t, s.fn(), r.fn(), c);
    TEST_ASSERT_EQUAL(k.held, out.held);
    TEST_ASSERT_EQUAL(k.status, !out.status.empty());
    TEST_ASSERT_EQUAL(k.probes, out.probes);
    TEST_ASSERT_EQUAL_size_t(k.probes, s.seen.size());
    TEST_ASSERT_TRUE(out.reply == (k.held ? r.answer : t.reply));
    if (k.held) {
      // The second probe reads the retry's reply.
      TEST_ASSERT_EQUAL_STRING(r.answer.c_str(), section(userOf(s.seen[1]), "reply").c_str());
    }
    const bool probedYes = k.status && (k.held ? k.second == Verdict::Yes : k.first == Verdict::Yes);
    TEST_ASSERT_EQUAL_UINT32(k.status && probedYes ? 1 : 0, c.receiptsProbed);
    TEST_ASSERT_EQUAL_UINT32(k.status && !probedYes ? 1 : 0, c.receiptsFixed);
  }
}

// Shadow: no hold ever; a failed write asks the probe now, for the receipt only.
void test_shadow_never_holds_and_probes_now_only_for_a_failed_write() {
  for (Verdict v : {Verdict::Yes, Verdict::No, Verdict::Timeout}) {
    truth::Turn t = enforceTurn("Logged: 150 g skyr.");
    t.mode = Truth::Shadow;
    t.now.push_back(eff("ledger_write", false, "busy", truth::Kind::Ledger));
    Scripted s;
    s.answers = {{v, 300}};
    Retry r;
    truth::Counters c;
    const truth::Outcome out = truth::check(t, s.fn(), r.fn(), c);
    TEST_ASSERT_FALSE(out.held);
    TEST_ASSERT_EQUAL_size_t(0, r.items.size());
    TEST_ASSERT_EQUAL_size_t(1, s.seen.size());
    TEST_ASSERT_FALSE(out.shadowDue);
    TEST_ASSERT_TRUE(out.reply == t.reply);
    TEST_ASSERT_EQUAL(v != Verdict::No, !out.status.empty());
  }
  truth::Turn t = enforceTurn("Logged: 150 g skyr.");
  t.mode = Truth::Shadow;
  Scripted s;
  s.answers = {{Verdict::Yes, 300}};
  Retry r;
  truth::Counters c;
  const truth::Outcome out = truth::check(t, s.fn(), r.fn(), c);
  TEST_ASSERT_EQUAL_size_t(0, s.seen.size());
  TEST_ASSERT_TRUE(out.shadowDue);
  TEST_ASSERT_FALSE(out.held);
  TEST_ASSERT_TRUE(out.status.empty());
  // The idle-step probe: counters and the window only.
  truth::HeadState h;
  h.mode = Truth::Shadow;
  truth::shadowProbe(t, s.fn(), h);
  TEST_ASSERT_EQUAL_size_t(1, s.seen.size());
  TEST_ASSERT_EQUAL_UINT32(1, h.c.shadowProbes);
  // A tainted turn is never probed, not even from the idle step.
  truth::Turn tainted = t;
  tainted.tainted = true;
  truth::shadowProbe(tainted, s.fn(), h);
  TEST_ASSERT_EQUAL_size_t(1, s.seen.size());
  TEST_ASSERT_EQUAL_UINT32(1, h.c.shadowHits);
  TEST_ASSERT_EQUAL_UINT32(1, h.windowHits());
  TEST_ASSERT_EQUAL_UINT32(0, h.c.hits);
  TEST_ASSERT_EQUAL_UINT32(0, h.c.holds);
  // A shadow probe that gets no answer counts as a failure, not as a probe outcome.
  Scripted bad;
  bad.answers = {{Verdict::Error, 0}};
  truth::shadowProbe(t, bad.fn(), h);
  TEST_ASSERT_EQUAL_UINT32(1, h.c.shadowProbes);
  TEST_ASSERT_EQUAL_UINT32(1, h.c.failError);
}

// ---- taint, fail-open, no word filter ---------------------------------------------------

void test_a_tainted_turn_never_probes() {
  truth::HeadState h;
  h.mode = Truth::Enforce;
  TEST_ASSERT_EQUAL((int)Truth::ReceiptOnly, (int)truth::turnMode(Truth::Enforce, h, true));
  TEST_ASSERT_EQUAL((int)Truth::Enforce, (int)truth::turnMode(Truth::Enforce, h, false));
  h.mode = Truth::Shadow;
  TEST_ASSERT_EQUAL((int)Truth::ReceiptOnly, (int)truth::turnMode(Truth::Enforce, h, true));
  TEST_ASSERT_EQUAL((int)Truth::Shadow, (int)truth::turnMode(Truth::Enforce, h, false));
  // A stored mode never lifts a receipt-only start.
  TEST_ASSERT_EQUAL((int)Truth::ReceiptOnly, (int)truth::turnMode(Truth::ReceiptOnly, h, false));
  h.mode = Truth::ReceiptOnly;
  TEST_ASSERT_EQUAL((int)Truth::ReceiptOnly, (int)truth::turnMode(Truth::Enforce, h, false));
  for (bool failed : {false, true}) {
    truth::Turn t = enforceTurn("Saved the train times as a note.");
    t.tainted = true;
    t.mode = truth::turnMode(Truth::Enforce, truth::HeadState{}, true);
    if (failed) t.now.push_back(eff("note_save", false, "pending: tainted turn", truth::Kind::Memory));
    Scripted s;
    s.answers = {{Verdict::Yes, 100}};
    Retry r;
    truth::Counters c;
    const truth::Outcome out = truth::check(t, s.fn(), r.fn(), c);
    TEST_ASSERT_EQUAL_size_t(0, s.seen.size());
    TEST_ASSERT_FALSE(out.held);
    TEST_ASSERT_EQUAL(failed, !out.status.empty());
    TEST_ASSERT_EQUAL_UINT32(1, c.tainted);
  }
  // Even a turn marked enforce by mistake does not probe while tainted.
  truth::Turn t = enforceTurn("Saved.");
  t.tainted = true;
  Scripted s;
  truth::Counters c;
  truth::check(t, s.fn(), Retry().fn(), c);
  TEST_ASSERT_EQUAL_size_t(0, s.seen.size());
}

void test_the_probe_fails_open_at_8_s_with_a_counter() {
  struct Case {
    truth::ProbeResult r;
    bool held;
  } cases[] = {
      {{Verdict::Yes, 8000}, true},       // on the bound: an answer
      {{Verdict::Yes, 8001}, false},      // past it: no answer, whatever it said
      {{Verdict::Yes, 60000}, false},
      {{Verdict::Timeout, 8000}, false},
      {{Verdict::Error, 10}, false},
      {{Verdict::Malformed, 10}, false},
  };
  for (const Case& k : cases) {
    truth::Turn t = enforceTurn("Logged.");
    Scripted s;
    s.answers = {k.r};
    Retry r;
    r.answer = "R";
    truth::Counters c;
    const truth::Outcome out = truth::check(t, s.fn(), r.fn(), c);
    TEST_ASSERT_EQUAL(k.held, out.held);
    TEST_ASSERT_EQUAL_UINT32(8000, s.seen[0].timeoutMs);
    if (!k.held) {
      TEST_ASSERT_TRUE(out.reply == t.reply);
      TEST_ASSERT_TRUE(out.status.empty());
      const uint32_t fails = c.failTimeout + c.failError + c.failMalformed;
      TEST_ASSERT_EQUAL_UINT32(1, fails);
      TEST_ASSERT_EQUAL_UINT32(k.r.verdict == Verdict::Error ? 1 : 0, c.failError);
      TEST_ASSERT_EQUAL_UINT32(k.r.verdict == Verdict::Malformed ? 1 : 0, c.failMalformed);
      TEST_ASSERT_EQUAL_UINT32(k.r.verdict == Verdict::Yes || k.r.verdict == Verdict::Timeout ? 1 : 0, c.failTimeout);
      TEST_ASSERT_EQUAL_UINT32(0, c.hits);
    }
  }
  // No probe available in an enforce turn: delivered, counted as an error.
  truth::Turn t = enforceTurn("Logged.");
  truth::Counters c;
  const truth::Outcome out = truth::check(t, truth::ProbeFn(), Retry().fn(), c);
  TEST_ASSERT_TRUE(out.reply == t.reply);
  TEST_ASSERT_EQUAL_UINT32(1, c.failError);
  TEST_ASSERT_EQUAL(0, out.probes);
  // A head with no probe wire is not asked either.
  t.host = "openai";
  Scripted s;
  truth::Counters c2;
  truth::check(t, s.fn(), Retry().fn(), c2);
  TEST_ASSERT_EQUAL_size_t(0, s.seen.size());
  TEST_ASSERT_EQUAL_UINT32(1, c2.failError);
}

// No word filter: an acknowledgement is probed like any reply (ruling 9 leaves acknowledgements
// out of the precision gate, not out of the check); an empty reply has nothing to check.
void test_no_word_filter_decides_what_is_probed() {
  for (const char* reply : {"Noted.", "Got it.", "ok", "Saved!", "I'll remember that.", "?"}) {
    truth::Turn t = enforceTurn(reply);
    Scripted s;
    truth::Counters c;
    truth::check(t, s.fn(), Retry().fn(), c);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(1, s.seen.size(), reply);
  }
  truth::Turn t = enforceTurn("");
  Scripted s;
  truth::Counters c;
  const truth::Outcome out = truth::check(t, s.fn(), Retry().fn(), c);
  TEST_ASSERT_EQUAL_size_t(0, s.seen.size());
  TEST_ASSERT_TRUE(out.reply.empty());
}

// The counters of the enforce-to-shadow rate: answered enforce probes on effect-shaped turns.
void test_effect_shaped_turns_are_counted() {
  truth::Counters c;
  Scripted s;
  s.answers = {{Verdict::No, 1}, {Verdict::No, 1}, {Verdict::Yes, 1}, {Verdict::Timeout, 1}};
  truth::Turn plain = enforceTurn("Hello there.");
  truth::check(plain, s.fn(), Retry().fn(), c);            // no effect, no hit: not effect-shaped
  truth::Turn acted = enforceTurn("Logged.");
  acted.now.push_back(eff("ledger_write", true, "added L1", truth::Kind::Ledger));
  truth::check(acted, s.fn(), Retry().fn(), c);            // an effect: counted, no hit
  truth::check(plain, s.fn(), Retry().fn(), c);            // a hit: counted
  truth::check(acted, s.fn(), Retry().fn(), c);            // no answer: not counted
  TEST_ASSERT_EQUAL_UINT32(2, c.effectTurns);
  TEST_ASSERT_EQUAL_UINT32(1, c.effectHits);
  TEST_ASSERT_EQUAL_UINT32(4, c.turns);
  TEST_ASSERT_EQUAL_UINT32(4, c.probes);
}

// ---- the old engine's two-step check (TurnCheck through a binding) ---------------------

namespace {
struct FakeBinding : truth::Binding {
  truth::Counters c;
  Scripted* s = nullptr;
  bool ringKnown = true, refuse = false;
  int opens = 0, closes = 0;
  std::string host, model;
  bool tainted = false;
  truth::Counters* open(const std::string&, const std::string& h, const std::string& m, bool t, truth::Turn& turn,
                        truth::ProbeFn& probe) override {
    ++opens;
    host = h, model = m, tainted = t;
    if (refuse) return nullptr;
    truth::HeadState hs;
    hs.mode = Truth::Enforce;
    turn.mode = truth::turnMode(truth::startMode(h.c_str(), m.c_str(), hq::Line::E1), hs, t);
    turn.ringKnown = ringKnown;
    turn.nowEpoch = kNow;
    turn.recent.push_back(entry(kNow - 600, "ledger_write", "added L00A1 skyr 150 g"));
    if (s) probe = s->fn();
    return &c;
  }
  void close() override { ++closes; }
};
}  // namespace

void test_the_engine_flow_holds_once_and_decides_the_line_at_the_end() {
  Scripted s;
  s.answers = {{Verdict::Yes, 300}, {Verdict::Yes, 300}};
  FakeBinding b;
  b.s = &s;
  truth::TurnCheck k;
  k.reset(&b, "111", "log my skyr");
  k.note(eff("ledger_write", false, "busy", truth::Kind::Ledger));   // before the check opens
  TEST_ASSERT_TRUE(k.open("anthropic", "claude-sonnet-5-5", false));
  TEST_ASSERT_TRUE(k.open("openai", "x", true));   // opens once: the first head stays
  TEST_ASSERT_EQUAL(1, b.opens);
  const std::string item = k.atFinal("Logged.");
  TEST_ASSERT_EQUAL_STRING(
      "[device] effects this turn: ledger_write FAILED: busy. Recent: 12:52 ledger_write: added L00A1 skyr 150 g.",
      item.c_str());
  TEST_ASSERT_EQUAL_STRING("", k.atFinal("Logged again.").c_str());   // one hold a turn
  k.note(eff("ledger_write", false, "busy again", truth::Kind::Ledger));   // the round's own write
  const truth::Outcome o = k.finish("Logged now.");
  TEST_ASSERT_TRUE(o.held && o.retried);
  TEST_ASSERT_EQUAL_STRING("Logged now.", o.reply.c_str());
  TEST_ASSERT_EQUAL_STRING("Note: the ledger change in this reply did not go through (busy again)", o.status.c_str());
  TEST_ASSERT_EQUAL_size_t(2, s.seen.size());
  TEST_ASSERT_EQUAL(1, b.closes);
  TEST_ASSERT_EQUAL_UINT32(1, b.c.turns);
  TEST_ASSERT_EQUAL_UINT32(1, b.c.holds);
  TEST_ASSERT_FALSE(k.active());
}

void test_the_engine_flow_without_a_round_never_holds() {
  // The loop never asked (a single shot, or another check used the one round).
  Scripted s;
  s.answers = {{Verdict::Yes, 300}};
  FakeBinding b;
  b.s = &s;
  truth::TurnCheck k;
  k.reset(&b, "111", "o");
  k.note(eff("files_write", false, "disk full", truth::Kind::File));
  k.open("anthropic", "claude-sonnet-5-5", false);
  const truth::Outcome o = k.finish("Saved it.");
  TEST_ASSERT_FALSE(o.held);
  TEST_ASSERT_EQUAL_STRING("Saved it.", o.reply.c_str());
  TEST_ASSERT_EQUAL_STRING("Note: the file change in this reply did not go through (disk full)", o.status.c_str());
  // A held turn whose round came back blank delivers the first reply.
  Scripted s2;
  s2.answers = {{Verdict::Yes, 300}};
  b.s = &s2;
  k.reset(&b, "111", "o");
  k.open("anthropic", "claude-sonnet-5-5", false);
  TEST_ASSERT_FALSE(k.atFinal("Saved.").empty());
  const truth::Outcome o2 = k.finish("  ");
  TEST_ASSERT_EQUAL_STRING("Saved.", o2.reply.c_str());
  TEST_ASSERT_EQUAL_UINT32(1, b.c.retryFailed);
}

// The loop never asked: once the reply is out only the receipt question is worth a probe.
void test_finish_without_an_ask_probes_only_for_a_failed_write() {
  Scripted s;
  s.answers = {{Verdict::Yes, 300}};
  FakeBinding b;
  b.s = &s;
  truth::TurnCheck k;
  k.reset(&b, "111", "o");
  k.open("anthropic", "claude-sonnet-5-5", false);
  const truth::Outcome o = k.finish("Logged.");
  TEST_ASSERT_EQUAL_size_t(0, s.seen.size());
  TEST_ASSERT_TRUE(o.status.empty());
  TEST_ASSERT_EQUAL_UINT32(1, b.c.turns);
  // Tainted after it opened (a held round read outside content): no probe at the end either.
  Scripted s2;
  s2.answers = {{Verdict::Yes, 300}, {Verdict::Yes, 300}};
  b.s = &s2;
  k.reset(&b, "111", "o");
  k.open("anthropic", "claude-sonnet-5-5", false);
  TEST_ASSERT_FALSE(k.atFinal("Logged.").empty());
  k.note(eff("ledger_write", false, "busy", truth::Kind::Ledger));
  k.taint(true);
  const truth::Outcome o2 = k.finish("Logged now.");
  TEST_ASSERT_EQUAL_size_t(1, s2.seen.size());
  TEST_ASSERT_FALSE(o2.status.empty());   // receipt-only now: the line is deterministic
}

// No binding, a refused open, a tainted turn or an unknown ring: never a probe.
void test_the_engine_flow_without_a_known_ring_never_probes() {
  for (int which = 0; which < 4; ++which) {
    Scripted s;
    s.answers = {{Verdict::Yes, 300}};
    FakeBinding b;
    b.s = &s;
    b.ringKnown = which != 2;
    b.refuse = which == 1;
    truth::TurnCheck k;
    k.reset(which == 0 ? nullptr : &b, "111", "o");
    k.note(eff("ledger_write", false, "busy", truth::Kind::Ledger));
    k.open("anthropic", "claude-sonnet-5-5", which == 3);
    TEST_ASSERT_EQUAL_STRING("", k.atFinal("Logged.").c_str());
    const truth::Outcome o = k.finish("Logged.");
    TEST_ASSERT_EQUAL_size_t(0, s.seen.size());
    TEST_ASSERT_EQUAL_STRING("Logged.", o.reply.c_str());
    // A refused or missing binding sends nothing; a known receipt-only turn sends the fixed line.
    TEST_ASSERT_EQUAL(which >= 2, !o.status.empty());
  }
  // The library's own check: an unknown ring is receipt-only too.
  truth::Turn t = enforceTurn("Logged.");
  t.ringKnown = false;
  Scripted s;
  truth::Counters c;
  truth::check(t, s.fn(), Retry().fn(), c);
  TEST_ASSERT_EQUAL_size_t(0, s.seen.size());
  truth::HeadState h;
  truth::shadowProbe(t, s.fn(), h);
  TEST_ASSERT_EQUAL_size_t(0, s.seen.size());
}

// Every effect class of the old engine has its kind: writes are writes, lookups and speech
// store nothing; a ledger tool is a ledger change whatever class the table gives it.
void test_every_old_engine_effect_class_has_its_kind() {
  using F = nimbus::orch::FxKind;
  using K = truth::Kind;
  std::map<int, K> want = {
      {(int)F::MemWrite, K::Memory}, {(int)F::MemUpdate, K::Memory}, {(int)F::MemPin, K::Memory},
      {(int)F::MemPinNoop, K::Read}, {(int)F::MemDelete, K::Memory}, {(int)F::Scratch, K::Memory},
      {(int)F::ScratchRamOnly, K::Memory}, {(int)F::FileWrite, K::File}, {(int)F::SkillSave, K::Memory},
      {(int)F::Spawn, K::Other}, {(int)F::SpawnRefused, K::Other}, {(int)F::Send, K::Send},
      {(int)F::Speak, K::Read}, {(int)F::Schedule, K::Routine}, {(int)F::Config, K::Setting},
      {(int)F::Read, K::Read}, {(int)F::SubSourced, K::Read}, {(int)F::SubUnsourced, K::Read},
      {(int)F::FileReplace, K::File}};
  TEST_ASSERT_EQUAL_size_t((size_t)F::kCount, want.size());   // a new class needs its row here
  for (int f = 0; f < (int)F::kCount; ++f)
    TEST_ASSERT_EQUAL_MESSAGE((int)want[f], (int)truth::kindOf("files_write", (F)f), nimbus::orch::fxSlug((F)f));
  TEST_ASSERT_EQUAL((int)K::Ledger, (int)truth::kindOf("ledger_write", F::FileWrite));
  TEST_ASSERT_EQUAL((int)K::Read, (int)truth::kindOf("ledger_day", F::Read));
  const truth::TurnEffect e = truth::effectOf("files_write", F::FileWrite, true, "disk full\nsecond line");
  TEST_ASSERT_FALSE(e.ok);
  TEST_ASSERT_EQUAL_STRING("disk full", e.receipt.c_str());
  TEST_ASSERT_EQUAL((int)K::File, (int)e.kind);
}

// ---- modes and the stored state ---------------------------------------------------------

// The device's table is the Clopper-Pearson bound itself: each entry is the first n (at least
// 300) where P(X <= k | n, 1%) <= 5%.
static double binomCdf(uint32_t k, uint32_t n) {
  double lt = n * std::log1p(-0.01), sum = 0;
  for (uint32_t i = 0; i <= k; ++i) {
    sum += std::exp(lt);
    lt += std::log((double)(n - i) / (double)(i + 1)) + std::log(0.01) - std::log1p(-0.01);
  }
  return sum;
}

void test_the_shadow_table_is_the_bound() {
  for (uint32_t k = 0; k <= 50; ++k) {
    const uint32_t n = truth::shadowTurnsFor(k);
    TEST_ASSERT_TRUE(n >= 300);
    TEST_ASSERT_TRUE(binomCdf(k, n) <= 0.05);
    TEST_ASSERT_TRUE(n == 300 || binomCdf(k, n - 1) > 0.05);
  }
  TEST_ASSERT_EQUAL_UINT32(0, truth::shadowTurnsFor(51));
}

void test_modes_move_only_as_the_prd_says() {
  struct Case {
    uint32_t hits, turns;
    bool moves;
  } cases[] = {{0, 299, false}, {0, 300, true},  {1, 472, false}, {1, 475, true},  {2, 627, false},
               {2, 630, true},  {3, 772, false}, {3, 773, true},  {50, 3000, false}, {30, 5000, true},
               {0, 0, false}};
  for (const Case& k : cases) {
    truth::HeadState h;
    h.mode = Truth::Enforce;
    h.c.effectTurns = k.turns;
    h.c.effectHits = k.hits;
    TEST_ASSERT_FALSE(truth::nextMode(h, false));   // v5.2.0: never on its own
    TEST_ASSERT_EQUAL((int)Truth::Enforce, (int)h.mode);
    TEST_ASSERT_EQUAL(k.moves, truth::nextMode(h, true));
    TEST_ASSERT_EQUAL((int)(k.moves ? Truth::Shadow : Truth::Enforce), (int)h.mode);
  }
  // Shadow back to enforce at 4 hits in the trailing 200 shadow probes; older hits roll out.
  truth::HeadState h;
  h.mode = Truth::Shadow;
  h.c.effectTurns = 1000;   // what earned shadow: forgotten on the way back
  for (int i = 0; i < 3; ++i) h.pushShadow(true);
  for (int i = 0; i < 197; ++i) h.pushShadow(false);
  TEST_ASSERT_FALSE(truth::nextMode(h, true));
  h.pushShadow(false);   // the first hit rolls out
  TEST_ASSERT_EQUAL_UINT32(2, h.windowHits());
  h.pushShadow(true);   // these two overwrite the two old hits: still 2
  h.pushShadow(true);
  TEST_ASSERT_EQUAL_UINT32(2, h.windowHits());
  TEST_ASSERT_FALSE(truth::nextMode(h, true));
  h.pushShadow(true);
  h.pushShadow(true);
  TEST_ASSERT_EQUAL_UINT32(4, h.windowHits());
  TEST_ASSERT_TRUE(truth::nextMode(h, true));
  TEST_ASSERT_EQUAL((int)Truth::Enforce, (int)h.mode);
  // Back in enforce, shadow has to be earned again from zero.
  TEST_ASSERT_EQUAL_UINT32(0, h.c.effectTurns);
  TEST_ASSERT_EQUAL_UINT32(0, h.windowHits());
  TEST_ASSERT_FALSE(truth::nextMode(h, true));
  // Receipt-only never moves.
  truth::HeadState r;
  r.mode = Truth::ReceiptOnly;
  r.c.effectTurns = 10000;
  TEST_ASSERT_FALSE(truth::nextMode(r, true));
  TEST_ASSERT_EQUAL((int)Truth::ReceiptOnly, (int)r.mode);
}

void test_the_state_file_round_trips_and_refuses_damage() {
  truth::State s;
  truth::HeadState& a = s.head(truth::stateKey("anthropic", "claude-sonnet-5-5", hq::Line::E1), Truth::Enforce);
  a.c.turns = 7;
  a.c.holds = 2;
  a.c.failTimeout = 1;
  a.c.effectTurns = 301;
  a.pushShadow(true);
  a.pushShadow(false);
  TEST_ASSERT_EQUAL_STRING("anthropic/claude-sonnet-5-5/e1", a.key.c_str());
  const std::string aKey = a.key;
  truth::HeadState& b = s.head(truth::stateKey("openai", "gpt-5.6-luna", hq::Line::E2), Truth::ReceiptOnly);
  b.c.receiptsFixed = 3;   // `a` may have moved: only `b` is used from here
  for (int i = 0; i < 450; ++i) b.pushShadow(i % 100 == 0);   // the window wraps: still 200 long
  const std::string json = truth::renderState(s);
  truth::State back;
  TEST_ASSERT_TRUE(truth::parseState(json, back));
  TEST_ASSERT_EQUAL_size_t(2, back.heads.size());
  TEST_ASSERT_EQUAL_STRING(json.c_str(), truth::renderState(back).c_str());
  TEST_ASSERT_EQUAL_UINT32(7, back.heads[0].c.turns);
  TEST_ASSERT_EQUAL_UINT32(1, back.heads[0].windowHits());
  TEST_ASSERT_EQUAL((int)Truth::Enforce, (int)back.heads[0].mode);
  TEST_ASSERT_EQUAL_UINT32(3, back.heads[1].c.receiptsFixed);
  TEST_ASSERT_EQUAL_UINT32(200, back.heads[1].winLen);
  TEST_ASSERT_EQUAL_UINT32(2, back.heads[1].windowHits());
  // An existing head is found, not made again.
  TEST_ASSERT_EQUAL_UINT32(7, back.head(aKey, Truth::Enforce).c.turns);
  TEST_ASSERT_EQUAL_size_t(2, back.heads.size());
  // Damage of any kind: refused, nothing kept.
  for (const std::string& bad : {std::string(""), std::string("{"), std::string("[]"), std::string("{\"v\":2,\"heads\":[]}"),
                                 json.substr(0, json.size() / 2),
                                 std::string("{\"v\":1,\"heads\":[{\"k\":\"a\",\"m\":\"loose\"}]}"),
                                 std::string("{\"v\":1,\"heads\":[{\"m\":\"enforce\"}]}"),
                                 json.substr(0, json.find("\"n\":") + 4) + "201" +
                                     json.substr(json.find(',', json.find("\"n\":")))}) {
    truth::State out;
    out.head("x", Truth::Enforce);
    TEST_ASSERT_FALSE_MESSAGE(truth::parseState(bad, out), bad.c_str());
    TEST_ASSERT_EQUAL_size_t(0, out.heads.size());
  }
  // The table decides what a stored mode may be: a receipt-only start forces receipt-only, and a
  // head that starts measured again leaves a stored receipt-only behind.
  truth::State t;
  t.head("k", Truth::Enforce).mode = Truth::Shadow;
  TEST_ASSERT_EQUAL((int)Truth::ReceiptOnly, (int)t.head("k", Truth::ReceiptOnly).mode);
  TEST_ASSERT_EQUAL((int)Truth::Enforce, (int)t.head("k", Truth::Enforce).mode);
  t.head("k", Truth::Enforce).mode = Truth::Shadow;
  TEST_ASSERT_EQUAL((int)Truth::Shadow, (int)t.head("k", Truth::Enforce).mode);
  // Bounded: the least recently used head is dropped past kMaxHeads.
  truth::State m;
  for (size_t i = 0; i < truth::kMaxHeads + 3; ++i) m.head("h" + std::to_string(i), Truth::Enforce);
  TEST_ASSERT_EQUAL_size_t(truth::kMaxHeads, m.heads.size());
  m.head("h3", Truth::Enforce);   // used again: kept
  m.head("new", Truth::Enforce);
  bool h3 = false, h4 = false;
  for (const auto& h : m.heads) {
    h3 |= h.key == "h3";
    h4 |= h.key == "h4";
  }
  TEST_ASSERT_TRUE(h3);
  TEST_ASSERT_FALSE(h4);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_prompt_is_the_measured_one_byte_for_byte);
  RUN_TEST(test_the_measured_table_is_probe_json);
  RUN_TEST(test_a_head_without_a_passing_row_is_receipt_only);
  RUN_TEST(test_receipt_only_never_probes);
  RUN_TEST(test_the_user_message_is_the_measured_render);
  RUN_TEST(test_untrusted_text_cannot_open_a_section);
  RUN_TEST(test_times_as_the_probe_sees_them);
  RUN_TEST(test_the_request_never_carries_a_photo);
  RUN_TEST(test_owner_and_reply_are_bounded);
  RUN_TEST(test_answers_are_read_as_measured);
  RUN_TEST(test_a_true_statement_about_an_effect_in_the_ring_is_never_a_hit);
  RUN_TEST(test_an_unbacked_success_is_held_once_and_the_retry_is_delivered_unchanged);
  RUN_TEST(test_a_failed_or_empty_retry_delivers_the_first_reply);
  RUN_TEST(test_the_retry_rounds_own_effects_count);
  RUN_TEST(test_a_throwing_probe_or_retry_fails_open);
  RUN_TEST(test_the_retry_item_holds_facts_only);
  RUN_TEST(test_the_retry_item_is_bounded_and_keeps_this_turn_whole);
  RUN_TEST(test_the_hold_sends_the_facts_item_of_this_turn);
  RUN_TEST(test_the_receipt_names_each_kind_whose_last_attempt_failed);
  RUN_TEST(test_the_receipt_follows_only_a_probe_yes_in_enforce);
  RUN_TEST(test_shadow_never_holds_and_probes_now_only_for_a_failed_write);
  RUN_TEST(test_a_tainted_turn_never_probes);
  RUN_TEST(test_the_probe_fails_open_at_8_s_with_a_counter);
  RUN_TEST(test_no_word_filter_decides_what_is_probed);
  RUN_TEST(test_effect_shaped_turns_are_counted);
  RUN_TEST(test_the_engine_flow_holds_once_and_decides_the_line_at_the_end);
  RUN_TEST(test_the_engine_flow_without_a_round_never_holds);
  RUN_TEST(test_the_engine_flow_without_a_known_ring_never_probes);
  RUN_TEST(test_finish_without_an_ask_probes_only_for_a_failed_write);
  RUN_TEST(test_every_old_engine_effect_class_has_its_kind);
  RUN_TEST(test_the_shadow_table_is_the_bound);
  RUN_TEST(test_modes_move_only_as_the_prd_says);
  RUN_TEST(test_the_state_file_round_trips_and_refuses_damage);
  return UNITY_END();
}
