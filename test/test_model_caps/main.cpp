#include <unity.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "nimbus/orch/model_caps.h"
#include "nimbus/orch/model_caps_data.h"
#include "nimbus/orch/model_defaults.h"
#include "nimbus/orch/model_price_defaults.h"
#include "nimbus/orch/model_window.h"
#include "nimbus/orch/request_budget.h"
#include "nimbus/orch/router_route.h"

// test_model_caps - the engine-2 router's table of wire-shape facts (model_caps.h, lane E2-R1;
// ROUTER-DESIGN.md sections 3 and 11 layer L0). The generator's own lints run in
// tools/release_gate/test_model_caps_gen.py; this suite holds what only the firmware can check:
// every row resolves in the SHARED functions (called here, never copied), the lookup order, the
// fail-safe defaults, the router overlay and the refused-field memory, which only narrows.

using namespace nimbus::orch;
using namespace nimbus::orch::caps;

void setUp() { installListedCtxLookup(nullptr); }
void tearDown() { installListedCtxLookup(nullptr); }

static const char* idOf(const ModelRow& r) { return poolStr(builtinCaps(), r.match); }
static bool directSlot(Slot s) { return s != Slot::CustomChat && s != Slot::CustomMessages; }

// ---- layout and size ---------------------------------------------------------------------------
static_assert(sizeof(ModelRow) == 26, "gen_caps.py MODEL_ROW_BYTES");
static_assert(sizeof(ProviderRow) == 20, "gen_caps.py PROVIDER_ROW_BYTES");
static_assert(sizeof(MarkerRow) == 8, "gen_caps.py MARKER_ROW_BYTES");

static void test_table_data_is_counted_and_under_its_limit() {
  const size_t bytes = sizeof(kCapsModels) + sizeof(kCapsProviders) + sizeof(kCapsMarkers) + sizeof(kCapsPool);
  TEST_ASSERT_EQUAL_UINT32(bytes, kCapsDataBytes);
  TEST_ASSERT_TRUE(kCapsDataBytes <= 8192);
  TEST_ASSERT_TRUE(builtinCaps().modelCount <= 128);
  TEST_ASSERT_EQUAL_CHAR(0, kCapsPool[0]);   // offset 0 is ""
}

static void test_every_pool_offset_is_inside_the_pool() {
  const CapsTable& t = builtinCaps();
  for (size_t i = 0; i < t.modelCount; i++) TEST_ASSERT_TRUE(t.models[i].match < t.poolBytes);
  for (size_t i = 0; i < t.providerCount; i++) {
    const ProviderRow& p = t.providers[i];
    for (uint16_t off : {p.host, p.host2, p.basePath, p.versionHeader, p.emptyText}) TEST_ASSERT_TRUE(off < t.poolBytes);
  }
  for (size_t i = 0; i < t.markerCount; i++) TEST_ASSERT_TRUE(t.markers[i].text > 0 && t.markers[i].text < t.poolBytes);
}

// ---- D1: every row resolves in the shared functions (called, not copied) ------------------------
// A named row's model must have its own window (not the 100,000-token default of an unknown
// model) and its own price row (not the provider's fallback) in the functions both engines read,
// direct and routed. The custom slots hold only '*' rows (the owner's model).
static void test_every_named_row_has_a_real_window_and_price_in_the_shared_functions() {
  const CapsTable& t = builtinCaps();
  size_t named = 0;
  for (size_t i = 0; i < t.modelCount; i++) {
    const ModelRow& r = t.models[i];
    if (r.kind == MatchKind::Any) continue;
    TEST_ASSERT_TRUE_MESSAGE(directSlot(r.slot), "a custom slot has no named rows");
    named++;
    const std::string host = slotName(r.slot), id = idOf(r);
    const std::string sel = host + "/" + id;
    TEST_ASSERT_NOT_EQUAL_MESSAGE(kCtxDefaultTokens, headWindowTokens(host, id), id.c_str());
    TEST_ASSERT_NOT_EQUAL_MESSAGE(kCtxDefaultTokens, headWindowTokens("cumulo", sel), sel.c_str());
    TEST_ASSERT_NOT_NULL_MESSAGE(builtinModelPrice(host.c_str(), id.c_str()), id.c_str());
    TEST_ASSERT_NOT_NULL_MESSAGE(builtinModelPrice("cumulo", sel.c_str()), sel.c_str());
    TEST_ASSERT_TRUE_MESSAGE(readWindowMsFor(host, id, false) > 0, id.c_str());
    TEST_ASSERT_TRUE_MESSAGE(readWindowMsFor("cumulo", sel, true) > 0, sel.c_str());
  }
  TEST_ASSERT_TRUE(named >= 20);
}

// Counter-test: the two checks above do fail for a model the shared functions do not know.
static void test_counter_an_unknown_model_fails_the_shared_function_checks() {
  TEST_ASSERT_EQUAL_UINT32(kCtxDefaultTokens, headWindowTokens("zai", "nope-1"));
  TEST_ASSERT_NULL(builtinModelPrice("openai", "gpt-5.6-nope"));
  TEST_ASSERT_FALSE(capsFor(Slot::OpenAI, "gpt-5.6-nope", false).known);
}

// Every role default (and the router's, read on its upstream) gets a named row, not the '*' one.
static void test_every_role_default_has_a_named_row() {
  for (size_t i = 0; i < kRoleDefaultCount; i++) {
    const RoleDefault& d = kRoleDefaults[i];
    Slot s;
    bool routed = false;
    std::string model = d.fallback;
    if (std::strcmp(d.slot, "cumulo") == 0) {
      const RouterRoute rr = resolveRouterRoute(d.fallback);
      TEST_ASSERT_TRUE(slotFromHost(rr.upstream.c_str(), &s));
      model = rr.model;
      routed = true;
    } else {
      TEST_ASSERT_TRUE_MESSAGE(slotFromHost(d.slot, &s), d.slot);
    }
    const Caps c = capsFor(s, model.c_str(), routed);
    TEST_ASSERT_TRUE_MESSAGE(c.known, d.fallback);
    TEST_ASSERT_EQUAL(routed, c.routed);
  }
}

// The routed heads the design's paid set names resolve, through the one selector split.
static void test_routed_selectors_resolve_on_their_upstream_rows() {
  for (const char* sel : {"anthropic/claude-haiku-5-5", "openai/gpt-5.6-luna", "mistral/mistral-large-4-0", "zai/glm-5.3"}) {
    const RouterRoute rr = resolveRouterRoute(sel);
    Slot s;
    TEST_ASSERT_TRUE(slotFromHost(rr.upstream.c_str(), &s));
    const Caps c = capsFor(s, rr.model.c_str(), true);
    TEST_ASSERT_TRUE_MESSAGE(c.known && c.routed, sel);
  }
}

// ---- lookup order --------------------------------------------------------------------------------
// A small table of its own: exact, two nested prefixes and the '*' row.
static constexpr char kPool[] = "\0m-1\0m-\0m-1x\0";   // 1 "m-1", 5 "m-", 8 "m-1x"
static constexpr ModelRow kRows[] = {
    {0, Slot::Mistral, MatchKind::Any, CapMode::SendAlways, CapParam::MaxTokens, 1, 3, 3072, 4, 4, 1, IdRule::Mistral9,
     Structured::None, Structured::None, Thinking::Off, 0, Replay::None, ReplayScope::OpenTurn, CacheStyle::Automatic,
     1024, UsageMode::InputIncludesCache, 47},
    {5, Slot::Mistral, MatchKind::Prefix, CapMode::SendAlways, CapParam::MaxTokens, 3, 3, 100, 5, 4, 1,
     IdRule::Mistral9, Structured::None, Structured::None, Thinking::Off, 0, Replay::None, ReplayScope::OpenTurn,
     CacheStyle::Automatic, 1024, UsageMode::InputIncludesCache, 47},
    {8, Slot::Mistral, MatchKind::Prefix, CapMode::SendAlways, CapParam::MaxTokens, 3, 3, 100, 6, 4, 1,
     IdRule::Mistral9, Structured::None, Structured::None, Thinking::Off, 0, Replay::None, ReplayScope::OpenTurn,
     CacheStyle::Automatic, 1024, UsageMode::InputIncludesCache, 47},
    {1, Slot::Mistral, MatchKind::Exact, CapMode::SendAlways, CapParam::MaxTokens, 3, 3, 100, 7, 4, 1,
     IdRule::Mistral9, Structured::None, Structured::None, Thinking::Off, 0, Replay::None, ReplayScope::OpenTurn,
     CacheStyle::Automatic, 1024, UsageMode::InputIncludesCache, 47},
};
static constexpr ProviderRow kProv[] = {
    {Slot::Mistral, Dialect::Chat, Auth::Bearer, CacheKey::None, 0, 0, 0, 0, 0, 25, 100, 0, 0}};
static const CapsTable kSmall{kRows, 4, kProv, 1, nullptr, 0, kPool, sizeof(kPool)};

static uint16_t imagesFor(const char* model) { return capsFrom(kSmall, Slot::Mistral, model, false, nullptr).row.imagesMax; }

static void test_exact_beats_prefix_and_the_longest_prefix_wins() {
  TEST_ASSERT_EQUAL_UINT16(7, imagesFor("m-1"));          // exact, though both prefixes match
  TEST_ASSERT_EQUAL_UINT16(7, imagesFor("M-1"));          // case is ignored
  TEST_ASSERT_EQUAL_UINT16(7, imagesFor("m-1-2026-01"));  // one version suffix
  TEST_ASSERT_EQUAL_UINT16(7, imagesFor("m-1-latest"));
  TEST_ASSERT_EQUAL_UINT16(6, imagesFor("m-1xy"));        // longest prefix
  TEST_ASSERT_EQUAL_UINT16(5, imagesFor("m-1-luna"));     // a capability suffix is not a version
  TEST_ASSERT_EQUAL_UINT16(5, imagesFor("m-9"));
  TEST_ASSERT_EQUAL_UINT16(4, imagesFor("other"));        // the '*' row
  TEST_ASSERT_EQUAL_UINT16(4, imagesFor(""));
  TEST_ASSERT_FALSE(capsFrom(kSmall, Slot::Mistral, "other", false, nullptr).known);
  TEST_ASSERT_TRUE(capsFrom(kSmall, Slot::Mistral, "m-9", false, nullptr).known);
  // A slot the table does not hold: no provider, no row, never a crash.
  const Caps none = capsFrom(kSmall, Slot::Zai, "m-1", false, nullptr);
  TEST_ASSERT_NULL(none.provider);
  TEST_ASSERT_FALSE(none.known);
}

static void test_the_money_safe_rule_keeps_capability_suffixes_apart() {
  const Caps luna = capsFor(Slot::OpenAI, "gpt-5.6-luna", false);
  TEST_ASSERT_EQUAL_STRING("gpt-5.6-luna", idOf(*luna.matched));
  TEST_ASSERT_EQUAL_STRING("gpt-5.6", idOf(*capsFor(Slot::OpenAI, "gpt-5.6", false).matched));
  TEST_ASSERT_EQUAL_STRING("claude-haiku-5-5", idOf(*capsFor(Slot::Anthropic, "claude-haiku-5-5-20261001", false).matched));
  TEST_ASSERT_FALSE(capsFor(Slot::OpenAI, "gpt-5.6-mini", false).known);
  // Another slot's id is not this slot's row.
  TEST_ASSERT_FALSE(capsFor(Slot::Mistral, "claude-haiku-5-5", false).known);
}

// ---- fail-safe defaults ---------------------------------------------------------------------------
// An unknown model of a known provider: text only, no structured output, no effort, no reasoning
// replay, nothing forced; the cap rule of its wire.
static void test_an_unknown_model_gets_the_fail_safe_row() {
  for (size_t i = 0; i < kSlotCount; i++) {
    const Slot s = (Slot)i;
    const Caps c = capsFor(s, "zz-unknown-9", false);
    TEST_ASSERT_NOT_NULL(c.provider);
    TEST_ASSERT_FALSE(c.known);
    TEST_ASSERT_EQUAL_UINT8(input::kText, c.row.inputKinds);
    TEST_ASSERT_EQUAL_UINT8(0, c.row.effortSet);
    TEST_ASSERT_EQUAL(Structured::None, c.row.structured);
    TEST_ASSERT_EQUAL(Structured::None, c.row.summarizerStructured);
    TEST_ASSERT_EQUAL(Thinking::Off, c.row.thinking);
    TEST_ASSERT_EQUAL(Replay::None, c.row.replay);
    TEST_ASSERT_EQUAL_UINT8(choice::kAuto, c.row.toolChoice);
    TEST_ASSERT_EQUAL_UINT8(0, c.row.flags & (flag::kFileRef | flag::kImageInToolResult | flag::kLongCacheOk));
    const bool messages = c.provider->dialect == Dialect::Messages;
    TEST_ASSERT_EQUAL(messages, c.row.capMode == CapMode::Required);
  }
  // Custom endpoints start in text mode (no tools) until the verify step says otherwise.
  TEST_ASSERT_EQUAL_UINT8(0, capsFor(Slot::CustomChat, "llama", false).row.flags & flag::kTools);
  TEST_ASSERT_EQUAL_UINT8(0, capsFor(Slot::CustomMessages, "x", false).row.flags & flag::kTools);
}

// ---- a few facts the design leans on, read through the lookup ---------------------------------------
static void test_rows_carry_the_facts_the_design_relies_on() {
  const Caps sonnet55 = capsFor(Slot::Anthropic, "claude-sonnet-5-5", false);
  TEST_ASSERT_EQUAL(Thinking::BetweenTools, sonnet55.row.thinking);
  TEST_ASSERT_EQUAL_UINT8(0, sonnet55.row.toolChoice & (choice::kAny | choice::kNamed | choice::kRequired));
  TEST_ASSERT_EQUAL(Replay::SignedBlocks, sonnet55.row.replay);
  TEST_ASSERT_EQUAL(ReplayScope::OpenTurn, sonnet55.row.replayScope);
  TEST_ASSERT_EQUAL(UsageMode::InputExcludesCache, sonnet55.row.usageMode);
  const Caps glm = capsFor(Slot::Zai, "glm-5.3", false);
  TEST_ASSERT_EQUAL_UINT8(input::kText, glm.row.inputKinds);
  TEST_ASSERT_EQUAL_UINT8(0, glm.row.toolChoice & choice::kNone);   // not recorded: auto and no dispatch
  TEST_ASSERT_EQUAL(Structured::None, glm.row.structured);
  TEST_ASSERT_EQUAL(UsageMode::InputIncludesCache, glm.row.usageMode);
  const Caps large4 = capsFor(Slot::Mistral, "mistral-large-4-0", false);
  TEST_ASSERT_EQUAL(IdRule::Mistral9, large4.row.idRule);
  TEST_ASSERT_EQUAL_UINT8(0, large4.row.effortSet & effort::kNone);   // refused with code 3051
  TEST_ASSERT_EQUAL(Structured::None, large4.row.summarizerStructured);
  TEST_ASSERT_EQUAL_STRING("(empty)", poolStr(builtinCaps(), large4.provider->emptyText));
  const Caps luna = capsFor(Slot::OpenAI, "gpt-5.6-luna", false);
  TEST_ASSERT_EQUAL(CapMode::OmitDirectCeilRouted, luna.row.capMode);
  TEST_ASSERT_EQUAL(CapParam::MaxOutputTokens, luna.row.capParam);
  TEST_ASSERT_EQUAL(Replay::EncryptedItems, luna.row.replay);
  TEST_ASSERT_EQUAL_UINT8(0, luna.row.flags & flag::kImageInToolResult);
  TEST_ASSERT_EQUAL_STRING("2023-06-01", poolStr(builtinCaps(), sonnet55.provider->versionHeader));
  TEST_ASSERT_EQUAL_STRING("open.bigmodel.cn", poolStr(builtinCaps(), glm.provider->host2));
}

// ---- the router overlay ------------------------------------------------------------------------------
static void test_a_routed_row_takes_the_routers_limits() {
  const Caps direct = capsFor(Slot::Anthropic, "claude-haiku-5-5", false);
  const Caps routed = capsFor(Slot::Anthropic, "claude-haiku-5-5", true);
  TEST_ASSERT_TRUE(direct.row.flags & flag::kFileRef);
  TEST_ASSERT_EQUAL_UINT8(0, routed.row.flags & flag::kFileRef);   // no provider file ids
  TEST_ASSERT_EQUAL_UINT16(kRoutedImageMaxKiB, routed.row.imageMaxKiB);
  TEST_ASSERT_EQUAL_UINT16(100, routed.row.imagesMax);                // the router's Anthropic count
  TEST_ASSERT_EQUAL_UINT16(600, direct.row.imagesMax);
  TEST_ASSERT_TRUE(routed.routed);
  // Every routed row is within the router's limits.
  const CapsTable& t = builtinCaps();
  for (size_t i = 0; i < t.modelCount; i++) {
    if (!directSlot(t.models[i].slot)) continue;
    const Caps c = capsFor(t.models[i].slot, idOf(t.models[i]), true);
    TEST_ASSERT_TRUE(c.routed);
    TEST_ASSERT_TRUE(c.row.imageMaxKiB <= kRoutedImageMaxKiB);
    TEST_ASSERT_TRUE(c.row.imagesMax <= c.provider->routedImagesMax);
    TEST_ASSERT_EQUAL_UINT8(0, c.row.flags & flag::kFileRef);
  }
  // A custom endpoint is never routed, whatever was asked.
  TEST_ASSERT_FALSE(capsFor(Slot::CustomChat, "x", true).routed);
}

// ---- the refused-field memory: only narrows ------------------------------------------------------------
static bool subset(uint8_t a, uint8_t b) { return (a & ~b) == 0; }

static bool narrowerOrEqual(const ModelRow& n, const ModelRow& r) {
  return n.match == r.match && n.slot == r.slot && subset(n.inputKinds, r.inputKinds) &&
         subset(n.imageFormats, r.imageFormats) && n.imageMaxKiB <= r.imageMaxKiB && n.imagesMax <= r.imagesMax &&
         subset(n.flags & (uint8_t)~flag::kTools, r.flags) && (n.flags & flag::kTools) == (r.flags & flag::kTools) &&
         subset(n.toolChoice, r.toolChoice) && subset(n.effortSet, r.effortSet) &&
         (n.structured == r.structured || n.structured == Structured::None) &&
         (n.summarizerStructured == r.summarizerStructured || n.summarizerStructured == Structured::None) &&
         (n.thinking == r.thinking || n.thinking == Thinking::Off) &&
         (n.replay == r.replay || n.replay == Replay::None) && n.replayScope == r.replayScope &&
         (n.cacheStyle == r.cacheStyle ||
          (r.cacheStyle == CacheStyle::OpenaiBreakpoints && n.cacheStyle == CacheStyle::OpenaiImplicit)) &&
         (n.capMode == r.capMode || n.capMode == CapMode::Omit) && n.capParam == r.capParam &&
         n.idRule == r.idRule && n.usageMode == r.usageMode && n.orderRules == r.orderRules &&
         n.cacheMinTok == r.cacheMinTok;
}

static void test_every_refusal_only_narrows_every_row() {
  const CapsTable& t = builtinCaps();
  for (size_t i = 0; i < t.modelCount; i++)
    for (uint16_t m = 0; m <= refused::kAll; m++) {
      const ModelRow n = narrowed(t.models[i], m);
      TEST_ASSERT_TRUE(narrowerOrEqual(n, t.models[i]));
      TEST_ASSERT_TRUE(narrowerOrEqual(narrowed(n, m), n));   // and again: nothing grows back
    }
  // Each bit takes away what it names.
  const ModelRow& opus = *capsFor(Slot::Anthropic, "claude-opus-5-5", false).matched;
  TEST_ASSERT_EQUAL_UINT8(0, narrowed(opus, refused::kImage).inputKinds & input::kImage);
  TEST_ASSERT_EQUAL_UINT8(0, narrowed(opus, refused::kEffort).effortSet);
  TEST_ASSERT_EQUAL(Thinking::Off, narrowed(opus, refused::kThinking).thinking);
  TEST_ASSERT_EQUAL(Structured::None, narrowed(opus, refused::kStructured).summarizerStructured);
  TEST_ASSERT_EQUAL_UINT8(0, narrowed(opus, refused::kChoiceNone).toolChoice & choice::kNone);
  TEST_ASSERT_EQUAL(Replay::None, narrowed(opus, refused::kReplay).replay);
  TEST_ASSERT_EQUAL_UINT8(0, narrowed(opus, refused::kFileRef).flags & flag::kFileRef);
  TEST_ASSERT_EQUAL(CapMode::Required, narrowed(opus, refused::kCapField).capMode);   // the wire needs it
  const ModelRow& luna = *capsFor(Slot::OpenAI, "gpt-5.6-luna", false).matched;
  TEST_ASSERT_EQUAL(CacheStyle::OpenaiImplicit, narrowed(luna, refused::kCacheBreakpoint).cacheStyle);
  TEST_ASSERT_EQUAL(CapMode::Omit, narrowed(luna, refused::kCapField).capMode);
}

static void test_the_memory_is_per_route_and_reaches_the_lookup() {
  RefusedFields mem;
  TEST_ASSERT_EQUAL_UINT16(0, mem.fieldsFor(Slot::Zai, "glm-5.3", false));
  mem.note(Slot::Zai, "glm-5.3", false, refused::kEffort);
  mem.note(Slot::Zai, "GLM-5.3", false, refused::kReplay);   // the same model: OR-ed in
  TEST_ASSERT_EQUAL_UINT16(refused::kEffort | refused::kReplay, mem.fieldsFor(Slot::Zai, "glm-5.3", false));
  TEST_ASSERT_EQUAL_UINT16(0, mem.fieldsFor(Slot::Zai, "glm-5.3", true));        // routed is its own route
  TEST_ASSERT_EQUAL_UINT16(0, mem.fieldsFor(Slot::Zai, "glm-5.3-flash", false));
  const Caps c = capsFor(Slot::Zai, "glm-5.3", false, &mem);
  TEST_ASSERT_EQUAL_UINT8(0, c.row.effortSet);
  TEST_ASSERT_EQUAL(Replay::None, c.row.replay);
  TEST_ASSERT_NOT_EQUAL(0, c.matched->effortSet);   // the table row itself is untouched
  mem.clear();
  TEST_ASSERT_EQUAL_UINT16(0, mem.fieldsFor(Slot::Zai, "glm-5.3", false));
}

static void test_a_full_memory_forgets_its_oldest_entry() {
  RefusedFields mem;
  char id[16];
  for (int i = 0; i < (int)RefusedFields::kEntries; i++) {
    std::snprintf(id, sizeof(id), "m%d", i);
    mem.note(Slot::Mistral, id, false, refused::kImage);
  }
  mem.note(Slot::Mistral, "m-new", false, refused::kEffort);
  TEST_ASSERT_EQUAL_UINT16(0, mem.fieldsFor(Slot::Mistral, "m0", false));   // the oldest went
  TEST_ASSERT_EQUAL_UINT16(refused::kImage, mem.fieldsFor(Slot::Mistral, "m1", false));
  TEST_ASSERT_EQUAL_UINT16(refused::kEffort, mem.fieldsFor(Slot::Mistral, "m-new", false));
  TEST_ASSERT_TRUE(sizeof(RefusedFields) <= 72);   // 8 entries of 8 B and a cursor
}

// ---- markers and slots ------------------------------------------------------------------------------
static void test_each_providers_markers_are_its_own_and_contiguous() {
  const CapsTable& t = builtinCaps();
  size_t total = 0;
  for (size_t i = 0; i < t.providerCount; i++) {
    const MarkerSpan sp = markersOf(t, t.providers[i]);
    for (size_t j = 0; j < sp.count; j++) TEST_ASSERT_EQUAL_UINT8(t.providers[i].markerSet, sp.rows[j].markerSet);
    total += sp.count;
  }
  TEST_ASSERT_EQUAL_UINT32(t.markerCount, total);
  // Z.ai's billing code is a code marker (matched on the code field, never a substring).
  const MarkerSpan zai = markersOf(t, *capsFor(Slot::Zai, "glm-5.3", false).provider);
  bool found = false;
  for (size_t j = 0; j < zai.count; j++)
    if (std::strcmp(poolStr(t, zai.rows[j].text), "1113") == 0)
      found = zai.rows[j].kind == MarkerKind::Code && zai.rows[j].cls == ErrClass::Billing;
  TEST_ASSERT_TRUE(found);
}

static void test_slots_and_hosts() {
  for (size_t i = 0; i < kSlotCount; i++) {
    const Caps c = capsFor((Slot)i, "", false);
    TEST_ASSERT_NOT_NULL(c.provider);
    TEST_ASSERT_EQUAL((Slot)i, c.provider->slot);
  }
  Slot s;
  TEST_ASSERT_TRUE(slotFromHost("anthropic", &s) && s == Slot::Anthropic);
  TEST_ASSERT_TRUE(slotFromHost("zai", &s) && s == Slot::Zai);
  TEST_ASSERT_TRUE(slotFromHost("custom", &s) && s == Slot::CustomChat);
  TEST_ASSERT_FALSE(slotFromHost("cumulo", &s));
  TEST_ASSERT_FALSE(slotFromHost(nullptr, &s));
  TEST_ASSERT_EQUAL_STRING("custom_messages", slotName(Slot::CustomMessages));
  TEST_ASSERT_EQUAL_STRING("", poolStr(builtinCaps(), 65535));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_table_data_is_counted_and_under_its_limit);
  RUN_TEST(test_every_pool_offset_is_inside_the_pool);
  RUN_TEST(test_every_named_row_has_a_real_window_and_price_in_the_shared_functions);
  RUN_TEST(test_counter_an_unknown_model_fails_the_shared_function_checks);
  RUN_TEST(test_every_role_default_has_a_named_row);
  RUN_TEST(test_routed_selectors_resolve_on_their_upstream_rows);
  RUN_TEST(test_exact_beats_prefix_and_the_longest_prefix_wins);
  RUN_TEST(test_the_money_safe_rule_keeps_capability_suffixes_apart);
  RUN_TEST(test_an_unknown_model_gets_the_fail_safe_row);
  RUN_TEST(test_rows_carry_the_facts_the_design_relies_on);
  RUN_TEST(test_a_routed_row_takes_the_routers_limits);
  RUN_TEST(test_every_refusal_only_narrows_every_row);
  RUN_TEST(test_the_memory_is_per_route_and_reaches_the_lookup);
  RUN_TEST(test_a_full_memory_forgets_its_oldest_entry);
  RUN_TEST(test_each_providers_markers_are_its_own_and_contiguous);
  RUN_TEST(test_slots_and_hosts);
  return UNITY_END();
}
