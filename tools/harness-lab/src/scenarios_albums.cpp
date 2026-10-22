#include "scenarios_albums.h"

#include <cstdio>
#include <string>

#include "nimbus/orch/blob_store.h"
#include "nimbus/orch/media_label.h"
#include "nimbus/orch/msg_batch.h"

namespace lab {
namespace albums {
namespace {

std::string lower(std::string s) {
  for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
  return s;
}
bool hasAny(const std::string& hay, std::initializer_list<const char*> needles) {
  const std::string h = lower(hay);
  for (const char* n : needles)
    if (h.find(lower(n)) != std::string::npos) return true;
  return false;
}

// What the device's current captioner (gpt-4o-mini, the device prompt, the album
// caption context) wrote for three synthetic photos on 22 Oct 2026: a protein bar's
// nutrition panel, a cafe receipt and a bus stop sign. Recorded once (OpenAI, about
// 0.01 USD) so this leg spends only on the head.
const char* const kCaptions[] = {
    "The image contains a list of nutrition facts for a protein bar, formatted in a simple and clear "
    "manner. It specifies that each bar contains 190 calories, 21 grams of protein, 9 grams of fat, and 3 "
    "grams of carbohydrates. The text is presented in a straightforward layout, making it easy to read.",
    "The image features a receipt from \"CAFE LUNE\" detailing a purchase. It lists two items: a \"Flat "
    "white\" priced at 3.40 and an \"Almond croissant\" priced at 2.80. The total amount due is 6.20, "
    "prominently displayed at the bottom of the receipt. The background is a soft, light color that "
    "contrasts with the dark text.",
    "The image features a dark blue background with bright yellow text. It displays information about Bus "
    "42, indicating it is heading to Riverside, with the next departure time set for 07:15. The clear "
    "layout highlights the bus number and destination prominently.",
};

// The turn text the device builds for this album: three media segments (the caption
// on the first photo) and the follow-up text, in arrival order, as one turn.
std::string albumTurn() {
  nimbus::orch::MessageBatcher b;
  b.addMedia("albums", "Owner", "Everything from my morning", 0, 1);
  b.addMedia("albums", "Owner", "", 1, 2);
  b.addMedia("albums", "Owner", "", 2, 3);
  b.add("albums", "Owner", "I sent 3 images. Did you get all of them? One line on each, please.", 4);
  for (size_t k = 0; k < 3; ++k) {
    nimbus::orch::MediaLabel l;
    l.k = (int)k + 1;
    l.n = 3;
    l.ref = nimbus::orch::blobHash("albums-one-turn photo " + std::to_string(k));
    l.detail = kCaptions[k];
    b.setMediaText(0, k, nimbus::orch::mediaSegment(l));
  }
  return b.render(0).c_str();
}

}  // namespace

void albumsOneTurn(LabRig& rig, const Report& r) {
  const std::string turn = albumTurn();
  r.check(turn.find("[image 3/3 ref=") != std::string::npos && turn.find("They wrote: Everything") != std::string::npos,
          "the turn carries every photo's label and the album caption");
  auto t = rig.say("albums", turn);
  std::printf("    < %s\n", t.reply.empty() ? "(no reply)" : t.reply.c_str());
  r.check(!t.reply.empty(), "one reply for the whole album");
  r.check(t.deliveries.size() == 1, "exactly one reply was delivered (got " + std::to_string(t.deliveries.size()) + ")");
  r.check(hasAny(t.reply, {"protein", "nutrition", "21"}), "the reply covers photo 1 (the nutrition panel)");
  r.check(hasAny(t.reply, {"flat white", "croissant", "cafe lune", "receipt"}), "the reply covers photo 2 (the receipt)");
  r.check(hasAny(t.reply, {"bus", "42", "riverside"}), "the reply covers photo 3 (the bus sign)");
  r.check(!hasAny(t.reply, {"didn't receive", "did not receive", "only see", "only got", "missing", "not receive"}),
          "the reply never claims a photo is missing");
  r.note("head " + t.host + ": " + std::to_string(t.tokensIn) + " in / " + std::to_string(t.tokensOut) + " out");
}

}  // namespace albums
}  // namespace lab
