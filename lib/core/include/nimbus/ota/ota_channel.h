#pragma once
// The owner's switch for the update channel (RB-OTA2). Portable, host-tested
// (test/test_ota_channel); the device glue is otaupd::setChannel/channel()
// (src/sys/ota_update.cpp), the /ota command (agent::orchestrator::handleMessage)
// and the route GET/POST /api/ota/channel (src/net/ota_channel_routes.cpp).
//
// Fail closed: a switch that fails at any step ends on the stable channel. A set to
// candidate that cannot be written and read back clears the file again; a set to
// stable that cannot clear the file (remove, then overwrite with "stable") latches
// stable in RAM until the next successful set, so no check in this boot reads the
// candidate manifest after a failed switch. A refused request (no sign-in code, a
// method other than GET or POST, an unknown value) changes nothing.

#include <cstddef>

#include "nimbus/ota/ota_logic.h"   // Channel, parseChannel, channelSlug

namespace nimbus {
namespace ota {

// What the owner asked for: "" shows the channel; "stable" or "candidate" (any case,
// surrounding spaces ignored) sets it; anything else is Unknown and changes nothing.
enum class ChannelAsk : uint8_t { Show, Stable, Candidate, Unknown };
ChannelAsk parseChannelAsk(const char* text, size_t len);

// The channel file, injected so the host test can fail every step. write() creates
// or truncates the file and writes all n bytes (true only when every byte landed);
// remove() deletes it (true when it is gone); read() is the channel the file says
// (missing, unreadable or any other text: Stable).
struct ChannelFile {
  bool (*write)(const char* text, size_t n);
  bool (*remove)();
  Channel (*read)();
};

// Set `want` on `f`. Returns true when `want` is saved and reads back. On false the
// channel in force is Stable (the file cleared, or `stableLatch` set when it could
// not be). A successful set clears the latch.
bool applyChannel(const ChannelFile& f, Channel want, bool& stableLatch);
// The channel the next update check uses: Stable while the latch holds, else the file.
Channel channelInForce(const ChannelFile& f, bool stableLatch);

// The one plain line for the owner: the channel in force and what it means, or why a
// switch did not happen. Never contains '"' or '\\' (it is put into JSON as is).
const char* channelLine(ChannelAsk asked, bool ok, Channel inForce);

// The device's set and get (otaupd::setChannel, otaupd::channel), injected.
struct ChannelOps {
  bool (*set)(Channel);
  Channel (*get)();
};

// The /ota command's answer for its arguments (the text after the verb).
const char* channelCommandReply(const char* args, size_t len, const ChannelOps& ops);

// The route GET/POST /api/ota/channel. `authed`: the request carried the device
// sign-in code (nimbus::net::webAuthOk). `post`: POST, else GET (the device binds no
// other method). `value`/`valueLen`: the POST body's `channel` field and its length
// (an embedded NUL is part of the value), nullptr when absent. Returns the HTTP status and writes the JSON answer into `body`:
//   401: neither set nor get runs, `body` is "" (the device sends the 401 every owner
//        route sends);
//   GET 200: {"ok":true,"channel":"...","line":"..."}, the channel in force;
//   POST: 400 for a missing or unknown value (nothing changes; "line" is the channel in force),
//         200 when set, 500 when the switch failed (the channel in force is then stable).
// `cap` of kRouteBodyCap always fits.
inline constexpr size_t kRouteBodyCap = 192;
int channelRoute(bool authed, bool post, const char* value, size_t valueLen, const ChannelOps& ops, char* body,
                 size_t cap);

}  // namespace ota
}  // namespace nimbus
