#pragma once
#include <cstddef>
#include <string>

// provider_error - ONE way to turn a provider's refusal into a line (CUM-620).
//
// Every provider seam used to build its own "<what> HTTP <code>: <text>" line: the
// head adapters through the harness wire helpers, the photo caption step, speech and
// the download scan with their own printf shapes and raw body excerpts (a raw body
// can carry a newline that forges a second log line). These three helpers are the
// one shape for all of them, in lib/core so the device seams use the same code.
namespace nimbus {
namespace orch {

// A provider's text as one log-safe line: control bytes become spaces (provider text
// can never forge a second log line, or reach an owner reply with them), capped at
// `cap` bytes on a character boundary.
std::string oneLineText(std::string s, size_t cap = 300);

// The provider's own error text from any JSON error body it answers with:
// {"error":{"message":...}}, {"error":"<code>"}, {"message":"..."}, Mistral's
// {"message":{"detail":[{"msg":...}]}}, {"detail":"..."} and {"detail":[{"msg":...}]}.
// "" when the body is not JSON or names nothing. Never the raw body.
std::string providerErrorText(const std::string& body);

// "<what> HTTP <status>[ <tag>][: <provider message>]" - the one shape every provider
// error line takes (classifyStepError reads the status back from it). `tag` carries
// an extra machine marker such as a 429's "[rl:day-utc]" quota window ("" = none).
std::string providerErrLine(const char* what, int status, const std::string& providerMsg,
                            const std::string& tag = std::string());

}  // namespace orch
}  // namespace nimbus
