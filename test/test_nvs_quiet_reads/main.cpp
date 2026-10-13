// Class guard for quiet NVS string reads (lane L31, bench 2026-10-13).
//
// A factory-fresh device in setup mode logged
//   [E][Preferences.cpp:506] getString(): nvs_get_str len fail: staSsid NOT_FOUND
// every 5 s: Arduino Preferences::getString() (what solide::memory::getString runs)
// logs an error for every read of a key that was never written, and nothing is
// saved in the Wi-Fi slot until the owner onboards. The fix is one quiet getter,
// nimbus::sys::nvsGetStr (src/sys/config_nvs.h), that probes has() first, so an
// absent key reads as its default with no log line. These pin the CLASS
// (AGENTS.md section 3), not the one key: no firmware source outside that getter
// calls solide::memory::getString (the settings store every setting read goes
// through), so a new reader of ANY possibly-unset setting fails here instead of
// shipping a fresh-device log flood. Direct Preferences handles opened by a few
// subsystems at boot are outside this seam. The device half (no error line on
// glass) is the bench check in the PR.

#include <unity.h>

#include <cstdio>
#include <filesystem>
#include <string>

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

static size_t countOf(const std::string& hay, const std::string& needle) {
  size_t n = 0;
  for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) ++n;
  return n;
}

static const char* kGetter = "src/sys/config_nvs.h";   // home of nvsGetStr

// Every firmware source under src/ and include/: no loud string read anywhere but
// inside the quiet getter itself. Bring-up sketches (src/bringup/) are serial
// diagnostics for a person at a bench, not the product image, so they are exempt.
static void test_no_loud_string_reads_in_the_firmware() {
  int scanned = 0;
  std::string offenders;
  for (const char* root : {"src", "include"}) {
    for (const auto& e : std::filesystem::recursive_directory_iterator(root)) {
      if (!e.is_regular_file()) continue;
      const std::string path = e.path().generic_string();
      const std::string ext = e.path().extension().string();
      if (ext != ".cpp" && ext != ".h") continue;
      if (path.rfind("src/bringup/", 0) == 0) continue;
      ++scanned;
      const size_t hits = countOf(readFile(path), "memory::getString(");
      const size_t allowed = (path == kGetter) ? 1 : 0;   // the getter's own body
      if (hits != allowed) offenders += path + " ";
    }
  }
  TEST_ASSERT_TRUE(scanned > 100);   // the walk really covered the firmware
  TEST_ASSERT_EQUAL_STRING_MESSAGE("", offenders.c_str(),
                                   "read NVS strings through nimbus::sys::nvsGetStr");
}

// The getter itself probes has() before it ever calls the logging read.
static void test_the_getter_probes_before_it_reads() {
  const std::string h = readFile(kGetter);
  const size_t fn = h.find("inline String nvsGetStr(const char* key, const char* def = \"\")");
  TEST_ASSERT_TRUE(fn != std::string::npos);
  const std::string body = h.substr(fn, h.find('}', fn) - fn);
  TEST_ASSERT_TRUE(body.find("solide::memory::has(key) ? solide::memory::getString(key, def)"
                             " : String(def)") != std::string::npos);
  // The persist seam's reads (agent::store, the Wi-Fi list) use it too.
  TEST_ASSERT_TRUE(h.find("String getString(const char* k, const char* def) { return nvsGetStr(k, def); }") !=
                   std::string::npos);
}

// The instance that flooded the log: every Wi-Fi slot reader goes through it.
static void test_the_wifi_slot_readers_are_quiet() {
  for (const char* path : {"src/net/wifi_link.cpp", "src/net/wifi_portal.cpp",
                           "src/net/wifi_store.cpp", "src/net/webui.cpp"}) {
    const std::string src = readFile(path);
    TEST_ASSERT_TRUE_MESSAGE(src.find("nvsGetStr(NIMBUS_KEY_STA_SSID)") != std::string::npos, path);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_no_loud_string_reads_in_the_firmware);
  RUN_TEST(test_the_getter_probes_before_it_reads);
  RUN_TEST(test_the_wifi_slot_readers_are_quiet);
  return UNITY_END();
}
