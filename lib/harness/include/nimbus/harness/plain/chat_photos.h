#pragma once
#include <string>
#include <vector>

#include "nimbus/harness/plain/wire.h"
#include "nimbus/orch/turn_images.h"
#include "nimbus/orch/tx/item.h"   // Part, kPhotoNoteMax

// plain/chat_photos.h - a message's photos into an engine-2 turn (lane E2-CHAT). The channel keeps
// each photo's bytes for the turn in the old engine's photo set (nimbus::orch::TurnImages, the
// Telegram intake's agent::vision set); engine 2 gets them as the message's Image parts
// ("p" + the photo's 16-hex ref) and reads the bytes back through its photo hook while the set is
// held. The engine shows a part as pixels in the turn it arrived and while the caps hold it after.
// Lane E2-P-PHOTO: a head that does not take images gets the parts too, each with one description
// as its note (the set's transcribe hook: engine 1's caption walk, vision_caption.cpp, over the
// owner's vision provider), made only then: a seeing head never causes a caption request. The turn
// is marked blind (ChatMessage::photosBlind) and every photo renders as "[photo p<ref> <when>:
// <description>]"; the bytes stay stored, so a later seeing head gets the pixels. Header only,
// host-tested in test_plain_chat_binding.
namespace agent {
namespace plain {

inline std::string photoPartRef(const nimbus::orch::TurnImage& img) { return "p" + img.ref; }

// Photo i's description for a blind head: its fallback segment (made once, on first use) without
// the bracket, on a character at most tx::kPhotoNoteMax: "(transcribed by <provider>) <text>", or
// "(not read: <why>)" when no provider could read it.
inline std::string photoCaption(nimbus::orch::TurnImages& set, size_t i) {
  const std::string& fb = set.fallbackOf(i);
  const std::string& tag = set.items[i].tag;
  size_t from = !tag.empty() && fb.compare(0, tag.size(), tag) == 0 ? tag.size() : 0;
  while (from < fb.size() && fb[from] == ' ') ++from;
  std::string out = fb.substr(from);
  if (out.size() > nimbus::orch::tx::kPhotoNoteMax) {
    size_t n = nimbus::orch::tx::kPhotoNoteMax;
    while (n > 0 && ((unsigned char)out[n] & 0xC0) == 0x80) --n;
    out.resize(n);
  }
  return out;
}

// The Image parts of `set` for a turn on (host, model). A head that takes no images: each part
// carries photoCaption as its note.
inline std::vector<nimbus::orch::tx::Part> photoParts(nimbus::orch::TurnImages* set, const std::string& host,
                                                     const std::string& model) {
  std::vector<nimbus::orch::tx::Part> out;
  if (!set) return out;
  const bool blind = !nimbus::orch::headTakesImages(host, model);
  for (size_t i = 0; i < set->items.size(); ++i) {
    const nimbus::orch::TurnImage& img = set->items[i];
    if (!img.bytes || !img.len || img.ref.empty()) continue;
    nimbus::orch::tx::Part p;
    p.kind = nimbus::orch::tx::PartKind::Image;
    p.ref = photoPartRef(img);
    p.mime = img.mime;
    p.bytes = (uint32_t)img.len;
    if (blind) p.note = photoCaption(*set, i);
    out.push_back(p);
  }
  return out;
}

// The message text for a head that takes no images: each attached photo's segment ("... (attached:
// the photo itself is in this message)", not true for it) becomes its bracket alone; the photo's
// own line carries the description. A seeing head, or no set: the text unchanged.
inline std::string photoText(const nimbus::orch::TurnImages* set, const std::string& host, const std::string& model,
                             const std::string& text) {
  if (!set || nimbus::orch::headTakesImages(host, model)) return text;
  std::string out = text;
  for (const nimbus::orch::TurnImage& img : set->items) {
    const size_t at = img.segment.empty() || img.tag.empty() ? std::string::npos : out.find(img.segment);
    if (at != std::string::npos) out.replace(at, img.segment.size(), img.tag);
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
