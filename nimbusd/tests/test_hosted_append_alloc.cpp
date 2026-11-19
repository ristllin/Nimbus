// test_hosted_append_alloc - RB-REC1 (ruling 30, owner ruling 41): the hosted files.append under
// an allocator that refuses. This binary replaces operator new: armed, it refuses the Nth
// allocation from arming on (N = 0: every one). For a food log, an expenses log and a training
// log alike, at every allocation of the call: the append answers a clean "out of memory" or
// lands its rows whole, never an exception; on an error the file's bytes, its sha8 and its
// index entry are what they were; and once memory is back the next append lands.
// Offline: no provider traffic at all.
#include <sys/stat.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>

#include "posix_files.h"
#include "test_util.h"

using namespace nimbusd;

static bool gArmed = false, gHit = false;
static long gAllocs = 0, gFailAt = -1;
void* operator new(std::size_t n) {
  if (gArmed && gAllocs++ >= gFailAt) {
    gHit = true;
    throw std::bad_alloc();
  }
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

// The bytes on the disk, read with no operator new (a fixed buffer).
static std::string disk(const std::string& path) {
  std::string out;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

static std::string sha8(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
  return std::string(buf, 8);
}

struct Table {
  const char *project, *name, *seed, *row;
};

static void sweep(ndtest::Ctx& c, const Table& t, bool seeded) {
  const std::string dir = ndtest::scratchDir(std::string("haa-") + t.project + (seeded ? "-s" : "-n"));
  const std::string path = dir + "/" + t.project + "/" + t.name, where = std::string(t.project) + "/" + t.name;
  const std::string text = std::string(t.row) + "\n" + t.row;   // two rows, no final line end
  int refusals = 0, steps = 0;
  bool clean = true;
  for (long k = 0;; ++k, ++steps) {
    ndtest::rmTree(dir);
    mkdir(dir.c_str(), 0755);
    PosixFiles files(dir);
    if (seeded) files.seed(where, t.seed);   // stored with no final line end
    const std::string before = disk(path);
    const std::string hashBefore = sha8(before);
    const bool listed = files.has(where);
    std::string err, note;
    err.reserve(64);
    note.reserve(256);
    bool ok = false, threw = false;
    gAllocs = 0;
    gFailAt = k;
    gHit = false;
    gArmed = true;
    try {
      ok = files.append(t.project, t.name, text, err, note);
    } catch (...) {
      threw = true;
    }
    gArmed = false;
    const std::string after = disk(path);
    if (threw) {
      c.ok(false, "an exception left files.append at allocation " + std::to_string(k));
      clean = false;
      break;
    }
    if (!gHit) {   // every allocation of the call has been refused once
      const std::string want = before + (before.empty() ? "" : "\n") + t.row + "\n" + t.row + "\n";
      c.ok(ok && after == want, std::string("with memory the rows land whole: ") + t.name);
      break;
    }
    if (ok) {
      // A refusal after the bytes landed (the index file write) still appended them whole.
      clean = clean && after == before + (before.empty() ? "" : "\n") + t.row + "\n" + t.row + "\n";
      continue;
    }
    ++refusals;
    clean = clean && err == nimbus::orch::kAppendNoMemory && after == before && sha8(after) == hashBefore &&
            files.has(where) == listed;
    // Memory is back: the same store appends.
    std::string e2, n2;
    clean = clean && files.append(t.project, t.name, t.row, e2, n2) && disk(path).size() > before.size();
  }
  c.ok(clean, std::string("every refusal is a clean error with the file as it was: ") + t.name +
                  (seeded ? " (stored)" : " (new)"));
  c.ok(refusals >= 3, std::string("the sweep reached the call's allocations (") + std::to_string(refusals) +
                          " refusals in " + std::to_string(steps) + " steps): " + t.name);
}

int main() {
  ndtest::Ctx c;
  std::printf("=== hosted files.append under a refusing allocator (ruling 30, offline) ===\n");
  const Table tables[] = {
      {"food", "food-log.csv", "date,time,item,qty,kcal\n2026-11-19,08:10,oats,1,350", "2026-11-19,12:40,soup,1,300"},
      {"money", "expenses.csv", "date,time,item,qty,amount\n2026-11-19,08:10,coffee,1,3.20", "2026-11-19,12:40,train,1,24.50"},
      {"gym", "training.csv", "date,time,item,qty,kg\n2026-11-19,08:10,row,5,60", "2026-11-19,12:40,squat,5,80"},
  };
  for (const Table& t : tables) {
    std::printf("  -- %s --\n", t.name);
    sweep(c, t, true);
    sweep(c, t, false);
  }
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
