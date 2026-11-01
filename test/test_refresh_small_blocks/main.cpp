// A model-list refresh holds nothing per row in small blocks.
//
// Why this exists: Lumi's internal SRAM low-water mark fell to 968 B during the Cumulo
// router's list refresh (10 minutes after each update, on the 8 KB verify task). On the device
// a plain block of 128 B or less stays in internal SRAM; only a turn sends small blocks to PSRAM.
// protectHeads and keepOrder held three vectors of one string per row, each under that limit:
// 155 rows x 3 x ~36 B, about 17 KB out of the ~22 KB the worker had left.
//
// The test counts live blocks within the device's 128 B limit through a whole fetch's pure steps
// (the same order as provider_verify's storeModelHarvest) and fails when that peak grows with
// the row count. Portable: a host std::string keeps a few more characters in the object than
// the device's does, which only makes this host count lower, and the row ids here are longer
// than either in-object buffer.
#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "nimbus/harness/router_catalog.h"
#include "nimbus/orch/model_catalog.h"

namespace {
// nimbus::heapwatch::kSmallAllocInternalLimit. IDF's malloc keeps a block of this size or less
// in internal SRAM: heap_caps_malloc_default, `size <= malloc_alwaysinternal_limit`.
constexpr size_t kInternalLimit = 128;
size_t g_live = 0;                       // live bytes in blocks within the limit
size_t g_peak = 0;
bool   g_on = false;

struct Head {
  size_t n;
  size_t pad;
};

void* counted(size_t n) {
  void* raw = std::malloc(n + sizeof(Head));
  if (!raw) return nullptr;
  static_cast<Head*>(raw)->n = n;
  if (g_on && n <= kInternalLimit) {
    g_live += n;
    if (g_live > g_peak) g_peak = g_live;
  }
  return static_cast<Head*>(raw) + 1;
}

void uncounted(void* p) {
  if (!p) return;
  Head* h = static_cast<Head*>(p) - 1;
  if (g_on && h->n <= kInternalLimit && g_live >= h->n) g_live -= h->n;
  std::free(h);
}
}  // namespace

void* operator new(size_t n) {
  void* p = counted(n);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](size_t n) {
  void* p = counted(n);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { uncounted(p); }
void operator delete[](void* p) noexcept { uncounted(p); }
void operator delete(void* p, size_t) noexcept { uncounted(p); }
void operator delete[](void* p, size_t) noexcept { uncounted(p); }

void setUp() {}
void tearDown() {}

using namespace nimbus::orch;

namespace {
const char* const kDefault = "openai/gpt-5.6";

// One router body shaped like the live list: <upstream>/<model> rows with a price, the model
// names long enough that no std::string keeps them in the object.
std::string routerBody(int rows) {
  static const char* const kUps[][2] = {{"anthropic", "claude-sonnet-long-name-5"},
                                        {"openai", "gpt-6-long-variant-name"},
                                        {"mistral", "mistral-large-long-name"},
                                        {"zai", "glm-5-long-model-name"}};
  std::string b = "{\"object\":\"list\",\"data\":[";
  char row[256];
  for (int i = 0; i < rows; ++i) {
    const auto& u = kUps[i % 4];
    std::snprintf(row, sizeof row,
                  "%s{\"id\":\"%s/%s-%d\",\"object\":\"model\",\"owned_by\":\"%s\",\"size_class\":\"large\","
                  "\"in_micro_per_mtok\":%d,\"out_micro_per_mtok\":%d}",
                  i ? "," : "", u[0], u[1], i, u[0], 1000 + i, 2000 + i);
    b += row;
  }
  b += "]}";
  return b;
}

struct Pass {
  size_t kept = 0;
  size_t peak = 0;
};

// provider_verify.cpp's fetch for the router, its pure steps in its order: the rows, then
// storeModelHarvest. Peak of the blocks within the limit, past the rows' own (which the row
// bound and the harvest's off-internal rule already keep to PSRAM on the device).
Pass runFetch(const std::string& body) {
  ModelHarvest h;
  h.provider = "cumulo";
  h.now = 0;
  harvestBody(h, body.data(), body.size());
  g_live = g_peak = 0;
  g_on = true;
  finishHarvest(h);
  size_t more = h.overflow + agent::router_catalog::protectHeads(h.rows, kDefault);
  more += boundModelRows(h.rows);
  ModelStoreMeta meta;
  meta.total = (uint32_t)(h.rows.size() + more);
  meta.more = (uint32_t)more;
  meta.src = "api";
  meta.choices = agent::router_catalog::storeChoices(h.rows, kDefault, h.now);
  const std::string text = serializeModelStore(meta, h.rows);
  std::vector<ModelRate> prices;
  const std::vector<std::string> keep = agent::router_catalog::priceKeepIds(meta.choices, kDefault, "");
  agent::router_catalog::routerRates(h.rows, keep, prices);
  g_on = false;
  Pass p;
  p.kept = h.rows.size();
  p.peak = g_peak;
  return p;
}
}  // namespace

// The class rule: twice the rows may not cost twice the small blocks. A vector of per-row
// strings under the limit (the 968 B path) fails here whatever its size.
static void test_small_block_peak_does_not_grow_with_the_rows() {
  const Pass few = runFetch(routerBody(155));
  const Pass many = runFetch(routerBody(310));
  std::printf("small blocks peak: %u B at %u rows, %u B at %u rows\n", (unsigned)few.peak, (unsigned)few.kept,
              (unsigned)many.peak, (unsigned)many.kept);
  TEST_ASSERT_GREATER_THAN_size_t(100, few.kept);        // the rows really went through
  TEST_ASSERT_GREATER_THAN_size_t(few.kept, many.kept);
  TEST_ASSERT_LESS_THAN_size_t(512, many.peak > few.peak ? many.peak - few.peak : 0);
}

// The margin the worker's spawn gate leaves is 16 KB, and a TLS connection takes its share of
// that. The whole store step stays far under it at the size of the live list.
static void test_small_block_peak_is_small_at_the_live_list_size() {
  const Pass p = runFetch(routerBody(223));
  TEST_ASSERT_LESS_THAN_size_t(2048, p.peak);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_small_block_peak_does_not_grow_with_the_rows);
  RUN_TEST(test_small_block_peak_is_small_at_the_live_list_size);
  return UNITY_END();
}
