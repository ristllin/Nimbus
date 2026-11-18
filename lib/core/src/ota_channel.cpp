// RB-OTA2: the owner's switch for the update channel. See nimbus/ota/ota_channel.h.
#include "nimbus/ota/ota_channel.h"

#include <cctype>
#include <cstring>

namespace nimbus {
namespace ota {

namespace {

constexpr char kStableLine[] = "Update channel: stable. The device gets regular releases only.";
constexpr char kCandidateLine[] =
    "Update channel: candidate. The device also gets test releases before everyone else.";
constexpr char kCandidateFailedLine[] =
    "Couldn't switch to the candidate channel. The device stays on stable and gets regular releases only.";
constexpr char kStableUnsavedLine[] =
    "Update channel: stable until the device restarts. The setting couldn't be saved, so set it again then.";
// Never reached through applyChannel (it fails closed), kept so the line is true for
// any state a caller reports.
constexpr char kCandidateStuckLine[] =
    "Couldn't change the update channel. The device is still on candidate, so try again.";
constexpr char kUsageLine[] = "Send /ota candidate or /ota stable. /ota on its own shows the channel.";

bool equalsWord(const char* s, size_t n, const char* word) {
  const size_t wl = std::strlen(word);
  if (n != wl) return false;
  for (size_t i = 0; i < n; i++)
    if (std::tolower((unsigned char)s[i]) != word[i]) return false;
  return true;
}

// Stable is the missing file. When it cannot be removed, any text but "candidate"
// is stable too (parseChannel).
bool clearFile(const ChannelFile& f) {
  static constexpr char kStable[] = "stable\n";
  if (f.remove() && f.read() == Channel::Stable) return true;
  return f.write(kStable, sizeof kStable - 1) && f.read() == Channel::Stable;
}

std::string json(bool withOk, bool ok, Channel c, const char* line) {
  std::string out = "{";
  if (withOk) out += ok ? "\"ok\":true," : "\"ok\":false,";
  out += "\"channel\":\"";
  out += channelSlug(c);
  out += "\",\"line\":\"";
  out += line;
  out += "\"}";
  return out;
}

}  // namespace

ChannelAsk parseChannelAsk(const char* text, size_t len) {
  if (!text) return ChannelAsk::Show;
  size_t b = 0, e = len;
  while (b < e && std::isspace((unsigned char)text[b])) b++;
  while (e > b && std::isspace((unsigned char)text[e - 1])) e--;
  if (b == e) return ChannelAsk::Show;
  if (equalsWord(text + b, e - b, "candidate")) return ChannelAsk::Candidate;
  if (equalsWord(text + b, e - b, "stable")) return ChannelAsk::Stable;
  return ChannelAsk::Unknown;
}

bool applyChannel(const ChannelFile& f, Channel want, bool& stableLatch) {
  bool ok;
  if (want == Channel::Candidate) {
    static constexpr char kCandidate[] = "candidate\n";
    ok = f.write(kCandidate, sizeof kCandidate - 1) && f.read() == Channel::Candidate;
    // Fail closed: whatever landed, the file goes back to stable; if even that fails
    // the latch keeps the stable channel for this boot.
    if (!ok) {
      stableLatch = !clearFile(f);
      return false;
    }
  } else {
    ok = clearFile(f);
  }
  stableLatch = !ok;
  return ok;
}

Channel channelInForce(const ChannelFile& f, bool stableLatch) {
  return stableLatch ? Channel::Stable : f.read();
}

const char* channelLine(ChannelAsk asked, bool ok, Channel inForce) {
  if (asked == ChannelAsk::Unknown) return kUsageLine;
  if (!ok && inForce == Channel::Candidate) return kCandidateStuckLine;
  if (!ok) return asked == ChannelAsk::Candidate ? kCandidateFailedLine : kStableUnsavedLine;
  return inForce == Channel::Candidate ? kCandidateLine : kStableLine;
}

std::string channelCommandReply(const std::string& args, const ChannelOps& ops) {
  const ChannelAsk a = parseChannelAsk(args.data(), args.size());
  if (a == ChannelAsk::Show || a == ChannelAsk::Unknown)
    return channelLine(a, a == ChannelAsk::Show, ops.get());
  const bool ok = ops.set(a == ChannelAsk::Candidate ? Channel::Candidate : Channel::Stable);
  return channelLine(a, ok, ops.get());
}

RouteReply channelRoute(bool authed, bool post, const char* value, const ChannelOps& ops) {
  if (!authed) return {401, kAuthRequiredJson};
  if (!post) {
    const Channel c = ops.get();
    return {200, json(false, true, c, channelLine(ChannelAsk::Show, true, c))};
  }
  const ChannelAsk a = value ? parseChannelAsk(value, std::strlen(value)) : ChannelAsk::Unknown;
  if (a != ChannelAsk::Stable && a != ChannelAsk::Candidate) {
    const Channel c = ops.get();
    return {400, json(true, false, c, "Send channel=candidate or channel=stable.")};
  }
  const bool ok = ops.set(a == ChannelAsk::Candidate ? Channel::Candidate : Channel::Stable);
  const Channel c = ops.get();
  return {ok ? 200 : 500, json(true, ok, c, channelLine(a, ok, c))};
}

}  // namespace ota
}  // namespace nimbus
