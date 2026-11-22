#pragma once
#include <string>
#include <vector>

#include "nimbus/harness/plain/wire.h"
#include "nimbus/orch/turn_images.h"
#include "nimbus/orch/tx/item.h"

// plain/chat_photos.h - a message's photos into an engine-2 turn (lane E2-CHAT). The channel keeps
// each photo's bytes for the turn in the old engine's photo set (nimbus::orch::TurnImages, the
// Telegram intake's agent::vision set); engine 2 gets them as the message's Image parts
// ("p" + the photo's 16-hex ref) and reads the bytes back through its photo hook while the set is
// held. A head that does not take images gets no part (its turn has the message text only); the
// engine shows a part as pixels in the turn it arrived and as its stub line after that. No caption
// request is made on this path. Header only, host-tested in test_plain_chat_binding.
namespace agent {
namespace plain {

inline std::string photoPartRef(const nimbus::orch::TurnImage& img) { return "p" + img.ref; }

// The Image parts of `set` for a turn on (host, model); none when the head takes no images.
inline std::vector<nimbus::orch::tx::Part> photoParts(const nimbus::orch::TurnImages* set, const std::string& host,
                                                     const std::string& model) {
  std::vector<nimbus::orch::tx::Part> out;
  if (!set || !nimbus::orch::headTakesImages(host, model)) return out;
  for (const nimbus::orch::TurnImage& img : set->items) {
    if (!img.bytes || !img.len || img.ref.empty()) continue;
    nimbus::orch::tx::Part p;
    p.kind = nimbus::orch::tx::PartKind::Image;
    p.ref = photoPartRef(img);
    p.mime = img.mime;
    p.bytes = (uint32_t)img.len;
    out.push_back(p);
  }
  return out;
}

// The photo hook's answer for `ref` from `set` (the bytes stay the set's). NotFound when the set
// does not hold it (an earlier turn's photo: the engine renders its stub).
inline Status photoBytesFrom(const nimbus::orch::TurnImages* set, const std::string& ref, PhotoBytes& out) {
  out = PhotoBytes{};
  if (set)
    for (const nimbus::orch::TurnImage& img : set->items)
      if (img.bytes && img.len && photoPartRef(img) == ref) {
        out.bytes = img.bytes;
        out.len = img.len;
        out.mime = img.mime;
        return Status::success();
      }
  return Status::fail(nimbus::orch::tx::Err::NotFound, "plain::photoBytesFrom");
}

}  // namespace plain
}  // namespace agent
