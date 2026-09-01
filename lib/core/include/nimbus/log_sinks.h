#pragma once
#include <string>
#include <vector>

#include "nimbus/logring.h"

namespace core {

// Emit `msg` to two sinks after a SINGLE redaction pass. Both sinks are handed the identical
// redacted string, so there is no un-redacted copy for any print path to grab: whatever a
// serial sink prints is, by construction, the same masked line the ring sink stores.
//
// This is the portable heart of the device agent-log seam (src/sys/agent_log.h). alog()/
// alogf() feed their Serial writer and their ring writer through here, so the F4 serial-
// bypass (Serial printing the raw message while only the ring was redacted) cannot come
// back without rewriting alog() to stop calling this - the host test on emitRedacted guards
// that a sink never receives the raw message.
//
// The redaction (which allocates) happens here, before either sink runs, so a ring sink is
// free to copy the bytes under a no-heap spinlock.
template <class SerialSink, class RingSink>
inline void emitRedacted(const char* msg, const std::vector<std::string>& secrets,
                         SerialSink&& toSerial, RingSink&& toRing) {
  const std::string red = LogRing::redact(msg ? msg : "", secrets);
  toSerial(red);
  toRing(red);
}

}  // namespace core
