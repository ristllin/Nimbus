#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/harness/http.h"
#include "nimbus/orch/psram_alloc.h"
#include "nimbus/orch/token_usage.h"

// vision_caption - the FALLBACK photo reader (owner ruling R1: photos go to the head
// model as images; a description is only for when that is not possible: the head
// has no image input, the bytes could not be kept or sent, or the provider refused
// the image). It also writes the short transcription a photo keeps in memory, and
// answers image.look's question about an older photo.
//
// Portable (lib/harness, host-tested over FakeHttpTransport). The device adapter
// (src/agent/adapters/image_vision.cpp) supplies keys, the transport and the owner
// notice; everything else is here:
//   - current models only (captionModel), detail high, low effort, the token field
//     per model family (OpenAI gpt-5 and newer and the o-series: max_completion_tokens;
//     every other: max_tokens);
//   - a sequential provider walk over the keyed providers in priority order, sticky
//     per Telegram page (the provider that answered keeps the rest of the album);
//   - every refusal classified and parked through the one shared park
//     (nimbus/orch/provider_park.h): a billing or key refusal closes that provider for
//     every seam and the walk moves on in the same photo; a 5xx, a rate limit or a
//     connect failure walks on without parking;
//   - one owner notice per billing episode, shared with every other seam's;
//   - a typed failure class per photo (ReadFail) that becomes its own label words,
//     never carrying a response body, a key or a URL;
//   - one error log line per refusal naming host, model and the provider's code.
namespace agent {
namespace vision {

// Why a photo was not read. Append-only (the label words and visionLastErr name them).
enum class ReadFail : uint8_t {
  None = 0,        // read
  NoProvider,      // no provider with a key can look at a photo
  Closed,          // every keyed provider is parked or paused (the park's words)
  Billing,         // out of credit (provider and HTTP status named)
  Auth,            // the key was refused
  TooLarge,        // over the photo budget
  Unsupported,     // a format no provider takes (HEIC, an animated GIF, not an image)
  LowMemory,       // the device could not hold the photo
  ProviderError,   // any other refusal, or an answer with no usable text
  Network,         // no answer
  Count,           // sentinel: tests walk every value below it
};
const char* readFailSlug(ReadFail f);   // "none", "no-provider", "closed", "billing", ...

// What the caption step is asked to write.
enum class CaptionKind : uint8_t {
  Fallback,     // the head cannot see the photo: a full transcription for it
  Memory,       // the short record a photo keeps in memory (one or two sentences)
  Question,     // image.look: answer one question about an older photo
};

struct CaptionRequest {
  const uint8_t* bytes = nullptr;   // the photo (PSRAM on the device); outlives the call
  size_t len = 0;
  std::string mime;                 // nimbus::orch::imagePartMime()
  std::string context;              // the sender's words, the album place ("" = none)
  std::string question;             // CaptionKind::Question: the head's question
  CaptionKind kind = CaptionKind::Fallback;
};

struct CaptionResult {
  std::string text;          // the transcription ("" when not read)
  ReadFail fail = ReadFail::None;
  std::string provider;      // who answered (or the refusal that decided `fail`)
  std::string model;
  int status = 0;            // that refusal's HTTP status (0 = none)
  std::string words;         // plain words for the label: never a body, a key or a URL
  nimbus::orch::TokenUsage usage;   // what the answering provider billed
  int requests = 0;          // requests sent (one per provider tried)
};

struct CaptionDeps {
  HttpTransport* http = nullptr;   // REQUIRED
  // The key a request to `provider` would carry ("" = it cannot look at a photo).
  std::function<std::string(const std::string& provider)> key;
  std::function<std::string()> priority;   // the provider priority CSV ("" = the default order)
  std::function<uint32_t()> nowMs;
  // The Cumulo router: its host (bare, "" = no router key) and the selected
  // "<upstream>/<model>" (the upstream picks the wire). Nullable.
  std::function<std::string()> routerHost;
  std::function<std::string()> routerSelection;
  // The caption model of a provider (device: the role default picked from its catalog,
  // lane DEF's store::captionModel). Nullable, or "" / a model without image input:
  // captionModel() below.
  std::function<std::string(const std::string& provider)> model;
  // The owner notice for a billing refusal: reachable() says an owner chat exists and
  // this is not a quiet background turn (only then is the shared latch taken), send()
  // delivers the words. Nullable (no notice).
  std::function<bool()> ownerReachable;
  std::function<void(const std::string& words)> notifyOwner;
};

// The caption model of each provider when the device names none: the role-default
// table's caption fallback (nimbus/orch/model_defaults.h, lane DEF), for the providers
// with an image path here (anthropic, openai, mistral); "" elsewhere.
const char* captionModel(const std::string& provider);

// The longest transcription kept (a longer answer is a logged failure: an endless
// reply is not a description).
constexpr size_t kMaxCaptionText = 16384;
// The longest error log line (host, model and the provider's code stay inside it).
constexpr size_t kErrLineMax = 160;

// One provider's request (exposed for the wire tests). `routedHost` != "" sends it
// through the Cumulo router at /router/<provider>/v1/... with Bearer auth.
bool buildCaptionRequest(const std::string& provider, const std::string& model, const std::string& key,
                         const CaptionRequest& in, const std::string& routedHost, HttpRequest& out);

// The error log line for a refused caption request: "vision: <host> <model> HTTP
// <status> <code>: <message>", at most kErrLineMax bytes, one line.
std::string errLine(const std::string& host, const std::string& model, int status,
                    const std::string& code, const std::string& message);

// Read one photo: the provider walk. Never throws; a failure is a typed result.
CaptionResult caption(const CaptionDeps& d, const CaptionRequest& in);

// The page (album) is over: the next photo walks from the top of the order again.
void endPage();

// image.look {ref, question} (R1: an older photo is looked at again only on demand):
// the ref must be exactly 16 lowercase hex characters (a blob hash) and the question
// non-empty and at most kLookQuestionMax bytes. false sets `err` (the tool's words).
constexpr size_t kLookQuestionMax = 500;
bool lookArgsValid(const std::string& ref, const std::string& question, std::string& err);

// ---- a photo's record in memory (R1) and the re-read after a park ends (L19-5) ----
// A photo the head saw itself is stored with a pending label; one short machine
// transcription is written for it afterwards, between Telegram cycles (never inside a
// turn), and replaces the row text. A photo whose fallback reading failed for a
// refusal or a closed provider is stored with its typed reason and read once more when
// a provider is open again. Bounded, newest first, only photos inside the size budget;
// a failed attempt waits longer each time and gives up after kRecordTries, so nothing
// loops. RAM only (a restart drops the queue; the rows keep their labels). The
// strings and the queue go through the working-set allocator (PSRAM on the device):
// an item can wait behind the upload deletes for many minutes.
struct PendingRecord {
  using Str = nimbus::orch::WorkingString;
  Str chatId;
  Str ref;        // the blob hash (16 hex): the row and the stored copy
  Str mime;
  Str tag;        // the photo's bracket, "[image 2/3 ref=...]"
  Str context;    // the sender's words with the photo
  Str ext;        // its stored copy's extension ("jpg" for a photo)
  size_t len = 0;
  uint8_t tries = 0;
  uint32_t notBeforeMs = 0;
};

class RecordQueue {
 public:
  static constexpr size_t kMax = 16;
  static constexpr uint8_t kRecordTries = 3;
  static constexpr uint32_t kRetryBaseMs = 10u * 60u * 1000u;   // 10, 20, 40 minutes
  // false (nothing queued) for a photo over the size budget or without a ref; a full
  // queue drops its oldest item. A photo already queued is not queued twice.
  bool push(const PendingRecord& r);
  // The newest item due now (nullptr = none). `providerOpen`: a provider can be asked.
  const PendingRecord* due(uint32_t nowMs, bool providerOpen) const;
  // The outcome of the item `ref`: done (written, or given up) removes it; a failure
  // keeps it, waiting kRetryBaseMs << tries before the next attempt.
  void settle(const PendingRecord::Str& ref, bool ok, uint32_t nowMs);
  size_t size() const { return items_.size(); }
  void clear() { items_.clear(); }

 private:
  // Oldest first. A vector, not an array: the records live on the heap (PSRAM on the
  // device), never in static internal RAM (the static DRAM release gate).
  nimbus::orch::WorkingVector<PendingRecord> items_;
};
RecordQueue& recordQueue();

// The label words of a typed failure ("too large", "openai out of credit (HTTP 429)").
std::string failWords(ReadFail f, const std::string& provider, int status);
// A failure the device reads again by itself once a provider is open (a refusal, a
// closed provider, no answer): the photo is queued, so its label says it is kept and
// the head never asks for it to be sent again.
bool rereadLater(ReadFail f);
// The words of a failed reading's label: the typed words, plus "; it is kept and re-read
// once the provider is back, so it need not be sent again (the owner can ask about it
// then)" when rereadLater(). Nothing tells the owner by itself once it is read.
std::string notReadWords(const CaptionResult& r);

// The last failure, for device.status (visionLastErr): when, which provider, the
// class and the HTTP status. `atMs` 0 = none since boot.
struct LastError {
  uint32_t atMs = 0;
  std::string provider;
  ReadFail fail = ReadFail::None;
  int status = 0;
};
LastError lastError();
void clearLastError();   // tests

}  // namespace vision
}  // namespace agent
