#pragma once
#include <string>
#include <vector>

// embedding - the PORTABLE half of the provider /embeddings call: build the
// request body and parse the response. The network TLS glue is a device seam
// (src/agent/adapters/embeddings.*); keeping the JSON build+parse here means the
// wire format is host-tested (pio test -e native) with no network. Uses the
// OpenAI-compatible embeddings shape.
//
// Request  (OpenAI /v1/embeddings): {model, input, dimensions?, encoding_format:"float"}
//   - `dimensions` is sent only when >0 (OpenAI text-embedding-3-* Matryoshka
//     truncation, e.g. 256). Providers without it (Mistral: fixed 1024) get a
//     request with no dimensions field.
// Response (OpenAI): {data:[{embedding:[float,...]}], ...} - we take data[0].
namespace nimbus {
namespace orch {

// Serialize an embeddings request body. `dims<=0` omits the dimensions field.
std::string buildEmbeddingRequest(const std::string& model, const std::string& input, int dims);

// Parse a response body; fill `out` with data[0].embedding floats. Returns false
// with `err` set on: bad JSON, missing/empty data, non-array embedding, or (when
// `expectedDims>0`) a length mismatch. Deterministic + allocation-bounded.
bool parseEmbeddingResponse(const char* json, int expectedDims,
                            std::vector<float>& out, std::string& err);

}  // namespace orch
}  // namespace nimbus
