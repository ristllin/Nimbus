// Lane DEGRADE: the device says when it runs degraded.
//
// What these pin (nimbus/orch/degraded.h):
//   - each of the six conditions raises from its facts, with a short status line and a
//     reason, and CLEARS when the facts recover;
//   - the main provider's fallback names the provider that is down and the one that
//     answers, from what the last turn went past;
//   - a flapping input moves the shown set at most once (no nagging);
//   - a fresh device and a Notifier show nothing;
//   - the same set feeds the screen strip, the details card and /api/state;
//   - the wiring: the device reads these from one module, nothing else.
// The class rule: a condition added to DegradedId with no fixture fails the walk below.

#include <unity.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "nimbus/orch/degraded.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// ---- helpers -----------------------------------------------------------------------

static const DegradedCondition* find(const DegradedSet& s, DegradedId id) {
  for (const auto& c : s)
    if (c.id == id) return &c;
  return nullptr;
}

// A pointer into a temporary set dangles: a call on a temporary is a compile error.
static const DegradedCondition* find(DegradedSet&&, DegradedId) = delete;
static const DegradedCondition* find(const DegradedSet&&, DegradedId) = delete;

// The condition `id` that `f` raises, copied out (nullptr when it does not).
static bool pick(const DegradedFacts& f, DegradedId id, DegradedCondition& out) {
  const DegradedSet s = computeDegraded(f);
  const DegradedCondition* c = find(s, id);
  if (c) out = *c;
  return c != nullptr;
}

static bool has(const DegradedSet& s, DegradedId id) { return find(s, id) != nullptr; }

static bool contains(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}

static std::string readFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static size_t count(const std::string& hay, const char* needle) {
  size_t n = 0, pos = 0;
  const size_t len = std::strlen(needle);
  while ((pos = hay.find(needle, pos)) != std::string::npos) { ++n; pos += len; }
  return n;
}

// Facts that raise exactly one condition (the class walk uses this for every id).
static DegradedFacts factsRaising(DegradedId id) {
  DegradedFacts f;
  f.uptimeMs = 10u * 60u * 1000u;
  switch (id) {
    case DegradedId::HeadFallback:
      f.fallback.active = true;
      f.fallback.head = "anthropic";
      f.fallback.served = "mistral";
      f.fallback.down = {"anthropic"};
      f.fallback.ageMs = 60u * 1000u;
      return f;
    case DegradedId::ProviderParked: {
      ParkedFact p;
      p.provider = "openai";
      p.kind = ParkKind::Billing;
      p.httpStatus = 429;
      p.seamMask = 0xFF;
      f.parks.push_back(p);
      return f;
    }
    case DegradedId::Memory:
      f.embedParked = true;
      return f;
    case DegradedId::StoreFull:
      f.storeFull = true;
      return f;
    case DegradedId::TelegramSilent:
      f.telegram.configured = true;
      f.telegram.idleMs = 12u * 60u * 1000u;
      return f;
    case DegradedId::ClockUnsynced:
      f.clockSynced = false;
      return f;
  }
  TEST_FAIL_MESSAGE("a DegradedId has no fixture in factsRaising: add one, and its tests");
  return f;
}

static bool printableAscii(const std::string& s) {
  for (unsigned char c : s)
    if (c != '\n' && (c < 0x20 || c > 0x7e)) return false;
  return true;
}

// ---- a fresh device and a Notifier -----------------------------------------------------

static void test_a_fresh_healthy_device_shows_nothing() {
  DegradedFacts f;   // every default is the healthy one
  f.uptimeMs = 5u * 60u * 1000u;
  TEST_ASSERT_EQUAL_size_t(0, computeDegraded(f).size());
}

static void test_a_device_just_booted_shows_nothing() {
  DegradedFacts f;
  f.uptimeMs = 3000;
  f.clockSynced = false;                 // not synced yet: normal for the first two minutes
  f.telegram.configured = true;          // the loop has not started: idle counts from boot
  f.telegram.idleMs = 3000;
  TEST_ASSERT_EQUAL_size_t(0, computeDegraded(f).size());
}

static void test_notifier_mode_shows_nothing_whatever_the_facts() {
  DegradedFacts f = factsRaising(DegradedId::HeadFallback);
  f.orchestrator = false;
  f.embedParked = true;
  f.storeFull = true;
  f.clockSynced = false;
  f.telegram.configured = true;
  f.telegram.idleMs = 3600u * 1000u;
  ParkedFact p;
  p.provider = "openai";
  p.kind = ParkKind::Billing;
  f.parks.push_back(p);
  TEST_ASSERT_EQUAL_size_t(0, computeDegraded(f).size());
}

// ---- the class walk -------------------------------------------------------------------

static void test_every_condition_raises_with_a_line_and_a_reason() {
  TEST_ASSERT_EQUAL_INT(6, kDegradedCount);
  std::string seen;
  for (int i = 0; i < kDegradedCount; i++) {
    const DegradedId id = DegradedId(i);
    const DegradedSet s = computeDegraded(factsRaising(id));
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)s.size(), degradedSlug(id));
    const DegradedCondition& c = s[0];
    TEST_ASSERT_TRUE_MESSAGE(c.id == id, degradedSlug(id));
    TEST_ASSERT_TRUE_MESSAGE(!c.line.empty() && c.line.size() <= kDegradedLineMax, c.line.c_str());
    TEST_ASSERT_TRUE_MESSAGE(printableAscii(c.line), c.line.c_str());
    TEST_ASSERT_TRUE_MESSAGE(!c.why.empty(), degradedSlug(id));
    TEST_ASSERT_TRUE_MESSAGE(printableAscii(c.why), c.why.c_str());
    // The slug is a machine id: lowercase snake case, unique, never empty.
    const std::string slug = degradedSlug(id);
    TEST_ASSERT_TRUE_MESSAGE(!slug.empty(), "empty slug");
    for (char ch : slug) TEST_ASSERT_TRUE_MESSAGE((ch >= 'a' && ch <= 'z') || ch == '_', slug.c_str());
    TEST_ASSERT_TRUE_MESSAGE(seen.find("|" + slug + "|") == std::string::npos, slug.c_str());
    seen += "|" + slug + "|";
  }
}

static void test_the_wire_slugs_are_frozen() {
  TEST_ASSERT_EQUAL_STRING("head_fallback", degradedSlug(DegradedId::HeadFallback));
  TEST_ASSERT_EQUAL_STRING("provider_parked", degradedSlug(DegradedId::ProviderParked));
  TEST_ASSERT_EQUAL_STRING("memory", degradedSlug(DegradedId::Memory));
  TEST_ASSERT_EQUAL_STRING("store_full", degradedSlug(DegradedId::StoreFull));
  TEST_ASSERT_EQUAL_STRING("telegram_silent", degradedSlug(DegradedId::TelegramSilent));
  TEST_ASSERT_EQUAL_STRING("clock_unsynced", degradedSlug(DegradedId::ClockUnsynced));
}

static void test_all_conditions_at_once_keep_display_order() {
  DegradedFacts f;
  f.uptimeMs = 10u * 60u * 1000u;
  for (int i = kDegradedCount - 1; i >= 0; i--) {   // merge every fixture, last id first
    const DegradedFacts one = factsRaising(DegradedId(i));
    if (one.fallback.active) f.fallback = one.fallback;
    for (const auto& p : one.parks) f.parks.push_back(p);
    f.embedParked = f.embedParked || one.embedParked;
    f.storeFull = f.storeFull || one.storeFull;
    if (one.telegram.configured) f.telegram = one.telegram;
    if (!one.clockSynced) f.clockSynced = false;
  }
  // The parked provider is openai and the fallback names anthropic: not folded.
  const DegradedSet s = computeDegraded(f);
  TEST_ASSERT_EQUAL_size_t(kDegradedCount, s.size());
  for (int i = 0; i < kDegradedCount; i++)
    TEST_ASSERT_TRUE_MESSAGE(s[(size_t)i].id == DegradedId(i), degradedSlug(DegradedId(i)));
}

// ---- 1. the main provider is unavailable, a later choice answers ------------------------

static void test_fallback_names_the_head_and_the_provider_that_answers() {
  ServingTracker t;
  t.noteTurn("anthropic", "mistral", "anthropic -> mistral", true, 1000);
  DegradedFacts f;
  f.uptimeMs = 600000;
  f.fallback = t.fact(2000);
  const DegradedSet s = computeDegraded(f);
  const DegradedCondition* c = find(s, DegradedId::HeadFallback);
  TEST_ASSERT_NOT_NULL(c);
  TEST_ASSERT_EQUAL_STRING("Anthropic down, Mistral answers", c->line.c_str());
  TEST_ASSERT_TRUE(contains(c->why, "Anthropic"));
  TEST_ASSERT_TRUE(contains(c->why, "Mistral"));
  TEST_ASSERT_TRUE(contains(c->why, "2nd choice"));
}

static void test_fallback_to_a_third_choice_names_every_provider_it_went_past() {
  ServingTracker t;
  t.noteTurn("anthropic", "zai", "anthropic -> openai -> zai", true, 1000);
  DegradedFacts f;
  f.uptimeMs = 600000;
  f.fallback = t.fact(2000);
  DegradedCondition cVal;
  const DegradedCondition* c = pick(f, DegradedId::HeadFallback, cVal) ? &cVal : nullptr;
  TEST_ASSERT_NOT_NULL(c);
  TEST_ASSERT_EQUAL_STRING("Anthropic, OpenAI down, Z.ai answers", c->line.c_str());
  TEST_ASSERT_TRUE(contains(c->why, "3rd choice"));
  TEST_ASSERT_TRUE(contains(c->why, "OpenAI"));
}

static void test_a_long_pair_of_names_falls_back_to_the_head_and_the_answer() {
  ServingTracker t;
  t.noteTurn("cumulo", "anthropic", "cumulo -> mistral -> anthropic", true, 1000);
  DegradedFacts f;
  f.uptimeMs = 600000;
  f.fallback = t.fact(2000);
  DegradedCondition cVal;
  const DegradedCondition* c = pick(f, DegradedId::HeadFallback, cVal) ? &cVal : nullptr;
  TEST_ASSERT_NOT_NULL(c);
  TEST_ASSERT_TRUE(c->line.size() <= kDegradedLineMax);
  TEST_ASSERT_TRUE(contains(c->line, "Cumulo Nimbus"));
  TEST_ASSERT_TRUE(contains(c->line, "Anthropic"));
  TEST_ASSERT_TRUE(contains(c->why, "Mistral"));   // the one the line had no room for
}

static void test_a_turn_served_by_the_head_clears_it() {
  ServingTracker t;
  t.noteTurn("anthropic", "mistral", "anthropic -> mistral", true, 1000);
  TEST_ASSERT_TRUE(t.fact(2000).active);
  t.noteTurn("anthropic", "anthropic", "anthropic", true, 5000);
  TEST_ASSERT_FALSE(t.fact(6000).active);
}

static void test_a_turn_nothing_answered_changes_nothing() {
  ServingTracker t;
  t.noteTurn("anthropic", "mistral", "anthropic -> mistral", true, 1000);
  t.noteTurn("anthropic", "anthropic", "anthropic", false, 5000);   // a failed turn: no host billed
  TEST_ASSERT_TRUE(t.fact(6000).active);
  ServingTracker u;
  u.noteTurn("anthropic", "anthropic", "anthropic", false, 1000);
  TEST_ASSERT_FALSE(u.fact(2000).active);
}

static void test_a_path_with_no_arrows_is_just_the_head() {
  ServingTracker t;
  t.noteTurn("openai", "mistral", "", true, 1000);   // served differs, the path was not kept
  const FallbackFact fact = t.fact(2000);
  TEST_ASSERT_TRUE(fact.active);
  TEST_ASSERT_EQUAL_STRING("openai", fact.head.c_str());
  TEST_ASSERT_EQUAL_STRING("mistral", fact.served.c_str());
  TEST_ASSERT_EQUAL_size_t(1, fact.down.size());
}

static void test_the_fallback_ends_when_the_head_is_open_and_the_quiet_window_passes() {
  ServingTracker t;
  t.noteTurn("anthropic", "mistral", "anthropic -> mistral", true, 1000);
  DegradedFacts f;
  f.uptimeMs = 4u * 3600u * 1000u;
  f.fallback = t.fact(1000 + kFallbackQuietMs - 1);
  TEST_ASSERT_TRUE(has(computeDegraded(f), DegradedId::HeadFallback));
  f.fallback = t.fact(1000 + kFallbackQuietMs + 1);
  TEST_ASSERT_FALSE_MESSAGE(has(computeDegraded(f), DegradedId::HeadFallback),
                            "an open head with no failed turn for the quiet window is recovered");
}

static void test_the_fallback_stays_while_the_head_is_held_off() {
  ServingTracker t;
  t.noteTurn("anthropic", "mistral", "anthropic -> mistral", true, 1000);
  DegradedFacts f;
  f.uptimeMs = 40u * 3600u * 1000u;
  f.fallback = t.fact(1000 + 24u * 3600u * 1000u);
  f.fallback.headClosed = true;
  f.fallback.headWhy = "is out of credit";
  f.fallback.headFix = "Add credit with the provider";
  DegradedCondition cVal;
  const DegradedCondition* c = pick(f, DegradedId::HeadFallback, cVal) ? &cVal : nullptr;
  TEST_ASSERT_NOT_NULL(c);
  TEST_ASSERT_TRUE(contains(c->why, "is out of credit"));
  TEST_ASSERT_TRUE(contains(c->why, "Add credit with the provider"));
}

// ---- 2. a parked or out-of-credit provider ----------------------------------------------

static ParkedFact parked(const char* provider, ParkKind kind, int status = 0, uint8_t paused = 0,
                         ParkCause cause = ParkCause::Unspecified) {
  ParkedFact p;
  p.provider = provider;
  p.kind = kind;
  p.httpStatus = status;
  p.seamMask = 0xFF;
  p.paused = paused;
  p.cause = cause;
  return p;
}

static std::string parkLine(const ParkedFact& p) {
  DegradedFacts f;
  f.uptimeMs = 600000;
  f.parks.push_back(p);
  DegradedCondition cVal;
  const DegradedCondition* c = pick(f, DegradedId::ProviderParked, cVal) ? &cVal : nullptr;
  return c ? c->line : std::string("<none>");
}

static void test_out_of_credit_and_a_refused_key_read_differently() {
  TEST_ASSERT_EQUAL_STRING("OpenAI is out of credit", parkLine(parked("openai", ParkKind::Billing, 429)).c_str());
  TEST_ASSERT_EQUAL_STRING("Mistral key refused", parkLine(parked("mistral", ParkKind::Auth, 401)).c_str());
}

static void test_a_403_on_some_seams_is_not_a_refused_key() {
  ParkedFact p = parked("openai", ParkKind::Auth, 403);
  p.seamMask = 1u << (int)PaidSeam::Vision;
  TEST_ASSERT_EQUAL_STRING("OpenAI refuses some requests", parkLine(p).c_str());
}

static void test_a_pause_by_the_owner_or_a_provider_limit_shows() {
  TEST_ASSERT_EQUAL_STRING("Mistral is paused",
                           parkLine(parked("mistral", ParkKind::None, 0, kPauseOwner)).c_str());
  TEST_ASSERT_EQUAL_STRING("Anthropic hit its monthly limit",
                           parkLine(parked("anthropic", ParkKind::None, 0, kPauseProviderLimit)).c_str());
}

static void test_the_spend_ceiling_pause_keeps_its_own_line() {
  // Lane DEF already tells it on the home screen and the web banner: not a second line.
  TEST_ASSERT_EQUAL_STRING("<none>",
                           parkLine(parked("openai", ParkKind::None, 0, kPauseSpendCeiling)).c_str());
  // Both bits: the provider's own limit still shows.
  TEST_ASSERT_TRUE(parkLine(parked("openai", ParkKind::None, 0, kPauseSpendCeiling | kPauseProviderLimit)) !=
                   "<none>");
}

static void test_a_record_that_holds_nothing_shows_nothing() {
  TEST_ASSERT_EQUAL_STRING("<none>", parkLine(parked("openai", ParkKind::None)).c_str());
}

static void test_a_router_park_uses_what_the_router_said() {
  TEST_ASSERT_EQUAL_STRING("Cumulo Nimbus needs a plan",
                           parkLine(parked("cumulo", ParkKind::Billing, 402, 0, ParkCause::NeedsPlan)).c_str());
  TEST_ASSERT_EQUAL_STRING("Cumulo Nimbus account suspended",
                           parkLine(parked("cumulo", ParkKind::Billing, 402, 0, ParkCause::Suspended)).c_str());
  TEST_ASSERT_EQUAL_STRING("Cumulo Nimbus budget used up",
                           parkLine(parked("cumulo", ParkKind::Billing, 402, 0, ParkCause::KeyBudget)).c_str());
}

static void test_several_parked_providers_are_one_line_that_names_them() {
  DegradedFacts f;
  f.uptimeMs = 600000;
  f.parks.push_back(parked("openai", ParkKind::Billing, 429));
  f.parks.push_back(parked("mistral", ParkKind::Auth, 401));
  const DegradedSet s = computeDegraded(f);
  TEST_ASSERT_EQUAL_size_t(1, s.size());
  TEST_ASSERT_EQUAL_STRING("OpenAI, Mistral unavailable", s[0].line.c_str());
  TEST_ASSERT_TRUE(contains(s[0].why, "OpenAI"));
  TEST_ASSERT_TRUE(contains(s[0].why, "out of credit"));
  TEST_ASSERT_TRUE(contains(s[0].why, "Mistral"));
  TEST_ASSERT_TRUE(contains(s[0].why, "refused"));
  f.parks.push_back(parked("anthropic", ParkKind::Billing, 400));
  f.parks.push_back(parked("zai", ParkKind::Billing, 402));
  const DegradedSet many = computeDegraded(f);
  TEST_ASSERT_TRUE(many[0].line.size() <= kDegradedLineMax);
  TEST_ASSERT_EQUAL_STRING("4 providers unavailable", many[0].line.c_str());
}

static void test_a_parked_provider_the_fallback_already_names_is_one_line_not_two() {
  DegradedFacts f = factsRaising(DegradedId::HeadFallback);   // anthropic down, mistral answers
  f.parks.push_back(parked("anthropic", ParkKind::Billing, 400));
  const DegradedSet s = computeDegraded(f);
  TEST_ASSERT_TRUE(has(s, DegradedId::HeadFallback));
  TEST_ASSERT_FALSE_MESSAGE(has(s, DegradedId::ProviderParked), "the head's park is the fallback line's cause");
  // A different parked provider still shows.
  f.parks.push_back(parked("openai", ParkKind::Billing, 429));
  const DegradedSet s2 = computeDegraded(f);
  TEST_ASSERT_TRUE(has(s2, DegradedId::ProviderParked));
  TEST_ASSERT_EQUAL_STRING("OpenAI is out of credit", find(s2, DegradedId::ProviderParked)->line.c_str());
}

static void test_the_park_reason_carries_the_one_next_step() {
  const ParkReason credit = degradedParkReason(parked("openai", ParkKind::Billing, 429));
  TEST_ASSERT_EQUAL_STRING("is out of credit", credit.phrase.c_str());
  TEST_ASSERT_EQUAL_STRING("Add credit with the provider", credit.fix.c_str());
  const ParkReason key = degradedParkReason(parked("openai", ParkKind::Auth, 401));
  TEST_ASSERT_EQUAL_STRING("Update the key under Assistant > Models", key.fix.c_str());
  const ParkReason none = degradedParkReason(parked("openai", ParkKind::None));
  TEST_ASSERT_TRUE(none.phrase.empty() && none.fix.empty());
}

// ---- 3. memory without its embeddings ------------------------------------------------------

static std::string memoryLine(bool parkedNow, int pending) {
  DegradedFacts f;
  f.uptimeMs = 600000;
  f.embedParked = parkedNow;
  f.pendingEmbeds = pending;
  DegradedCondition cVal;
  const DegradedCondition* c = pick(f, DegradedId::Memory, cVal) ? &cVal : nullptr;
  return c ? c->line : std::string("<none>");
}

static void test_memory_says_keyword_only_queued_or_both() {
  TEST_ASSERT_EQUAL_STRING("Memory is keyword-only", memoryLine(true, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("Memory saves are queued", memoryLine(false, 3).c_str());
  TEST_ASSERT_EQUAL_STRING("Memory is keyword-only, saves queued", memoryLine(true, 3).c_str());
  TEST_ASSERT_EQUAL_STRING("<none>", memoryLine(false, 0).c_str());
}

static void test_the_memory_line_never_carries_a_count() {
  // A count in the line would repaint the panel at every queued fact.
  TEST_ASSERT_TRUE(memoryLine(true, 3) == memoryLine(true, 40));
  TEST_ASSERT_TRUE(memoryLine(false, 3) == memoryLine(false, 40));
}

// ---- 4. the file store is full ---------------------------------------------------------------

static void test_a_full_file_store_and_a_full_flash_each_show() {
  DegradedFacts f;
  f.uptimeMs = 600000;
  f.storeFull = true;
  DegradedCondition cVal;
  const DegradedCondition* c = pick(f, DegradedId::StoreFull, cVal) ? &cVal : nullptr;
  TEST_ASSERT_NOT_NULL(c);
  TEST_ASSERT_EQUAL_STRING("File storage is full", c->line.c_str());
  TEST_ASSERT_TRUE(contains(c->why, "Delete"));
  DegradedFacts g;
  g.uptimeMs = 600000;
  g.flashFull = true;
  DegradedCondition dVal;
  const DegradedCondition* d = pick(g, DegradedId::StoreFull, dVal) ? &dVal : nullptr;
  TEST_ASSERT_NOT_NULL(d);
  TEST_ASSERT_EQUAL_STRING("Device storage is full", d->line.c_str());
  f.flashFull = true;   // both: one line, the file store first
  TEST_ASSERT_EQUAL_STRING("File storage is full", [&] { DegradedCondition v; pick(f, DegradedId::StoreFull, v); return v.line; }().c_str());
}

// ---- 5. Telegram is silent ----------------------------------------------------------------------

static std::string tgLine(const TelegramFact& t) {
  DegradedFacts f;
  f.uptimeMs = 3600u * 1000u;
  f.telegram = t;
  DegradedCondition cVal;
  const DegradedCondition* c = pick(f, DegradedId::TelegramSilent, cVal) ? &cVal : nullptr;
  return c ? c->line : std::string("<none>");
}

static void test_telegram_is_silent_only_when_it_is_set_up_and_the_loop_stopped_checking_in() {
  TelegramFact t;
  TEST_ASSERT_EQUAL_STRING("<none>", tgLine(t).c_str());                   // no token: not set up
  t.configured = true;
  t.idleMs = 20u * 1000u;
  TEST_ASSERT_EQUAL_STRING("<none>", tgLine(t).c_str());                   // a normal long poll
  t.idleMs = kTgIdleMs - 1;
  TEST_ASSERT_EQUAL_STRING("<none>", tgLine(t).c_str());
  t.idleMs = kTgIdleMs + 1;
  TEST_ASSERT_EQUAL_STRING("Telegram is not responding", tgLine(t).c_str());
}

static void test_a_long_turn_on_the_poll_task_gets_the_longer_allowance() {
  TelegramFact t;
  t.configured = true;
  t.turnBusy = true;
  t.idleMs = 25u * 60u * 1000u;
  TEST_ASSERT_EQUAL_STRING("<none>", tgLine(t).c_str());
  t.idleMs = kTgIdleBusyMs + 1;
  TEST_ASSERT_EQUAL_STRING("Telegram is not responding", tgLine(t).c_str());
}

static void test_a_rejected_token_a_dead_task_and_repeated_failures_each_say_so() {
  TelegramFact t;
  t.configured = true;
  t.authRejected = true;
  TEST_ASSERT_EQUAL_STRING("Telegram rejects the bot token", tgLine(t).c_str());
  TelegramFact dead;
  dead.configured = true;
  dead.taskAlive = false;
  TEST_ASSERT_EQUAL_STRING("Telegram has stopped", tgLine(dead).c_str());
  TelegramFact fails;
  fails.configured = true;
  fails.fails = kTgFailsUnreachable - 1;
  TEST_ASSERT_EQUAL_STRING("<none>", tgLine(fails).c_str());
  fails.fails = kTgFailsUnreachable;
  TEST_ASSERT_EQUAL_STRING("Telegram is unreachable", tgLine(fails).c_str());
}

static void test_telegram_stays_quiet_about_a_network_that_is_down() {
  TelegramFact t;
  t.configured = true;
  t.wifiUp = false;
  t.idleMs = 3600u * 1000u;
  t.fails = 50;
  TEST_ASSERT_EQUAL_STRING("<none>", tgLine(t).c_str());   // the Wi-Fi glyph carries that news
}

// ---- 6. the clock has not synced ------------------------------------------------------------------

static void test_an_unsynced_clock_shows_after_the_boot_grace_only() {
  DegradedFacts f;
  f.clockSynced = false;
  f.uptimeMs = kClockGraceMs - 1;
  TEST_ASSERT_FALSE(has(computeDegraded(f), DegradedId::ClockUnsynced));
  f.uptimeMs = kClockGraceMs + 1;
  DegradedCondition cVal;
  const DegradedCondition* c = pick(f, DegradedId::ClockUnsynced, cVal) ? &cVal : nullptr;
  TEST_ASSERT_NOT_NULL(c);
  TEST_ASSERT_EQUAL_STRING("Clock not synced", c->line.c_str());
  f.clockSynced = true;
  TEST_ASSERT_FALSE(has(computeDegraded(f), DegradedId::ClockUnsynced));
}

static void test_the_clock_reason_tells_a_missing_network_from_a_silent_time_server() {
  DegradedFacts f;
  f.clockSynced = false;
  f.uptimeMs = 600000;
  f.wifiUp = true;
  DegradedCondition onV;
  pick(f, DegradedId::ClockUnsynced, onV);
  const std::string online = onV.why;
  f.wifiUp = false;
  DegradedCondition offV;
  pick(f, DegradedId::ClockUnsynced, offV);
  const std::string offline = offV.why;
  TEST_ASSERT_TRUE(online != offline);
  TEST_ASSERT_TRUE(contains(offline, "Wi-Fi"));
  TEST_ASSERT_TRUE(contains(online, "time server"));
}

// ---- the calm: no nagging ----------------------------------------------------------------------------

static DegradedSet one(DegradedId id) { return computeDegraded(factsRaising(id)); }

static void test_a_condition_shows_only_after_it_has_held_for_the_raise_dwell() {
  DegradedDebounce d;
  const DegradedSet s = one(DegradedId::StoreFull);
  TEST_ASSERT_FALSE(d.update(s, 0));
  TEST_ASSERT_FALSE(d.update(s, kRaiseMs - 1));
  TEST_ASSERT_EQUAL_size_t(0, d.shown().size());
  TEST_ASSERT_TRUE(d.update(s, kRaiseMs));
  TEST_ASSERT_EQUAL_size_t(1, d.shown().size());
  TEST_ASSERT_EQUAL_UINT32(1, d.generation());
}

static void test_a_condition_hides_only_after_it_has_been_gone_for_the_clear_dwell() {
  DegradedDebounce d;
  const DegradedSet s = one(DegradedId::StoreFull);
  d.update(s, 0);
  d.update(s, kRaiseMs);
  const DegradedSet none;
  const uint32_t t0 = kRaiseMs + 1000;
  TEST_ASSERT_FALSE(d.update(none, t0));
  TEST_ASSERT_FALSE(d.update(none, t0 + kClearMs - 1));
  TEST_ASSERT_EQUAL_size_t(1, d.shown().size());
  TEST_ASSERT_TRUE(d.update(none, t0 + kClearMs));
  TEST_ASSERT_EQUAL_size_t(0, d.shown().size());
  TEST_ASSERT_EQUAL_UINT32(2, d.generation());
}

static void test_a_flapping_input_never_raises_a_condition() {
  // The class rule: assert the absence of thrash, not only the final state. The input
  // alternates every second for ten minutes and never holds for the dwell.
  DegradedDebounce d;
  const DegradedSet s = one(DegradedId::Memory);
  const DegradedSet none;
  int changes = 0;
  for (uint32_t t = 0; t < 600u * 1000u; t += 1000)
    if (d.update((t / 1000) % 2 ? s : none, t)) ++changes;
  TEST_ASSERT_EQUAL_INT(0, changes);
}

static void test_a_shown_condition_that_flaps_changes_the_panel_at_most_once() {
  DegradedDebounce d;
  const DegradedSet s = one(DegradedId::Memory);
  const DegradedSet none;
  d.update(s, 0);
  TEST_ASSERT_TRUE(d.update(s, kRaiseMs));   // raised: one change
  int changes = 0;
  // Gone for 5 s, back for 5 s, over and over: never gone for the clear dwell.
  for (uint32_t t = kRaiseMs + 1000; t < kRaiseMs + 600u * 1000u; t += 1000) {
    const bool present = ((t - kRaiseMs) / 5000) % 2 == 0;
    if (d.update(present ? s : none, t)) ++changes;
  }
  TEST_ASSERT_EQUAL_INT(0, changes);
  TEST_ASSERT_EQUAL_size_t(1, d.shown().size());
}

static void test_a_reason_change_updates_silently_and_a_line_change_counts() {
  DegradedDebounce d;
  DegradedSet s = one(DegradedId::Memory);
  d.update(s, 0);
  d.update(s, kRaiseMs);
  const uint32_t gen = d.generation();
  s[0].why = "A different reason.";
  TEST_ASSERT_FALSE(d.update(s, kRaiseMs + 1000));
  TEST_ASSERT_EQUAL_UINT32(gen, d.generation());
  TEST_ASSERT_EQUAL_STRING("A different reason.", d.shown()[0].why.c_str());
  s[0].line = "Memory is keyword-only, saves queued";
  TEST_ASSERT_TRUE(d.update(s, kRaiseMs + 2000));
  TEST_ASSERT_EQUAL_UINT32(gen + 1, d.generation());
}

static void test_conditions_raise_and_clear_on_their_own_clocks() {
  DegradedDebounce d;
  DegradedSet both = one(DegradedId::StoreFull);
  both.push_back(one(DegradedId::ClockUnsynced)[0]);
  d.update(both, 0);
  d.update(both, kRaiseMs);
  TEST_ASSERT_EQUAL_size_t(2, d.shown().size());
  const DegradedSet onlyClock = one(DegradedId::ClockUnsynced);
  d.update(onlyClock, kRaiseMs + 1000);
  d.update(onlyClock, kRaiseMs + 1000 + kClearMs);
  TEST_ASSERT_EQUAL_size_t(1, d.shown().size());
  TEST_ASSERT_TRUE(d.shown()[0].id == DegradedId::ClockUnsynced);
}

static void test_the_whole_cycle_from_failure_to_recovery_from_facts() {
  // The tick's loop: facts -> compute -> debounce. Anthropic fails over at 1 s; the head
  // answers a turn again at 100 s; the line is gone one clear dwell later.
  ServingTracker tr;
  DegradedDebounce d;
  tr.noteTurn("anthropic", "mistral", "anthropic -> mistral", true, 1000);
  uint32_t shownAt = 0, hiddenAt = 0;
  for (uint32_t t = 2000; t < 300u * 1000u; t += 2000) {
    if (t == 100u * 1000u) tr.noteTurn("anthropic", "anthropic", "anthropic", true, t);
    DegradedFacts f;
    f.uptimeMs = t;
    f.fallback = tr.fact(t);
    d.update(computeDegraded(f), t);
    if (!shownAt && !d.shown().empty()) shownAt = t;
    if (shownAt && !hiddenAt && d.shown().empty()) hiddenAt = t;
  }
  TEST_ASSERT_TRUE(shownAt >= 1000 + 0 && shownAt <= 2000 + kRaiseMs + 2000);
  TEST_ASSERT_TRUE(hiddenAt >= 100u * 1000u + kClearMs);
  TEST_ASSERT_TRUE(hiddenAt <= 100u * 1000u + kClearMs + 4000);
  TEST_ASSERT_EQUAL_UINT32(2, d.generation());
}

// ---- the surfaces ------------------------------------------------------------------------------------------

static DegradedSet three() {
  DegradedSet s = one(DegradedId::HeadFallback);
  s.push_back(one(DegradedId::StoreFull)[0]);
  s.push_back(one(DegradedId::ClockUnsynced)[0]);
  return s;
}

static void test_the_strip_has_a_row_per_condition_and_folds_the_rest() {
  const DegradedSet s3 = three();
  TEST_ASSERT_EQUAL_size_t(0, degradedStripRows(DegradedSet(), 3).size());
  TEST_ASSERT_EQUAL_size_t(1, degradedStripRows(one(DegradedId::Memory), 3).size());
  const std::vector<std::string> rows3 = degradedStripRows(s3, 3);
  TEST_ASSERT_EQUAL_size_t(3, rows3.size());
  TEST_ASSERT_EQUAL_STRING(s3[0].line.c_str(), rows3[0].c_str());
  TEST_ASSERT_EQUAL_STRING(s3[2].line.c_str(), rows3[2].c_str());
  const std::vector<std::string> rows2 = degradedStripRows(s3, 2);
  TEST_ASSERT_EQUAL_size_t(2, rows2.size());
  TEST_ASSERT_EQUAL_STRING(s3[0].line.c_str(), rows2[0].c_str());
  TEST_ASSERT_EQUAL_STRING("+2 more", rows2[1].c_str());
  DegradedSet s6;
  for (int i = 0; i < kDegradedCount; i++) s6.push_back(one(DegradedId(i))[0]);
  const std::vector<std::string> rows6 = degradedStripRows(s6, 3);
  TEST_ASSERT_EQUAL_size_t(3, rows6.size());
  TEST_ASSERT_EQUAL_STRING("+4 more", rows6[2].c_str());
}

static void test_the_details_card_holds_every_line_and_reason() {
  TEST_ASSERT_EQUAL_STRING("", degradedDetailText(DegradedSet()).c_str());
  const DegradedSet s = three();
  const std::string t = degradedDetailText(s);
  for (const auto& c : s) {
    TEST_ASSERT_TRUE_MESSAGE(contains(t, c.line.c_str()), c.line.c_str());
    TEST_ASSERT_TRUE_MESSAGE(contains(t, c.why.c_str()), c.why.c_str());
  }
  TEST_ASSERT_TRUE(printableAscii(t));
}

static void test_api_state_carries_the_same_set() {
  JsonDocument doc;
  degradedInto(DegradedSet(), 0, doc.to<JsonObject>());
  TEST_ASSERT_EQUAL_INT(0, doc["count"].as<int>());
  TEST_ASSERT_TRUE(doc["items"].is<JsonArray>());
  TEST_ASSERT_EQUAL_size_t(0, doc["items"].as<JsonArray>().size());
  TEST_ASSERT_TRUE(doc["gen"].is<uint32_t>());

  const DegradedSet s = three();
  JsonDocument d2;
  degradedInto(s, 7, d2.to<JsonObject>());
  TEST_ASSERT_EQUAL_INT(3, d2["count"].as<int>());
  TEST_ASSERT_EQUAL_UINT32(7, d2["gen"].as<uint32_t>());
  JsonArray items = d2["items"].as<JsonArray>();
  TEST_ASSERT_EQUAL_size_t(3, items.size());
  for (size_t i = 0; i < s.size(); i++) {
    TEST_ASSERT_EQUAL_STRING(degradedSlug(s[i].id), items[i]["id"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING(s[i].line.c_str(), items[i]["line"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING(s[i].why.c_str(), items[i]["why"].as<const char*>());
  }
}

// ---- copy ----------------------------------------------------------------------------------------------------

static void test_the_copy_follows_the_style_guide() {
  // Every string the module writes, over a spread of facts: ASCII, no em dash, no
  // exclamation mark, the device never says I or me, sentence case on every line.
  std::vector<DegradedFacts> spread;
  for (int i = 0; i < kDegradedCount; i++) spread.push_back(factsRaising(DegradedId(i)));
  DegradedFacts mix = factsRaising(DegradedId::HeadFallback);
  mix.fallback.down = {"anthropic", "openai"};
  mix.fallback.served = "zai";
  mix.fallback.headClosed = true;
  mix.fallback.headWhy = "is out of credit";
  mix.fallback.headFix = "Add credit with the provider";
  mix.embedParked = true;
  mix.pendingEmbeds = 4;
  mix.flashFull = true;
  mix.clockSynced = false;
  mix.wifiUp = false;
  mix.telegram.configured = true;
  mix.telegram.authRejected = true;
  mix.parks.push_back(parked("cumulo", ParkKind::Billing, 402, 0, ParkCause::Suspended));
  mix.parks.push_back(parked("mistral", ParkKind::None, 0, kPauseOwner));
  spread.push_back(mix);
  for (const DegradedFacts& f : spread)
    for (const DegradedCondition& c : computeDegraded(f)) {
      for (const std::string* s : {&c.line, &c.why}) {
        TEST_ASSERT_TRUE_MESSAGE(printableAscii(*s), s->c_str());
        TEST_ASSERT_FALSE_MESSAGE(contains(*s, "!"), s->c_str());
        TEST_ASSERT_FALSE_MESSAGE(contains(*s, " I "), s->c_str());
        TEST_ASSERT_FALSE_MESSAGE(contains(*s, " me "), s->c_str());
        TEST_ASSERT_FALSE_MESSAGE(contains(*s, " my "), s->c_str());
        TEST_ASSERT_FALSE_MESSAGE(contains(*s, "WiFi"), s->c_str());
        TEST_ASSERT_FALSE_MESSAGE(contains(*s, "wifi"), s->c_str());
      }
      TEST_ASSERT_TRUE_MESSAGE((c.line[0] >= 'A' && c.line[0] <= 'Z') || (c.line[0] >= '0' && c.line[0] <= '9'), c.line.c_str());
      TEST_ASSERT_TRUE_MESSAGE(c.line.back() != '.', c.line.c_str());   // a status line, not a sentence
      TEST_ASSERT_TRUE_MESSAGE(c.why.back() == '.', c.why.c_str());
    }
}

// ---- the wiring: one module, every surface ----------------------------------------------------------------------

static void test_device_wiring() {
  // The device collects facts in ONE place and every surface reads that module.
  const std::string dev = readFile("src/agent/degraded_device.cpp");
  TEST_ASSERT_TRUE(dev.size() > 0);
  TEST_ASSERT_TRUE(contains(dev, "computeDegraded("));
  TEST_ASSERT_TRUE(contains(dev, "DegradedDebounce"));
  TEST_ASSERT_TRUE(contains(dev, "alogf(\"degraded: shown=%s gen=%u\""));   // every change leaves a durable line
  const std::string orch = readFile("src/agent/orchestrator.cpp");
  TEST_ASSERT_EQUAL_size_t(1, count(orch, "degraded::noteTurn("));   // the turn-end hook feeds the tracker
  const std::string main = readFile("src/main.cpp");
  TEST_ASSERT_TRUE(contains(main, "agent::degraded::begin();"));    // setup allocates the state (the mutex is a FreeRTOS object)
  TEST_ASSERT_TRUE(contains(main, "agent::degraded::tick("));       // the main loop runs the tick
  // ... and a device still in first-run setup is not degraded, it is not set up yet.
  TEST_ASSERT_TRUE(contains(main, "g_orchMode && !deviceNeedsSetup(), net::staConnected()"));
  TEST_ASSERT_TRUE(contains(main, "Action::Degraded"));             // a tap opens the reason
  const std::string web = readFile("src/net/webui.cpp");
  TEST_ASSERT_TRUE(contains(web, "agent::degraded::into("));        // /api/state
  const std::string js = readFile("include/web/ui_js.h");
  TEST_ASSERT_TRUE(contains(js, "d.degraded"));                     // the page reads it
  // The facts: the embedding read never waits for the card lock (the main loop calls it),
  // and the Telegram loop stamps its heartbeat at the top of every cycle and after staged work.
  const std::string mem = readFile("src/agent/memory_subsystem.cpp");
  const size_t eh = mem.find("bool embedHealth(");
  TEST_ASSERT_TRUE(eh != std::string::npos);
  TEST_ASSERT_TRUE_MESSAGE(mem.find("LoopLock g(0);", eh) - eh < 200, "embedHealth must open with LoopLock g(0)");
  const std::string tg = readFile("src/agent/telegram.cpp");
  TEST_ASSERT_TRUE(count(tg, "s_loopMs = millis();") >= 3);   // pollTask start, loop top, after staged work
  TEST_ASSERT_TRUE(contains(tg, "h.idleMs = millis() - s_loopMs;"));
  TEST_ASSERT_TRUE(contains(readFile("src/agent/files_subsystem.cpp"), "bool storeFullActive() { return g_full.active(); }"));
  // The device keeps its degraded state on the heap: no static object here (static RAM is
  // about 400 B from its ceiling; check_static_dram.py is the gate, this keeps the shape).
  TEST_ASSERT_FALSE(contains(dev, "static State s"));
  TEST_ASSERT_TRUE(contains(dev, "State* s_state = nullptr;"));
  TEST_ASSERT_TRUE(contains(main, "static DegradedPaint* p = nullptr;"));
  // The ring-only repaint keeps the strip of the last full paint.
  TEST_ASSERT_TRUE(contains(main, "buildCtx(-1, /*fullPaint=*/false)"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_fresh_healthy_device_shows_nothing);
  RUN_TEST(test_a_device_just_booted_shows_nothing);
  RUN_TEST(test_notifier_mode_shows_nothing_whatever_the_facts);
  RUN_TEST(test_every_condition_raises_with_a_line_and_a_reason);
  RUN_TEST(test_the_wire_slugs_are_frozen);
  RUN_TEST(test_all_conditions_at_once_keep_display_order);
  RUN_TEST(test_fallback_names_the_head_and_the_provider_that_answers);
  RUN_TEST(test_fallback_to_a_third_choice_names_every_provider_it_went_past);
  RUN_TEST(test_a_long_pair_of_names_falls_back_to_the_head_and_the_answer);
  RUN_TEST(test_a_turn_served_by_the_head_clears_it);
  RUN_TEST(test_a_turn_nothing_answered_changes_nothing);
  RUN_TEST(test_a_path_with_no_arrows_is_just_the_head);
  RUN_TEST(test_the_fallback_ends_when_the_head_is_open_and_the_quiet_window_passes);
  RUN_TEST(test_the_fallback_stays_while_the_head_is_held_off);
  RUN_TEST(test_out_of_credit_and_a_refused_key_read_differently);
  RUN_TEST(test_a_403_on_some_seams_is_not_a_refused_key);
  RUN_TEST(test_a_pause_by_the_owner_or_a_provider_limit_shows);
  RUN_TEST(test_the_spend_ceiling_pause_keeps_its_own_line);
  RUN_TEST(test_a_record_that_holds_nothing_shows_nothing);
  RUN_TEST(test_a_router_park_uses_what_the_router_said);
  RUN_TEST(test_several_parked_providers_are_one_line_that_names_them);
  RUN_TEST(test_a_parked_provider_the_fallback_already_names_is_one_line_not_two);
  RUN_TEST(test_the_park_reason_carries_the_one_next_step);
  RUN_TEST(test_memory_says_keyword_only_queued_or_both);
  RUN_TEST(test_the_memory_line_never_carries_a_count);
  RUN_TEST(test_a_full_file_store_and_a_full_flash_each_show);
  RUN_TEST(test_telegram_is_silent_only_when_it_is_set_up_and_the_loop_stopped_checking_in);
  RUN_TEST(test_a_long_turn_on_the_poll_task_gets_the_longer_allowance);
  RUN_TEST(test_a_rejected_token_a_dead_task_and_repeated_failures_each_say_so);
  RUN_TEST(test_telegram_stays_quiet_about_a_network_that_is_down);
  RUN_TEST(test_an_unsynced_clock_shows_after_the_boot_grace_only);
  RUN_TEST(test_the_clock_reason_tells_a_missing_network_from_a_silent_time_server);
  RUN_TEST(test_a_condition_shows_only_after_it_has_held_for_the_raise_dwell);
  RUN_TEST(test_a_condition_hides_only_after_it_has_been_gone_for_the_clear_dwell);
  RUN_TEST(test_a_flapping_input_never_raises_a_condition);
  RUN_TEST(test_a_shown_condition_that_flaps_changes_the_panel_at_most_once);
  RUN_TEST(test_a_reason_change_updates_silently_and_a_line_change_counts);
  RUN_TEST(test_conditions_raise_and_clear_on_their_own_clocks);
  RUN_TEST(test_the_whole_cycle_from_failure_to_recovery_from_facts);
  RUN_TEST(test_the_strip_has_a_row_per_condition_and_folds_the_rest);
  RUN_TEST(test_the_details_card_holds_every_line_and_reason);
  RUN_TEST(test_api_state_carries_the_same_set);
  RUN_TEST(test_the_copy_follows_the_style_guide);
  RUN_TEST(test_device_wiring);
  return UNITY_END();
}
