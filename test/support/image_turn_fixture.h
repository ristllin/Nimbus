#pragma once
#include <ArduinoJson.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nimbus/orch/turn_images.h"

// R1 wire-suite fixture: a turn carrying two small photos (known bytes, so a test can
// assert the exact base64 the request carries), the routed turn text holding their
// attached labels, and helpers that read the image parts back out of a request body.
namespace harness_test {

struct PhotoTurn {
  std::vector<uint8_t> a{0xFF, 0xD8, 0xFF, 0xE0, 0x10, 0x20, 0x30};   // 7 bytes: a padded tail
  std::vector<uint8_t> b{0x89, 'P', 'N', 'G', 0x0D, 0x0A};            // 6 bytes: an exact group
  std::unique_ptr<nimbus::orch::TurnImages> set;
  int transcribed = 0;
  std::string text;   // the seeded user text (inputs)

  PhotoTurn() {
    set.reset(new nimbus::orch::TurnImages());
    set->nonce = "t3st";
    set->uploadOnce = false;   // inline unless a test asks for the upload
    add(1, "0123456789abcdef", "image/jpeg", a);
    add(2, "fedcba9876543210", "image/png", b);
    set->transcribe = [this](const nimbus::orch::TurnImage& it) {
      ++transcribed;
      return it.tag + " (machine transcription by anthropic: photo " + std::to_string(it.k) + " shows a label)";
    };
    text = "[CHANNEL] telegram\n\n[USER]\n" + set->items[0].segment + "\n" + set->items[1].segment +
           "\n\nThey wrote: what do the labels say?";
  }
  void add(int k, const char* ref, const char* mime, const std::vector<uint8_t>& bytes) {
    nimbus::orch::TurnImage it;
    it.ref = ref;
    it.mime = mime;
    it.bytes = bytes.data();
    it.len = bytes.size();
    it.k = k;
    it.n = 2;
    it.tag = "[image " + std::to_string(k) + "/2 ref=" + ref + "]";
    it.segment = it.tag + " (attached: the photo itself is in this message)";
    set->items.push_back(it);
  }
};

inline std::string b64Of(const std::vector<uint8_t>& v) {
  static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < v.size(); i += 3) {
    const uint32_t n = (uint32_t)v[i] << 16 | (i + 1 < v.size() ? (uint32_t)v[i + 1] << 8 : 0) |
                       (i + 2 < v.size() ? v[i + 2] : 0);
    out += k[(n >> 18) & 63];
    out += k[(n >> 12) & 63];
    out += i + 1 < v.size() ? k[(n >> 6) & 63] : '=';
    out += i + 2 < v.size() ? k[n & 63] : '=';
  }
  return out;
}

// Count the parts of `type` anywhere in a JSON value, and whether any of them (or any
// object carrying an image source) has a cache_control.
inline void walkParts(JsonVariantConst v, const char* type, int& count, bool& cached) {
  if (v.is<JsonArrayConst>()) {
    for (JsonVariantConst e : v.as<JsonArrayConst>()) walkParts(e, type, count, cached);
    return;
  }
  if (!v.is<JsonObjectConst>()) return;
  JsonObjectConst o = v.as<JsonObjectConst>();
  if (!strcmp(o["type"] | "", type)) {
    ++count;
    if (!o["cache_control"].isNull()) cached = true;
  }
  for (JsonPairConst kv : o) walkParts(kv.value(), type, count, cached);
}

}  // namespace harness_test
