#pragma once
#include <ArduinoJson.h>

#include <string>

// image_gen - the OpenAI image-generation wire, as pure portable helpers so the
// device adapter (src/agent/adapters/image_gen.cpp) and the host test agree on
// exactly one request shape. Response is always requested as base64 (never a URL
// to re-fetch) and stream-decoded to SD by the adapter - the device never holds
// a whole PNG in its ~266 KB internal heap.

namespace nimbus {
namespace orch {

// Normalize a requested size to one the model actually accepts, defaulting to the
// universally valid 1024x1024. gpt-image-1 (the default) takes 1024x1024 /
// 1536x1024 / 1024x1536 / auto; dall-e-3 takes 1024x1024 / 1792x1024 / 1024x1792;
// dall-e-2 takes 256x256 / 512x512 / 1024x1024. An unknown/blank size (or a size
// the model would 400 on) collapses to 1024x1024 rather than failing the call.
inline std::string normalizeImageSize(const std::string& model,
                                      const std::string& size) {
  if (model == "dall-e-2") {
    if (size == "256x256" || size == "512x512" || size == "1024x1024") return size;
    return "1024x1024";
  }
  if (model == "dall-e-3") {
    if (size == "1024x1024" || size == "1792x1024" || size == "1024x1792") return size;
    return "1024x1024";
  }
  // gpt-image-1 (and the default): square / landscape / portrait / auto.
  if (size == "1024x1024" || size == "1536x1024" || size == "1024x1536" || size == "auto")
    return size;
  return "1024x1024";
}

// Build POST /v1/images/generations body. `model` blank => gpt-image-1: it returns
// base64 image data NATIVELY, which the device streams straight to SD. dall-e-*
// only emit base64 via a `response_format` param the current API now REJECTS
// (400 "Unknown parameter: 'response_format'"), and otherwise return a URL this
// one-TLS-slot device can't re-fetch - so gpt-image-1 is the supported model and
// `response_format` is never sent. `quality` (gpt-image-1 only) defaults to "low":
// fast, small, and cheap - the right tier for a device that streams the whole PNG
// to SD over a single TLS socket with a bounded deadline.
inline std::string imageGenRequestBody(const std::string& prompt,
                                       const std::string& model,
                                       const std::string& size,
                                       const std::string& quality = "") {
  const std::string m = model.empty() ? std::string("gpt-image-1") : model;
  ArduinoJson::JsonDocument doc;
  doc["model"] = m;
  doc["prompt"] = prompt;
  doc["n"] = 1;
  doc["size"] = normalizeImageSize(m, size);
  // dall-e-* use a different quality vocabulary (standard/hd), so only set the
  // low/medium/high tier for gpt-image-1.
  if (m == "gpt-image-1") doc["quality"] = quality.empty() ? std::string("low") : quality;
  std::string out;
  ArduinoJson::serializeJson(doc, out);
  return out;
}

}  // namespace orch
}  // namespace nimbus
