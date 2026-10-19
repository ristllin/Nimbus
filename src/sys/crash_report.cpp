#include "crash_report.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_app_desc.h>    // esp_app_get_elf_sha256 - fw-match
#include <esp_attr.h>        // RTC_NOINIT_ATTR - the breadcrumb
#include <esp_heap_caps.h>
#include <esp_partition.h>   // the coredump partition: blank check + stored CRC
#include <esp_rom_sys.h>     // esp_rom_get_reset_reason - the raw hardware code
#include <esp_sleep.h>
#include <esp_system.h>
#include <time.h>

#include <cstring>
#include <new>
#if __has_include(<esp_core_dump.h>)
#include <esp_core_dump.h>
#endif

#include "agent_log.h"
#include "config_nvs.h"   // deviceName() for the owner notice
#include "errlog_fs.h"
#include "nimbus/crash_summary.h"
#include "nimbus/harness/hooks.h"   // TurnSource - mirrored by value in turn_journal.h
#include "nimbus/logring.h"
#include "nimbus/orch/turn_journal.h"
#include "nimbus/sys/boot_record.h"
#include "nimbus/sys/panic_arm.h"
#include "nimbus/sys/reset_reason.h"
#include "ota_update.h"   // lastResult() - the boot line's ota= field
#include "version.h"
#include "../agent/memory_subsystem.h"
#include "../agent/orchestrator.h"   // firstAllowedChat - the owner notice's recipient
#include "../agent/telegram.h"       // send - the reply queue

using nimbus::sys::ResetReason;

// One table for every reset reason (lib/core reset_reason.h): a new or renumbered IDF
// value fails the build here instead of silently printing "unknown".
static_assert(ESP_RST_UNKNOWN == (int)ResetReason::Unknown, "reset mirror");
static_assert(ESP_RST_POWERON == (int)ResetReason::PowerOn, "reset mirror");
static_assert(ESP_RST_EXT == (int)ResetReason::Ext, "reset mirror");
static_assert(ESP_RST_SW == (int)ResetReason::Sw, "reset mirror");
static_assert(ESP_RST_PANIC == (int)ResetReason::Panic, "reset mirror");
static_assert(ESP_RST_INT_WDT == (int)ResetReason::IntWdt, "reset mirror");
static_assert(ESP_RST_TASK_WDT == (int)ResetReason::TaskWdt, "reset mirror");
static_assert(ESP_RST_WDT == (int)ResetReason::Wdt, "reset mirror");
static_assert(ESP_RST_DEEPSLEEP == (int)ResetReason::DeepSleep, "reset mirror");
static_assert(ESP_RST_BROWNOUT == (int)ResetReason::Brownout, "reset mirror");
static_assert(ESP_RST_SDIO == (int)ResetReason::Sdio, "reset mirror");
static_assert(ESP_RST_USB == (int)ResetReason::Usb, "reset mirror");
static_assert(ESP_RST_JTAG == (int)ResetReason::Jtag, "reset mirror");
static_assert(ESP_RST_EFUSE == (int)ResetReason::Efuse, "reset mirror");
static_assert(ESP_RST_PWR_GLITCH == (int)ResetReason::PwrGlitch, "reset mirror");
static_assert(ESP_RST_CPU_LOCKUP == (int)ResetReason::CpuLockup, "reset mirror");
static_assert(ESP_RST_CPU_LOCKUP == nimbus::sys::kLastResetReason,
              "ESP_RST_CPU_LOCKUP must stay the last reset reason; extend reset_reason.h");

static_assert((int)agent::TurnSource::Owner == nimbus::orch::kTurnSourceOwner, "turn source mirror");
static_assert((int)agent::TurnSource::Synthesis == nimbus::orch::kTurnSourceSynthesis, "turn source mirror");
static_assert((int)agent::TurnSource::Loop == nimbus::orch::kTurnSourceLoop, "turn source mirror");
static_assert((int)agent::TurnSource::Serial == nimbus::orch::kTurnSourceSerial, "turn source mirror");

namespace nimbus::crash {

namespace {

constexpr char kSeenName[] = "crash.seen";
constexpr char kJournalDir[] = "/crash";
constexpr char kJournalPath[] = "/crash/turn.json";

int      g_reason = 0;
uint32_t g_raw = 0;
uint64_t g_epoch = 0;
bool     g_reported = false;
// Allocated only when a dump exists (PSRAM: it is far over the 128 B spill threshold),
// so a device that never crashed carries no static cost for it.
Report*  g_report = nullptr;

// RTC breadcrumb (F4): survives software, panic, watchdog and USB resets and deep
// sleep; power-on garbage fails its magic/CRC. Updated from the boot task, the loop
// and the turn task, so every update runs under a spinlock (byte copies only).
RTC_NOINIT_ATTR nimbus::sys::Breadcrumb s_crumb;
portMUX_TYPE g_crumbMux = portMUX_INITIALIZER_UNLOCKED;
std::string g_prev = "none";   // where the previous run stopped
nimbus::sys::Breadcrumb g_prevCrumb{};   // the previous run's record, as found at boot
uint32_t g_crumbTickMs = 0;

// Crash UX journal: the in-flight turn, mirrored in RAM so a round update rewrites
// the file only when the round actually moves. tg_poll only (the turn task).
nimbus::orch::TurnJournal g_journal;
bool        g_journalActive = false;
bool        g_journalOnDisk = false;   // the turn-start write landed (turnEnded removes it)
std::string g_textHead;
// The journal a crash left behind, read once at boot (collectTurnJournal).
nimbus::orch::TurnJournal g_lostTurn;
bool        g_lostTurnFound = false;

const char* wakeCause() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "none";
    case ESP_SLEEP_WAKEUP_TIMER:     return "timer";
    case ESP_SLEEP_WAKEUP_EXT0:      return "ext0";
    case ESP_SLEEP_WAKEUP_EXT1:      return "ext1";
    case ESP_SLEEP_WAKEUP_GPIO:      return "gpio";
    case ESP_SLEEP_WAKEUP_TOUCHPAD:  return "touch";
    case ESP_SLEEP_WAKEUP_ULP:       return "ulp";
    default:                         return "other";
  }
}

std::string redacted(const std::string& s) {
  return core::LogRing::redact(s, agent::logring::g_secrets);
}

#if __has_include(<esp_core_dump.h>) && CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && \
    CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
// The partition's first word is the image size; 0xFFFFFFFF is a blank (or erased)
// partition - a fresh device, or a factory reset.
bool partitionBlank(const esp_partition_t* part) {
  uint32_t word = 0;
  if (esp_partition_read(part, 0, &word, sizeof word) != ESP_OK) return false;
  return word == 0xFFFFFFFFu;
}

// Read the dump into `r`: state, fingerprint, size, summary and panic reason. Never
// erases it, never copies the register file or any stack byte.
void readDump(Report& r) {
  const esp_partition_t* part =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
  if (!part) return;   // no coredump partition on this table: nothing to report
  r.partBytes = (uint32_t)part->size;
  if (partitionBlank(part)) return;   // ImageState::None
  size_t addr = 0, size = 0;
  if (esp_core_dump_image_get(&addr, &size) != ESP_OK) {
    r.state = ImageState::BadSize;
    return;
  }
  r.size = (uint32_t)size;
  if (size >= 4) esp_partition_read(part, size - 4, &r.fp, sizeof r.fp);   // stored CRC32
  const esp_err_t chk = esp_core_dump_image_check();
  if (chk == ESP_ERR_INVALID_CRC) { r.state = ImageState::BadCrc; return; }
  if (chk != ESP_OK) { r.state = ImageState::ReadError; return; }
  r.state = ImageState::Valid;

  // The summary is ~220 B: keep it off the boot task's stack.
  auto* sum = (esp_core_dump_summary_t*)heap_caps_malloc(sizeof(esp_core_dump_summary_t),
                                                         MALLOC_CAP_SPIRAM);
  if (!sum) sum = (esp_core_dump_summary_t*)malloc(sizeof(esp_core_dump_summary_t));
  if (sum) {
    memset(sum, 0, sizeof *sum);
    if (esp_core_dump_get_summary(sum) == ESP_OK) {
      RawSummary raw;   // exc_a stays zero: the device never copies the registers
      static_assert(sizeof raw.exc_task == sizeof sum->exc_task, "summary mirror");
      static_assert(sizeof raw.bt == sizeof sum->exc_bt_info.bt, "summary mirror");
      static_assert(sizeof raw.app_elf_sha256 == sizeof sum->app_elf_sha256, "summary mirror");
      memcpy(raw.exc_task, sum->exc_task, sizeof raw.exc_task);
      raw.exc_pc = sum->exc_pc;
      memcpy(raw.bt, sum->exc_bt_info.bt, sizeof raw.bt);
      raw.depth = sum->exc_bt_info.depth;
      raw.corrupted = sum->exc_bt_info.corrupted;
      memcpy(raw.app_elf_sha256, sum->app_elf_sha256, sizeof raw.app_elf_sha256);
      raw.app_elf_sha256[sizeof raw.app_elf_sha256 - 1] = 0;
      raw.exc_cause = sum->ex_info.exc_cause;
      raw.exc_vaddr = sum->ex_info.exc_vaddr;
      fromRaw(raw, r);
    }
    heap_caps_free(sum);
  }
  char why[kMaxWhyBytes + 1] = {};
  if (esp_core_dump_get_panic_reason(why, sizeof why) == ESP_OK) r.why = why;
}
#else
void readDump(Report&) {}   // a build without the ELF flash dump has nothing to read
#endif

// Small LittleFS helpers for the turn journal (LittleFS is mounted in Orchestrator
// mode, the only mode that runs turns). Caller holds no lock; these take it. The
// write is skipped under the forensic memory floors (turn_journal.h says why).
bool writeJournal() {
  if (!nimbus::orch::journalWriteAllowed(
          (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
          (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)))
    return false;
  const std::string body = nimbus::orch::renderTurnJournal(g_journal);
  agent::memory::Lock g;
  LittleFS.mkdir(kJournalDir);
  File f = LittleFS.open(kJournalPath, FILE_WRITE);
  if (!f) return false;
  const size_t n = f.write((const uint8_t*)body.data(), body.size());
  f.close();
  return n == body.size();
}

// Read and delete the journal (every boot).
bool takeJournal(nimbus::orch::TurnJournal& out) {
  std::string body;
  {
    agent::memory::Lock g;
    File f = LittleFS.open(kJournalPath, FILE_READ);
    if (!f) return false;
    uint8_t buf[128];
    while (body.size() < 512) {
      const int n = f.read(buf, sizeof buf);
      if (n <= 0) break;
      body.append((const char*)buf, (size_t)n);
    }
    f.close();
    LittleFS.remove(kJournalPath);
  }
  return nimbus::orch::parseTurnJournal(body, out);
}

std::string localHhmm(uint64_t epoch) {
  if (!epoch) return {};
  const time_t t = (time_t)epoch;
  struct tm tmv;
  localtime_r(&t, &tmv);
  char b[8];
  snprintf(b, sizeof b, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
  return b;
}

}  // namespace

void captureBreadcrumb() {
  g_prevCrumb = s_crumb;
  g_prev = nimbus::sys::breadcrumbSummary(s_crumb);
  stage(nimbus::sys::Stage::OtaGuard);
}

void stage(nimbus::sys::Stage st, const char* mark) {
  const uint32_t heapMin = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  const uint32_t boot = errlog::bootNumber();
  portENTER_CRITICAL(&g_crumbMux);
  nimbus::sys::markBreadcrumb(s_crumb, st, mark, millis(), heapMin, boot);
  portEXIT_CRITICAL(&g_crumbMux);
}

void breadcrumbTick() {
  const uint32_t now = millis();
  if (now - g_crumbTickMs < 5000) return;
  g_crumbTickMs = now;
  char mark[sizeof s_crumb.mark];
  nimbus::sys::Stage st;
  portENTER_CRITICAL(&g_crumbMux);
  memcpy(mark, s_crumb.mark, sizeof mark);
  st = (nimbus::sys::Stage)s_crumb.stage;
  portEXIT_CRITICAL(&g_crumbMux);
  mark[sizeof mark - 1] = 0;
  stage(st, mark);
}

void bootRecord(bool orch) {
  g_reason = (int)esp_reset_reason();
  g_raw = (uint32_t)esp_rom_get_reset_reason(0);
  const time_t now = time(nullptr);
  g_epoch = now >= 1600000000 ? (uint64_t)now : 0;   // the RTC keeps it across SW/panic/WDT resets
  nimbus::sys::BootFacts f;
  f.fw = NIMBUS_FW_VERSION;
  f.build = NIMBUS_FW_BUILD;
  f.reason = g_reason;
  f.raw = g_raw;
  f.wake = wakeCause();
  f.orch = orch;
  f.epoch = g_epoch;
  f.ota = std::string(otaupd::lastResult().c_str());
  f.prev = g_prev;
  agent::alogc(errlog::cat::kBoot, nimbus::sys::bootLine(f).c_str());
}

void reportOnce() {
  if (g_reported) return;
  g_reported = true;
  Report r;
  readDump(r);
  if (r.state == ImageState::None) return;   // fresh or erased: crash=none, silent
  r.thisReset = nimbus::sys::isCrashReset(g_reason);
  char elf[sizeof(RawSummary::app_elf_sha256)] = {};
  esp_app_get_elf_sha256(elf, sizeof elf);
  r.fwMatch = !r.elf.empty() && r.elf == elf;
  const bool attached = errlog::storageAttached();
  r.newKnown = attached;
  if (attached) r.isNew = isNewCrash(parseSeen(errlog::readState(kSeenName, 64)), r.fp);
  for (const std::string& line : reportLines(r))
    agent::alogc(errlog::cat::kBoot, line.c_str());
  // Recorded after the lines were handed to the log: a reset in between reports the
  // dump again (a duplicate), never loses it.
  if (attached && r.isNew) errlog::writeState(kSeenName, renderSeen(r.fp, r.elf, errlog::bootNumber()));
  g_report = new (std::nothrow) Report(std::move(r));
}

int resetReason() { return g_reason; }
uint32_t rawResetCode() { return g_raw; }
bool crashBoot() { return nimbus::sys::isCrashReset(g_reason); }

std::string bootRowSuffix() {
  if (!g_report) return {};
  const std::string one = oneLiner(*g_report);   // redacted where it is read, not stored
  return one.empty() ? std::string() : " " + redacted(one);
}

void fillBootState(JsonObject o) {
  o["n"] = errlog::bootNumber();
  o["reason"] = nimbus::sys::resetSlug(g_reason, nimbus::sys::ResetVocab::BootRow);
  o["raw"] = nimbus::sys::hexCode(g_raw);
  o["t"] = g_epoch;
  if (!g_report) {
    o["crash"] = nullptr;
    return;
  }
  const Report& r = *g_report;
  JsonObject c = o["crash"].to<JsonObject>();
  c["state"] = imageStateStr(r.state);
  c["fp"] = hex8(r.fp);
  if (r.newKnown) c["new"] = r.isNew;
  c["thisReset"] = r.thisReset;
  c["size"] = r.size;
  if (r.state != ImageState::Valid) return;
  c["task"] = r.task;
  c["pc"] = "0x" + hex8(r.pc);
  c["cause"] = causeName(r.cause);
  c["why"] = redacted(r.why);   // at read time: provider keys are registered by then
  c["fwMatch"] = r.fwMatch;
}

void noteTurnText(const std::string& text) { g_textHead = text.substr(0, 16); }

void turnStarted(int source, const std::string& chat, const std::string& textHead) {
  g_journal = nimbus::orch::TurnJournal();
  g_journal.chat = chat;
  g_journal.kind = nimbus::orch::turnKindFor(source, chat, textHead.empty() ? g_textHead : textHead);
  const time_t now = time(nullptr);
  g_journal.at = now >= 1600000000 ? (uint64_t)now : 0;
  g_journal.upMs = millis();
  g_journalActive = true;
  g_textHead.clear();
  stage(nimbus::sys::Stage::Turn, g_journal.kind.c_str());
  g_journalOnDisk = writeJournal();
}

void turnRound(int round) {
  // Turns run on tg_poll; a head loop elsewhere must not move this turn's journal.
  if (strcmp(pcTaskGetName(nullptr), "tg_poll") != 0) return;
  if (!g_journalActive || round <= g_journal.round) return;
  g_journal.round = round;
  // RTC only: a file open per round would put an abort-capable allocation on the
  // turn path at the moment the round gate measured memory (turn_journal.h).
  stage(nimbus::sys::Stage::Turn, nimbus::orch::turnMark(g_journal.kind, round).c_str());
}

#ifdef NIMBUS_TEST
static void dropUnfiredArm();   // test builds: the armed mid-turn panic (end of file)
#endif

void turnEnded() {
  if (!g_journalActive) return;
#ifdef NIMBUS_TEST
  dropUnfiredArm();
#endif
  g_journalActive = false;
  stage(nimbus::sys::Stage::Idle);
  if (!g_journalOnDisk) return;
  g_journalOnDisk = false;
  agent::memory::Lock g;
  LittleFS.remove(kJournalPath);
}

void collectTurnJournal() {
  g_lostTurnFound = takeJournal(g_lostTurn);
  if (g_lostTurnFound || !nimbus::sys::breadcrumbValid(g_prevCrumb) ||
      g_prevCrumb.stage != (uint8_t)nimbus::sys::Stage::Turn)
    return;
  // The journal write was skipped (low memory) but the breadcrumb says a turn was in
  // flight: name its kind from the mark, without a time.
  const std::string kind = nimbus::orch::kindFromTurnMark(g_prevCrumb.mark);
  if (kind.empty()) return;
  g_lostTurn = nimbus::orch::TurnJournal();
  g_lostTurn.kind = kind;
  g_lostTurnFound = true;
}

void queueCrashNotice() {
  if (!crashBoot()) return;
  const String owner = agent::orchestrator::firstAllowedChat();
  if (!owner.length()) return;
  const nimbus::orch::TurnJournal* j = g_lostTurnFound ? &g_lostTurn : nullptr;
  const std::string msg = nimbus::orch::crashNotice(std::string(sys::deviceName().c_str()), j,
                                                    j ? localHhmm(j->at) : std::string());
  const bool queued = agent::telegram::send(owner, String(msg.c_str()), /*block=*/false);
  agent::alogcf(errlog::cat::kBoot, "crash: owner notice %s (turn=%s)",
                queued ? "queued" : "not queued", j ? j->kind.c_str() : "none");
}

#ifdef NIMBUS_TEST
namespace {
// Written by the console (loop task), read by tg_poll: a two-field copy under a
// spinlock, never torn.
nimbus::sys::PanicArm g_arm;
portMUX_TYPE g_armMux = portMUX_INITIALIZER_UNLOCKED;

// Each frame keeps a live 512 B buffer, so the recursion walks tg_poll's stack down
// into the canary without the optimizer folding it away.
[[gnu::noinline]] uint32_t deepen(uint32_t depth) {
  volatile uint8_t pad[512];
  for (size_t i = 0; i < sizeof pad; ++i) pad[i] = (uint8_t)(depth + i);
  return pad[depth % sizeof pad] + deepen(depth + 1);
}

[[gnu::noinline]] void nullStore() {
  volatile uint32_t* p = nullptr;
  *p = 0x0badc0de;
}

nimbus::sys::PanicArm takeArm() {
  portENTER_CRITICAL(&g_armMux);
  const nimbus::sys::PanicArm a = g_arm;
  portEXIT_CRITICAL(&g_armMux);
  return a;
}

void setArm(const nimbus::sys::PanicArm& a) {
  portENTER_CRITICAL(&g_armMux);
  g_arm = a;
  portEXIT_CRITICAL(&g_armMux);
}
}  // namespace

String armTurnPanic(const String& spec) {
  nimbus::sys::PanicArm a;
  if (!nimbus::sys::parsePanicArm(std::string(spec.c_str()), a))
    return "PANIC arm=turn: kind must be nullptr|abort|canary, optional round=N (0-50)";
  setArm(a);
  return String(nimbus::sys::panicArmReply(a).c_str());
}

void testPanicPoint() {
  const nimbus::sys::PanicArm a = takeArm();
  if (!nimbus::sys::panicArmFires(a, g_journalActive, g_journal.round)) return;
  setArm(nimbus::sys::PanicArm());
  Serial.printf("PANIC %s firing at round %d of the turn\n", nimbus::sys::panicKindName(a.kind),
                g_journal.round);
  Serial.flush();
  if (a.kind == nimbus::sys::PanicKind::Abort) abort();
  if (a.kind == nimbus::sys::PanicKind::Canary) deepen(0);
  nullStore();
}

// The armed turn ended before its round: drop the arm so it never fires in a later,
// unrelated turn.
static void dropUnfiredArm() {
  const nimbus::sys::PanicArm a = takeArm();
  if (a.kind == nimbus::sys::PanicKind::None) return;
  setArm(nimbus::sys::PanicArm());
  agent::alogf("PANIC arm=turn dropped: the turn ended at round %d, before round %d",
               g_journal.round, a.round);
}
#endif

}  // namespace nimbus::crash
