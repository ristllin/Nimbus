// The conversation migration (lane RB-MG1a, PRD section 9 "v6 stores"): the old store's rows
// become transcript items behind one honest first digest. Properties over whole classes: every
// cut point (a power cut after each single write, whole and torn, and before each rename),
// every refused write, every refused allocation, every digest-0 failure kind. The one
// invariant (world.h expectAccounted): every conversation row is in the new store exactly
// once, in order, covered by digest 0 or its omission line, named by a note, or imported as
// a text item; the old store is never written.
#include <ArduinoJson.h>
#include <unity.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <new>

#include "../../lib/harness/src/providers/plain_body.h"
#include "../support/scripted_provider/scripted_provider.h"
#include "nimbus/harness/plain/compact.h"
#include "nimbus/harness/plain/texts.h"
#include "nimbus/harness/platform.h"
#include "world.h"

namespace sp = harness_test::scripted;
namespace plain = agent::plain;
namespace body = agent::plain::body;
using namespace mg;

// ---- a counting, refusing operator new (ruling 30) -------------------------------------------
namespace {
long gRefuseAt = -1;   // the allocation with this index throws (counted while >= 0)
long gAllocs = 0;
size_t gLive = 0, gPeak = 0;
}  // namespace
void* operator new(size_t n) {
  if (gRefuseAt >= 0 && gAllocs++ == gRefuseAt) throw std::bad_alloc();
  void* p = std::malloc(n + 16);
  if (!p) throw std::bad_alloc();
  *(size_t*)p = n;
  gLive += n;
  gPeak = std::max(gPeak, gLive);
  return (char*)p + 16;
}
void operator delete(void* p) noexcept {
  if (!p) return;
  char* b = (char*)p - 16;
  gLive -= *(size_t*)b;
  std::free(b);
}
void operator delete(void* p, size_t) noexcept { operator delete(p); }

void setUp() {}
void tearDown() {}

namespace {

std::string dayString(uint64_t t) { return dayOf(t - 4 * kHour); }   // day start 04:00, UTC

// A digest-0 answer of `text` (the driver's digest text from it), counting the asks.
struct Answers {
  std::vector<std::string> texts;   // answer i for ask i; past the end: the last
  std::vector<bool> fail;           // ask i is not answered
  int asks = 0;
  tx::Digest0Fn fn() {
    return [this](const tx::Digest0Ask& a, tx::Digest0Answer& out) {
      const size_t i = (size_t)asks++;
      if (i < fail.size() && fail[i]) return tx::Status::fail(tx::Err::Io, "test answer");
      out.summary = texts.empty() ? std::string() : texts[std::min(i, texts.size() - 1)];
      out.day = dayString(a.at);
      return tx::digestText(out.day, out.summary, out.text);
    };
  }
};

const char* kGood =
    "The person planned the mooring repair and the piano lessons, asked for short answers and kept a list of "
    "weekend walks; the assistant answered and kept the notes. The river trip is still open.";

struct World {
  txb::MemFiles files;
  OldStore old{files};
  std::unique_ptr<StoreSource> src;
  Month m;
  Answers answers;
  std::map<std::string, std::string> exported;
  bool refuseExport = false;
  size_t budget = tx::kImportBudgetBytes;
  const tx::TxArena* arena = nullptr;

  World() { src.reset(new StoreSource(old, kChat)); }
  tx::MigrateDeps deps() {
    tx::MigrateDeps d;
    d.files = &files;
    d.chatKey = kChatKey;
    d.rows = src.get();
    d.fold = old.oldFold();
    d.digest0 = answers.fn();
    d.exportText = [this](const std::string& p, const std::string& t) {
      if (refuseExport) return false;
      exported[p] = t;
      return true;
    };
    d.now = kMonthStart + 40 * kDay;
    d.importBudget = budget;
    d.arena = arena;
    return d;
  }
  tx::Status run(tx::MigrationOutcome& out) { return tx::migrate(deps(), out); }
  tx::MigrationOutcome done(const char* when) {
    tx::MigrationOutcome out;
    const tx::Status s = run(out);
    char msg[200];
    std::snprintf(msg, sizeof msg, "%s: %s at %s", when, tx::errSlug(s.err), s.where);
    TEST_ASSERT_TRUE_MESSAGE(s.ok(), msg);
    TEST_ASSERT_EQUAL_MESSAGE((int)tx::MigrationStep::Done, (int)out.step, when);
    return out;
  }
  std::vector<tx::Item> items() { return stored(files); }
  std::vector<tx::Item> digests() {
    std::vector<tx::Item> out;
    for (const tx::Item& it : items())
      if (it.kind == tx::ItemKind::Digest) out.push_back(it);
    return out;
  }
};

bool sameItems(const std::vector<tx::Item>& a, const std::vector<tx::Item>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    std::string x, y;
    tx::Item p = a[i], q = b[i];
    if (!tx::encodeLine(p, x).ok() || !tx::encodeLine(q, y).ok() || x != y) return false;
  }
  return true;
}

// ---- pure rules -------------------------------------------------------------------------------

void test_figures_leave_out_dates_and_keep_quantities() {
  using V = std::vector<std::string>;
  TEST_ASSERT_TRUE(tx::figuresOf("ate 540 kcal and 12.5 g, total 1,450") == (V{"540", "12.5", "1450"}));
  TEST_ASSERT_TRUE(tx::figuresOf("on 17 Nov 2026 at 13:02, Nov 3rd, 2026-11-17, 4th of July") == V{});
  TEST_ASSERT_TRUE(tx::figuresOf("3 meals, 7 days") == V{});   // one digit is not a figure
  TEST_ASSERT_TRUE(tx::figuresOf("150g skyr, 150 g again") == (V{"150"}));
  TEST_ASSERT_TRUE(tx::figuresOf("the 2026 plan cost 2026.5") == (V{"2026.5"}));
  TEST_ASSERT_TRUE(tx::figuresOf("") == V{});
}

void test_path_like_tokens_are_found_and_prose_is_not() {
  for (const char* p : {"see notes/boat.md", "the file boat.md", "/data/x", "C:\\tmp", "https://example.test/a",
                        "(food.csv)", "photo.jpeg.", "logs/2026"})
    TEST_ASSERT_TRUE_MESSAGE(tx::hasPathLikeToken(p), p);
  for (const char* p : {"half 1/2 a cup", "e.g. a walk", "end of it. Next day", "a.m. and p.m.", "U.S. trip", "",
                        "it costs 3.50 now", "mid-day. Then"})
    TEST_ASSERT_FALSE_MESSAGE(tx::hasPathLikeToken(p), p);
  TEST_ASSERT_TRUE(tx::hasPathLikeToken("and/or"));   // a slash between words reads as a path: refused, safe side
}

void test_digest0_rule_is_768_bytes_per_day_no_path_no_tool_figure() {
  const auto none = [](const std::string&) { return false; };
  TEST_ASSERT_EQUAL(0, (int)tx::checkDigest0(std::string(768, 'a'), 1, none));
  TEST_ASSERT_EQUAL((int)tx::Digest0Verdict::Size, (int)tx::checkDigest0(std::string(769, 'a'), 1, none));
  TEST_ASSERT_EQUAL(0, (int)tx::checkDigest0(std::string(768 * 3, 'a'), 3, none));
  TEST_ASSERT_EQUAL((int)tx::Digest0Verdict::Size, (int)tx::checkDigest0(std::string(768 * 3 + 1, 'a'), 3, none));
  TEST_ASSERT_EQUAL((int)tx::Digest0Verdict::Size, (int)tx::checkDigest0(std::string(769, 'a'), 0, none));
  TEST_ASSERT_EQUAL((int)tx::Digest0Verdict::Path, (int)tx::checkDigest0("kept in notes/boat.md", 1, none));
  const auto tool = [](const std::string& f) { return f == "540"; };
  TEST_ASSERT_EQUAL((int)tx::Digest0Verdict::Figure, (int)tx::checkDigest0("lunch was 540 kcal", 1, tool));
  TEST_ASSERT_EQUAL(0, (int)tx::checkDigest0("lunch was 541 kcal", 1, tool));
  TEST_ASSERT_EQUAL(0, (int)tx::checkDigest0("on 17 Nov it was fine", 1, [](const std::string&) { return true; }));
  for (int v = 0; v <= (int)tx::Digest0Verdict::Text; ++v)
    TEST_ASSERT_TRUE(std::string(tx::digest0VerdictSlug((tx::Digest0Verdict)v)).find_first_not_of("abcdefghijklmnopqrstuvwxyz-") == std::string::npos);
}

void test_the_note_names_turns_and_omitted_rows() {
  tx::ImportPlan p;
  p.turns = 3;
  p.firstT = kMonthStart + 9 * kHour;
  p.lastT = kMonthStart + kDay + 13 * kHour;
  TEST_ASSERT_EQUAL_STRING(
      "[device] 3 turns between Sat 17 Oct 2026 09:00 and Sun 18 Oct 2026 13:00 were answered by the earlier "
      "engine; their tool work is not shown.",
      tx::importNote(p).c_str());
  p.named = 12;
  TEST_ASSERT_EQUAL_STRING(
      "[device] 3 turns between Sat 17 Oct 2026 09:00 and Sun 18 Oct 2026 13:00 were answered by the earlier "
      "engine; their tool work is not shown. The 12 earlier messages before Sat 17 Oct 2026 09:00 are not shown: "
      "search finds them.",
      tx::importNote(p).c_str());
  p.turns = 1;
  p.named = 1;
  p.firstT = p.lastT = 0;
  TEST_ASSERT_EQUAL_STRING(
      "[device] 1 turn was answered by the earlier engine; their tool work is not shown. The 1 earlier message is "
      "not shown: search finds them.",
      tx::importNote(p).c_str());
}

// ---- a month ----------------------------------------------------------------------------------

void expectDigest0(World& w, bool accepted, const char* when) {
  const std::vector<tx::Item> items = w.items();
  TEST_ASSERT_TRUE_MESSAGE(!items.empty(), when);
  const tx::Item& anchor = items[0];
  TEST_ASSERT_EQUAL_MESSAGE((int)tx::ItemKind::DeviceNote, (int)anchor.kind, when);
  TEST_ASSERT_TRUE_MESSAGE(anchor.x.find("was kept by the earlier engine") != std::string::npos, when);
  const std::vector<tx::Item> ds = w.digests();
  if (!accepted) {
    TEST_ASSERT_EQUAL_MESSAGE(0, ds.size(), when);
    TEST_ASSERT_EQUAL_MESSAGE((int)tx::NoteClass::Omit, (int)anchor.note, when);
    return;
  }
  TEST_ASSERT_EQUAL_MESSAGE(1, ds.size(), when);
  TEST_ASSERT_EQUAL_UINT64_MESSAGE(anchor.q, ds[0].thru, when);
  TEST_ASSERT_EQUAL_UINT64_MESSAGE(anchor.q + 1, ds[0].q, when);
  tx::TxLog log(w.files, kChatKey);
  tx::HeadState head;
  TEST_ASSERT_TRUE(log.open(head).ok());
  TEST_ASSERT_EQUAL_UINT64_MESSAGE(anchor.q, head.thru, when);
  std::vector<tx::DigestEntry> live;
  TEST_ASSERT_TRUE(tx::loadDigests(w.files, kChatKey, head, live).ok());
  TEST_ASSERT_EQUAL_MESSAGE(1, live.size(), when);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(ds[0].x.c_str(), live[0].text.c_str(), when);
}

void test_a_month_migrates_every_row_once() {
  World w;
  w.m = month(w.old, 7, 6000, 31, 18, 27, 3500);
  w.answers.texts = {kGood};
  const auto before = w.old.snapshot();
  gPeak = gLive;
  const size_t base = gLive;
  const auto t0 = std::chrono::steady_clock::now();
  const tx::MigrationOutcome out = w.done("month");
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const size_t rowsRead = w.src->rowsRead;
  std::printf("MIGRATION-6000: rows=%u read=%u ms=%.1f peak_heap_bytes=%u items=%u imported=%u named=%u turns=%u\n",
              (unsigned)w.old.rows.size(), (unsigned)rowsRead, ms, (unsigned)(gPeak - base), out.items, out.imported,
              out.named, out.turns);
  TEST_ASSERT_TRUE(w.old.rows.size() >= 5800 && w.old.rows.size() <= 6400);
  TEST_ASSERT_TRUE(out.digest0);
  TEST_ASSERT_EQUAL(1, w.answers.asks);
  TEST_ASSERT_FALSE(out.reentry);
  TEST_ASSERT_TRUE(out.named > 0);   // rows past the budget are named, the newest imported
  TEST_ASSERT_TRUE(gPeak - base < 256 * 1024);   // pages, not the store
  expectDigest0(w, true, "month");
  expectAccounted(w.old, w.files, w.m.thru, out.hi, "month");
  TEST_ASSERT_TRUE_MESSAGE(before == w.old.snapshot(), "the old store was written");
  TEST_ASSERT_TRUE(w.exported.empty());
  // the imported rows fit the budget
  size_t bytes = 0;
  for (const tx::Item& it : w.items()) {
    std::string line;
    tx::Item c = it;
    if ((it.kind == tx::ItemKind::User || it.kind == tx::ItemKind::Assistant) && tx::encodeLine(c, line).ok())
      bytes += line.size();
  }
  TEST_ASSERT_TRUE(bytes <= tx::kImportBudgetBytes + 4096);
  // the restart fill takes it whole
  tx::TxLog log(w.files, kChatKey);
  tx::HeadState head;
  TEST_ASSERT_TRUE(log.open(head).ok());
  tx::TxArena arena;
  TEST_ASSERT_TRUE(arena.init(tx::kActiveChatArenaBytes, [](size_t n) { return std::malloc(n); }, [](void* p) { std::free(p); }).ok());
  tx::FillReport rep;
  TEST_ASSERT_TRUE(log.fill(head, arena, rep).ok());
  TEST_ASSERT_EQUAL(0, rep.dropped);
  TEST_ASSERT_TRUE(arena.count() > 0);
}

void test_a_chat_with_nothing_old_gets_a_marker_and_no_item() {
  World w;
  w.done("empty");
  TEST_ASSERT_EQUAL(0, w.items().size());
  bool re = false;
  TEST_ASSERT_TRUE(tx::isReentry(w.files, kChatKey, re).ok());
  TEST_ASSERT_TRUE(re);
  const auto files = w.files.files;
  const tx::MigrationOutcome again = w.done("empty again");
  TEST_ASSERT_TRUE(again.reentry);
  TEST_ASSERT_TRUE(files == w.files.files);
}

void test_rows_with_no_old_pointer_are_imported_without_digest0() {
  World w;
  w.m = month(w.old, 3, 300, 5, 0, 0, 0);
  const tx::MigrationOutcome out = w.done("no pointer");
  TEST_ASSERT_FALSE(out.digest0);
  TEST_ASSERT_EQUAL(0, w.answers.asks);
  TEST_ASSERT_EQUAL(0, w.digests().size());
  expectAccounted(w.old, w.files, 0, out.hi, "no pointer");
}

// ---- digest 0 ---------------------------------------------------------------------------------

void test_digest0_not_accepted_exports_the_old_text_and_names_it() {
  struct Case {
    const char* name;
    std::string answer;
    tx::Digest0Verdict verdict;
  };
  // a figure only a tool row of the covered rows holds
  World probe;
  probe.m = month(probe.old, 11, 400, 6, 4, 0, 1200);
  std::string toolFig, ownerFig;
  for (const OldRow& r : probe.old.rows) {
    if (r.id > probe.m.thru || r.session != kChat) continue;
    std::vector<std::string> f = tx::figuresOf(r.text);
    if (r.kind == "tool_output" && toolFig.empty())
      for (const std::string& x : f)
        if (x.size() == 4) toolFig = x;
  }
  for (const OldRow& r : probe.old.rows)   // the owner never wrote it
    if (r.role == "user" && r.text.find(toolFig) != std::string::npos) toolFig.clear();
  TEST_ASSERT_FALSE(toolFig.empty());
  const Case cases[] = {
      {"path", "The person kept the list in notes/walks.md and planned the trip.", tx::Digest0Verdict::Path},
      {"figure", "The person logged a meal of " + toolFig + " units and planned the trip.", tx::Digest0Verdict::Figure},
      {"size", std::string(768 * 6 + 1, 'w'), tx::Digest0Verdict::Size},
  };
  for (const Case& c : cases) {
    for (bool refuse : {false, true}) {
      World w;
      w.m = month(w.old, 11, 400, 6, 4, 0, 1200);
      w.answers.texts = {c.answer};
      w.refuseExport = refuse;
      const tx::MigrationOutcome out = w.done(c.name);
      TEST_ASSERT_EQUAL_MESSAGE((int)c.verdict, (int)out.verdict, c.name);
      TEST_ASSERT_FALSE(out.digest0);
      TEST_ASSERT_EQUAL(1, w.answers.asks);
      expectDigest0(w, false, c.name);
      const std::string anchor = w.items()[0].x;
      const std::string path = "export-pre-v2/summary-" + dayString(w.deps().fold.foldEpoch) + "-" + kChatKey + ".md";
      if (refuse) {
        TEST_ASSERT_TRUE(w.exported.empty());
        TEST_ASSERT_TRUE(anchor.find("Its old summary was not saved.") != std::string::npos);
      } else {
        TEST_ASSERT_EQUAL(1, w.exported.size());
        TEST_ASSERT_EQUAL_STRING(w.deps().fold.summary.c_str(), w.exported[path].c_str());
        TEST_ASSERT_TRUE_MESSAGE(anchor.find("Its old summary is saved in " + path + ".") != std::string::npos, anchor.c_str());
      }
      expectAccounted(w.old, w.files, w.m.thru, out.hi, c.name);
    }
  }
  // the same figure written by the owner is the owner's: accepted
  World w;
  w.m = month(w.old, 11, 400, 6, 4, 0, 1200);
  w.old.add(kChat, kMonthStart + kHour, "user", "message", "it was " + toolFig + " exactly");
  // (that row is newer than the pointer: only rows under it count)
  w.answers.texts = {"The person logged a meal of " + toolFig + " units."};
  const tx::MigrationOutcome out = w.done("owner figure out of range");
  TEST_ASSERT_EQUAL((int)tx::Digest0Verdict::Figure, (int)out.verdict);
}

void test_owner_figures_are_the_owners() {
  World w;
  w.old.add(kChat, kMonthStart + 8 * kHour, "user", "message", "my weight is 82.5 now");
  w.old.add(kChat, kMonthStart + 8 * kHour, "tool", "tool_output", "saved 82.5 to health.csv row 4411");
  const uint64_t thru = w.old.add(kChat, kMonthStart + 9 * kHour, "assistant", "message", "Noted 82.5.");
  w.old.add(kChat, kMonthStart + 30 * kHour, "user", "message", "and today?");
  w.old.fold("## Facts\nweight 82.5, row 4411", thru, 0, kMonthStart + kDay);
  w.answers.texts = {"The person said the weight is 82.5 and the assistant noted it."};
  TEST_ASSERT_TRUE(w.done("owner figure").digest0);
  World x;
  x.old.add(kChat, kMonthStart + 8 * kHour, "user", "message", "my weight please");
  x.old.add(kChat, kMonthStart + 8 * kHour, "tool", "tool_output", "saved 82.5 to health.csv row 4411");
  const uint64_t t2 = x.old.add(kChat, kMonthStart + 9 * kHour, "assistant", "message", "Noted 82.5.");
  x.old.fold("## Facts\nweight 82.5", t2, 0, kMonthStart + kDay);
  x.answers.texts = {"The weight was 82.5 and row 4411 was written."};
  const tx::MigrationOutcome out = x.done("tool figure");
  TEST_ASSERT_FALSE(out.digest0);
  TEST_ASSERT_EQUAL((int)tx::Digest0Verdict::Figure, (int)out.verdict);
}

void test_digest0_is_asked_at_most_twice_then_the_old_text_is_named() {
  World w;
  w.m = month(w.old, 5, 300, 5, 3, 0, 900);
  w.answers.fail = {true, true, true};
  tx::MigrationOutcome out = w.done("two failures");
  TEST_ASSERT_EQUAL(2, w.answers.asks);
  TEST_ASSERT_EQUAL((int)tx::Digest0Verdict::Asked, (int)out.verdict);
  expectDigest0(w, false, "two failures");
  expectAccounted(w.old, w.files, w.m.thru, out.hi, "two failures");
  // one failure, then an answer
  World x;
  x.m = month(x.old, 5, 300, 5, 3, 0, 900);
  x.answers.fail = {true};
  x.answers.texts = {kGood};
  out = x.done("one failure");
  TEST_ASSERT_EQUAL(2, x.answers.asks);
  TEST_ASSERT_TRUE(out.digest0);
  // no summarizer at all: the omission line, nothing asked
  World y;
  y.m = month(y.old, 5, 300, 5, 3, 0, 900);
  tx::MigrateDeps d = y.deps();
  d.digest0 = nullptr;
  TEST_ASSERT_TRUE(tx::migrate(d, out).ok());
  TEST_ASSERT_FALSE(out.digest0);
  TEST_ASSERT_EQUAL(1, y.exported.size());
}

// A minimal Anthropic-shaped wire for the scripted provider (as test_plain_compact's).
struct TestWire : plain::WireCodec {
  plain::Dialect dialect() const override { return plain::Dialect::AnthropicMessages; }
  plain::Status render(const plain::RenderInput& in, agent::HttpRequest& out) const override {
    out.method = "POST";
    out.host = in.head->host;
    out.path = "/v1/messages";
    body::BodyWriter w(out.body, &out.splices);
    w.beginObject().key("model").str(in.turn->model);
    if (in.maxTokens) w.key("max_tokens").unum(in.maxTokens);
    w.key("system").beginArray().beginObject().key("type").str("text").key("text").str(in.system->core).endObject().endArray();
    w.key("messages").beginArray();
    for (size_t i = 0; i < in.items->count(); ++i) {
      tx::Item it;
      if (!in.items->get(i, it).ok() || it.kind != tx::ItemKind::User) continue;
      w.beginObject().key("role").str("user").key("content").str(it.x).endObject();
    }
    w.endArray();
    w.endObject();
    return w.status();
  }
  plain::Status parse(const agent::HttpResponse& in, plain::ParsedStep& out) const override {
    JsonDocument doc;
    if (deserializeJson(doc, in.body) || !doc["content"].is<JsonArrayConst>())
      return plain::Status::fail(plain::Err::InvalidArgument, "test parse");
    for (JsonObjectConst b : doc["content"].as<JsonArrayConst>())
      if (std::string(b["type"] | "") == "text") out.text += b["text"] | "";
    const std::string stop = doc["stop_reason"] | "";
    out.stop = stop == "max_tokens" ? plain::StopReason::MaxTokens : plain::StopReason::EndTurn;
    return plain::Status::success();
  }
};
TestWire gWire;
uint32_t gClock = 0;
agent::Platform gPlatform;

struct Driver {
  sp::ScriptedProvider model{sp::Dialect::Anthropic};
  std::vector<std::string> parked;
  plain::CompactDeps deps() {
    plain::CompactDeps d;
    d.platform = &gPlatform;
    d.transport = &model;
    d.rungs = [](std::vector<plain::SummarizerRung>& out) {
      out.clear();
      for (const char* m : {"claude-haiku-5-5", "gpt-5.6-luna"}) {
        plain::SummarizerRung r;
        r.wire = &gWire;
        r.host = m[0] == 'c' ? "anthropic" : "openai";
        r.model = m;
        r.endpoint.host = "api.example.test";
        r.endpoint.key = [] { return std::string("test-key"); };
        out.push_back(r);
      }
    };
    d.park = [this](const plain::SummarizerRung& r) { parked.push_back(r.model); };
    return d;
  }
};

void test_digest0_is_asked_through_the_drivers_slice_and_prompt() {
  World w;
  w.m = month(w.old, 9, 500, 8, 5, 7, 2600);
  Driver drv;
  drv.model.push(sp::say(kGood));
  tx::MigrateDeps d = w.deps();
  d.digest0 = plain::digest0Asker(drv.deps());
  tx::MigrationOutcome out;
  TEST_ASSERT_TRUE(tx::migrate(d, out).ok());
  TEST_ASSERT_TRUE(out.digest0);
  TEST_ASSERT_EQUAL(1, drv.model.requests().size());
  const sp::Recorded& rec = drv.model.requests()[0];
  TEST_ASSERT_EQUAL_STRING(plain::texts::summarizer::kPrompt, rec.view.system().c_str());
  TEST_ASSERT_EQUAL_STRING("claude-haiku-5-5", rec.view.model.c_str());
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, rec.body));
  TEST_ASSERT_EQUAL(plain::kSummarizerMaxTokens, doc["max_tokens"] | 0);
  // the input is planSlice's rendering of the old summary as one closed turn of its day
  tx::TxArena a;
  TEST_ASSERT_TRUE(a.init(64 * 1024, [](size_t n) { return std::malloc(n); }, [](void* p) { std::free(p); }).ok());
  uint64_t at = 0;
  for (const OldRow& r : w.old.rows)
    if (r.session == kChat && r.id <= w.m.thru) at = std::max<uint64_t>(at, r.hours * kHour);
  tx::Item it;
  it.q = 1, it.turn = 1, it.kind = tx::ItemKind::Assistant, it.t = at, it.x = d.fold.summary, it.fin = true;
  TEST_ASSERT_TRUE(a.append(it).ok());
  tx::Item e;
  e.q = 2, e.turn = 1, e.kind = tx::ItemKind::TurnEnd, e.t = at;
  TEST_ASSERT_TRUE(a.append(e).ok());
  plain::SliceRules rules;
  rules.now = at + 2 * kDay;
  rules.high = 1;
  rules.manual = true;
  plain::SlicePlan plan;
  TEST_ASSERT_TRUE(plain::planSlice(a, 0, rules, plan).ok());
  TEST_ASSERT_EQUAL(1, rec.view.messages.size());
  TEST_ASSERT_EQUAL_STRING(plan.input.c_str(), rec.view.messages[0].blocks[0].text.c_str());
  TEST_ASSERT_TRUE(plan.input.find(" assistant: ") != std::string::npos);
  // the stored digest is the driver's text of that answer
  std::string text;
  TEST_ASSERT_TRUE(tx::digestText(plan.day, kGood, text).ok());
  TEST_ASSERT_EQUAL_STRING(text.c_str(), w.digests()[0].x.c_str());
  expectAccounted(w.old, w.files, w.m.thru, out.hi, "driver");
}

void test_the_drivers_acceptance_holds_for_digest0() {
  // a cut answer and a thin one are not taken; the second try asks the next rung
  for (int kind = 0; kind < 4; ++kind) {
    World w;
    w.m = month(w.old, 9, 500, 8, 5, 0, 2600);
    Driver drv;
    if (kind == 0) drv.model.push(sp::cut(kGood)).push(sp::cut(kGood));
    if (kind == 1) drv.model.push(sp::say("Fine.")).push(sp::say("Fine."));
    if (kind == 2) drv.model.push(sp::httpError(500)).push(sp::httpError(500));
    if (kind == 3) drv.model.push(sp::cut(kGood)).push(sp::say(kGood));
    drv.model.respondWith([](const sp::Recorded&, size_t) { return sp::httpError(500); });
    tx::MigrateDeps d = w.deps();
    d.digest0 = plain::digest0Asker(drv.deps());
    tx::MigrationOutcome out;
    TEST_ASSERT_TRUE(tx::migrate(d, out).ok());
    TEST_ASSERT_EQUAL(2, out.asks);
    TEST_ASSERT_EQUAL(kind == 3, out.digest0);
    const std::vector<sp::Recorded>& reqs = drv.model.requests();
    TEST_ASSERT_TRUE(reqs.size() >= 2);
    TEST_ASSERT_EQUAL_STRING("gpt-5.6-luna", reqs.back().view.model.c_str());
    expectDigest0(w, kind == 3, "acceptance");
    expectAccounted(w.old, w.files, w.m.thru, out.hi, "acceptance");
  }
}

// ---- rows between the two old pointers ------------------------------------------------------

void test_rows_between_the_pointers_come_first_and_are_compacted_first() {
  World w;
  w.m = month(w.old, 21, 900, 10, 4, 8, 1500);
  w.budget = 1u << 30;   // everything fits: nothing is named
  w.answers.texts = {kGood};
  const tx::MigrationOutcome out = w.done("between");
  TEST_ASSERT_EQUAL(0, out.named);
  expectAccounted(w.old, w.files, w.m.thru, out.hi, "between");
  // the first imported row is the first between the pointers
  uint64_t firstM = 0;
  for (const tx::Item& it : w.items())
    if (it.kind == tx::ItemKind::User && !firstM) firstM = it.m;
  TEST_ASSERT_TRUE(firstM > w.m.thru && firstM <= w.m.shift);
  // the compaction driver's decision rule takes them first: its first slice is all between rows
  tx::TxLog log(w.files, kChatKey);
  tx::HeadState head;
  TEST_ASSERT_TRUE(log.open(head).ok());
  tx::TxArena arena;
  TEST_ASSERT_TRUE(arena.init(4u << 20, [](size_t n) { return std::malloc(n); }, [](void* p) { std::free(p); }).ok());
  TEST_ASSERT_TRUE(log.fill(head, arena).ok());
  plain::SliceRules rules;
  rules.now = kMonthStart + 40 * kDay;
  rules.high = 20000;
  rules.low = 10000;
  plain::SlicePlan plan;
  TEST_ASSERT_TRUE(plain::planSlice(arena, head.thru, rules, plan).ok());
  std::printf("between: thru=%llx shift=%llx plan.thru=%llu lastRow=%llx turns=%u day=%s resident=%u\n",
              (unsigned long long)w.m.thru, (unsigned long long)w.m.shift, (unsigned long long)plan.thru,
              (unsigned long long)plan.lastRow, plan.turns, plan.day.c_str(), (unsigned)plan.resident);
  TEST_ASSERT_TRUE(plan.thru > 0);
  TEST_ASSERT_TRUE(plan.lastRow > w.m.thru && plan.lastRow <= w.m.shift);
  TEST_ASSERT_TRUE(plan.resident > rules.high);
}

void test_rows_between_the_pointers_over_the_budget_are_named() {
  World w;
  w.m = month(w.old, 22, 900, 10, 4, 8, 1500);
  w.budget = 8000;
  w.answers.texts = {kGood};
  const tx::MigrationOutcome out = w.done("between named");
  TEST_ASSERT_TRUE(out.named > 0);
  uint32_t between = 0;
  for (const OldRow& r : w.old.rows)
    if (r.session == kChat && r.id > w.m.thru && r.id <= w.m.shift && ((r.role == "user" && r.kind != "log") || (r.role == "assistant" && r.kind == "message")))
      ++between;
  TEST_ASSERT_TRUE(out.named >= between);
  expectAccounted(w.old, w.files, w.m.thru, out.hi, "between named");
  // the note says so, once
  uint32_t notes = 0;
  for (const tx::Item& it : w.items())
    if (importNoteText(it)) {
      ++notes;
      TEST_ASSERT_EQUAL((int)tx::NoteClass::Omit, (int)it.note);
      TEST_ASSERT_EQUAL(out.named, namedIn(it.x));
    }
  TEST_ASSERT_EQUAL(1, notes);
}

// ---- interruptions --------------------------------------------------------------------------

struct Small {
  uint32_t seed;
  size_t rows;
  size_t budget;
};

void fill(World& w, const Small& s) {
  w.m = month(w.old, s.seed, s.rows, 6, 3, 5, 700);
  w.budget = s.budget;
  w.answers.texts = {kGood};
}

// Run to Done through cuts: each call may be cut (Killed); the next one resumes.
tx::MigrationOutcome resume(World& w, int maxCalls, const char* when) {
  tx::MigrationOutcome out;
  for (int i = 0; i < maxCalls; ++i) {
    bool fin = false;
    try {
      fin = w.run(out).ok() && out.step == tx::MigrationStep::Done;
    } catch (const txb::Killed&) {
    }
    w.files.clearFaults();   // a cut that did not come is not left armed
    if (fin) return out;
  }
  TEST_FAIL_MESSAGE(when);
  return out;
}

void test_a_power_cut_at_every_write_resumes_to_the_same_store() {
  for (const Small s : {Small{31, 260, 3000}, Small{32, 200, 1u << 30}}) {
    World clean;
    fill(clean, s);
    const tx::MigrationOutcome ref = clean.done("clean");
    const std::vector<tx::Item> want = clean.items();
    const size_t appends = clean.files.appends, renames = clean.files.renames;
    TEST_ASSERT_TRUE(appends > 5 && renames > 3);
    char msg[160];
    for (int torn = 0; torn < 2; ++torn)
      for (size_t k = 0; k < appends; ++k) {
        World w;
        fill(w, s);
        const auto before = w.old.snapshot();
        w.files.killAtAppend = (long)k;
        w.files.tornKill = torn;
        std::snprintf(msg, sizeof msg, "cut at append %u torn %d", (unsigned)k, torn);
        const tx::MigrationOutcome out = resume(w, 4, msg);
        expectAccounted(w.old, w.files, w.m.thru, out.hi, msg);
        TEST_ASSERT_TRUE_MESSAGE(sameItems(want, w.items()), msg);
        TEST_ASSERT_TRUE_MESSAGE(w.answers.asks == 1, msg);   // the accepted answer is planned before items
        TEST_ASSERT_TRUE_MESSAGE(before == w.old.snapshot(), msg);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(ref.hi, out.hi, msg);
      }
    for (size_t k = 0; k < renames; ++k) {
      World w;
      fill(w, s);
      w.files.killAtRename = (long)k;
      std::snprintf(msg, sizeof msg, "cut at rename %u", (unsigned)k);
      const tx::MigrationOutcome out = resume(w, 4, msg);
      expectAccounted(w.old, w.files, w.m.thru, out.hi, msg);
      TEST_ASSERT_TRUE_MESSAGE(sameItems(want, w.items()), msg);
      TEST_ASSERT_TRUE_MESSAGE(w.answers.asks <= (int)tx::kDigest0Tries, msg);
    }
  }
}

void test_a_refused_write_stops_the_step_and_the_next_call_goes_on() {
  const Small s{41, 220, 3000};
  World clean;
  fill(clean, s);
  clean.done("clean");
  const std::vector<tx::Item> want = clean.items();
  char msg[160];
  for (size_t k = 0; k < clean.files.appends; ++k) {
    World w;
    fill(w, s);
    w.files.refuseAppend = (long)k;
    tx::MigrationOutcome out;
    const tx::Status st = w.run(out);
    std::snprintf(msg, sizeof msg, "refused append %u: %s", (unsigned)k, tx::errSlug(st.err));
    TEST_ASSERT_TRUE_MESSAGE(!st.ok() || out.step == tx::MigrationStep::Done, msg);
    if (!st.ok()) TEST_ASSERT_TRUE_MESSAGE(st.err == tx::Err::NotStored || st.err == tx::Err::Io, msg);
    w.files.clearFaults();
    out = w.done(msg);
    expectAccounted(w.old, w.files, w.m.thru, out.hi, msg);
    TEST_ASSERT_TRUE_MESSAGE(sameItems(want, w.items()), msg);
  }
  // the marker cannot be saved: nothing is appended, the next call does it all
  World w;
  fill(w, s);
  w.files.refuseRename = true;
  tx::MigrationOutcome out;
  TEST_ASSERT_EQUAL((int)tx::Err::Io, (int)w.run(out).err);
  TEST_ASSERT_EQUAL(0, w.items().size());
  w.files.clearFaults();
  out = w.done("after refused renames");
  expectAccounted(w.old, w.files, w.m.thru, out.hi, "after refused renames");
  // the old store would not answer
  for (long k = 0; k < 12; ++k) {
    World v;
    fill(v, s);
    v.src->refuseAt = k;
    tx::MigrationOutcome o;
    const tx::Status st = v.run(o);
    TEST_ASSERT_TRUE(st.ok() || st.err == tx::Err::Io);
    v.src->refuseAt = -1;
    o = v.done("source refused");
    expectAccounted(v.old, v.files, v.m.thru, o.hi, "source refused");
    TEST_ASSERT_TRUE(sameItems(want, v.items()));
  }
}

void test_a_refused_allocation_is_a_status_and_the_next_call_finishes() {
  const Small s{51, 40, 1500};
  World clean;
  fill(clean, s);
  gRefuseAt = -1;
  gAllocs = 0;
  clean.done("clean");
  const std::vector<tx::Item> want = clean.items();
  World probe;
  fill(probe, s);
  const tx::MigrateDeps pd = probe.deps();
  tx::MigrationOutcome o;
  gAllocs = 0;
  gRefuseAt = 1L << 40;
  TEST_ASSERT_TRUE(tx::migrate(pd, o).ok());
  const long total = gAllocs;
  gRefuseAt = -1;
  TEST_ASSERT_TRUE(total > 50);
  char msg[160];
  for (long k = 0; k < total; ++k) {
    World w;
    fill(w, s);
    const tx::MigrateDeps d = w.deps();
    tx::MigrationOutcome out;
    tx::Status st;
    gAllocs = 0;
    gRefuseAt = k;
    try {
      st = tx::migrate(d, out);
    } catch (const std::bad_alloc&) {
      gRefuseAt = -1;
      std::snprintf(msg, sizeof msg, "allocation %ld escaped tx::migrate", k);
      TEST_FAIL_MESSAGE(msg);
    }
    gRefuseAt = -1;
    std::snprintf(msg, sizeof msg, "refused allocation %ld: %s at %s", k, tx::errSlug(st.err), st.where);
    TEST_ASSERT_TRUE_MESSAGE(st.ok() || st.err == tx::Err::TooLarge, msg);
    out = w.done(msg);
    expectAccounted(w.old, w.files, w.m.thru, out.hi, msg);
    TEST_ASSERT_TRUE_MESSAGE(sameItems(want, w.items()), msg);
  }
}

// ---- q, the dry run --------------------------------------------------------------------------

void test_every_q_is_above_the_log_and_the_arena() {
  World w;
  fill(w, Small{61, 120, 1u << 30});
  tx::TxArena arena;
  TEST_ASSERT_TRUE(arena.init(64 * 1024, [](size_t n) { return std::malloc(n); }, [](void* p) { std::free(p); }).ok());
  tx::Item it;
  it.kind = tx::ItemKind::User;
  it.x = "E2 a message kept in RAM only";
  it.q = 50;
  TEST_ASSERT_TRUE(arena.append(it).ok());
  w.arena = &arena;
  w.done("arena");
  const std::vector<tx::Item> items = w.items();
  TEST_ASSERT_TRUE(items.front().q > 50);
  const std::vector<tx::Item> ds = w.digests();
  TEST_ASSERT_EQUAL(1, ds.size());
  TEST_ASSERT_TRUE(ds[0].q > ds[0].thru && ds[0].thru > 50);
}

void test_a_dry_run_writes_nothing_asks_nothing_and_shows_no_content() {
  World w;
  w.m = month(w.old, 71, 1500, 12, 6, 10, 900);
  w.budget = 6000;
  const auto files = w.files.files;
  tx::MigrationReport r;
  TEST_ASSERT_TRUE(tx::dryRun(w.deps(), r).ok());
  TEST_ASSERT_TRUE(files == w.files.files);
  TEST_ASSERT_EQUAL(0, w.answers.asks);
  TEST_ASSERT_EQUAL(0, w.files.appends + w.files.renames + w.files.wholeWrites);
  uint32_t rows = 0, conv = 0, covered = 0, between = 0;
  for (const OldRow& o : w.old.rows) {
    if (o.session != kChat) continue;
    ++rows;
    const bool c = (o.role == "user" && o.kind != "log") || (o.role == "assistant" && o.kind == "message");
    if (!c) continue;
    ++conv;
    covered += o.id <= w.m.thru;
    between += o.id > w.m.thru && o.id <= w.m.shift;
  }
  TEST_ASSERT_EQUAL(rows, r.rows);
  TEST_ASSERT_EQUAL(conv, r.conversation);
  TEST_ASSERT_EQUAL(rows - conv, r.toolRows);
  TEST_ASSERT_EQUAL(covered, r.covered);
  TEST_ASSERT_EQUAL(between, r.betweenPointers);
  TEST_ASSERT_EQUAL(12, r.days);
  TEST_ASSERT_TRUE(r.toName > 0 && r.toImport > 0);
  TEST_ASSERT_EQUAL(conv - covered, r.toImport + r.toName);
  for (const std::string& c : r.anomalyClasses)
    TEST_ASSERT_TRUE(c.find_first_not_of("abcdefghijklmnopqrstuvwxyz0-9-") == std::string::npos);
  // what the dry run says is what the migration does
  w.answers.texts = {kGood};
  const tx::MigrationOutcome out = w.done("after dry run");
  TEST_ASSERT_EQUAL(r.toName, out.named);
  TEST_ASSERT_EQUAL(r.toImport, out.imported);
}

}  // namespace

int main(int, char**) {
  gPlatform.nowMs = [] { return gClock; };
  gPlatform.delayMs = [](uint32_t ms) { gClock += ms; };
  gPlatform.nowEpoch = [] { return (uint64_t)0; };
  UNITY_BEGIN();
  RUN_TEST(test_figures_leave_out_dates_and_keep_quantities);
  RUN_TEST(test_path_like_tokens_are_found_and_prose_is_not);
  RUN_TEST(test_digest0_rule_is_768_bytes_per_day_no_path_no_tool_figure);
  RUN_TEST(test_the_note_names_turns_and_omitted_rows);
  RUN_TEST(test_a_month_migrates_every_row_once);
  RUN_TEST(test_a_chat_with_nothing_old_gets_a_marker_and_no_item);
  RUN_TEST(test_rows_with_no_old_pointer_are_imported_without_digest0);
  RUN_TEST(test_digest0_not_accepted_exports_the_old_text_and_names_it);
  RUN_TEST(test_owner_figures_are_the_owners);
  RUN_TEST(test_digest0_is_asked_at_most_twice_then_the_old_text_is_named);
  RUN_TEST(test_digest0_is_asked_through_the_drivers_slice_and_prompt);
  RUN_TEST(test_the_drivers_acceptance_holds_for_digest0);
  RUN_TEST(test_rows_between_the_pointers_come_first_and_are_compacted_first);
  RUN_TEST(test_rows_between_the_pointers_over_the_budget_are_named);
  RUN_TEST(test_a_power_cut_at_every_write_resumes_to_the_same_store);
  RUN_TEST(test_a_refused_write_stops_the_step_and_the_next_call_goes_on);
  RUN_TEST(test_a_refused_allocation_is_a_status_and_the_next_call_finishes);
  RUN_TEST(test_every_q_is_above_the_log_and_the_arena);
  RUN_TEST(test_a_dry_run_writes_nothing_asks_nothing_and_shows_no_content);
  return UNITY_END();
}
