#pragma once
#include <cstdint>
#include <string>
#include <vector>

// fetch_policy - the owner-governed trust ladder for files.fetch(url) (W18).
//
// Downloading arbitrary URLs onto the device is a real risk surface (malicious
// content entering the file store, prompt-injection documents entering future
// context). The owner picks how much trust the model gets - the SAME fail-closed
// pattern as agent skills and loops (created inert until approved):
//
//   Off     - the tool refuses, naming the setting (never silently absent).
//   Approve - every request queues; the OWNER approves each URL individually
//             (Telegram "/fetch approve <id>" or the web card). DEFAULT.
//   Scan    - the firmware downloads to QUARANTINE (unindexed temp - nothing
//             serves or recalls it), runs an AI verdict over the content head,
//             and promotes to the file store only on SAFE. Firmware-enforced:
//             a model cannot skip the scan by phrasing.
//   Yolo    - direct download, full trust.
//
// Portable + host-tested (test_fetch_policy): policy decisions, the request
// state machine, URL parsing and redirect resolution. The device supplies the
// actual TLS download, the quarantine filesystem and the verdict call.
namespace nimbus {
namespace orch {

enum class FetchPolicy : uint8_t { Off = 0, Approve = 1, Scan = 2, Yolo = 3 };

inline FetchPolicy fetchPolicyFromInt(int v) {
  return (v < 0 || v > 3) ? FetchPolicy::Approve : (FetchPolicy)v;
}
inline const char* fetchPolicyName(FetchPolicy p) {
  switch (p) {
    case FetchPolicy::Off:     return "off";
    case FetchPolicy::Approve: return "approve";
    case FetchPolicy::Scan:    return "scan";
    case FetchPolicy::Yolo:    return "yolo";
  }
  return "approve";
}

// ---- request lifecycle -------------------------------------------------------
enum class FetchState : uint8_t {
  PendingApproval,  // approve mode: waiting on the owner
  Ready,            // approved / yolo: the pump may download to the store
  Scanning,         // scan mode: downloaded to quarantine, verdict pending
  Done,             // file landed in the store
  Denied,           // owner said no
  Failed,           // download or scan failed (err says why)
};
inline const char* fetchStateName(FetchState s) {
  switch (s) {
    case FetchState::PendingApproval: return "pending";
    case FetchState::Ready:           return "ready";
    case FetchState::Scanning:        return "scanning";
    case FetchState::Done:            return "done";
    case FetchState::Denied:          return "denied";
    case FetchState::Failed:          return "failed";
  }
  return "failed";
}

struct FetchReq {
  uint32_t    id = 0;
  std::string url;
  std::string project, name;   // destination in the file store
  std::string requestedBy;     // chat id ("web"/"serial"/tg id) - audit trail
  FetchState  state = FetchState::PendingApproval;
  std::string err;             // Failed/Denied reason; scan verdict summary
  uint64_t    bytes = 0;       // downloaded size (Done)
};

// Bounded pending set. RAM-only by design: a reboot drops unfinished requests -
// honest and safe (the model re-asks; nothing half-approved survives a
// power-cycle that nobody remembers).
class FetchQueue {
 public:
  static constexpr size_t kMax = 4;         // in-flight + waiting, total
  static constexpr size_t kDoneKeep = 6;    // finished rows kept for the surfaces

  // Enqueue under the CURRENT policy. Returns the request id (>0), or 0 with
  // err set (off / full / duplicate URL still pending).
  uint32_t request(FetchPolicy pol, const std::string& url, const std::string& project,
                   const std::string& name, const std::string& by, std::string& err);

  bool approve(uint32_t id);   // owner: PendingApproval -> Ready
  bool deny(uint32_t id);      // owner: PendingApproval -> Denied

  // The pump's work selector: the first request in `s`, or nullptr.
  FetchReq*       firstIn(FetchState s);
  const FetchReq* find(uint32_t id) const;
  FetchReq*       findMut(uint32_t id);

  // Finish a request (Done/Denied/Failed): sets state + trims old finished rows.
  void finish(uint32_t id, FetchState s, const std::string& err, uint64_t bytes);

  const std::vector<FetchReq>& all() const { return q_; }
  int pendingCount() const;

 private:
  std::vector<FetchReq> q_;
  uint32_t nextId_ = 1;
};

// ---- URL parsing (portable, host-tested) ------------------------------------
// https ONLY: a plaintext-http download onto the device is an easy MITM write
// primitive - refuse it outright rather than warn.
struct ParsedUrl {
  std::string host;
  uint16_t    port = 443;
  std::string path;   // begins with '/'
  bool        ok = false;
};
ParsedUrl parseHttpsUrl(const std::string& url);

// Resolve a redirect Location against the current URL (absolute https URL,
// or an absolute path on the same host). "" = unusable (http downgrade,
// protocol-relative, or relative-path forms we do not follow).
std::string resolveRedirect(const ParsedUrl& from, const std::string& location);

}  // namespace orch
}  // namespace nimbus
