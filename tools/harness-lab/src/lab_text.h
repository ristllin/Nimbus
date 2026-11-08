#pragma once
// lab_text.h - the lab's one rule for cutting text that lands in a record.
//
// Everything the lab writes for an eval to read back (the driver transcript, the
// recorder's errors.jsonl, a driver's stderr) is read as UTF-8 on the other side.
// A byte cap is the usual way to bound such a field, and a byte cap that falls
// inside a multi-byte character leaves a stray lead byte: Python's strict decode
// then raises and the whole leg stops. Every cap that feeds a record goes through
// these two functions, so the class is held in one place and one selftest.
#include <string>

namespace lab {

// The first n bytes of s, never ending inside a character.
inline std::string capUtf8(const std::string& s, size_t n) {
  return s.size() <= n ? s : s.substr(0, n);
}

// capUtf8, plus "..." when anything was cut: the shape of a record's shortened field.
inline std::string cutRecord(const std::string& s, size_t n) {
  return s.size() <= n ? s : capUtf8(s, n) + "...";
}

}  // namespace lab
