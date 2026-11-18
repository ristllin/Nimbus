// test_hosted_effects - each chat's recent effects on a Virtual Nimbus (lane RB-FX):
//   (a) an effect lands in <data>/mem/fx/<key>.ring and survives a restart of the instance;
//   (b) chats are kept apart on the volume and in RAM, and the key of a non-numeric chat id
//       is hex, never a path;
//   (c) the volume binding refuses every path outside /mem/fx/ and lists a missing folder
//       as empty;
//   (d) a torn tail on the real disk is dropped at load and the next effect is not glued;
//   (e) the file stays bounded (rewritten by temp and rename, no temp left behind);
//   (f) a volume that cannot be written runs the ring from RAM and says so.
// Offline: no provider traffic.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include "hosted_effects.h"
#include "test_util.h"

using namespace nimbusd;
namespace fx = nimbus::orch::fx;

static std::string freshDir(const std::string& tag) {
  ndtest::rmTree(ndtest::scratchDir(tag));
  return ndtest::scratchDir(tag) + "/data";
}

static std::vector<std::string> receipts(HostedEffects& h, const std::string& chat) {
  std::vector<fx::RingEntry> v;
  h.ring().recent(chat, fx::kRingEntries, v);
  std::vector<std::string> out;
  for (const auto& e : v) out.push_back(e.receipt);
  return out;
}

static bool fileExists(const std::string& p) {
  struct stat st;
  return ::stat(p.c_str(), &st) == 0;
}

static void testStoredAndRestarted(ndtest::Ctx& c) {
  std::printf("\n-- stored and restarted --\n");
  const std::string dir = freshDir("fx-a");
  {
    HostedEffects h(dir);
    c.ok(h.record("123456", 1763470920, "ledger_write", "added L00A3 skyr 150 g") == fx::AppendResult::Stored,
         "an effect is stored");
    c.ok(h.ring().backing("123456") == fx::Backing::Storage, "the ring says it is on the volume");
  }
  c.ok(fileExists(dir + "/mem/fx/123456.ring"), "the file is <data>/mem/fx/<chat>.ring");
  HostedEffects h(dir);
  std::vector<fx::RingEntry> v;
  h.ring().recent("123456", 5, v);
  c.ok(v.size() == 1 && v[0].epoch == 1763470920 && std::string(v[0].tool) == "ledger_write" &&
           std::string(v[0].receipt) == "added L00A3 skyr 150 g" && v[0].stored,
       "after a restart the entry reads back whole");
  ndtest::rmTree(ndtest::scratchDir("fx-a"));
}

static void testChatsApart(ndtest::Ctx& c) {
  std::printf("\n-- chats apart --\n");
  const std::string dir = freshDir("fx-b");
  {
    HostedEffects h(dir);
    for (int i = 0; i < 30; ++i) {
      h.record("-1001", i, "t", "group " + std::to_string(i));
      h.record("web", i, "t", "web " + std::to_string(i));
      h.record("../../etc", i, "t", "odd " + std::to_string(i));
    }
  }
  HostedEffects h(dir);
  const auto g = receipts(h, "-1001"), w = receipts(h, "web"), o = receipts(h, "../../etc");
  bool apart = g.size() == 30 && w.size() == 30 && o.size() == 30;
  for (int i = 0; i < 30 && apart; ++i)
    apart = g[i] == "group " + std::to_string(i) && w[i] == "web " + std::to_string(i) && o[i] == "odd " + std::to_string(i);
  c.ok(apart, "three chats, each reads only its own 30 entries after a restart");
  c.ok(fileExists(dir + "/mem/fx/k776562.ring"), "a non-numeric chat id is keyed as hex");
  std::string p;
  c.ok(fx::ringPath("../../etc", p) && p.find("..") == std::string::npos && fileExists(dir + p),
       "a chat id shaped like a path stays a hex name inside /mem/fx");
  c.ok(receipts(h, "1001").empty(), "a chat with no effects reads none");
  ndtest::rmTree(ndtest::scratchDir("fx-b"));
}

static void testBindingConfined(ndtest::Ctx& c) {
  std::printf("\n-- the binding is confined to /mem/fx --\n");
  const std::string dir = freshDir("fx-c");
  PosixFxFiles f(dir);
  std::vector<std::string> names{"x"};
  c.ok(f.list("/mem/fx", names) && names.empty(), "a missing folder lists as empty");
  c.ok(!f.append("/mem/ledger/owner/x", "a", 1), "a path outside /mem/fx is refused");
  c.ok(!f.append("/mem/fx/../ledger/x", "a", 1), "a path with /.. is refused");
  c.ok(!f.writeWhole("/data/x", "a"), "a whole write outside is refused");
  c.ok(!f.list("/mem", names), "only /mem/fx can be listed");
  c.ok(!fileExists(dir + "/mem/ledger"), "nothing was written outside");
  c.ok(f.append("/mem/fx/1.ring", "ab", 2), "an append inside makes the folder");
  size_t sz = 0, got = 0;
  char buf[8];
  c.ok(f.size("/mem/fx/1.ring", sz) && sz == 2 && f.readAt("/mem/fx/1.ring", 1, buf, 8, got) && got == 1 && buf[0] == 'b',
       "size and readAt read it back");
  c.ok(f.readAt("/mem/fx/1.ring", 9, buf, 8, got) && got == 0, "a read past the end reads nothing");
  c.ok(!f.size("/mem/fx/2.ring", sz) && !f.readAt("/mem/fx/2.ring", 0, buf, 8, got), "a missing file answers false");
  ndtest::rmTree(ndtest::scratchDir("fx-c"));
}

static void testTornTail(ndtest::Ctx& c) {
  std::printf("\n-- torn tail on disk --\n");
  const std::string dir = freshDir("fx-d");
  {
    HostedEffects h(dir);
    h.record("7", 1, "t", "one");
    h.record("7", 2, "t", "two");
  }
  const std::string path = dir + "/mem/fx/7.ring";
  struct stat st;
  ::stat(path.c_str(), &st);
  c.ok(::truncate(path.c_str(), st.st_size - 3) == 0, "the last line is torn on disk");
  {
    HostedEffects h(dir);
    const auto r = receipts(h, "7");
    c.ok(r.size() == 1 && r[0] == "one", "the torn entry is dropped at load");
    h.record("7", 3, "t", "three");
  }
  HostedEffects h(dir);
  const auto r = receipts(h, "7");
  c.ok(r.size() == 2 && r[0] == "one" && r[1] == "three", "the next effect is whole, not glued");
  ndtest::rmTree(ndtest::scratchDir("fx-d"));
}

static void testBounded(ndtest::Ctx& c) {
  std::printf("\n-- bounded file --\n");
  const std::string dir = freshDir("fx-e");
  {
    HostedEffects h(dir);
    for (int i = 0; i < 333; ++i) h.record("8", i, "files.append", "row " + std::to_string(i));
    c.ok(h.ring().stats().rewrites >= 4, "the file was rewritten from RAM");
  }
  struct stat st;
  ::stat((dir + "/mem/fx/8.ring").c_str(), &st);
  c.ok((size_t)st.st_size <= fx::kCompactLines * fx::kLineMax, "the file stays under the rewrite bound");
  c.ok(!fileExists(dir + "/mem/fx/8.ring.tmp"), "no temp is left behind");
  HostedEffects h(dir);
  const auto r = receipts(h, "8");
  c.ok(r.size() == fx::kRingEntries && r.front() == "row 283" && r.back() == "row 332", "the newest 50 survive");
  ndtest::rmTree(ndtest::scratchDir("fx-e"));
}

static void testNoVolume(ndtest::Ctx& c) {
  std::printf("\n-- no writable volume --\n");
  const std::string base = freshDir("fx-f");
  fsutil::mkdirs(fsutil::dirOf(base));
  FILE* blocker = std::fopen(base.c_str(), "w");   // the data dir is a file: nothing can be made under it
  if (blocker) std::fclose(blocker);
  HostedEffects h(base);
  c.ok(h.record("9", 1, "t", "kept") == fx::AppendResult::RamOnly, "the effect is kept in RAM only");
  c.ok(h.ring().backing("9") == fx::Backing::RamOnly, "and the ring says so");
  const auto r = receipts(h, "9");
  c.ok(r.size() == 1 && r[0] == "kept", "it still reads back in this run");
  ndtest::rmTree(ndtest::scratchDir("fx-f"));
}

int main() {
  ndtest::Ctx c;
  std::printf("=== hosted effects (lane RB-FX, offline) ===\n");
  testStoredAndRestarted(c);
  testChatsApart(c);
  testBindingConfined(c);
  testTornTail(c);
  testBounded(c);
  testNoVolume(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
