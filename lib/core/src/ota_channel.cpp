// RB-OTA2: the owner's switch for the update channel. See nimbus/ota/ota_channel.h.
#include "nimbus/ota/ota_channel.h"

#include <cctype>
#include <cstdio>
#include <strings.h>

namespace nimbus {
namespace ota {

namespace {

constexpr char kStableLine[] = "Update channel: stable (regular releases only).";
constexpr char kCandidateLine[] = "Update channel: candidate (test releases first).";
// A failed switch: stable is in force (applyChannel fails closed); when even the file
// could not be cleared, the RAM latch holds it until a successful switch or a restart.
constexpr char kFailedLine[] = "Couldn't save the update channel: stable for now.";
// Never reached through applyChannel; keeps the line true for any state a caller reports.
constexpr char kFailedCandidateLine[] = "Couldn't save the update channel: candidate for now.";
constexpr char kUsageLine[] = "Send /ota candidate or /ota stable.";

// Stable is the missing file. When it cannot be removed, any text but "candidate"
// is stable too (parseChannel).
bool clearFile(const ChannelFile& f) {
  if (f.remove() && f.read() == Channel::Stable) return true;
  return f.write("stable\n", 7) && f.read() == Channel::Stable;
}

}  // namespace

ChannelAsk parseChannelAsk(const char* text, size_t len) {
  if (!text) return ChannelAsk::Show;
  while (len && std::isspace((unsigned char)*text)) text++, len--;
  while (len && std::isspace((unsigned char)text[len - 1])) len--;
  if (!len) return ChannelAsk::Show;
  if (len == 9 && !strncasecmp(text, "candidate", 9)) return ChannelAsk::Candidate;
  if (len == 6 && !strncasecmp(text, "stable", 6)) return ChannelAsk::Stable;
  return ChannelAsk::Unknown;
}

bool applyChannel(const ChannelFile& f, Channel want, bool& stableLatch) {
  bool ok = want == Channel::Candidate
                ? f.write("candidate\n", 10) && f.read() == Channel::Candidate
                : clearFile(f);
  // Fail closed: a failed switch to candidate puts the file back to stable; whenever the
  // file cannot be made stable, the latch holds the stable channel for this boot.
  if (!ok && want == Channel::Candidate) stableLatch = !clearFile(f);
  else stableLatch = !ok;
  return ok;
}

Channel channelInForce(const ChannelFile& f, bool stableLatch) {
  return stableLatch ? Channel::Stable : f.read();
}

const char* channelLine(ChannelAsk asked, bool ok, Channel inForce) {
  if (asked == ChannelAsk::Unknown) return kUsageLine;
  if (!ok) return inForce == Channel::Candidate ? kFailedCandidateLine : kFailedLine;
  return inForce == Channel::Candidate ? kCandidateLine : kStableLine;
}

// Set the asked channel (Stable or Candidate) and answer with the line.
static int setAndSay(ChannelAsk a, const ChannelOps& ops, const char** line) {
  const bool ok = ops.set(a == ChannelAsk::Candidate ? Channel::Candidate : Channel::Stable);
  *line = channelLine(a, ok, ops.get());
  return ok ? 200 : 500;
}

const char* channelCommandReply(const char* args, size_t len, const ChannelOps& ops) {
  const ChannelAsk a = parseChannelAsk(args, len);
  if (a == ChannelAsk::Show || a == ChannelAsk::Unknown) return channelLine(a, true, ops.get());
  const char* line;
  setAndSay(a, ops, &line);
  return line;
}

int channelRoute(bool authed, bool post, const char* value, size_t valueLen, const ChannelOps& ops,
                 char (&body)[kRouteBodyCap]) {
  body[0] = '\0';
  if (!authed) return 401;
  const ChannelAsk a = post && value ? parseChannelAsk(value, valueLen) : ChannelAsk::Show;
  int status = 200;
  const char* line;
  if (a == ChannelAsk::Stable || a == ChannelAsk::Candidate) {
    status = setAndSay(a, ops, &line);
  } else {
    if (post) status = 400;   // a missing, empty or unknown value changes nothing
    line = channelLine(ChannelAsk::Show, true, ops.get());
  }
  std::snprintf(body, sizeof body, "{\"ok\":%s,\"channel\":\"%s\",\"line\":\"%s\"}", status == 200 ? "true" : "false",
                channelSlug(ops.get()), line);
  return status;
}

}  // namespace ota
}  // namespace nimbus
