#include "nimbus/orch/retention.h"

#include <cctype>

namespace nimbus {
namespace orch {

namespace {
// Lowercased path with "//" collapsed and "." segments dropped. Sets `dotdot`
// when any ".." segment appears (the caller then fails closed).
std::string normalizedPath(const std::string& path, bool& dotdot) {
  dotdot = false;
  std::string out;
  out.reserve(path.size());
  size_t i = 0;
  while (i < path.size()) {
    if (path[i] == '/') { ++i; continue; }
    size_t j = path.find('/', i);
    if (j == std::string::npos) j = path.size();
    const size_t n = j - i;
    if (n == 1 && path[i] == '.') { i = j; continue; }
    if (n == 2 && path[i] == '.' && path[i + 1] == '.') dotdot = true;
    out += '/';
    for (size_t k = i; k < j; ++k) out += (char)std::tolower((unsigned char)path[k]);
    i = j;
  }
  return out;
}
}  // namespace

bool isRetentionExempt(const std::string& path) {
  bool dotdot = false;
  const std::string p = normalizedPath(path, dotdot);
  if (dotdot) return true;
  bool rootDotdot = false;
  const std::string root = normalizedPath(kTgArchiveRoot, rootDotdot);
  if (p.compare(0, root.size(), root) != 0) return false;
  return p.size() == root.size() || p[root.size()] == '/';
}

const char* retentionStatusName(RetentionStatus s) {
  switch (s) {
    case RetentionStatus::Ran:          return "ran";
    case RetentionStatus::DeferredTurn: return "deferred-turn";
    case RetentionStatus::DeferredBusy: return "deferred-busy";
    case RetentionStatus::NoStore:      return "no-store";
    case RetentionStatus::ClockUnset:   return "clock-unset";
  }
  return "unknown";
}

std::string retentionSummary(RetentionStatus st, const EpiPruneReport& rep) {
  std::string out = std::string("status=") + retentionStatusName(st);
  if (st != RetentionStatus::Ran) return out;
  out += " dayfiles=" + std::to_string(rep.removedDayFiles.size());
  out += rep.blobGcSkipped ? std::string(" blobs=deferred")
                           : " blobs=" + std::to_string(rep.removedBlobs.size());
  out += " kept=" + std::to_string(rep.keptMessages);
  return out;
}

RetentionResult runEpisodicRetention(AppendLogEpisodicStore* store, uint32_t nowDay,
                                     int retentionDays, const std::string& blobDir,
                                     bool turnInFlight) {
  RetentionResult res;
  if (!store || retentionDays <= 0) { res.status = RetentionStatus::NoStore; return res; }
  if (nowDay < (uint32_t)retentionDays) { res.status = RetentionStatus::ClockUnset; return res; }
  if (turnInFlight) { res.status = RetentionStatus::DeferredTurn; return res; }
  res.cutoffDay = nowDay - (uint32_t)retentionDays;
  res.report = store->prune(res.cutoffDay, blobDir);
  res.status = RetentionStatus::Ran;
  return res;
}

bool RetentionSchedule::record(RetentionStatus st, uint32_t nowMs) {
  if (st == RetentionStatus::DeferredTurn || st == RetentionStatus::DeferredBusy) {
    const bool first = !pending_;
    pending_ = true;
    return first;
  }
  pending_ = false;
  lastMs_ = nowMs;
  return false;
}

}  // namespace orch
}  // namespace nimbus
