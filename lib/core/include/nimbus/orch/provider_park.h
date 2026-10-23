#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// provider_park - ONE gate per provider that every paid seam asks before it sends a
// request: head turns, sub-agent spawns, embeddings, the photo caption step, speech
// to text, text to speech, the download scan and image generation.
//
// Why one gate. On 19 Oct 2026 an OpenAI account ran out of credit. The embedding
// seam learned it and parked embeddings; the photo captioner kept uploading every
// photo to the same dead account (47 refused uploads), the sub-agent picker kept
// choosing OpenAI because it had a key, and the chat head kept its own separate
// park. Each seam had its own memo, or none. Now a refusal learned on any seam
// closes the provider for every seam, the owner is told once per episode, and the
// state survives a restart (a reboot used to cost one more doomed request and a
// second notice).
//
// What closes a provider:
//   - a billing refusal (billing_refusal.h): every seam, until a person adds credit;
//   - a key refusal (HTTP 401): every seam, until the key changes;
//   - a 403: only the seam that met it (OpenAI answers 403 for one model or one
//     endpoint the project may not use; the key still works elsewhere);
//   - the pause switch (below): every seam, never probed, until the cause that set
//     it lifts it.
// A rate limit, a server error, a timeout or a network failure closes nothing.
//
// Re-check: kRecheckMs after the refusal one request is let through (the probe, on
// whichever seam asks first; it may ask again within kProbeLeaseMs while it builds the
// request). Its success ends the episode; a refusal re-parks quietly (no second
// notice); any other failure, or no answer within the lease, re-arms the wait. A
// changed key is a new start: the park and its notice latch end at once.
//
// Durable: the parks, their re-check times (wall clock when known), the notice
// latch (with its one short answer) and the daily spend-warning latch are saved to a
// flash file on every change
// (the device registers the storage; never NVS). The pause switch is RAM only: the
// lane that pauses keeps its own durable state and re-applies the pause at boot.
namespace nimbus {
namespace orch {

// Every seam that spends money at a provider. Append-only (tests walk it).
enum class PaidSeam : uint8_t {
  Chat = 0,     // head turns: owner turns, routines, dreams, wakeups, synthesis
  Spawn,        // sub-agent dispatch
  Embeddings,   // memory writes and recall
  Vision,       // the photo caption step and image.look
  Stt,          // speech to text
  Tts,          // text to speech
  UrlFetch,     // the download safety scan
  ImageGen,     // image generation
};
inline constexpr int kPaidSeamCount = 8;
const char* paidSeamSlug(PaidSeam s);   // "chat", "spawn", "embeddings", ...

// Why a provider is parked. The numbers are persisted: append-only.
enum class ParkKind : uint8_t {
  None = 0,
  Billing = 1,   // out of credit or quota: a person must add credit
  Auth = 2,      // the key is refused: a person must fix the key
};
const char* parkKindSlug(ParkKind k);   // "none" | "billing" | "auth"

// The kind a provider's answer parks for: Billing for any billing refusal
// (isBillingRefusal), Auth for 401 and 403, else None (nothing parks).
ParkKind parkKindFor(int httpStatus, const std::string& text);

// ---- the pause switch ----------------------------------------------------------
// Bit flags: why paid requests to a provider are held off. Causes are independent:
// lifting one never releases another. kPauseAllProviders as the provider name
// pauses every provider, known or not.
enum PauseCause : uint8_t {
  kPauseOwner = 1u << 0,          // the owner paused the provider by hand
  kPauseSpendCeiling = 1u << 1,   // the monthly spend ceiling is reached
  kPauseProviderLimit = 1u << 2,  // lane DEF2: this provider's own monthly limit is reached
};
inline constexpr const char* kPauseAllProviders = "*";

// The answer to "may this seam send a request to this provider now?".
struct ParkVerdict {
  bool     open = true;            // send it
  bool     probe = false;          // open only as the one re-check request
  ParkKind kind = ParkKind::None;  // closed by a refusal: which one
  uint8_t  paused = 0;             // closed by the pause switch: its causes (bit flags)
  int      httpStatus = 0;         // the refusal's HTTP status (0 = none)
  uint32_t recheckInMs = 0;        // time until the re-check request (refusals only)
  bool     keyChanged = false;     // open because the key changed: the park just ended
};

// Plain words for a closed verdict: "openai says the account is out of credit
// (HTTP 429); it is checked again in 12 minutes", "paid requests to openai are
// paused". "" for an open verdict.
std::string parkWords(const char* provider, const ParkVerdict& v);
// The short form for a 48-character screen line: "openai is out of credit. Add
// credit.", "openai refused the key.", "Paid requests are paused." ("" when open).
std::string parkShortWords(const char* provider, const ParkVerdict& v);

// The fingerprint a park remembers its key by (32-bit FNV-1a of the key; a key
// change ends the park). 0 for an empty key; a caller passing 0 skips the check.
uint32_t parkKeyFingerprint(const std::string& key);

// The device's flash file (registered once at start-up; host tests may register a
// fake to prove the state survives a restart).
struct ParkStorage {
  // Read the stored blob: 1 = read, 0 = nothing stored, -1 = not ready (retry later).
  std::function<int(std::string& blob)> load;
  std::function<bool(const std::string& blob)> save;
  // Wall clock in seconds since the epoch; 0 = not known yet (no network time).
  std::function<uint32_t()> epochS;
  // The monotonic clock the callers pass as nowMs (device: millis), for the calls
  // that carry no time of their own (a latch, a success). Null = the last time seen.
  std::function<uint32_t()> nowMs;
};

// A read-only view of one provider's record (status surfaces, tests).
struct ParkEntry {
  char     provider[16] = {};
  ParkKind kind = ParkKind::None;
  int      httpStatus = 0;
  uint8_t  seamMask = 0;       // the seams it closes (bit = 1 << PaidSeam)
  uint32_t recheckInMs = 0;    // 0 = the re-check is due
  uint32_t sinceEpoch = 0;     // when it parked, wall clock (0 = unknown)
  bool     probing = false;    // the re-check request is out
  bool     noticed = false;    // the owner was told this episode
  bool     answered = false;   // ...and the one short answer to a refused turn went out
  uint32_t warnedDay = 0;      // the last day the daily spend warning went out
  uint8_t  paused = 0;         // the pause switch's causes
};

class ProviderPark {
 public:
  static constexpr size_t   kMaxProviders = 8;
  static constexpr uint32_t kRecheckMs = 30u * 60u * 1000u;
  // The re-check request's lease: the seam that got it may ask again while it builds
  // the request (a check, then the call), but no answer within the lease reads as a
  // failed probe, so a seam that never reports cannot leave the provider open.
  static constexpr uint32_t kProbeLeaseMs = 150u * 1000u;

  void setStorage(ParkStorage s);

  // Where the records live (kMaxProviders of them, allocated on the first question):
  // the device passes a PSRAM-first allocator, so the park keeps only its locks and
  // hooks in internal RAM; null (the default) is the C++ heap. Records already made
  // stay where they are.
  using RecordAlloc = void* (*)(size_t bytes);
  void setRecordMemory(RecordAlloc alloc);

  // THE question every paid seam asks before a request. `keyFp` is the
  // fingerprint of the key that request would carry (0 = unknown: not compared).
  // A paused provider is closed whatever its park; a parked one is closed until
  // its re-check is due, then open once as the probe.
  ParkVerdict available(const char* provider, PaidSeam seam, uint32_t keyFp, uint32_t nowMs);

  // A request was answered with a non-2xx `httpStatus`; `text` is everything the
  // provider said (message, type, code). Parks on a billing or key refusal and
  // returns the kind; anything else re-arms an open probe and returns None.
  ParkKind noteAnswer(const char* provider, PaidSeam seam, uint32_t keyFp, int httpStatus,
                      const std::string& text, uint32_t nowMs);
  // Park for a refusal the caller already typed (the chat fabric's step kinds).
  void park(const char* provider, PaidSeam seam, uint32_t keyFp, ParkKind kind, int httpStatus,
            uint32_t nowMs);
  // A request on `seam` succeeded: the episode ends (the park and the notice latch)
  // when the park covers that seam (a 403 closed only its own seam, so a success
  // elsewhere proves nothing about it). True when something ended.
  bool noteSuccess(const char* provider, PaidSeam seam);
  // A request failed without a refusal (network, timeout, 5xx, rate limit): when
  // it was the probe, the park re-arms for another kRecheckMs.
  void noteFailure(const char* provider, uint32_t nowMs);

  // The pause switch. `provider` may be kPauseAllProviders.
  void pause(const char* provider, uint8_t causes);
  void resume(const char* provider, uint8_t causes);
  uint8_t pausedBy(const char* provider) const;   // its own causes plus the all-providers ones

  // One owner notice per billing episode per provider, shared by every seam:
  // true exactly once until the episode ends (noteSuccess, a key change).
  bool noticeDue(const char* provider);
  void episodeEnd(const char* provider);
  // After that notice, one short answer to an owner turn refused for the same
  // episode (the turn the park answered with no request, or a re-check refused
  // again): true exactly once until the episode ends; every later refused turn of
  // the episode is answered with nothing. Saved with the notice latch.
  bool shortAnswerDue(const char* provider);
  // How long until the provider's re-check request (0 when it is due, or when the
  // provider is not parked). Reads only: it never hands out the probe.
  uint32_t recheckInMs(const char* provider);

  // One daily spend warning per provider per day: true once for `dayKey`.
  bool dailyWarningDue(const char* provider, uint32_t dayKey);
  // Whether that warning already went out for `dayKey` (no state change).
  bool warnedOn(const char* provider, uint32_t dayKey);

  // Every record with state (a park, a latch or a pause), up to `cap`.
  size_t snapshot(ParkEntry* out, size_t cap, uint32_t nowMs);

  // The persisted form ("PK1" then one record per line). Pauses are not in it.
  std::string serialize(uint32_t nowMs) const;
  bool deserialize(const std::string& blob, uint32_t nowMs);

  void clear();          // RAM state only (tests, a factory erase); storage untouched
  // The bench's clean slate (test console PARK clear): every park and latch ends and
  // the file is saved without them, so a restart does not bring them back. Pauses stay
  // (they belong to the owner and the spend limit).
  void forgetParks(uint32_t nowMs);
  void forgetStorage();  // tests: drop the registered storage and the loaded flag

 private:
  struct Rec {
    char     provider[16] = {};
    uint32_t keyFp = 0;
    ParkKind kind = ParkKind::None;
    int      httpStatus = 0;
    uint8_t  seamMask = 0;
    uint32_t recheckAtMs = 0;
    uint32_t sinceEpoch = 0;
    bool     probing = false;
    uint32_t probeUntilMs = 0;   // the probe's lease ends (probing only)
    bool     noticed = false;
    bool     answered = false;   // the episode's one short answer went out (implies noticed)
    uint32_t warnedDay = 0;
    uint8_t  paused = 0;
  };
  // The record table: allocated on first use through recordAlloc_ (under mx_).
  struct Span {
    Rec* b;
    Rec* e;
    Rec* begin() const { return b; }
    Rec* end() const { return e; }
    Rec& operator[](size_t i) const { return b[i]; }
  };
  Span recs();
  Span recs() const;   // empty until the table exists (a const call never allocates)
  Rec* find(const char* provider);
  const Rec* find(const char* provider) const;
  Rec* ensure(const char* provider);
  void dropIfEmpty(Rec& r);
  void endEpisode(Rec& r);
  void parkLocked(Rec& r, PaidSeam seam, ParkKind kind, int httpStatus, uint32_t nowMs);
  void loadLocked(uint32_t nowMs);
  void saveLocked();   // marks the state for the file; flush() writes it
  // Writes the file when the state changed, OUTSIDE the state lock (a seam on another
  // task never waits on a flash write); saveMx_ keeps the writes in order. Every
  // public mutator ends with it (FlushOnExit).
  void flush();
  struct FlushOnExit {
    ProviderPark& p;
    ~FlushOnExit() { p.flush(); }
  };
  uint32_t epochNow() const;
  uint32_t clockNow();   // storage's monotonic clock, else the last nowMs seen

  mutable std::recursive_mutex mx_;
  std::mutex  saveMx_;
  bool        dirty_ = false;
  Rec*        recs_ = nullptr;   // kMaxProviders records (device: PSRAM)
  RecordAlloc recordAlloc_ = nullptr;
  uint8_t     pausedAll_ = 0;
  ParkStorage storage_;
  bool        loaded_ = false;
  uint32_t    lastNowMs_ = 0;   // the latest nowMs any call carried
};

// The process-wide park every seam shares.
ProviderPark& providerPark();

// The walk a seam with several possible providers makes (the caption step, the
// download scan): the first provider in `order` that has a key AND is open for
// `seam`. A closed provider is skipped with no request; `closedWhy` keeps the words
// of the first one skipped, so a seam left with none can say why ("" when nothing
// was closed). `keyOf` returns the key a request there would carry ("" = no key).
struct ProviderPick {
  std::string provider;    // "" = no provider is keyed and open
  std::string closedWhy;   // parkWords of the first closed provider skipped
};
ProviderPick pickOpenProvider(const std::vector<std::string>& order,
                              const std::function<std::string(const std::string&)>& keyOf, PaidSeam seam,
                              uint32_t nowMs);

}  // namespace orch
}  // namespace nimbus
