// test_platform - offline (T1) proof of the cgroup v2 heap mapping that lets the
// engine's memory gates degrade a hosted instance gracefully BEFORE the kernel
// OOM-kills the pod. Fixture files stand in for /sys/fs/cgroup, so the mapping
// (available - reserve, clamped) is asserted deterministically with no cgroup.
#include <string>

#include "posix_fs.h"
#include "posix_platform.h"
#include "test_util.h"

using namespace nimbusd;

static void writeFixture(const std::string& path, const std::string& v) {
  fsutil::writeFileAtomic(path, v);
}

int main() {
  ndtest::Ctx c;
  std::printf("=== cgroup heap mapping (T1, offline) ===\n");

  const std::string dir = ndtest::scratchDir("cg");
  ndtest::rmTree(dir);
  const std::string curP = dir + "/memory.current";
  const std::string maxP = dir + "/memory.max";

  const uint64_t MiB = 1024 * 1024;
  const uint64_t reserve = 32 * MiB;
  const uint64_t cap = 64 * MiB;

  // ---- 1. plenty of headroom -> reports the cap (no gating) -----------------
  writeFixture(maxP, "536870912\n");  // 512 MiB limit
  writeFixture(curP, "104857600\n");  // 100 MiB used -> ~412 MiB avail
  {
    CgroupMemory m;
    m.configure(curP, maxP, reserve, cap);
    c.eqi(m.freeBytes(), (long)cap, "abundant headroom clamps to the cap (gates never fire)");
  }

  // ---- 2. near the limit -> reports available-minus-reserve (gates fire) ----
  // 512 MiB limit, 500 MiB used -> 12 MiB available; minus 32 MiB reserve -> 0.
  writeFixture(curP, std::to_string(512 * MiB - 12 * MiB) + "\n");
  {
    CgroupMemory m;
    m.configure(curP, maxP, reserve, cap);
    c.eqi(m.freeBytes(), 0, "12 MiB free under a 32 MiB reserve reports 0 (engine sheds load)");
  }

  // ---- 3. a mid-band value maps linearly ------------------------------------
  // 512 MiB limit, used so that available = 40 MiB; minus 32 MiB reserve -> 8 MiB.
  writeFixture(curP, std::to_string(512 * MiB - 40 * MiB) + "\n");
  {
    CgroupMemory m;
    m.configure(curP, maxP, reserve, cap);
    c.eqi(m.freeBytes(), (long)(8 * MiB), "40 MiB free reports 8 MiB after the reserve");
  }

  // ---- 4. unlimited cgroup ("max") -> the cap (dev/unconstrained) -----------
  writeFixture(maxP, "max\n");
  {
    CgroupMemory m;
    m.configure(curP, maxP, reserve, cap);
    c.eqi(m.freeBytes(), (long)cap, "an unlimited cgroup never gates (reports the cap)");
  }

  // ---- 5. no cgroup files at all (macOS/dev) -> the cap, never a crash -------
  {
    CgroupMemory m;
    m.configure(dir + "/nope.current", dir + "/nope.max", reserve, cap);
    c.eqi(m.freeBytes(), (long)cap, "missing cgroup files fall back to the cap safely");
  }

  ndtest::rmTree(dir);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
