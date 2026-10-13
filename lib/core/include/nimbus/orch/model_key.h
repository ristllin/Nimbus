#pragma once
#include <cstddef>

// model_key - the ONE rule for turning a turn's model into the key its spend and
// its price are filed under (owner 2026-10-13: "different models cost differently
// for the same provider"). Portable (NO Arduino) and allocation-free (char buffers
// only), so the per-turn record path can call it without touching the heap.
//
// The key is (provider slot, normalized model):
//   - direct providers: the bare id with ONE version or snapshot suffix stripped,
//     so a dated echo ("claude-haiku-4-5-20251001", "gpt-4o-2024-08-06") lands on
//     its alias ("claude-haiku-4-5", "gpt-4o") and the stored key set stays bounded;
//   - "cumulo": "<upstream>/<bare>", kept apart from direct-key usage of the same
//     model because Cumulo bills at its own (router) rates;
//   - "custom": the raw id, never stripped (a LAN model's name is whatever the owner
//     configured).
// The money-safe rule (the cloud's F8 rule, ported): only a version or snapshot
// suffix is stripped, never a capability suffix (-mini, -luna, -terra, -pro, -flash,
// -air) and never a 3-digit suffix (-002, -405): those are different models with
// different prices. The coarse turnChipDisclosure family is NOT used here, because
// it merges gpt-5.6 with gpt-5.6-luna.

namespace nimbus {
namespace orch {

// Key buffer size: a key is 1..47 characters plus the NUL.
inline constexpr size_t kModelKeyCap = 48;
// The reserved fold row ("every other model of this provider"). Never accepted from
// input: '~' is outside the key charset, so modelKeySafe() rejects it.
inline constexpr const char* kOtherModel = "~other";

// A key the ledger text formats can carry: 1..47 characters of [A-Za-z0-9._:/+-]
// (no '|' or ';', the record separators, no whitespace, no '~').
bool modelKeySafe(const char* s);

// A provider id the ledger text formats can carry: 1..15 characters of [a-z0-9_-].
bool provKeySafe(const char* s);

// Strip ONE trailing version or snapshot suffix from `raw` into `out`
// (kModelKeyCap bytes): -latest, -YYYYMMDD, -YYYY-MM-DD, -YYYY-MM (year 20xx), -vN,
// or a 4-digit build (-2411, -0125). Never lowercases. A raw id that does not fit
// the buffer yields "". Returns strlen(out).
size_t normalizeModelId(const char* raw, char* out);

// True when `suffix` is exactly one suffix normalizeModelId would strip
// ("-latest", "-20251001", "-2024-08-06", "-2026-01", "-v2", "-2411").
bool isVersionSuffix(const char* suffix);

// The upstream a bare router model id belongs to when the echo carries no prefix:
// claude* -> anthropic; mistral*, pixtral*, codestral*, ministral* -> mistral;
// glm* -> zai; anything else -> openai (the router's default upstream).
const char* inferUpstream(const char* bare);

// The key a turn's spend is filed under (out: kModelKeyCap bytes; "" = record at
// provider level only). `configured` is the host's configured model (for "cumulo"
// the router selector, e.g. "anthropic/claude-opus-5"); `served` is the model the
// provider echoed ("" when it echoed none). The served echo is used only when it
// is a REAL substitution: both normalized and still different. An alias echo
// ("gpt-5.5" -> "gpt-5.5-2026-01") files under the configured key; a substitution
// ("gpt-5.6" -> "gpt-5.6-luna") files under the served key. A result that fails
// modelKeySafe (or a Cumulo key whose model part holds a further '/') is "".
// Returns strlen(out).
size_t usageModelKey(const char* host, const char* configured, const char* served, char* out);

}  // namespace orch
}  // namespace nimbus
