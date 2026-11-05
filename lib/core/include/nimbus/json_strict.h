#pragma once
#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <string>

// json_strict - request JSON a strict parser accepts, whatever the text holds.
//
// JSON forbids a raw byte under 0x20 inside a string. ArduinoJson escapes NUL, \b, \f,
// \n, \r and \t and writes the other 26 control bytes as they are, so one such byte in
// a tool result, a stored file or a sender's message made the whole request unreadable:
// Anthropic and OpenAI answer 400, Z.ai 400, Mistral 422, and the turn ends (1 Nov,
// files.read of a PNG; probed live on all four). Each of them accepts the \u00XX form.
//
// The compact form has no byte under 0x20 outside a string, so the writer can escape
// every one it sees, in values and keys alike, in one pass with no second buffer. A
// body with no such byte comes out byte-identical to serializeJson, so cache prefixes
// do not move.
//
// Header-only and Arduino-free: lib/core, the harness and the device adapters share it.
namespace nimbus {

class StrictJsonWriter {
 public:
  explicit StrictJsonWriter(std::string& out) : out_(out) {}
  size_t write(uint8_t c) {
    if (c >= 0x20) {
      out_.push_back(static_cast<char>(c));
      return 1;
    }
    static constexpr char kHex[] = "0123456789abcdef";
    const char esc[6] = {'\\', 'u', '0', '0', kHex[c >> 4], kHex[c & 0x0F]};
    out_.append(esc, sizeof(esc));
    return 1;   // ArduinoJson counts what it handed over, not what landed
  }
  size_t write(const uint8_t* s, size_t n) {
    for (size_t i = 0; i < n; i++) write(s[i]);
    return n;
  }

 private:
  std::string& out_;
};

// Appends `doc` to `out`; room for the plain form is reserved first.
template <typename TDoc>
inline void serializeStrictJson(const TDoc& doc, std::string& out) {
  out.reserve(out.size() + measureJson(doc));
  StrictJsonWriter w(out);
  serializeJson(doc, w);
}

}  // namespace nimbus
