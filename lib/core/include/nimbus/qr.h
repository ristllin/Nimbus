#pragma once
#include <cstdint>
#include <string>

// qr - a portable QR Code encoder for the config/setup screens.
//
// Deliberately tiny scope: BYTE mode only, ECC level M only, versions 1-6
// auto-selected (byte capacities at M: 14/26/42/62/84/106 chars - the config
// URLs this encodes are ~20 chars => v2, 25 modules). Deterministic, no
// Arduino, no clock: the same text always yields the identical module map, so
// the golden-image suite can byte-compare rendered QR screens and
// test/test_qr pins the output against reference vectors.
//
// Adapted from Project Nayuki's QR-Code-generator library (MIT License),
// https://www.nayuki.io/page/qr-code-generator-library - see qr.cpp for the
// attribution header. Mask selection is the full penalty-based algorithm,
// kept identical to the reference so codes stay maximally scannable and the
// known-answer vectors stay stable.

namespace nimbus::qr {

// One encoded symbol. `size` = 17 + 4*version modules per side (max 41 at
// v6). Modules are packed row-major, MSB-first, bit = dark. module() is
// bounds-safe: out-of-range reads return false (light), so renderers can
// probe the quiet zone without guards.
struct QrCode {
  int version = 0;             // 1..6; 0 = not encoded / encode() failed
  int size = 0;                // 17 + 4*version
  uint8_t modules[211] = {};   // (41*41 + 7) / 8 bytes
  bool module(int x, int y) const;
};

// Encode `text` as one byte-mode QR at ECC level M, auto version 1-6, with
// penalty-selected mask (ISO/IEC 18004). Returns false - leaving `out`
// zeroed - when text is empty or exceeds the v6-M byte capacity (107+
// bytes). Never throws; heap use is a few small std::vector scratch buffers.
bool encode(const std::string& text, QrCode& out);

}  // namespace nimbus::qr
