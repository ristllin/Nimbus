#include <unity.h>

#include <climits>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "nimbus/crash_summary.h"
#include "nimbus/exchange_trace.h"
#include "nimbus/logring.h"
#include "nimbus/orch/turn_journal.h"
#include "nimbus/sys/boot_record.h"
#include "nimbus/sys/breadcrumb.h"
#include "nimbus/sys/log_window.h"
#include "nimbus/sys/panic_arm.h"
#include "nimbus/sys/reset_reason.h"

// test_crash_forensics - the portable halves of boot and crash forensics (the merged
// crash-forensics and panic suites). Every test pins a CLASS (AGENTS.md section 3):
// every reset value, every backtrace depth, every dedupe transition, every request
// kind, not one instance.

using namespace nimbus::sys;
using namespace nimbus::crash;

void setUp() {}
void tearDown() {}

static const std::vector<std::string> kNoSecrets;

static bool contains(const std::string& s, const std::string& n) { return s.find(n) != std::string::npos; }

// ---- reset reasons: the one table ---------------------------------------------

// Every in-range value has a row at its own index, so the device static_asserts that
// mirror ESP_RST_* onto this enum cover the whole table.
static void test_reset_table_rows_are_positional() {
  for (int v = 0; v < kResetReasonCount; ++v) {
    const ResetEntry* e = resetEntry(v);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(v, (int)e->id);
    TEST_ASSERT_TRUE(e->bootRow && e->bootRow[0]);
    TEST_ASSERT_TRUE(e->wake && e->wake[0]);
  }
  TEST_ASSERT_EQUAL_INT(kResetReasonCount - 1, kLastResetReason);
  TEST_ASSERT_NULL(resetEntry(-1));
  TEST_ASSERT_NULL(resetEntry(kResetReasonCount));
}

// "unknown" iff the value is ESP_RST_UNKNOWN (0) or out of range, and then always with
// the number. In both vocabularies. (verdict 2: never "none of them map to unknown")
static void test_unknown_only_for_zero_and_out_of_range() {
  const int outOfRange[] = {-1, 16, 17, 99, INT_MAX, INT_MIN};
  for (int vocab = 0; vocab < 2; ++vocab) {
    const ResetVocab vc = vocab ? ResetVocab::Wake : ResetVocab::BootRow;
    for (int v = 0; v < kResetReasonCount; ++v) {
      const std::string s = resetSlug(v, vc);
      if (v == 0) TEST_ASSERT_EQUAL_STRING("unknown(0)", s.c_str());
      else TEST_ASSERT_FALSE_MESSAGE(contains(s, "unknown"), s.c_str());
    }
    for (int v : outOfRange)
      TEST_ASSERT_EQUAL_STRING(("unknown(" + std::to_string(v) + ")").c_str(), resetSlug(v, vc).c_str());
  }
}

// The frozen vocabularies: the eight values the old switches named keep their slugs
// byte for byte (on devices, in heads' episodic history, parsed by the HIL suite).
static void test_frozen_vocabularies() {
  struct Row { int v; const char* boot; const char* wake; };
  const Row frozen[] = {
      {1, "power-on", "power-on"},          {3, "software-restart", "software-restart"},
      {4, "crash(panic)", "panic"},         {5, "crash(watchdog)", "watchdog"},
      {6, "crash(watchdog)", "watchdog"},   {7, "crash(watchdog)", "watchdog"},
      {8, "deep-sleep wake", "deep-sleep"}, {9, "brownout", "brownout"},
  };
  for (const Row& r : frozen) {
    TEST_ASSERT_EQUAL_STRING(r.boot, resetSlug(r.v, ResetVocab::BootRow).c_str());
    TEST_ASSERT_EQUAL_STRING(r.wake, resetSlug(r.v, ResetVocab::Wake).c_str());
  }
  // The values the old switches missed now have a name (USB is the reachable one on
  // the S3: a host USB-CDC reopen, rst:0x15).
  TEST_ASSERT_EQUAL_STRING("usb", resetSlug(11, ResetVocab::BootRow).c_str());
  TEST_ASSERT_EQUAL_STRING("usb", resetSlug(11, ResetVocab::Wake).c_str());
}

// Distinct values get distinct slugs, except the frozen watchdog group (three IDF
// watchdog values share "crash(watchdog)" / "watchdog" on devices already).
static void test_slugs_distinct_except_frozen_watchdog_group() {
  for (int vocab = 0; vocab < 2; ++vocab) {
    const ResetVocab vc = vocab ? ResetVocab::Wake : ResetVocab::BootRow;
    std::set<std::string> seen;
    for (int v = 0; v < kResetReasonCount; ++v) {
      if (v == 6 || v == 7) {
        TEST_ASSERT_EQUAL_STRING(resetSlug(5, vc).c_str(), resetSlug(v, vc).c_str());
        continue;
      }
      TEST_ASSERT_TRUE_MESSAGE(seen.insert(resetSlug(v, vc)).second, resetSlug(v, vc).c_str());
    }
  }
}

static void test_is_crash_reset_exact() {
  const std::set<int> crash = {4, 5, 6, 7, 15};
  for (int v = -2; v < kResetReasonCount + 3; ++v)
    TEST_ASSERT_EQUAL_MESSAGE(crash.count(v) == 1, isCrashReset(v), std::to_string(v).c_str());
}

static void test_raw_code_beside_slug() {
  TEST_ASSERT_EQUAL_STRING("0x15", hexCode(0x15).c_str());
  TEST_ASSERT_EQUAL_STRING("0x01", hexCode(1).c_str());
  TEST_ASSERT_EQUAL_STRING("usb(0x15)", resetWithRaw(11, 0x15, ResetVocab::Wake).c_str());
  TEST_ASSERT_EQUAL_STRING("crash(panic)(0x0c)", resetWithRaw(4, 0x0c, ResetVocab::BootRow).c_str());
  TEST_ASSERT_EQUAL_STRING("unknown(0)(0x01)", resetWithRaw(0, 1, ResetVocab::BootRow).c_str());
}

// ---- the boot line --------------------------------------------------------------

static BootFacts worstBoot() {
  BootFacts f;
  f.bootNo = 4294967295u;
  f.fw = std::string(80, 'v');
  f.build = std::string(200, 'b');
  f.reason = INT_MIN;
  f.raw = 0xffffffffu;
  f.wake = std::string(40, 'w');
  f.orch = false;
  f.epoch = 18446744073709551615ull;
  f.ota = std::string(100, 'o');
  f.prev = std::string(100, 'p');
  return f;
}

static void test_boot_line_shape() {
  BootFacts f;
  f.fw = "v4.5.14";
  f.build = "v4.5.14-3-gabc1234";
  f.reason = 11;
  f.raw = 0x15;
  f.orch = true;
  f.ota = "ok v4.5.14";
  const std::string l = bootLine(f);
  TEST_ASSERT_EQUAL_STRING(
      "boot: n=? fw=v4.5.14 build=v4.5.14-3-gabc1234 reason=usb(0x15) wake=none mode=orch t=0 "
      "ota=ok_v4.5.14 prev=none",
      l.c_str());
}

// Within budget for every combination of extreme fields, every reset value, both modes.
static void test_boot_line_bounded_and_never_redacted() {
  for (int v = -1; v <= kResetReasonCount; ++v) {
    for (int m = 0; m < 2; ++m) {
      BootFacts f = worstBoot();
      f.reason = v;
      f.orch = m == 1;
      const std::string l = bootLine(f);
      TEST_ASSERT_TRUE_MESSAGE(l.size() <= kMaxBootLineBytes, l.c_str());
      TEST_ASSERT_TRUE(l.compare(0, 8, "boot: n=") == 0);
      TEST_ASSERT_EQUAL_STRING(l.c_str(), core::LogRing::redact(l, kNoSecrets).c_str());
    }
  }
  // A value can never forge a field or a second line.
  BootFacts f;
  f.fw = "v1 reason=fake\nboot: n=9";
  const std::string l = bootLine(f);
  TEST_ASSERT_FALSE(contains(l, "\n"));
  TEST_ASSERT_FALSE(contains(l, " reason=fake"));
}

static void test_stamp_boot_number() {
  std::string line = "17 3 boot boot: n=? fw=v reason=power-on(0x01)\n";
  TEST_ASSERT_TRUE(stampBootNo(line, 42));
  TEST_ASSERT_EQUAL_STRING("17 3 boot boot: n=42 fw=v reason=power-on(0x01)\n", line.c_str());
  TEST_ASSERT_FALSE(stampBootNo(line, 43));   // already stamped
  std::string other = "18 4 - memory: n=? something\n";
  TEST_ASSERT_FALSE(stampBootNo(other, 5));    // only the boot record's placeholder
  std::string zero = "1 1 boot boot: n=? fw=v\n";
  TEST_ASSERT_FALSE(stampBootNo(zero, 0));      // unknown stays "?"
  TEST_ASSERT_EQUAL_UINT(0, parseBootCounter(""));
  TEST_ASSERT_EQUAL_UINT(0, parseBootCounter("x12"));
  TEST_ASSERT_EQUAL_UINT(0, parseBootCounter("99999999999"));
  TEST_ASSERT_EQUAL_UINT(12, parseBootCounter("12\n"));
  TEST_ASSERT_EQUAL_UINT(7, parseBootCounter(renderBootCounter(7)));
}

// ---- the core dump summary --------------------------------------------------------

// A planted pattern in the register file (exc_a) and in a "stack" buffer: stacks and
// registers can hold a provider key or the bot token (main.cpp factory-reset note).
static const uint32_t kPlanted = 0x5ec2e7a1u;

static RawSummary plantedRaw(uint32_t depth, const std::string& task) {
  RawSummary r;
  for (size_t i = 0; i < 16; ++i) r.exc_a[i] = kPlanted ^ (uint32_t)i;
  for (size_t i = 0; i < kBtMax; ++i) r.bt[i] = 0x42000000u + (uint32_t)i * 0x100u;
  r.depth = depth;
  r.exc_pc = 0x4201abcdu;
  r.exc_cause = 28;
  r.exc_vaddr = 0;
  r.corrupted = depth % 2 == 1;
  for (size_t i = 0; i < sizeof r.exc_task && i < task.size(); ++i) r.exc_task[i] = task[i];
  const char elf[] = "0badf00d1";
  for (size_t i = 0; i < sizeof elf; ++i) r.app_elf_sha256[i] = elf[i];
  return r;
}

static Report validReport(uint32_t depth, const std::string& task, const std::string& why) {
  Report r;
  r.state = ImageState::Valid;
  r.fp = 0x1a2b3c4du;
  r.size = 65536;
  r.thisReset = true;
  fromRaw(plantedRaw(depth, task), r);
  r.why = why;
  return r;
}

static std::vector<std::string> everyLine(const Report& r) {
  std::vector<std::string> v = {crashLine(r), crashBtLine(r), crashWhyLine(r), oneLiner(r)};
  return v;
}

// Every line within budget over backtrace depth 0..16 (and an over-deep raw depth),
// long task names, long and hostile panic reasons.
static void test_summary_lines_bounded() {
  const std::string tasks[] = {"", "tg_poll", std::string(15, 'T'), std::string(16, 'U')};
  const std::string whys[] = {"", "abort() was called at PC 0x40375a2b on core 0",
                              std::string(2000, 'r'), std::string("ctl\x01\n\r\x7f") + std::string(400, '"')};
  for (uint32_t depth = 0; depth <= kBtMax + 4; ++depth) {
    for (const auto& t : tasks) {
      for (const auto& w : whys) {
        const Report r = validReport(depth, t, w);
        for (const std::string& l : everyLine(r)) {
          TEST_ASSERT_TRUE_MESSAGE(l.size() <= kMaxLineBytes, l.c_str());
          TEST_ASSERT_FALSE(contains(l, "\n"));
          TEST_ASSERT_FALSE(contains(l, "\r"));
        }
        TEST_ASSERT_TRUE(r.depth <= kBtMax);
        TEST_ASSERT_TRUE(oneLiner(r).size() <= 160);
      }
    }
  }
}

// The planted register/stack pattern never appears in any output, in any form.
static void test_registers_and_stack_never_printed() {
  for (uint32_t depth = 0; depth <= kBtMax; ++depth) {
    const Report r = validReport(depth, "tg_poll", "Stack canary watchpoint triggered (tg_poll)");
    for (const std::string& l : everyLine(r)) {
      for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t v = kPlanted ^ i;
        TEST_ASSERT_FALSE_MESSAGE(contains(l, hex8(v)), l.c_str());
        TEST_ASSERT_FALSE(contains(l, std::to_string(v)));
      }
    }
  }
}

// redact(line) == line: no field name collides with the redactor's key words, so the
// forensic lines reach the durable log intact (random benign summaries).
static void test_summary_lines_never_redacted() {
  uint32_t rng = 7;
  auto next = [&rng]() { rng = rng * 1664525u + 1013904223u; return rng; };
  const char* words[] = {"abort()", "was", "called", "at", "PC", "0x4037", "on", "core", "0",
                         "Stack", "canary", "watchpoint", "triggered", "(tg_poll)", "assert",
                         "failed:", "queue.c:1709", "LoadProhibited", "Cache", "error"};
  for (int i = 0; i < 300; ++i) {
    std::string why;
    for (int w = 0; w < 1 + (int)(next() % 12); ++w) why += std::string(words[next() % 20]) + " ";
    std::string task = "t" + std::to_string(next() % 100000);
    Report r = validReport(next() % 17, task, why);
    r.fp = next();
    r.pc = next();
    r.vaddr = next();
    r.cause = next() % 40;
    r.isNew = next() % 2;
    r.fwMatch = next() % 2;
    for (const std::string& l : everyLine(r))
      TEST_ASSERT_EQUAL_STRING(l.c_str(), core::LogRing::redact(l, kNoSecrets).c_str());
  }
}

static void test_crash_line_fields() {
  Report r = validReport(3, "tg_poll", "Stack canary watchpoint triggered (tg_poll)");
  r.fwMatch = false;
  const std::string l = crashLine(r);
  TEST_ASSERT_TRUE(l.compare(0, 7, "crash: ") == 0);
  TEST_ASSERT_TRUE(contains(l, "fp=1a2b3c4d"));
  TEST_ASSERT_TRUE(contains(l, "new=1 this-reset=1"));
  TEST_ASSERT_TRUE(contains(l, "task=tg_poll"));
  TEST_ASSERT_TRUE(contains(l, "pc=0x4201abcd"));
  TEST_ASSERT_TRUE(contains(l, "cause=28(LoadProhibited)"));
  TEST_ASSERT_TRUE(contains(l, "size=65536/65536"));
  TEST_ASSERT_TRUE(contains(l, "elf=0badf00d1"));
  TEST_ASSERT_TRUE(contains(l, "fw-match=0"));
  const std::string bt = crashBtLine(r);
  TEST_ASSERT_EQUAL_STRING("crash-bt: depth=3 corrupt=1 0x42000000,0x42000100,0x42000200", bt.c_str());
  TEST_ASSERT_EQUAL_STRING("crash-why: Stack canary watchpoint triggered (tg_poll)", crashWhyLine(r).c_str());
  r.why.clear();   // a CPU exception records no panic message: the cause states it
  TEST_ASSERT_EQUAL_STRING(
      "crash-why: LoadProhibited in tg_poll: a read from an invalid address (0x00000000); "
      "no panic message in the dump",
      crashWhyLine(r).c_str());
  TEST_ASSERT_EQUAL_STRING(
      "crash=tg_poll@0x4201abcd \"LoadProhibited in tg_poll: a read from an invalid address (0x00000000); "
      "no panic message in the dump\"",
      oneLiner(r).c_str());
  r.newKnown = false;
  TEST_ASSERT_TRUE(contains(crashLine(r), "new=?"));
  TEST_ASSERT_EQUAL_STRING("other", causeName(99));
  TEST_ASSERT_EQUAL_STRING("CoprocessorDisabled", causeName(33));
  TEST_ASSERT_EQUAL_STRING("DebugException", causeName(65));
  TEST_ASSERT_EQUAL_STRING("IntWdtCpu1", causeName(70));
}

// Every cause the summary can carry states a reason when the dump recorded no panic
// message (IDF records one only for abort() and the task watchdog): every Xtensa
// EXCCAUSE, every IDF pseudo cause, values beyond both, and the invalid marker 0xFFFF.
// The placeholder "(no reason recorded)" is gone for good.
static void test_every_cause_states_a_reason() {
  const char* tasks[] = {"loopTask", "tg_poll", "", "a task name that is far too long"};
  std::vector<uint32_t> causes;
  for (uint32_t c = 0; c <= 80; ++c) causes.push_back(c);
  causes.push_back(0xFFFF);
  causes.push_back(UINT32_MAX);
  for (uint32_t cause : causes) {
    for (const char* task : tasks) {
      Report r = validReport(4, task, "");
      r.cause = cause;
      r.vaddr = 0xdeadbee0u;
      const std::string why = whyOf(r);
      TEST_ASSERT_FALSE_MESSAGE(why.empty(), task);
      TEST_ASSERT_TRUE(why.size() <= kMaxWhyBytes);
      TEST_ASSERT_FALSE(contains(why, "no reason recorded"));
      TEST_ASSERT_TRUE_MESSAGE(why.compare(0, strlen(causeName(cause)), causeName(cause)) == 0, why.c_str());
      TEST_ASSERT_TRUE(contains(why, std::string(" in ") + fieldValue(task, 15) + ": "));
      for (unsigned char ch : why) TEST_ASSERT_TRUE(ch >= 0x20 && ch < 0x7f);
      TEST_ASSERT_EQUAL_STRING(("crash-why: " + why).c_str(), crashWhyLine(r).c_str());
      TEST_ASSERT_TRUE(contains(oneLiner(r), "\""));
      for (const std::string& l : everyLine(r))   // the redactor leaves every line whole
        TEST_ASSERT_EQUAL_STRING(l.c_str(), core::LogRing::redact(l, kNoSecrets).c_str());
      const std::vector<std::string> lines = reportLines(r);
      TEST_ASSERT_EQUAL_UINT(3, (unsigned)lines.size());
      TEST_ASSERT_EQUAL_STRING(("crash-why: " + why).c_str(), lines[2].c_str());
    }
  }
  Report recorded = validReport(4, "loopTask", "abort() was called at PC 0x42001234 on core 1");
  recorded.cause = 29;   // a recorded message wins over the cause
  TEST_ASSERT_EQUAL_STRING("abort() was called at PC 0x42001234 on core 1", whyOf(recorded).c_str());
}

// Every PANIC kind the test console fires, as its dump presents it (cause, recorded panic
// message or none), states a reason in crash-why and a plain cause for the owner.
struct KindCase {
  const char* kind;
  const char* task;
  uint32_t cause;
  const char* recorded;   // the panic message IDF stores, "" when it stores none
  int reset;              // this boot's reset reason
  const char* whyHas;     // crash-why must contain this
  CrashKind owner;
};
static void test_every_panic_kind_states_a_reason() {
  const int panic = (int)ResetReason::Panic, twdt = (int)ResetReason::TaskWdt;
  const KindCase cases[] = {
      {"abort", "loopTask", 0, "abort() was called at PC 0x42001234 on core 1", panic, "abort()",
       CrashKind::SoftwareError},
      {"nullptr", "loopTask", 29, "", panic, "a write to an invalid address (0x00000000)", CrashKind::SoftwareError},
      {"canary", "loopTask", 65, "", panic, "stack overflow", CrashKind::TaskOutOfMemory},
      {"twdt", "IDLE0", 0, "Task watchdog got triggered. The following tasks did not reset the watchdog in time:",
       twdt, "Task watchdog", CrashKind::Unresponsive},
      {"oom", "loopTask", 29, "", panic, "a write to an invalid address", CrashKind::SoftwareError},
      {"nullptr arm=turn", "tg_poll", 29, "", panic, "StoreProhibited in tg_poll", CrashKind::SoftwareError},
      {"abort arm=turn", "tg_poll", 0, "abort() was called at PC 0x42005678 on core 0", panic, "abort()",
       CrashKind::SoftwareError},
      {"canary arm=turn", "tg_poll", 0, "***ERROR*** A stack overflow in task tg_poll has been detected.", panic,
       "stack overflow in task tg_poll", CrashKind::TaskOutOfMemory},
  };
  for (const KindCase& k : cases) {
    Report r = validReport(5, k.task, k.recorded);
    r.cause = k.cause;
    r.vaddr = 0;
    const std::string line = crashWhyLine(r);
    TEST_ASSERT_TRUE_MESSAGE(contains(line, k.whyHas), (std::string(k.kind) + ": " + line).c_str());
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)k.owner, (int)crashKind(&r, k.reset), k.kind);
    const std::string notice = nimbus::orch::crashNotice("Lumi", nullptr, "", plainCause(crashKind(&r, k.reset)));
    TEST_ASSERT_TRUE_MESSAGE(contains(notice, std::string("(") + plainCause(k.owner) + ")"), notice.c_str());
  }
}

// The owner's notice names a plain cause for every crash reset, every cause, every kind
// of recorded message, with and without this crash's dump, and never a technical term.
static void test_crash_kind_for_every_crash_reset_and_cause() {
  const char* recorded[] = {"", "abort() was called at PC 0x42001234 on core 1",
                            "***ERROR*** A stack overflow in task tg_poll has been detected.",
                            "Task watchdog got triggered.", "Failed to allocate 4096 bytes",
                            "Stack canary watchpoint triggered (tg_poll)"};
  const std::set<std::string> vocabulary = {plainCause(CrashKind::SoftwareError), plainCause(CrashKind::Unresponsive),
                                            plainCause(CrashKind::OutOfMemory), plainCause(CrashKind::TaskOutOfMemory)};
  TEST_ASSERT_EQUAL_UINT(4, (unsigned)vocabulary.size());
  const char* jargon[] = {"panic", "watchdog", "exception", "stack", "heap", "abort", "0x", "Prohibited", "CPU",
                          "core", "dump", "firmware"};
  std::set<std::string> seen;
  int crashResets = 0;
  for (int reset = 0; reset <= kLastResetReason + 1; ++reset) {
    if (!isCrashReset(reset)) continue;
    ++crashResets;
    TEST_ASSERT_TRUE(vocabulary.count(plainCause(crashKind(nullptr, reset))) == 1);   // no dump written
    for (uint32_t cause = 0; cause <= 72; ++cause) {
      for (const char* msg : recorded) {
        Report r = validReport(3, "tg_poll", msg);
        r.cause = cause;
        const std::string plain = plainCause(crashKind(&r, reset));
        TEST_ASSERT_TRUE_MESSAGE(vocabulary.count(plain) == 1, plain.c_str());
        seen.insert(plain);
        for (const char* bad : jargon) TEST_ASSERT_FALSE_MESSAGE(contains(plain, bad), plain.c_str());
      }
    }
  }
  TEST_ASSERT_TRUE(crashResets >= 4);           // panic and the three watchdog classes, at least
  TEST_ASSERT_EQUAL_UINT(4, (unsigned)seen.size());   // every plain cause is reachable
  Report stale = validReport(3, "loopTask", "");
  stale.state = ImageState::BadCrc;   // an unreadable dump names nothing
  TEST_ASSERT_EQUAL_INT((int)CrashKind::Unresponsive, (int)crashKind(&stale, (int)ResetReason::TaskWdt));
  TEST_ASSERT_EQUAL_INT((int)CrashKind::SoftwareError, (int)crashKind(&stale, (int)ResetReason::Panic));
}

// Which lines a boot emits, over every image state x new/stale x store known/unknown.
static void test_report_lines_by_state() {
  Report none;
  TEST_ASSERT_EQUAL_UINT(0, (unsigned)reportLines(none).size());   // fresh device: crash=none, silent
  TEST_ASSERT_TRUE(oneLiner(none).empty());
  const ImageState bad[] = {ImageState::BadCrc, ImageState::BadSize, ImageState::ReadError};
  for (ImageState s : bad) {
    Report r;
    r.state = s;
    const auto lines = reportLines(r);
    TEST_ASSERT_EQUAL_UINT(1, (unsigned)lines.size());
    TEST_ASSERT_TRUE(contains(lines[0], std::string("image=") + imageStateStr(s)));
    TEST_ASSERT_TRUE(oneLiner(r).empty());
  }
  Report fresh = validReport(4, "loopTask", "abort()");
  TEST_ASSERT_EQUAL_UINT(3, (unsigned)reportLines(fresh).size());
  fresh.isNew = false;   // already reported on an earlier boot
  const auto stale = reportLines(fresh);
  TEST_ASSERT_EQUAL_UINT(1, (unsigned)stale.size());
  TEST_ASSERT_TRUE(contains(stale[0], "new=0"));
  fresh.newKnown = false;   // no storage to dedupe against: report in full
  TEST_ASSERT_EQUAL_UINT(3, (unsigned)reportLines(fresh).size());
}

// The dedupe state machine across boots: a new dump reports once in full, the same
// dump on later boots is stale, a new panic reports again, a plain reboot with a dump
// present says this-reset=0.
static void test_dedupe_state_machine() {
  std::string seenFile;   // /log/crash.seen
  struct Boot { uint32_t fp; bool crashReset; bool wantNew; };
  const Boot boots[] = {
      {0xaaaa0001, true, true},    // the panic: new=1 this-reset=1
      {0xaaaa0001, false, false},  // a plain reboot: stale, new=0 this-reset=0
      {0xaaaa0001, false, false},  // and again
      {0xbbbb0002, true, true},    // a second panic overwrote the dump: new again
      {0xbbbb0002, true, false},   // a crash reset whose dump did not change (no write)
  };
  uint32_t bootNo = 0;
  for (const Boot& b : boots) {
    ++bootNo;
    const Seen seen = parseSeen(seenFile);
    const bool isNew = isNewCrash(seen, b.fp);
    TEST_ASSERT_EQUAL(b.wantNew, isNew);
    if (isNew) seenFile = renderSeen(b.fp, "0badf00d1", bootNo);
  }
  const Seen last = parseSeen(seenFile);
  TEST_ASSERT_TRUE(last.valid);
  TEST_ASSERT_EQUAL_HEX32(0xbbbb0002, last.fp);
  TEST_ASSERT_EQUAL_UINT(4, last.bootNo);
  TEST_ASSERT_EQUAL_STRING("0badf00d1", last.elf.c_str());
  // Garbage or a fresh store reads as "nothing seen": the dump is new.
  TEST_ASSERT_FALSE(parseSeen("").valid);
  TEST_ASSERT_FALSE(parseSeen("zz 1 2").valid);
  TEST_ASSERT_FALSE(parseSeen("123 x 1").valid);
  TEST_ASSERT_TRUE(isNewCrash(parseSeen("garbage"), 0xaaaa0001));
}

// ---- crash UX: the turn journal and the owner notice -------------------------------

static void test_turn_kind_classes() {
  using namespace nimbus::orch;
  TEST_ASSERT_EQUAL_STRING("photo", turnKindFor(kTurnSourceOwner, "61234", "[image] a dog").c_str());
  TEST_ASSERT_EQUAL_STRING("file", turnKindFor(kTurnSourceOwner, "61234", "[file] report.pdf").c_str());
  TEST_ASSERT_EQUAL_STRING("voice", turnKindFor(kTurnSourceOwner, "voice", "turn on").c_str());
  TEST_ASSERT_EQUAL_STRING("message", turnKindFor(kTurnSourceOwner, "web", "hello").c_str());
  TEST_ASSERT_EQUAL_STRING("routine", turnKindFor(kTurnSourceLoop, "61234", "[image] x").c_str());
  TEST_ASSERT_EQUAL_STRING("background", turnKindFor(kTurnSourceSynthesis, "", "").c_str());
  TEST_ASSERT_EQUAL_STRING("console", turnKindFor(kTurnSourceSerial, "serial", "x").c_str());
}

static void test_turn_journal_round_trip() {
  using namespace nimbus::orch;
  TurnJournal j;
  j.chat = "6098532481";
  j.kind = "photo";
  j.at = 1792326622;
  j.upMs = 367161639;
  j.round = 2;
  const std::string json = renderTurnJournal(j);
  TEST_ASSERT_TRUE(json.size() <= 200);
  TurnJournal back;
  TEST_ASSERT_TRUE(parseTurnJournal(json, back));
  TEST_ASSERT_EQUAL_STRING("6098532481", back.chat.c_str());
  TEST_ASSERT_EQUAL_STRING("photo", back.kind.c_str());
  TEST_ASSERT_EQUAL_UINT64(1792326622ull, back.at);
  TEST_ASSERT_EQUAL_UINT(367161639u, back.upMs);
  TEST_ASSERT_EQUAL_INT(2, back.round);
  TurnJournal huge = j;
  huge.chat = std::string(500, 'c');
  TEST_ASSERT_TRUE(renderTurnJournal(huge).size() <= 200);
  TEST_ASSERT_FALSE(parseTurnJournal("", back));
  TEST_ASSERT_FALSE(parseTurnJournal("{not json", back));
  TEST_ASSERT_FALSE(parseTurnJournal("{\"v\":2,\"kind\":\"photo\"}", back));
  TEST_ASSERT_FALSE(parseTurnJournal("{\"v\":1}", back));
}

// The journal write never runs below the forensic memory floors (a file open is an
// abort-capable internal allocation), and the breadcrumb mark carries the kind so the
// notice can still name the request when the write was skipped.
static void test_journal_write_gate_and_mark() {
  using namespace nimbus::orch;
  TEST_ASSERT_TRUE(journalWriteAllowed(kForensicMinFree, kForensicMinLargest));
  TEST_ASSERT_FALSE(journalWriteAllowed(kForensicMinFree - 1, 50000));
  TEST_ASSERT_FALSE(journalWriteAllowed(50000, kForensicMinLargest - 1));
  const char* kinds[] = {"photo", "file", "voice", "message", "routine", "background", "console"};
  for (const char* k : kinds) {
    for (int round = 0; round < 12; ++round) {
      const std::string m = turnMark(k, round);
      TEST_ASSERT_TRUE(m.size() < 24);   // fits the RTC record's mark
      TEST_ASSERT_EQUAL_STRING(k, kindFromTurnMark(m).c_str());
    }
  }
  TEST_ASSERT_TRUE(kindFromTurnMark("r2").empty());
  TEST_ASSERT_TRUE(kindFromTurnMark("").empty());
  TEST_ASSERT_TRUE(kindFromTurnMark("garbage/r1").empty());
}

// The notice for every kind: plain words (no jargon the owner would not know), the
// device speaks as "it" (never I/me/my), no em dash, the request is named, a time
// only when the clock was set.
static void test_crash_notice_copy_for_every_kind() {
  using namespace nimbus::orch;
  const char* kinds[] = {"photo", "file", "voice", "message", "console", "routine", "background"};
  const char* jargon[] = {"panic", "watchdog", "coredump", "core dump", "reset", "stack", "heap",
                          "abort", "firmware", "exception", "\xE2\x80\x94"};
  const CrashKind causes[] = {CrashKind::SoftwareError, CrashKind::Unresponsive, CrashKind::OutOfMemory,
                              CrashKind::TaskOutOfMemory};
  for (const char* k : kinds) {
    TurnJournal j;
    j.kind = k;
    for (int withTime = 0; withTime < 2; ++withTime) {
      for (int c = -1; c < 4; ++c) {   // -1: no cause given
        const std::string cause = c < 0 ? std::string() : plainCause(causes[c]);
        const std::string m = crashNotice("Lumi", &j, withTime ? "12:30" : "", cause);
        TEST_ASSERT_TRUE(m.compare(0, 4, "Lumi") == 0);
        TEST_ASSERT_TRUE(contains(m, "restarted after a crash"));
        TEST_ASSERT_EQUAL(withTime == 1, contains(m, "from 12:30"));
        TEST_ASSERT_EQUAL(c >= 0, contains(m, "crash (" + cause + ")"));
        for (const char* bad : jargon) TEST_ASSERT_FALSE_MESSAGE(contains(m, bad), m.c_str());
        TEST_ASSERT_FALSE(contains(m, " I ") || contains(m, " me ") || contains(m, " my "));
        TEST_ASSERT_TRUE(m.size() < 200);
      }
    }
  }
  TurnJournal voice;
  voice.kind = "voice";
  TEST_ASSERT_EQUAL_STRING(
      "Lumi restarted after a crash (one of its jobs ran out of memory) while answering your voice "
      "message from 09:05. That reply was lost, so please send it again if you still need it.",
      crashNotice("Lumi", &voice, "09:05", plainCause(CrashKind::TaskOutOfMemory)).c_str());
  TurnJournal photo;
  photo.kind = "photo";
  TEST_ASSERT_EQUAL_STRING(
      "Lumi restarted after a crash while answering your photo from 12:30. That reply was lost, "
      "so please send it again if you still need it.",
      crashNotice("Lumi", &photo, "12:30").c_str());
  TEST_ASSERT_EQUAL_STRING("Nimbus restarted after a crash. Nothing was in progress, so nothing was lost.",
                           crashNotice("", nullptr, "").c_str());
}

// ---- the forensic window (rows 44, 57) ----------------------------------------------

// The heaptrace level: the transport writes a trace to the durable log only when
// ExchangeTrace::abnormal() says so (a failed stage, a failed allocation, or memory under
// the forensic floors at any sampled phase or at the monitored minimum). Every stage x
// failed-allocation count x free/largest at each of the four phases x monitored minimum.
static_assert(nimbus::heapwatch::kTraceMinFree == kForensicMinFree, "one forensic floor (free)");
static_assert(nimbus::heapwatch::kTraceMinLargest == kForensicMinLargest, "one forensic floor (largest)");
static void test_trace_abnormal_grid() {
  using nimbus::heapwatch::ExchangeTrace;
  using nimbus::heapwatch::HeapSample;
  using nimbus::heapwatch::Stage;
  const Stage stages[] = {Stage::None, Stage::Connect, Stage::Head, Stage::Body,
                          Stage::Read, Stage::Headers, Stage::Parse};
  const uint32_t frees[] = {0, kForensicMinFree - 1, kForensicMinFree, 40000, UINT32_MAX};
  const uint32_t largests[] = {0, kForensicMinLargest - 1, kForensicMinLargest, 20000, UINT32_MAX};
  const uint32_t monitored[] = {UINT32_MAX, kForensicMinFree - 1, kForensicMinFree};
  const HeapSample healthy{40000, 20000};
  for (Stage st : stages)
    for (uint32_t af = 0; af < 3; ++af)
      for (int phase = 0; phase < 4; ++phase)
        for (uint32_t f : frees)
          for (uint32_t l : largests)
            for (uint32_t mon : monitored) {
              ExchangeTrace t;
              t.fail = st;
              t.af = af;
              t.minFree = mon;
              t.pre = t.conn = t.wrote = t.done = healthy;
              HeapSample* at[] = {&t.pre, &t.conn, &t.wrote, &t.done};
              *at[phase] = HeapSample{f, l};
              const bool taken = f || l;   // (0, 0) is "not taken", never a reading
              const bool want = st != Stage::None || af > 0 || mon < kForensicMinFree ||
                                (taken && (f < kForensicMinFree || l < kForensicMinLargest));
              TEST_ASSERT_EQUAL(want, t.abnormal());
            }
}

// A body-less exchange leaves its post-write sample at (0, 0): the predicate must
// ignore it, or every GET would read as abnormal and flood the durable log.
static void test_trace_samples_ignore_the_unsampled_point() {
  nimbus::heapwatch::ExchangeTrace t;
  t.pre = {30000, 20000};
  t.conn = {26000, 15000};
  t.wrote = {0, 0};   // no body
  t.done = {29000, 19000};
  TEST_ASSERT_FALSE(t.abnormal());
  t.done = {0, 7000};   // a real reading with nothing free is still counted
  TEST_ASSERT_TRUE(t.abnormal());
  TEST_ASSERT_FALSE(nimbus::heapwatch::ExchangeTrace().abnormal());   // nothing sampled, not failed
}

static void test_headloop_classification() {
  TEST_ASSERT_TRUE(headloopRoutine("headloop round=2 tools=1 heap=22152 intLargest=9716"));
  TEST_ASSERT_FALSE(headloopRoutine("headloop round=2 tools=0 heap=22152 intLargest=9716 cap=rounds"));
  TEST_ASSERT_FALSE(headloopRoutine("headloop round=3 tools=1 heap=11999 intLargest=9716"));
  TEST_ASSERT_FALSE(headloopRoutine("headloop round=3 tools=1 heap=30000 intLargest=8191"));
  TEST_ASSERT_FALSE(headloopRoutine("headloop round=1 tools=1"));          // no samples: keep it
  TEST_ASSERT_FALSE(headloopRoutine("headloop: ran 3 tool(s)"));          // other harness lines
  TEST_ASSERT_FALSE(headloopRoutine("provider: transport fail"));
  int r = -1;
  TEST_ASSERT_TRUE(parseHeadloopRound("headloop round=7 tools=1", r));
  TEST_ASSERT_EQUAL_INT(7, r);
  TEST_ASSERT_FALSE(parseHeadloopRound("headloop round=x", r));
  TEST_ASSERT_FALSE(parseHeadloopRound("xheadloop round=1", r));
}

static void test_relay_fold_window() {
  RelayFold fold;
  RelayFold::Serve ok{200, 1000, 30000};
  RelayFold::Serve notMod{304, 0, 30000};
  uint32_t t = 1000;
  // Inside the window: no durable line however many serves.
  for (int i = 0; i < 500; ++i, t += 1000) {
    const std::string l = fold.note(i % 5 ? ok : notMod, t, 50000);
    if ((uint32_t)(t - 1000) < RelayFold::kWindowMs) TEST_ASSERT_TRUE(l.empty());
  }
  // Fresh fold: the first serve past the window emits one summary with the counts.
  RelayFold f2;
  TEST_ASSERT_TRUE(f2.note(ok, 0, 40000).empty());
  TEST_ASSERT_TRUE(f2.note(notMod, 1000, 6664).empty());
  const std::string s = f2.note(RelayFold::Serve{404, 10, 25000}, RelayFold::kWindowMs, 9000);
  TEST_ASSERT_TRUE(contains(s, "relay: 3 serve(s) in 15 min"));
  TEST_ASSERT_TRUE(contains(s, "2xx=1 3xx=1 4xx=1 5xx=0 none=0"));
  TEST_ASSERT_TRUE(contains(s, "bytes=1010"));
  TEST_ASSERT_TRUE(contains(s, "intMin=25000"));
  TEST_ASSERT_TRUE(contains(s, "heapMin=6664"));
  TEST_ASSERT_TRUE(f2.note(ok, RelayFold::kWindowMs + 1, 9000).empty());   // a new window opened
  // Abnormal serves keep their own durable line.
  TEST_ASSERT_TRUE(RelayFold::abnormal(RelayFold::Serve{0, 0, 30000}));
  TEST_ASSERT_TRUE(RelayFold::abnormal(RelayFold::Serve{502, 10, 30000}));
  TEST_ASSERT_TRUE(RelayFold::abnormal(RelayFold::Serve{200, 10, kForensicMinFree - 1}));
  TEST_ASSERT_FALSE(RelayFold::abnormal(ok));
  TEST_ASSERT_FALSE(RelayFold::abnormal(RelayFold::Serve{404, 10, 30000}));
}

// ---- the RTC breadcrumb (F4) ---------------------------------------------------------

// A power-on leaves RTC slow memory full of garbage: every random record must read as
// "none" (magic + CRC), so a cold boot never claims a previous run it did not have.
static void test_breadcrumb_rejects_power_on_garbage() {
  uint32_t rng = 0x1234567u;
  for (int i = 0; i < 20000; ++i) {
    Breadcrumb b;
    uint8_t* p = reinterpret_cast<uint8_t*>(&b);
    for (size_t k = 0; k < sizeof b; ++k) {
      rng = rng * 1103515245u + 12345u;
      p[k] = (uint8_t)(rng >> 16);
    }
    if (i % 2) b.magic = kBreadcrumbMagic;   // even with the right magic, the CRC catches it
    TEST_ASSERT_FALSE(breadcrumbValid(b));
    TEST_ASSERT_EQUAL_STRING("none", breadcrumbSummary(b).c_str());
  }
  Breadcrumb zero{};
  TEST_ASSERT_FALSE(breadcrumbValid(zero));
}

// A sealed record round-trips, and any single flipped byte invalidates it.
static void test_breadcrumb_round_trip_and_single_byte_damage() {
  Breadcrumb b;
  markBreadcrumb(b, Stage::Turn, "r2", 367161639u, 1180u, 17u);
  TEST_ASSERT_TRUE(breadcrumbValid(b));
  TEST_ASSERT_EQUAL_STRING("n17/367161s/turn-r2/h1180", breadcrumbSummary(b).c_str());
  uint8_t* p = reinterpret_cast<uint8_t*>(&b);
  for (size_t k = 0; k < sizeof b; ++k) {
    Breadcrumb c = b;
    reinterpret_cast<uint8_t*>(&c)[k] ^= 0x01;
    TEST_ASSERT_FALSE_MESSAGE(breadcrumbValid(c), std::to_string(k).c_str());
  }
  (void)p;
  // Every stage has a name, and the summary always fits the boot line's prev= field.
  for (uint8_t st = 0; st < (uint8_t)Stage::Count; ++st) {
    TEST_ASSERT_TRUE(std::string(stageName(st)) != "?");
    Breadcrumb x;
    markBreadcrumb(x, (Stage)st, "a very long mark that will not fit = here", 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu);
    const std::string sum = breadcrumbSummary(x);
    TEST_ASSERT_TRUE(sum.size() <= 48);
    TEST_ASSERT_TRUE(sum.find(' ') == std::string::npos && sum.find('=') == std::string::npos);
  }
  // The early-boot abort case the OTA drill produces: the guard stage, no boot number yet.
  Breadcrumb guard;
  markBreadcrumb(guard, Stage::OtaGuard, nullptr, 40, 250000, 0);
  TEST_ASSERT_EQUAL_STRING("0s/ota-guard/h250000", breadcrumbSummary(guard).c_str());
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc32((const uint8_t*)"123456789", 9));   // the CRC-32 check value
}

// ---- the test-build mid-turn panic switch ------------------------------------------

static void test_panic_arm_spec_grammar() {
  PanicArm a;
  TEST_ASSERT_TRUE(parsePanicArm("", a));
  TEST_ASSERT_EQUAL(PanicKind::NullStore, a.kind);
  TEST_ASSERT_EQUAL_INT(0, a.round);
  TEST_ASSERT_TRUE(parsePanicArm("abort round=2", a));
  TEST_ASSERT_EQUAL(PanicKind::Abort, a.kind);
  TEST_ASSERT_EQUAL_INT(2, a.round);
  TEST_ASSERT_TRUE(parsePanicArm("  round=7   canary ", a));
  TEST_ASSERT_EQUAL(PanicKind::Canary, a.kind);
  TEST_ASSERT_EQUAL_INT(7, a.round);
  TEST_ASSERT_TRUE(parsePanicArm("round=0", a));
  TEST_ASSERT_EQUAL(PanicKind::NullStore, a.kind);
  const char* bad[] = {"twdt", "oom", "abort abort", "round=", "round=51", "round=-1", "round=1 round=2",
                       "round=x", "nullptr extra", "ROUND=1", "round=100"};
  for (const char* b : bad) {
    PanicArm keep;
    keep.kind = PanicKind::Canary;
    TEST_ASSERT_FALSE_MESSAGE(parsePanicArm(b, keep), b);
    TEST_ASSERT_EQUAL(PanicKind::Canary, keep.kind);   // a refused spec changes nothing
  }
  TEST_ASSERT_EQUAL_STRING("PANIC armed: abort at round 2 of the next turn",
                           panicArmReply(PanicArm{PanicKind::Abort, 2}).c_str());
}

// Fires exactly inside a turn at or past the armed round; never outside a turn (the
// photo description call before the turn starts), never unarmed.
static void test_panic_arm_fires_only_in_the_armed_round() {
  for (int armed = 0; armed <= 5; ++armed) {
    const PanicArm a{PanicKind::NullStore, armed};
    for (int round = 0; round <= 6; ++round) {
      TEST_ASSERT_EQUAL(round >= armed, panicArmFires(a, true, round));
      TEST_ASSERT_FALSE(panicArmFires(a, false, round));
    }
  }
  TEST_ASSERT_FALSE(panicArmFires(PanicArm(), true, 9));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_reset_table_rows_are_positional);
  RUN_TEST(test_unknown_only_for_zero_and_out_of_range);
  RUN_TEST(test_frozen_vocabularies);
  RUN_TEST(test_slugs_distinct_except_frozen_watchdog_group);
  RUN_TEST(test_is_crash_reset_exact);
  RUN_TEST(test_raw_code_beside_slug);
  RUN_TEST(test_boot_line_shape);
  RUN_TEST(test_boot_line_bounded_and_never_redacted);
  RUN_TEST(test_stamp_boot_number);
  RUN_TEST(test_summary_lines_bounded);
  RUN_TEST(test_registers_and_stack_never_printed);
  RUN_TEST(test_summary_lines_never_redacted);
  RUN_TEST(test_crash_line_fields);
  RUN_TEST(test_every_cause_states_a_reason);
  RUN_TEST(test_every_panic_kind_states_a_reason);
  RUN_TEST(test_crash_kind_for_every_crash_reset_and_cause);
  RUN_TEST(test_report_lines_by_state);
  RUN_TEST(test_dedupe_state_machine);
  RUN_TEST(test_turn_kind_classes);
  RUN_TEST(test_turn_journal_round_trip);
  RUN_TEST(test_journal_write_gate_and_mark);
  RUN_TEST(test_crash_notice_copy_for_every_kind);
  RUN_TEST(test_trace_abnormal_grid);
  RUN_TEST(test_trace_samples_ignore_the_unsampled_point);
  RUN_TEST(test_headloop_classification);
  RUN_TEST(test_relay_fold_window);
  RUN_TEST(test_breadcrumb_rejects_power_on_garbage);
  RUN_TEST(test_breadcrumb_round_trip_and_single_byte_damage);
  RUN_TEST(test_panic_arm_spec_grammar);
  RUN_TEST(test_panic_arm_fires_only_in_the_armed_round);
  return UNITY_END();
}
