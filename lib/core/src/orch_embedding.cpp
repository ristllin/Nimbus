#include "nimbus/orch/embedding.h"

#include <ArduinoJson.h>

using ArduinoJson::DeserializationError;
using ArduinoJson::JsonArrayConst;
using ArduinoJson::JsonDocument;

namespace nimbus {
namespace orch {

std::string buildEmbeddingRequest(const std::string& model, const std::string& input, int dims) {
  JsonDocument d;
  d["model"] = model;
  d["input"] = input;
  if (dims > 0) d["dimensions"] = dims;
  d["encoding_format"] = "float";
  std::string out;
  serializeJson(d, out);
  return out;
}

bool parseEmbeddingResponse(const char* json, int expectedDims,
                            std::vector<float>& out, std::string& err) {
  out.clear();
  err.clear();
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, json);
  if (e) { err = std::string("bad json: ") + e.c_str(); return false; }

  // Some providers surface an error object instead of data.
  if (doc["error"].is<ArduinoJson::JsonObjectConst>()) {
    const char* m = doc["error"]["message"] | "provider error";
    err = m;
    return false;
  }

  JsonArrayConst data = doc["data"].as<JsonArrayConst>();
  if (data.isNull() || data.size() == 0) { err = "no data[]"; return false; }
  JsonArrayConst emb = data[0]["embedding"].as<JsonArrayConst>();
  if (emb.isNull()) { err = "no embedding[]"; return false; }

  out.reserve(emb.size());
  for (ArduinoJson::JsonVariantConst v : emb) {
    if (!v.is<float>() && !v.is<double>() && !v.is<int>()) { err = "non-numeric element"; out.clear(); return false; }
    out.push_back((float)v.as<double>());
  }
  if (out.empty()) { err = "empty embedding"; return false; }
  if (expectedDims > 0 && (int)out.size() != expectedDims) {
    err = "dim mismatch: got " + std::to_string((int)out.size()) +
          " expected " + std::to_string(expectedDims);
    out.clear();
    return false;
  }
  return true;
}

}  // namespace orch
}  // namespace nimbus
