// test_hosted_compact - the engine-2 compaction driver's files on a Virtual Nimbus (lane RB-C2,
// hosted_fold.h hostedTextIO): the control record and the chatsum mirror survive a restart of
// the instance on the real disk, a missing file reads as fresh, and a volume that cannot be
// written answers false (the driver then keeps its state in RAM). Offline.
#include <sys/stat.h>

#include <cstdio>
#include <string>

#include "hosted_fold.h"
#include "test_util.h"

int main() {
  ndtest::Ctx c;
  std::printf("=== hosted compaction files (lane RB-C2, offline) ===\n");
  const std::string dir = ndtest::scratchDir("hosted_compact");
  ndtest::rmTree(dir);
  mkdir(dir.c_str(), 0700);
  const std::string path = dir + "/compact_ctl.txt";
  agent::plain::TextIO io = nimbusd::hostedTextIO(path);
  c.ok(io.load().empty(), "a missing file reads as empty");
  agent::plain::CompactCtl ctl;
  ctl.day = 20409;
  ctl.paid = 4;
  ctl.chat("c1").fails = 2;
  ctl.chat("c1").next = 1794916800ULL;
  c.ok(io.save(ctl.encode()), "the control record is saved");
  // A restart: a new binding over the same path reads it back.
  const agent::plain::CompactCtl back = agent::plain::CompactCtl::decode(nimbusd::hostedTextIO(path).load());
  c.ok(back.paid == 4 && back.day == 20409, "the day's attempts survive a restart");
  c.ok(back.chats.size() == 1 && back.chats[0].fails == 2 && back.chats[0].next == 1794916800ULL,
       "the chat's failures and next try survive a restart");
  agent::plain::TextIO bad = nimbusd::hostedTextIO(path + "/x.txt")   /* its parent is a file */;
  c.ok(!bad.save("x"), "a volume that cannot be written answers false");
  ndtest::rmTree(dir);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
