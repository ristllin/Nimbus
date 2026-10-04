#pragma once
#include <string>
#include <vector>

// embedding - the PORTABLE half of the provider /embeddings call: build the
// request body and parse the response. The network TLS glue is a device seam
// (src/agent/adapters/embeddings.*); keeping the JSON build+parse here means the
// wire format is host-tested (pio test -e native) with no network.
//
// The request body is PROVIDER-AWARE (CUM-469). It used to be one OpenAI-shaped
// body for every provider, and Mistral's /v1/embeddings schema has no
// `dimensions` field (mistral-embed is fixed at 1024), so every mistral embed
// call 422'd live. Each provider's body dialect is an EmbedWire, carried on its
// EmbedRoute below, so one table says everything about a provider's embeddings
// call (path, host/key source, body shape):
//
//   OpenAI  (openai, cumulo): {model, input, dimensions?, encoding_format:"float"}
//     `dimensions` is sent only when >0 (text-embedding-3-* Matryoshka
//     truncation, e.g. 256).
//   Mistral (mistral):        {model, input}
//     Per docs.mistral.ai/api/endpoint/embeddings the schema is model, input,
//     output_dimension, output_dtype, encoding_format, metadata: there is no
//     `dimensions`. `output_dimension` exists only for models that support it
//     (codestral-embed), so the width is never requested; `encoding_format` is
//     optional and float arrays are the default, so it is left out too.
//
// Width contract: the response must be exactly the configured width. When the
// provider cannot be asked for a width (Mistral), the vector comes back at the
// model's native width (mistral-embed: 1024), so the stored dims must equal it.
// A vector of any other width is REFUSED ("dim mismatch"), never truncated or
// padded: mistral-embed is not a Matryoshka model, so a truncated vector would
// compare as noise, and the vector store cannot hold a second width anyway.
//
// Response (both dialects): {data:[{embedding:[float,...]}], ...} - we take data[0].
namespace nimbus {
namespace orch {

// The request-body dialect a provider's /v1/embeddings speaks.
enum class EmbedWire : unsigned char {
  None = 0,  // no device embeddings route: no body is built
  OpenAI,    // {model, input, dimensions?, encoding_format:"float"}
  Mistral,   // {model, input} - the width is the model's own
};

// Where the device POSTs the embeddings call for a given embed provider, and in
// which body dialect. openai and mistral hit the provider's own /v1/embeddings.
// cumulo is the one-key flagship path: it proxies OpenAI's embed models through
// the router at /router/openai/v1/embeddings with the single router key, so the
// request body and response are byte-identical to a direct OpenAI call - a vector
// DB embedded via direct openai stays comparable after switching to cumulo, and
// vice versa. `known` is false (and `wire` None) for a provider with no device
// embeddings route.
struct EmbedRoute {
  const char* path;       // HTTP path to POST
  bool viaCumuloRouter;   // host + key come from the cumulo router, not a direct provider
  bool known;             // false => this provider has no embeddings route
  EmbedWire wire;         // request-body dialect
};
inline EmbedRoute embedRouteFor(const std::string& provider) {
  if (provider == "openai")  return {"/v1/embeddings", false, true, EmbedWire::OpenAI};
  if (provider == "mistral") return {"/v1/embeddings", false, true, EmbedWire::Mistral};
  if (provider == "cumulo")
    return {"/router/openai/v1/embeddings", true, true, EmbedWire::OpenAI};
  // anthropic has no public embeddings API; zai and custom are future work.
  return {"", false, false, EmbedWire::None};
}

// Serialize the embeddings request body for `provider` (an embedRouteFor slug) in
// that provider's dialect. `dims<=0` omits the OpenAI dimensions field; Mistral
// never carries a width. Returns "" for a provider with no embeddings route -
// never a silent OpenAI-shaped fallback.
std::string buildEmbeddingRequest(const std::string& provider, const std::string& model,
                                  const std::string& input, int dims);

// Parse a response body; fill `out` with data[0].embedding floats. Returns false
// with `err` set on: bad JSON, missing/empty data, non-array embedding, or (when
// `expectedDims>0`) a length mismatch ("dim mismatch: got N expected M" - see the
// width contract above). Deterministic + allocation-bounded.
bool parseEmbeddingResponse(const char* json, int expectedDims,
                            std::vector<float>& out, std::string& err);

}  // namespace orch
}  // namespace nimbus
