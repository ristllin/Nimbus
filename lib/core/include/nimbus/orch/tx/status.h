#pragma once
#include <cstdint>

// tx::Status - the one typed outcome of every engine-2 function (transcript core in
// lib/core and the plain engine in lib/harness). A function that has a body to write
// answers Err::NotImplemented until its lane writes it, so a caller (or a pending test)
// can tell "not built yet" from a real failure without reading words.
//
// The Err values and their slugs ride logs and the e item; append new values at the
// end and never renumber.
namespace nimbus {
namespace orch {
namespace tx {

enum class Err : uint8_t {
  None = 0,
  NotImplemented = 1,   // the lane that owns this body has not written it yet
  InvalidArgument = 2,  // the caller passed something the contract rules out
  NotStored = 3,        // an append failed: the item stays in RAM, marked not stored
  NotFound = 4,         // no such item, file, handle or registration
  Torn = 5,             // a line failed its crc or has no end: dropped, never repaired
  TooLarge = 6,         // over a stated ceiling (a line over 60,000 B, an arena full)
  Io = 7,               // the storage layer refused (card gone, rename failed)
  Busy = 8,             // a registration slot already holds a handler
};

inline const char* errSlug(Err e) {
  switch (e) {
    case Err::None: return "ok";
    case Err::NotImplemented: return "not-implemented";
    case Err::InvalidArgument: return "invalid-argument";
    case Err::NotStored: return "not-stored";
    case Err::NotFound: return "not-found";
    case Err::Torn: return "torn";
    case Err::TooLarge: return "too-large";
    case Err::Io: return "io";
    case Err::Busy: return "busy";
  }
  return "unknown";
}

// `where` is a static string naming the function that answered (a stub names itself),
// so a pending test can report which seam is still missing.
struct [[nodiscard]] Status {
  Err err = Err::None;
  const char* where = "";

  bool ok() const { return err == Err::None; }
  bool notImplemented() const { return err == Err::NotImplemented; }

  static Status success() { return Status{}; }
  static Status fail(Err e, const char* at) { return Status{e, at}; }
  static Status pending(const char* at) { return Status{Err::NotImplemented, at}; }
};

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
