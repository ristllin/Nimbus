#pragma once
// upload_reassembly - the portable, host-tested state machine the device uses to
// reassemble a chunked tunnel upload (protocol 2). It enforces ordered, gapless
// delivery, a hard byte cap, and the declared total length, and reports the durable
// "acked" offset the device echoes back to the cloud (which drives real progress +
// backpressure). A sink callback consumes each accepted slice - on the device it
// writes into the local web server over the loopback socket; in the host test it
// collects the bytes so reassembly can be asserted byte-for-byte. No Arduino, no
// sockets: this is lib/core, unit-tested via test/test_upload_reassembly.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace nimbus {
namespace cloud {

class UploadReassembly {
 public:
  enum class Status { Idle, Active, Done, Failed };
  // Why a chunk was (not) accepted. Anything but Accepted leaves the acked offset
  // unchanged; the failure outcomes also move Status to Failed so the caller aborts.
  enum class ChunkOutcome {
    Accepted,
    NotActive,   // no upload in progress
    WrongId,     // a chunk for a different correlation id
    OutOfOrder,  // seq or offset did not match the next expected byte
    OverCap,     // would exceed the hard byte ceiling
    OverTotal,   // would exceed the declared total length
    SinkFailed,  // the sink refused the bytes (e.g. SD write failed)
  };

  // Begin a fresh upload. `cap` is the hard byte ceiling (e.g. the device store cap);
  // `totalLen` is the declared body length (0 means "unknown", only the cap bounds it).
  void begin(const std::string& id, uint32_t totalLen, uint32_t cap);

  // Feed one ordered chunk. `sink(data,len)` must durably accept the bytes and return
  // true; on success the acked offset advances by `len`. Returns the outcome.
  ChunkOutcome chunk(const std::string& id, uint32_t seq, uint32_t off,
                     const uint8_t* data, size_t len,
                     const std::function<bool(const uint8_t*, size_t)>& sink);

  // Finalize. Complete only if the received length matches a declared totalLen (or any
  // length when totalLen was 0). A mismatch fails the upload.
  bool end(const std::string& id);

  // Abandon the in-flight upload (relay uabort, a drop, or a sink failure).
  void abort();

  Status status() const { return status_; }
  bool active() const { return status_ == Status::Active; }
  // The durable acked offset the device reports in `uack` (bytes accepted so far).
  uint32_t acked() const { return received_; }
  const std::string& id() const { return id_; }
  uint32_t totalLen() const { return totalLen_; }

 private:
  Status status_ = Status::Idle;
  std::string id_;
  uint32_t totalLen_ = 0;
  uint32_t cap_ = 0;
  uint32_t received_ = 0;
  uint32_t expectedSeq_ = 0;
};

}  // namespace cloud
}  // namespace nimbus
