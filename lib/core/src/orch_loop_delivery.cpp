// orch_loop_delivery - see nimbus/orch/loop_delivery.h. Pure, host-tested.
#include "nimbus/orch/loop_delivery.h"

#include <algorithm>

namespace nimbus {
namespace orch {

namespace {

const char kWebChat[] = "web";

std::string trimmed(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return std::string();
  const size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// Every non-empty, trimmed entry of a comma-separated list, in order.
std::vector<std::string> entries(const std::string& list) {
  std::vector<std::string> out;
  size_t s = 0;
  while (s <= list.size()) {
    size_t e = list.find(',', s);
    if (e == std::string::npos) e = list.size();
    std::string t = trimmed(list.substr(s, e - s));
    if (!t.empty()) out.push_back(std::move(t));
    s = e + 1;
  }
  return out;
}

bool contains(const std::vector<std::string>& v, const std::string& x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

// The model-facing note a fallen-back turn carries: say where the reply goes, and open
// with the user-facing sentence.
std::string fallbackInstruction(const Delivery& d) {
  return std::string("\n\n[DELIVERY]\nThis was set up to reach the owner on ") +
         deliveryChannelLabel(d.from) +
         ", which is not available on this device right now, so your reply is delivered to " +
         deliveryChannelLabel(d.to) +
         " instead. Open your reply with this sentence, word for word, then continue "
         "normally:\n" + deliveryFallbackSentence(d);
}

}  // namespace

DeliveryChannel deliveryChannelOf(const std::string& chatId) {
  if (chatId.empty()) return DeliveryChannel::OwnerDefault;
  if (chatId == kWebChat) return DeliveryChannel::Web;
  if (chatId == "voice") return DeliveryChannel::Voice;
  if (chatId == "serial") return DeliveryChannel::Serial;
  return DeliveryChannel::Telegram;
}

bool isDeviceChannel(DeliveryChannel c) {
  return c == DeliveryChannel::Web || c == DeliveryChannel::Voice || c == DeliveryChannel::Serial;
}

const char* deliveryChannelLabel(DeliveryChannel c) {
  switch (c) {
    case DeliveryChannel::OwnerDefault: return "your default channel";
    case DeliveryChannel::Web:          return "the web chat";
    case DeliveryChannel::Voice:        return "the device screen";
    case DeliveryChannel::Serial:       return "the serial console";
    case DeliveryChannel::Telegram:     return "Telegram";
    case DeliveryChannel::Count:        break;
  }
  return "an unknown channel";
}

bool DeliveryWorld::isOwnerChat(const std::string& chatId) const {
  return !chatId.empty() && contains(ownerChats, chatId);
}

DeliveryWorld deliveryWorld(const std::string& botToken, const std::string& allowlist,
                            const std::string& owners) {
  DeliveryWorld w;
  w.botToken = !trimmed(botToken).empty();
  const std::vector<std::string> allowed = entries(allowlist);
  const std::vector<std::string> listed = entries(owners);
  if (listed.empty()) {
    // telegram::isOwner: the text before the first comma, NOT the first non-empty entry.
    const std::string first = trimmed(allowlist.substr(0, allowlist.find(',')));
    if (!first.empty()) w.ownerChats.push_back(first);
  } else {
    for (const std::string& o : listed)
      if (contains(allowed, o)) w.ownerChats.push_back(o);
  }
  if (!w.ownerChats.empty()) w.ownerTelegram = w.ownerChats.front();
  return w;
}

Delivery resolveDelivery(const std::string& storedChatId, const DeliveryWorld& w,
                         bool chatAllowed) {
  Delivery d;
  d.from = deliveryChannelOf(storedChatId);
  if (isDeviceChannel(d.from)) {   // the device's own channels always exist
    d.to = d.from;
    d.target = storedChatId;
    return d;
  }
  if (d.from == DeliveryChannel::OwnerDefault) {
    const bool ownerTelegram = w.botToken && !w.ownerTelegram.empty();
    d.to = ownerTelegram ? DeliveryChannel::Telegram : DeliveryChannel::Web;
    d.target = ownerTelegram ? w.ownerTelegram : kWebChat;
    return d;
  }
  // A Telegram chat.
  d.to = DeliveryChannel::Telegram;
  if (w.botToken && chatAllowed) {
    d.target = storedChatId;
    return d;
  }
  if (!w.botToken && chatAllowed && w.isOwnerChat(storedChatId)) {
    d.verdict = DeliveryVerdict::FellBack;   // the owner turned the bot off, not a revoke
    d.to = DeliveryChannel::Web;
    d.target = kWebChat;
    return d;
  }
  d.verdict = DeliveryVerdict::Revoked;
  return d;
}

std::string originChat(const std::string& requested, const std::string& creatingTurnChat) {
  const std::string r = trimmed(requested);
  return r.empty() ? trimmed(creatingTurnChat) : r;
}

std::string deliveryFallbackSentence(const Delivery& d) {
  return std::string("Showing this in ") + deliveryChannelLabel(d.to) + " because " +
         deliveryChannelLabel(d.from) + " isn't available right now.";
}

LoopFireRequest buildFireRequest(const LoopRecord& l, const Delivery& d, uint64_t nowEpoch) {
  LoopFireRequest req;
  req.id     = l.id;
  req.name   = l.name;
  req.chatId = d.target;
  req.prompt = l.prompt;
  if (d.verdict == DeliveryVerdict::FellBack) req.prompt += fallbackInstruction(d);
  // A Once loop renders the [WAKEUP] framing; an owner-set one (/remind) is a
  // [REMINDER] - the owner scheduled it, not the model.
  req.once          = (l.sched.kind == SchedKind::Once);
  req.ownerReminder = (req.once && l.createdBy == CreatedBy::Owner);
  req.scheduledFor  = nowEpoch;
  return req;
}

bool liftDeviceChannelRevoke(LoopRecord& l) {
  if (l.enabled || l.pauseReason != PauseReason::ChatRevoked) return false;
  if (l.sched.kind == SchedKind::Once) return false;
  if (!isDeviceChannel(deliveryChannelOf(l.chatId))) return false;
  l.enabled = true;
  l.pauseReason = PauseReason::None;
  return true;
}

}  // namespace orch
}  // namespace nimbus
