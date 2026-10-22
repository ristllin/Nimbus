#include "nimbus/orch/spend_ceiling.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nimbus/orch/model_defaults.h"
#include "nimbus/orch/model_price_defaults.h"

namespace nimbus {
namespace orch {

// ---- month key -----------------------------------------------------------------

uint32_t ceilingMonthKey(time_t now) {
  if (now < kCeilingClockValid) return 0;
  struct tm tmv;
  if (!localtime_r(&now, &tmv)) return 0;
  return (uint32_t)(tmv.tm_year + 1900) * 100u + (uint32_t)(tmv.tm_mon + 1);
}

uint32_t nextMonthKey(uint32_t monthKey) {
  if (monthKey == 0) return 0;
  const uint32_t y = monthKey / 100, m = monthKey % 100;
  return m >= 12 ? (y + 1) * 100 + 1 : y * 100 + m + 1;
}

const char* noticeChannelSlug(NoticeChannel c) {
  switch (c) {
    case NoticeChannel::Telegram: return "telegram";
    case NoticeChannel::Web:      return "web";
    case NoticeChannel::Screen:   return "screen";
  }
  return "?";
}

namespace {
uint8_t bitOf(NoticeChannel c) { return (uint8_t)(1u << (uint8_t)c); }
}  // namespace

// ---- the ceiling -----------------------------------------------------------------

bool SpendCeiling::frozenWith(uint64_t pendingMicro) const {
  if (frozen) return true;
  return addSatMicro(spentMicro, pendingMicro) >= limitMicro;
}

void SpendCeiling::settle() {
  const bool over = spentMicro >= limitMicro;
  if (over == frozen) return;
  frozen = over;
  noticesSent = 0;   // a new episode owes every notice; a closed one owes none
  if (over) ++episodes;
}

bool SpendCeiling::roll(uint32_t nowMonth) {
  if (nowMonth == 0 || nowMonth == monthKey) return false;
  if (monthKey == 0 || nowMonth < monthKey) {
    // The first month the clock reads takes the spend made while it was unset; a
    // clock set back moves the count to the earlier month and keeps it (fail-closed).
    monthKey = nowMonth;
    return true;
  }
  // A later month: a new count, the freeze closes, and no notice goes out.
  monthKey = nowMonth;
  spentMicro = 0;
  warned = false;
  frozen = false;
  noticesSent = 0;
  return true;
}

bool SpendCeiling::add(uint64_t micro, uint32_t nowMonth) {
  bool changed = roll(nowMonth);
  if (micro == 0) return changed;
  spentMicro = addSatMicro(spentMicro, micro);
  settle();
  return true;
}

bool SpendCeiling::setLimit(uint64_t micro) {
  if (micro < kMinLimitMicro) micro = kMinLimitMicro;
  if (micro > kMaxLimitMicro) micro = kMaxLimitMicro;
  if (micro == limitMicro && ownerSet) return false;
  limitMicro = micro;
  ownerSet = true;
  // A ceiling moved above the warning line owes a fresh warning when it is reached.
  if (spentMicro < limitMicro / 100 * kWarnPercent) warned = false;
  settle();
  return true;
}

bool SpendCeiling::noticeOwed(NoticeChannel c) const {
  return frozen && !(noticesSent & bitOf(c));
}

bool SpendCeiling::markNotice(NoticeChannel c) {
  if (noticesSent & bitOf(c)) return false;
  noticesSent |= bitOf(c);
  return true;
}

bool SpendCeiling::warnOwed() const {
  return !warned && !frozen && spentMicro >= limitMicro / 100 * kWarnPercent;
}

bool SpendCeiling::markWarned() {
  if (warned) return false;
  warned = true;
  return true;
}

bool SpendCeiling::migrationNoticeOwed(NoticeChannel c) const {
  return (migrationOwed & bitOf(c)) != 0;
}

bool SpendCeiling::markMigrationNotice(NoticeChannel c) {
  if (!(migrationOwed & bitOf(c))) return false;
  migrationOwed &= (uint8_t)~bitOf(c);
  return true;
}

SpendCeiling::Notice SpendCeiling::nextNotice(NoticeChannel ch) const {
  if (migrationNoticeOwed(ch)) return Notice::Migration;
  if (ch == NoticeChannel::Screen) return Notice::None;   // the screen shows a line
  if (noticeOwed(ch)) return Notice::Freeze;
  return warnOwed() ? Notice::Warn : Notice::None;
}

SpendCeiling SpendCeiling::firstBoot(uint32_t month, bool spentBefore, uint64_t monthToDateMicro,
                                     bool telegram) {
  SpendCeiling c;
  c.monthKey = month;
  if (!spentBefore) return c;
  c.add(monthToDateMicro, month);
  c.migrationOwed = bitOf(NoticeChannel::Web);
  if (telegram) c.migrationOwed |= bitOf(NoticeChannel::Telegram);
  return c;
}

bool SpendCeiling::markDelivered(NoticeChannel ch, Notice kind) {
  switch (kind) {
    case Notice::Migration: {
      const bool changed = markMigrationNotice(ch);
      return (frozen && markNotice(ch)) || changed;
    }
    case Notice::Freeze:    return markNotice(ch);
    case Notice::Warn:      return markWarned();
    case Notice::None:      break;
  }
  return false;
}

std::string SpendCeiling::serialize() const {
  char buf[200];
  snprintf(buf, sizeof(buf), "!v|1;%llu|%d|%lu|%llu|%d|%lu|%u|%d|%u",
           (unsigned long long)limitMicro, ownerSet ? 1 : 0, (unsigned long)monthKey,
           (unsigned long long)spentMicro, frozen ? 1 : 0, (unsigned long)episodes,
           (unsigned)noticesSent, warned ? 1 : 0, (unsigned)migrationOwed);
  return buf;
}

namespace {
// Parse one unsigned decimal field; false on anything but digits (empty reads 0).
bool parseField(const char*& p, uint64_t& out) {
  out = 0;
  while (*p && *p != '|') {
    if (*p < '0' || *p > '9') return false;
    const uint64_t d = (uint64_t)(*p - '0');
    if (out > (UINT64_MAX - d) / 10) return false;
    out = out * 10 + d;
    ++p;
  }
  if (*p == '|') ++p;
  return true;
}

// The fields of a "!v|1;..." line (missing tail fields read 0); false when malformed.
bool parseFields(const std::string& text, uint64_t (&f)[9]) {
  const size_t semi = text.find(';');
  if (semi == std::string::npos || text.compare(0, semi, "!v|1") != 0) return false;
  std::string body = text.substr(semi + 1);
  while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
  const char* p = body.c_str();
  for (int i = 0; i < 9 && *p; ++i)
    if (!parseField(p, f[i])) return false;
  return true;
}

uint64_t clampLimit(uint64_t v) {
  if (v == 0) return SpendCeiling::kDefaultLimitMicro;
  if (v < SpendCeiling::kMinLimitMicro) return SpendCeiling::kMinLimitMicro;
  return v > SpendCeiling::kMaxLimitMicro ? SpendCeiling::kMaxLimitMicro : v;
}

uint32_t validMonth(uint64_t v) {
  return (v % 100 >= 1 && v % 100 <= 12 && v <= 999912) ? (uint32_t)v : 0;
}
}  // namespace

bool SpendCeiling::deserialize(const std::string& text) {
  *this = SpendCeiling();
  uint64_t f[9] = {0};
  if (!parseFields(text, f)) return false;
  SpendCeiling s;
  s.limitMicro = clampLimit(f[0]);
  s.ownerSet = f[1] != 0;
  s.monthKey = validMonth(f[2]);
  s.spentMicro = f[3];
  s.frozen = f[4] != 0;
  s.episodes = (uint32_t)f[5];
  s.noticesSent = (uint8_t)(f[6] & 0x07);
  s.warned = f[7] != 0;
  s.migrationOwed = (uint8_t)(f[8] & 0x07);
  // The stored flag follows the stored count (a hand-edited or torn file cannot hold
  // an open episode under the ceiling, or a count over it with no episode).
  const bool notices = s.noticesSent != 0;
  const uint8_t sent = s.noticesSent;
  s.settle();
  if (s.frozen && notices) s.noticesSent = sent;
  *this = s;
  return true;
}

uint64_t parseCeilingCents(const std::string& field) {
  constexpr uint64_t kMaxCents = SpendCeiling::kMaxLimitMicro / 10000ULL;
  if (field.empty()) return 0;
  uint64_t cents = 0;
  for (char ch : field) {
    if (ch < '0' || ch > '9') return 0;
    cents = cents * 10 + (uint64_t)(ch - '0');
    if (cents > kMaxCents) cents = kMaxCents + 1;   // saturate: clamped below
  }
  if (cents == 0) return 0;
  const uint64_t micro = (cents > kMaxCents ? kMaxCents : cents) * 10000ULL;
  return micro < SpendCeiling::kMinLimitMicro ? SpendCeiling::kMinLimitMicro : micro;
}

// ---- copy ------------------------------------------------------------------------

std::string formatUsd(uint64_t micro) {
  const uint64_t cents = micro / 10000ULL;
  char buf[40];
  snprintf(buf, sizeof(buf), "$%llu.%02llu", (unsigned long long)(cents / 100),
           (unsigned long long)(cents % 100));
  return buf;
}

std::string resetDateText(uint32_t monthKey) {
  static const char* kMonths[] = {"January", "February", "March",     "April",
                                  "May",     "June",     "July",      "August",
                                  "September", "October", "November", "December"};
  const uint32_t next = nextMonthKey(monthKey);
  if (next == 0) return std::string();
  return std::string("1 ") + kMonths[(next % 100) - 1];
}

namespace {
constexpr const char* kWhere =
    "open the device's web page, go to Assistant > Usage, and set the monthly ceiling";

std::string resumeSentence(uint32_t monthKey) {
  const std::string d = resetDateText(monthKey);
  return d.empty() ? std::string("Otherwise paid calls resume when the next month starts.")
                   : "Otherwise paid calls resume on " + d + ".";
}

// The provider head row whose price table holds `head` (the first that does), or nullptr.
const RoleDefault* headRowPricing(const std::string& head) {
  if (head.empty()) return nullptr;
  for (const RoleDefault& r : kRoleDefaults)
    if (r.role == DefaultRole::Head && builtinModelPrice(r.slot, head.c_str())) return &r;
  return nullptr;
}

// The head model's list price over its provider's standard (role default) head, or 0.
// Only a head priced at twice the standard or more counts as premium.
double premiumRatio(const std::string& head, const RoleDefault* row) {
  if (!row) return 0;
  const BuiltinPrice* h = builtinModelPrice(row->slot, head.c_str());
  const BuiltinPrice* s = builtinModelPrice(row->slot, row->fallback);
  if (!h || !s || h == s) return 0;
  const double hp = (double)h->microIn + h->microOut, sp = (double)s->microIn + s->microOut;
  return (sp > 0 && hp >= 2 * sp) ? hp / sp : 0;
}

int daysInMonth(uint32_t monthKey) {
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const uint32_t y = monthKey / 100, m = monthKey % 100;
  if (m < 1 || m > 12) return 0;
  const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  return kDays[m - 1] + (m == 2 && leap ? 1 : 0);
}

}  // namespace

std::string freezeNoticeText(const CeilingFacts& f) {
  return "Paid AI calls are paused. This month's estimated spend (" + formatUsd(f.spentMicro) +
         ") reached the monthly spend ceiling of " + formatUsd(f.limitMicro) +
         ", so replies, helpers, voice, photo and image features stay off until the ceiling "
         "is raised or the month ends. To raise it, " + kWhere + ". " +
         resumeSentence(f.monthKey);
}

std::string freezeScreenLine(const CeilingFacts&) { return "Paid AI paused"; }

std::string freezeScreenDetail(const CeilingFacts& f) {
  return formatUsd(f.limitMicro) + " monthly ceiling reached. Raise it on the web page.";
}

std::string warnNoticeText(const CeilingFacts& f) {
  return "This month's estimated AI spend is " + formatUsd(f.spentMicro) + ", " +
         std::to_string(SpendCeiling::kWarnPercent) + "% of the " + formatUsd(f.limitMicro) +
         " monthly spend ceiling. At the ceiling, paid AI calls pause until the month ends or "
         "the ceiling is raised. To change it, " + kWhere + ".";
}

std::string migrationNoticeText(const CeilingFacts& f) {
  std::string s = "New in this update: a monthly spend ceiling. Paid AI calls (replies, helpers, "
                  "voice, photos and images) pause once this month's estimated spend across all "
                  "providers reaches " + formatUsd(f.limitMicro) +
                  ", and resume when the month ends or the ceiling is raised.";
  s += " This month so far: " + formatUsd(f.spentMicro) + ".";
  if (f.spentMicro >= f.limitMicro) {
    s += " That is already over the ceiling, so paid calls are paused now.";
  } else if (f.dayOfMonth > 0 && f.spentMicro > 0) {
    const uint64_t perDay = f.spentMicro / (uint64_t)f.dayOfMonth;
    const uint64_t days = perDay ? (f.limitMicro - f.spentMicro) / perDay : UINT64_MAX;
    const int left = daysInMonth(f.monthKey) - f.dayOfMonth;
    s += " At this month's pace (about " + formatUsd(perDay) + " a day)";
    if (left >= 0 && days > (uint64_t)left)
      s += " the month ends under the ceiling.";
    else
      s += days <= 1 ? std::string(" the ceiling is reached in about a day.")
                     : " the ceiling is reached in about " + std::to_string(days) + " days.";
  }
  const RoleDefault* row = headRowPricing(f.headModel);
  const double ratio = premiumRatio(f.headModel, row);
  if (ratio > 0) {
    char r[16];
    snprintf(r, sizeof(r), "%.1f", ratio);
    s += " The assistant runs on " + f.headModel +
         ", a premium model, so the ceiling is reached sooner; a standard model (" +
         row->fallback + ") costs about " + r + "x less for the same work.";
  }
  s += " To change it, " + std::string(kWhere) + ".";
  return s;
}

std::string noticeText(SpendCeiling::Notice kind, const CeilingFacts& f) {
  switch (kind) {
    case SpendCeiling::Notice::Migration: return migrationNoticeText(f);
    case SpendCeiling::Notice::Freeze:    return freezeNoticeText(f);
    case SpendCeiling::Notice::Warn:      return warnNoticeText(f);
    case SpendCeiling::Notice::None:      break;
  }
  return std::string();
}

std::string setupDefaultsText(uint64_t limitMicro) {
  return "Each provider starts on a standard model for the assistant and its cheapest model for "
         "helpers, photo captions, routines and the nightly dream. Paid AI calls pause once a "
         "month's estimated spend reaches " + formatUsd(limitMicro) +
         ". Change models on Assistant > Models and the ceiling on Assistant > Usage.";
}

// ---- the paid-call gate ------------------------------------------------------------

namespace {
std::atomic<SpendFrozenFn> g_frozenFn{nullptr};
std::atomic<SpendPricer> g_pricer{nullptr};
std::atomic<PendingSink> g_sink{nullptr};
std::atomic<SeamSpendSink> g_seamSink{nullptr};
// The per-seam counts exist only in host and test builds (the seam test and the bench
// SPENDSET? line read them): the production image keeps its static RAM.
#if defined(NIMBUS_NATIVE) || defined(NIMBUS_TEST)
std::atomic<uint32_t> g_consults[kSpendSeamCount];
std::atomic<uint32_t> g_refusals[kSpendSeamCount];
std::atomic<uint32_t> g_meterReports[kSpendSeamCount];
void count(std::atomic<uint32_t>* table, SpendSeam seam) {
  const int i = (int)seam;
  if (i >= 0 && i < kSpendSeamCount) table[i].fetch_add(1);
}
uint32_t read(const std::atomic<uint32_t>* table, SpendSeam seam) {
  const int i = (int)seam;
  return (i >= 0 && i < kSpendSeamCount) ? table[i].load() : 0;
}
#define NIMBUS_GATE_COUNT(table, seam) count(table, seam)
#else
#define NIMBUS_GATE_COUNT(table, seam) ((void)0)
#endif
}  // namespace

const char* spendSeamSlug(SpendSeam s) {
  // In SpendSeam order (append-only, like the enum).
  static const char* const kSlugs[kSpendSeamCount] = {
      "head", "wire", "sub", "embed", "vision-native", "vision-fallback", "stt", "tts",
      "image-gen", "url-fetch", "search", "dream", "routine", "wakeup", "fold", "moderation"};
  const int i = (int)s;
  return (i >= 0 && i < kSpendSeamCount) ? kSlugs[i] : "?";
}

void installSpendFreeze(SpendFrozenFn fn) { g_frozenFn.store(fn); }

namespace {
std::atomic<FreezeListener> g_listener{nullptr};
std::atomic<int> g_lastFreeze{-1};   // -1 = never reported
}  // namespace

void installFreezeListener(FreezeListener fn) {
  g_listener.store(fn);
  g_lastFreeze.store(-1);   // the next report reaches the new listener
}

void noteFreeze(bool frozen) {
  const int v = frozen ? 1 : 0;
  if (g_lastFreeze.exchange(v) == v) return;
  const FreezeListener fn = g_listener.load();
  if (fn) fn(frozen);
}

bool spendFrozen() {
  const SpendFrozenFn fn = g_frozenFn.load();
  return fn && fn();
}

bool paidCallAllowed(SpendSeam seam) {
  NIMBUS_GATE_COUNT(g_consults, seam);
  if (!spendFrozen()) return true;
  NIMBUS_GATE_COUNT(g_refusals, seam);
  return false;
}

#if defined(NIMBUS_NATIVE) || defined(NIMBUS_TEST)
uint32_t paidGateConsults(SpendSeam seam) { return read(g_consults, seam); }
uint32_t paidGateRefusals(SpendSeam seam) { return read(g_refusals, seam); }
void resetPaidGateCounts() {
  for (int i = 0; i < kSpendSeamCount; ++i) {
    g_consults[i].store(0);
    g_refusals[i].store(0);
  }
}
uint32_t seamMeterReports(SpendSeam seam) { return read(g_meterReports, seam); }
void resetSeamMeterCounts() {
  for (int i = 0; i < kSpendSeamCount; ++i) g_meterReports[i].store(0);
}
#else
uint32_t paidGateConsults(SpendSeam) { return 0; }
uint32_t paidGateRefusals(SpendSeam) { return 0; }
void resetPaidGateCounts() {}
uint32_t seamMeterReports(SpendSeam) { return 0; }
void resetSeamMeterCounts() {}
#endif

bool isSpendFrozenError(const std::string& err) {
  return err.find(kSpendFrozenError) != std::string::npos;
}

void installPendingSpend(SpendPricer pricer, PendingSink sink) {
  g_pricer.store(pricer);
  g_sink.store(sink);
}

void notePendingUsage(const char* host, const char* configuredModel, const TokenUsage& before,
                      const TokenUsage& after) {
  const SpendPricer pricer = g_pricer.load();
  const PendingSink sink = g_sink.load();
  if (!pricer || !sink || !host) return;
  auto minus = [](uint32_t a, uint32_t b) { return a > b ? a - b : 0u; };
  TokenUsage d;
  d.promptTokens = minus(after.promptTokens, before.promptTokens);
  d.completionTokens = minus(after.completionTokens, before.completionTokens);
  d.cacheReadTokens = minus(after.cacheReadTokens, before.cacheReadTokens);
  d.cacheWriteTokens = minus(after.cacheWriteTokens, before.cacheWriteTokens);
  d.responses = (uint16_t)minus(after.responses, before.responses);
  d.servedModel = after.servedModel;
  d.maxRequestIn = after.maxRequestIn;
  if (d.empty() && !d.cacheReadTokens && !d.cacheWriteTokens) return;
  TurnSpend ts;
  ts.add(host, configuredModel ? configuredModel : "", d);
  const uint64_t micro = pricer(ts);
  if (micro) sink(micro);
}

void installSeamMeter(SeamSpendSink sink) { g_seamSink.store(sink); }

void meterSeamUsage(SpendSeam seam, const char* provider, const char* model, const TokenUsage& u) {
  if (!provider || !*provider) return;
  if (u.empty() && !u.cacheReadTokens && !u.cacheWriteTokens) return;
  NIMBUS_GATE_COUNT(g_meterReports, seam);
  TokenUsage billed = u;
  if (!billed.responses) billed.responses = 1;   // an answered request billed once
  TurnSpend ts;
  ts.add(provider, model ? model : "", billed);
  const SeamSpendSink sink = g_seamSink.load();
  if (sink) sink(seam, provider, ts, 0);
}

void meterSeamMicro(SpendSeam seam, const char* provider, uint64_t micro) {
  if (!provider || !*provider || micro == 0) return;
  NIMBUS_GATE_COUNT(g_meterReports, seam);
  const SeamSpendSink sink = g_seamSink.load();
  if (sink) sink(seam, provider, TurnSpend(), micro);
}
#undef NIMBUS_GATE_COUNT

}  // namespace orch
}  // namespace nimbus
