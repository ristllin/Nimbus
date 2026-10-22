// test_archive_web - the Telegram archive view's server side: Export day as a ZIP
// (streamed in one pass, data descriptors, stored files) and the day page budget
// (prism, wave 1 row 7). The ZIP is checked the way an unzip tool reads it: the
// end record points at the central directory, every central entry points at its
// local header, and each file's bytes, size and CRC-32 match what was archived,
// for every chunk size the response filler may ask for.
#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../test_tg_archive/fakes.h"
#include "nimbus/net/archive_page.h"
#include "nimbus/tg_archive_zip.h"

namespace arc = nimbus::tgarc;

void setUp() {}
void tearDown() {}

static uint32_t u16(const std::string& z, size_t at) {
  return (uint8_t)z[at] | ((uint8_t)z[at + 1] << 8);
}
static uint32_t u32(const std::string& z, size_t at) { return u16(z, at) | (u16(z, at + 2) << 16); }

static std::string streamAll(arc::DayZip& zip, size_t chunk) {
  std::string out;
  std::vector<uint8_t> buf(chunk);
  for (int guard = 0; guard < 1000000; guard++) {
    const size_t n = zip.fill(buf.data(), chunk);
    if (n == 0) break;
    out.append((const char*)buf.data(), n);
  }
  return out;
}

static fakes::Card dayCard(std::map<std::string, std::string>& want) {
  fakes::Card c;
  const std::string dir = arc::dayDirPath("12345", "2026-10-21");
  c.mkdirs(dir);
  std::string photo;
  for (int i = 0; i < 9000; i++) photo += (char)((i * 131 + 7) & 0xFF);
  want["index.jsonl"] = "{\"id\":\"1\",\"kind\":\"photo\"}\n{\"id\":\"2\",\"kind\":\"voice\"}\n";
  want["r00000000000000a1.jpg"] = photo;
  want["r00000000000000a2.ogg"] = std::string(3000, '\x01');
  for (const auto& kv : want) c.files[dir + "/" + kv.first] = kv.second;
  c.files[dir + "/r00000000000000a3.jpg.part"] = "half a file";   // a write in progress
  return c;
}

// Parse the ZIP as an unzip tool does (end record -> central directory -> local
// headers) and compare every file with what was archived.
static void checkZip(const std::string& z, const std::map<std::string, std::string>& want) {
  TEST_ASSERT_TRUE(z.size() >= 22);
  const size_t eocd = z.size() - 22;
  TEST_ASSERT_EQUAL_HEX32(0x06054b50, u32(z, eocd));
  const uint32_t n = u16(z, eocd + 10), cdSize = u32(z, eocd + 12), cdAt = u32(z, eocd + 16);
  TEST_ASSERT_EQUAL((int)want.size(), (int)n);
  TEST_ASSERT_EQUAL(eocd, cdAt + cdSize);
  size_t at = cdAt;
  for (uint32_t i = 0; i < n; i++) {
    TEST_ASSERT_EQUAL_HEX32(0x02014b50, u32(z, at));
    TEST_ASSERT_EQUAL(0, (int)u16(z, at + 10));   // stored
    const uint32_t crc = u32(z, at + 16), size = u32(z, at + 24);
    const uint32_t nameLen = u16(z, at + 28), local = u32(z, at + 42);
    const std::string name = z.substr(at + 46, nameLen);
    TEST_ASSERT_TRUE_MESSAGE(want.count(name), name.c_str());
    const std::string& body = want.at(name);
    TEST_ASSERT_EQUAL((int)body.size(), (int)size);
    TEST_ASSERT_EQUAL_HEX32(arc::zipCrc32(0, (const uint8_t*)body.data(), body.size()), crc);
    // The local header, then the bytes, then the data descriptor.
    TEST_ASSERT_EQUAL_HEX32(0x04034b50, u32(z, local));
    TEST_ASSERT_EQUAL_STRING(name.c_str(), z.substr(local + 30, u16(z, local + 26)).c_str());
    const size_t data = local + 30 + nameLen;
    TEST_ASSERT_TRUE(z.compare(data, body.size(), body) == 0);
    TEST_ASSERT_EQUAL_HEX32(0x08074b50, u32(z, data + body.size()));
    TEST_ASSERT_EQUAL_HEX32(crc, u32(z, data + body.size() + 4));
    at += 46 + nameLen;
  }
}

static void test_crc32_reference_vector() {
  const char* v = "123456789";
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926, arc::zipCrc32(0, (const uint8_t*)v, 9));
  // Chained over two parts equals one pass.
  const uint32_t a = arc::zipCrc32(0, (const uint8_t*)v, 4);
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926, arc::zipCrc32(a, (const uint8_t*)v + 4, 5));
}

// Every chunk size the filler may be asked for yields the same valid ZIP, whose size
// is the one estimated up front (the admission input).
static void test_day_zip_is_valid_for_every_chunk_size() {
  std::map<std::string, std::string> want;
  for (size_t chunk : {1u, 7u, 30u, 100u, 1460u, 4096u, 65536u}) {
    fakes::Card c = dayCard(want);
    arc::DayZip zip(c, "12345", "2026-10-21");
    TEST_ASSERT_EQUAL(3, (int)zip.fileCount());   // the .part write is left out
    const std::string z = streamAll(zip, chunk);
    TEST_ASSERT_FALSE(zip.failed());
    TEST_ASSERT_EQUAL((int)zip.estimatedBytes(), (int)z.size());
    checkZip(z, want);
    if (chunk == 4096 && std::getenv("ZIP_DUMP")) {   // evidence: unzip -t on the bytes
      FILE* f = std::fopen(std::getenv("ZIP_DUMP"), "wb");
      if (f) { std::fwrite(z.data(), 1, z.size(), f); std::fclose(f); }
    }
  }
  fakes::Card c = dayCard(want);
  TEST_ASSERT_EQUAL_STRING("12345-2026-10-21.zip", arc::DayZip(c, "12345", "2026-10-21").downloadName().c_str());
}

// A day the ZIP format cannot hold without Zip64 is refused, not written corrupt.
static void test_oversized_day_is_refused() {
  fakes::Card c;
  const std::string dir = arc::dayDirPath("12345", "2026-10-19");
  c.mkdirs(dir);
  c.files[dir + "/a.jpg"] = "x";
  arc::DayZip small(c, "12345", "2026-10-19");
  TEST_ASSERT_FALSE(small.tooLarge());
  struct BigCard : fakes::Card {
    std::vector<nimbus::tgarc::Entry> list(const std::string&) override {
      std::vector<nimbus::tgarc::Entry> out;
      for (int i = 0; i < 300; i++) {
        nimbus::tgarc::Entry e;
        e.name = "r" + std::to_string(i) + ".mp4";
        e.size = 20ull * 1024 * 1024;   // 300 x 20 MB > 4 GiB
        out.push_back(e);
      }
      return out;
    }
  } big;
  TEST_ASSERT_TRUE(arc::DayZip(big, "12345", "2026-10-19").tooLarge());
}

// A day with nothing archived is a valid, empty ZIP (the route answers 404 first).
static void test_empty_day_is_an_empty_zip() {
  fakes::Card c;
  arc::DayZip zip(c, "12345", "2026-10-20");
  TEST_ASSERT_EQUAL(0, (int)zip.fileCount());
  const std::string z = streamAll(zip, 512);
  TEST_ASSERT_EQUAL(22, (int)z.size());
  checkZip(z, {});
}

// A read that fails mid-file stops the stream and says so: never a ZIP whose
// directory claims bytes that were not sent.
struct FailingCard : fakes::Card {
  struct Bad : nimbus::tgarc::Reader {
    int calls = 0;
    int read(uint8_t* d, size_t cap) override {
      if (calls++ == 0) { memset(d, 'x', cap < 10 ? cap : 10); return (int)(cap < 10 ? cap : 10); }
      return -1;
    }
  };
  std::unique_ptr<nimbus::tgarc::Reader> openRead(const std::string&) override {
    return std::unique_ptr<nimbus::tgarc::Reader>(new Bad());
  }
};
static void test_read_failure_stops_the_stream() {
  std::map<std::string, std::string> want;
  fakes::Card base = dayCard(want);
  FailingCard c;
  c.files = base.files;
  c.dirs = base.dirs;
  arc::DayZip zip(c, "12345", "2026-10-21");
  const std::string z = streamAll(zip, 4096);
  TEST_ASSERT_TRUE(zip.failed());
  TEST_ASSERT_TRUE(z.size() < 200);
  TEST_ASSERT_TRUE(z.find("PK\x05\x06") == std::string::npos);   // no end record
}

// Prism row 7: a 200-row day page stays under the small-reply budget and always
// moves forward.
static void test_day_page_stays_under_budget() {
  std::vector<size_t> rows(200, 300);
  const size_t fit = nimbus::net::archiveRowsThatFit(rows, 160);
  TEST_ASSERT_TRUE(fit > 0 && fit < 200);
  TEST_ASSERT_TRUE(160 + fit * 301 <= nimbus::net::kArchivePageBudget);
  TEST_ASSERT_TRUE(160 + (fit + 1) * 301 > nimbus::net::kArchivePageBudget);
  TEST_ASSERT_TRUE(nimbus::net::kArchivePageBudget < 16 * 1024);
  // One oversized row still goes out alone, so paging never stalls.
  TEST_ASSERT_EQUAL(1, (int)nimbus::net::archiveRowsThatFit({20000, 10}, 160));
  // A short day fits whole.
  TEST_ASSERT_EQUAL(3, (int)nimbus::net::archiveRowsThatFit({100, 100, 100}, 160));
  TEST_ASSERT_EQUAL(0, (int)nimbus::net::archiveRowsThatFit({}, 160));
}

static std::string readFile(const char* path) {
  std::string out;
  FILE* f = std::fopen(path, "rb");
  if (!f) return out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

// The line of every streamed response in `src` that no admission check guards: a
// beginChunkedResponse with no "if (!admission::admit(" between it and the end of
// the previous function (a "}" at the start of a line).
static std::vector<int> ungatedStreams(const std::string& src) {
  std::vector<int> out;
  for (size_t at = src.find("beginChunkedResponse("); at != std::string::npos;
       at = src.find("beginChunkedResponse(", at + 1)) {
    const size_t fnStart = src.rfind("\n}", at);
    const size_t gate = src.rfind("if (!admission::admit(", at);
    const bool gated = gate != std::string::npos && (fnStart == std::string::npos || gate > fnStart);
    if (!gated) out.push_back(1 + (int)std::count(src.begin(), src.begin() + at, '\n'));
  }
  return out;
}

// Prism row 6: archive media (the archive item route and the Files download of a
// Telegram chat folder both stream through streamItem) and the day ZIP go through the
// admission gate, so a big file trickles or answers busy while memory is short. The
// guard reads the route file itself; the counter-cases prove it sees an ungated
// stream and does not credit a gate in another function.
static void test_archive_streams_go_through_admission() {
  const std::string src = readFile("src/net/web_tg_archive.cpp");
  TEST_ASSERT_TRUE_MESSAGE(!src.empty(), "cannot read src/net/web_tg_archive.cpp (run from repo root)");
  TEST_ASSERT_TRUE(src.find("beginChunkedResponse(") != std::string::npos);
  const std::vector<int> bad = ungatedStreams(src);
  TEST_ASSERT_EQUAL_MESSAGE(0, (int)bad.size(), bad.empty() ? "" : "an archive stream skips admission::admit");
  // One file filler for card files: the archive keeps no copy of its own.
  TEST_ASSERT_TRUE(src.find("admission::cardFileFiller(") != std::string::npos);
  TEST_ASSERT_TRUE(src.find("->read(") == std::string::npos);

  const std::string ungated = "void a() {\n  r->beginChunkedResponse(t, fill);\n}\n";
  TEST_ASSERT_EQUAL(1, (int)ungatedStreams(ungated).size());
  const std::string gateElsewhere =
      "void a() {\n  if (!admission::admit(r, n, false, fill)) return;\n}\n"
      "void b() {\n  r->beginChunkedResponse(t, fill);\n}\n";
  TEST_ASSERT_EQUAL(1, (int)ungatedStreams(gateElsewhere).size());
  const std::string gated = "void a() {\n  if (!admission::admit(r, n, false, fill)) return;\n"
                            "  r->beginChunkedResponse(t, fill);\n}\n";
  TEST_ASSERT_EQUAL(0, (int)ungatedStreams(gated).size());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_crc32_reference_vector);
  RUN_TEST(test_day_zip_is_valid_for_every_chunk_size);
  RUN_TEST(test_empty_day_is_an_empty_zip);
  RUN_TEST(test_oversized_day_is_refused);
  RUN_TEST(test_read_failure_stops_the_stream);
  RUN_TEST(test_day_page_stays_under_budget);
  RUN_TEST(test_archive_streams_go_through_admission);
  return UNITY_END();
}
