#include <unity.h>

#include <string>

#include "nimbus/orch/result_store.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// An admin (readAll) sees every entry; a tenant sees only its own namespace.
static Principal admin() { return principalForRole("ownerns", Role::Admin); }
static Principal tenant(const char* ns) { return principalForRole(ns, Role::User); }

// ---- put/get round-trip + tag forms ------------------------------------------
static void test_put_get_roundtrip() {
  ResultStore s;
  std::string t1 = s.put("tool", "memory_search", "the full result body", 1000);
  TEST_ASSERT_EQUAL_STRING("r1", t1.c_str());
  std::string t2 = s.put("sub", "gpt-5.5", "sub agent result", 2000, "job0007");
  TEST_ASSERT_EQUAL_STRING("sub:job0007", t2.c_str());
  std::string out;
  size_t total = 0;
  TEST_ASSERT_TRUE(s.get("r1", 0, 0, out, total, admin()));
  TEST_ASSERT_EQUAL_STRING("the full result body", out.c_str());
  TEST_ASSERT_EQUAL_UINT32(20, (uint32_t)total);
  TEST_ASSERT_FALSE(s.get("nope", 0, 0, out, total, admin()));
}

// ---- pagination: offset windows cover the whole text -------------------------
static void test_get_pagination() {
  ResultStore s;
  std::string big(1000, 'x');
  big += "END";
  std::string tag = s.put("tool", "web_search", big, 0);
  std::string out;
  size_t total = 0;
  TEST_ASSERT_TRUE(s.get(tag, 0, 400, out, total, admin()));
  TEST_ASSERT_EQUAL_UINT32(1003, (uint32_t)total);
  TEST_ASSERT_EQUAL_UINT32(400, (uint32_t)out.size());
  std::string rest, acc = out;
  size_t off = out.size();
  while (off < total) {
    TEST_ASSERT_TRUE(s.get(tag, off, 400, rest, total, admin()));
    acc += rest;
    off += rest.size();
    TEST_ASSERT_TRUE(rest.size() > 0);
  }
  TEST_ASSERT_EQUAL_STRING(big.c_str(), acc.c_str());
  // offset past end: valid tag, empty window.
  TEST_ASSERT_TRUE(s.get(tag, total + 5, 100, out, total, admin()));
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)out.size());
}

// ---- slot ring: 17th put evicts the oldest -----------------------------------
static void test_slot_ring_eviction() {
  ResultStore s;
  std::string first = s.put("tool", "t", "first", 0);
  for (int i = 0; i < ResultStore::kSlots; i++) s.put("tool", "t", "filler", 0);
  TEST_ASSERT_EQUAL_UINT32(ResultStore::kSlots, (uint32_t)s.count());
  std::string out;
  size_t total;
  TEST_ASSERT_FALSE(s.get(first, 0, 0, out, total, admin()));  // oldest evicted
}

// ---- byte ring: total cap evicts oldest --------------------------------------
static void test_byte_ring_eviction() {
  ResultStore s;
  // 9 entries x 64KB(clipped) = 576KB > 512KB total => oldest evicted.
  std::string big(ResultStore::kEntryMax + 100, 'a');  // clips to kEntryMax
  std::string first = s.put("tool", "t0", big, 0);
  for (int i = 1; i < 9; i++) s.put("tool", ("t" + std::to_string(i)).c_str(), big, 0);
  std::string out;
  size_t total;
  TEST_ASSERT_FALSE(s.get(first, 0, 0, out, total, admin()));
  TEST_ASSERT_TRUE(s.count() <= 8);
}

// ---- same-tag re-put replaces -------------------------------------------------
static void test_same_tag_replaces() {
  ResultStore s;
  s.put("sub", "m", "v1", 0, "jobA");
  s.put("sub", "m", "v2 newer", 0, "jobA");
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)s.count());
  std::string out;
  size_t total;
  TEST_ASSERT_TRUE(s.get("sub:jobA", 0, 0, out, total, admin()));
  TEST_ASSERT_EQUAL_STRING("v2 newer", out.c_str());
}

// ---- registrar: get pages + miss points at episodic --------------------------
static void test_registered_tools() {
  ResultStore s;
  s.put("tool", "memory_search", "stored text here", 7);
  ToolRegistry reg;
  ResultHandlers h;
  h.get = [&s](const std::string& tag, size_t off, size_t maxB, std::string& out,
               size_t& total, const Principal& w) { return s.get(tag, off, maxB, out, total, w); };
  h.list = [&s](const Principal& w) { return s.list(w); };
  registerResultTools(reg, h);

  ArduinoJson::JsonDocument d1;
  deserializeJson(d1, "{\"tag\":\"r1\"}");
  auto ok = reg.dispatch("results.get", d1.as<ArduinoJson::JsonObjectConst>(), admin());
  TEST_ASSERT_TRUE(ok.success);
  TEST_ASSERT_TRUE(ok.output.find("bytes 0-16 of 16") == 0);
  TEST_ASSERT_TRUE(ok.output.find("stored text here") != std::string::npos);

  ArduinoJson::JsonDocument d2;
  deserializeJson(d2, "{\"tag\":\"r99\"}");
  auto miss = reg.dispatch("results.get", d2.as<ArduinoJson::JsonObjectConst>(), admin());
  TEST_ASSERT_FALSE(miss.success);
  TEST_ASSERT_TRUE(miss.error.find("memory.episodic") != std::string::npos);

  ArduinoJson::JsonDocument d3;
  deserializeJson(d3, "{}");
  auto lst = reg.dispatch("results.list", d3.as<ArduinoJson::JsonObjectConst>(), admin());
  TEST_ASSERT_TRUE(lst.success);
  TEST_ASSERT_TRUE(lst.output.find("r1 tool memory_search 16B") != std::string::npos);
}

// ---- null handlers fail soft --------------------------------------------------
static void test_null_handlers_fail_soft() {
  ToolRegistry reg;
  registerResultTools(reg, ResultHandlers{});
  ArduinoJson::JsonDocument d;
  deserializeJson(d, "{\"tag\":\"r1\"}");
  auto r = reg.dispatch("results.get", d.as<ArduinoJson::JsonObjectConst>(), admin());
  TEST_ASSERT_FALSE(r.success);
  TEST_ASSERT_TRUE(r.error.find("not supported") != std::string::npos);
}


// ---- CRITICAL (prism 2026-08-05): the ring is a DATA BOUNDARY ----------------
// The ring holds full tool outputs + sub-agent replies from every turn. Without
// namespace scoping, any approved chat (or the LAN /mcp client) could read the
// admin's data. Mutation check: delete visibleTo()'s ns comparison and this
// goes red.
static void test_tenant_cannot_read_another_namespace() {
  ResultStore s;
  s.put("tool", "memory_search", "ADMIN SECRET", 0, "", admin().ns);
  s.put("sub", "m", "guest work", 0, "jobG", tenant("guestchat").ns);
  std::string out;
  size_t total = 0;
  // The guest sees its own entry...
  TEST_ASSERT_TRUE(s.get("sub:jobG", 0, 0, out, total, tenant("guestchat")));
  TEST_ASSERT_EQUAL_STRING("guest work", out.c_str());
  // ...but the admin's is INVISIBLE, and indistinguishable from missing.
  TEST_ASSERT_FALSE(s.get("r1", 0, 0, out, total, tenant("guestchat")));
  // list() is scoped the same way - no tag leakage to probe with.
  const std::string gl = s.list(tenant("guestchat"));
  TEST_ASSERT_TRUE(gl.find("sub:jobG") != std::string::npos);
  TEST_ASSERT_TRUE(gl.find("r1") == std::string::npos);
  // The admin (readAll) sees both.
  TEST_ASSERT_TRUE(s.get("r1", 0, 0, out, total, admin()));
  TEST_ASSERT_EQUAL_STRING("ADMIN SECRET", out.c_str());
}

// A device-internal spill (empty ns) is admin-only - never guest-readable.
static void test_unowned_entry_is_admin_only() {
  ResultStore s;
  s.put("tool", "web_search", "device internal", 0);   // no ns
  std::string out; size_t total = 0;
  TEST_ASSERT_FALSE(s.get("r1", 0, 0, out, total, tenant("guestchat")));
  TEST_ASSERT_TRUE(s.get("r1", 0, 0, out, total, admin()));
}

// A crafted sub-agent tag cannot forge another entry's namespace-key.
static void test_crafted_jobtag_cannot_forge_a_tag() {
  ResultStore s;
  const std::string t = s.put("sub", "m", "x", 0, "evil:r1", "ns");
  TEST_ASSERT_TRUE(t.find("sub:evil_r1") == 0);   // the ':' was neutralized
}

// ---- CRITICAL (prism): the page + its header must fit the loop's clamp -------
// The head loop clips the WHOLE tool result at maxToolResultBytes. If the view
// were sized at the clamp, the header would survive while payload bytes were
// clipped off the tail - and the header is exactly what the model uses as its
// paging cursor, so it would page past bytes it never saw. Mutation check:
// make viewCap return 0 (falling back to a literal >= cap) and this goes red.
static void test_page_plus_header_fits_the_clamp() {
  ResultStore s;
  s.put("tool", "web_search", std::string(60000, 'z'), 0, "", tenant("c1").ns);
  ToolRegistry reg;
  ResultHandlers h;
  h.get = [&s](const std::string& tag, size_t off, size_t maxB, std::string& out,
               size_t& total, const Principal& w) { return s.get(tag, off, maxB, out, total, w); };
  h.list = [&s](const Principal& w) { return s.list(w); };
  const size_t clamp = 8192;                 // the derived per-result cap
  h.viewCap = [clamp] { return clamp; };
  registerResultTools(reg, h);
  ArduinoJson::JsonDocument d;
  deserializeJson(d, "{\"tag\":\"r1\"}");
  auto r = reg.dispatch("results.get", d.as<ArduinoJson::JsonObjectConst>(), tenant("c1"));
  TEST_ASSERT_TRUE(r.success);
  // The ENTIRE returned result (header + payload) must survive the clamp intact.
  TEST_ASSERT_TRUE_MESSAGE(r.output.size() <= clamp,
                           "page+header exceeds the clamp - the loop would clip the payload");
  // And the header must describe the payload the model actually keeps.
  const size_t nl = r.output.find('\n');
  const size_t payload = r.output.size() - (nl + 1);
  TEST_ASSERT_TRUE(r.output.find("bytes 0-" + std::to_string((unsigned)payload) + " of 60000") == 0);
}

// A caller-supplied offset landing mid-codepoint must not yield a leading
// continuation byte (invalid UTF-8 400s the next provider request).
static void test_offset_aligns_to_utf8_boundary() {
  ResultStore s;
  std::string txt;
  for (int i = 0; i < 100; i++) txt += "\xE2\x82\xAC";   // 3-byte chars
  s.put("tool", "t", txt, 0, "", tenant("c1").ns);
  std::string out; size_t total = 0;
  TEST_ASSERT_TRUE(s.get("r1", 1, 30, out, total, tenant("c1")));   // offset 1 = mid-char
  TEST_ASSERT_TRUE(out.size() > 0);
  TEST_ASSERT_TRUE_MESSAGE(((unsigned char)out[0] & 0xC0) != 0x80,
                           "window starts on a continuation byte");
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_put_get_roundtrip);
  RUN_TEST(test_get_pagination);
  RUN_TEST(test_slot_ring_eviction);
  RUN_TEST(test_byte_ring_eviction);
  RUN_TEST(test_same_tag_replaces);
  RUN_TEST(test_registered_tools);
  RUN_TEST(test_null_handlers_fail_soft);
  RUN_TEST(test_tenant_cannot_read_another_namespace);
  RUN_TEST(test_unowned_entry_is_admin_only);
  RUN_TEST(test_crafted_jobtag_cannot_forge_a_tag);
  RUN_TEST(test_page_plus_header_fits_the_clamp);
  RUN_TEST(test_offset_aligns_to_utf8_boundary);
  return UNITY_END();
}
