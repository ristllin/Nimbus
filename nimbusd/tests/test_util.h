#pragma once
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

// test_util - a dependency-free assertion harness for the nimbusd host tests.
//
// nimbusd is a plain Makefile target (like harness-lab), not a PlatformIO
// `native` env, so it does not get Unity. These tests are ordinary executables:
// each registers checks, the runner prints ok/FAIL lines and a summary, and the
// process exits non-zero if anything failed - the marker a CI tier greps for.
namespace ndtest {

struct Ctx {
  const char* suite = "";
  int checks = 0;
  int failures = 0;

  void ok(bool cond, const std::string& what) {
    checks++;
    if (cond) {
      std::printf("    ok   %s\n", what.c_str());
    } else {
      failures++;
      std::printf("    FAIL %s\n", what.c_str());
    }
  }

  void eq(const std::string& got, const std::string& want, const std::string& what) {
    if (got == want) {
      ok(true, what);
    } else {
      ok(false, what + "  (got \"" + got + "\" want \"" + want + "\")");
    }
  }

  void eqi(long got, long want, const std::string& what) {
    if (got == want) {
      ok(true, what);
    } else {
      ok(false, what + "  (got " + std::to_string(got) + " want " +
                    std::to_string(want) + ")");
    }
  }
};

// A scratch directory unique to this process, cleaned on demand. Lives under
// $TMPDIR (or /tmp) so nothing lands in the project tree.
inline std::string scratchDir(const std::string& tag) {
  const char* base = std::getenv("TMPDIR");
  std::string dir = std::string(base && *base ? base : "/tmp") + "/nimbusd-test-" +
                    tag + "-" + std::to_string((long)getpid());
  return dir;
}

inline void rmTree(const std::string& dir) {
  // Test-local scratch under $TMPDIR only; a plain system() rm is fine here (it
  // is outside the project tree, so no guardrail concern).
  std::string cmd = "rm -rf '" + dir + "'";
  int rc = std::system(cmd.c_str());
  (void)rc;
}

}  // namespace ndtest
