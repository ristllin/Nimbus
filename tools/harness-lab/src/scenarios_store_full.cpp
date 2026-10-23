#include "scenarios_store_full.h"

#include <cstdio>
#include <string>
#include <vector>

#include "nimbus/harness/turn_inbox.h"
#include "nimbus/orch/blob_store.h"
#include "nimbus/orch/media_label.h"
#include "nimbus/orch/msg_batch.h"
#include "nimbus/orch/store_full.h"

namespace lab {
namespace storefull {
namespace {

constexpr int kPhotos = 5;
// A reply that explains the store runs long: the storm's replies (the store state, the
// error, the photo's blob path, two options and a question) were 477 to 772 B. A reply
// that says "not saved" and at most offers a resend fits well under this.
constexpr size_t kShortReply = 240;

std::string lower(std::string s) {
  for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
  return s;
}
bool has(const std::string& hay, const char* needle) { return lower(hay).find(needle) != std::string::npos; }

// The reply's sentences (split at . ? ! and line ends), each with its closing mark.
std::vector<std::string> sentences(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : text) {
    cur += c;
    if (c == '.' || c == '?' || c == '!' || c == '\n') {
      if (cur.find_first_not_of(" \t\r\n.?!") != std::string::npos) out.push_back(cur);
      cur.clear();
    }
  }
  if (cur.find_first_not_of(" \t\r\n") != std::string::npos) out.push_back(cur);
  return out;
}

bool aboutTheStore(const std::string& s) {
  for (const char* w : {"full", "file store", "storage", "space", "room", "delete", "remove", "too many"})
    if (has(s, w)) return true;
  return false;
}

// Sentences that explain the store, and questions that ask the owner what to delete.
int storeSentences(const std::string& reply) {
  int n = 0;
  for (const auto& s : sentences(reply)) n += aboutTheStore(s);
  return n;
}
int deleteQuestions(const std::string& reply) {
  int n = 0;
  for (const auto& s : sentences(reply))
    n += s.back() == '?' && (has(s, "delete") || has(s, "remove") || has(s, "free") || has(s, "space") ||
                             has(s, "which file") || has(s, "what file"));
  return n;
}

bool triedSave(const TurnRecord& t) {
  for (const auto& c : t.toolCalls)
    for (const char* save : {"artifact", "files_append", "files.append", "files_pdf", "files.pdf"})
      if (c.rfind(save, 0) == 0) return true;
  return false;
}

int assistantRows(LabRig& rig, const std::string& chat) {
  nimbus::orch::MsgQuery q;
  q.sessionId = chat;
  q.limit = 200;
  int n = 0;
  for (const auto& m : rig.episodic().query(q)) n += m.role == "assistant";
  return n;
}

// Photo k's turn exactly as the device builds it: the batcher, the one media label (the
// bench photo could not be described, as on the storm bench) and the store fact; its
// media row and the turn's inbox note as the receive path writes them.
std::string photoTurn(LabRig& rig, const std::string& chat, int k) {
  nimbus::orch::MessageBatcher b;
  b.addMedia(chat, "Owner", "keep this photo", 0, 100 + k);
  nimbus::orch::MediaLabel l;
  l.k = 1;
  l.n = 1;
  l.ref = nimbus::orch::blobHash("store-full photo " + std::to_string(k));
  l.cls = nimbus::orch::MediaClass::NotDescribed;
  const std::string row = nimbus::orch::mediaSegment(l);
  b.setMediaText(0, 0, row + nimbus::orch::storeFullFact(true, true));
  LabRig::Row media;
  media.chat = chat;
  media.kind = nimbus::orch::MsgKind::Image;
  media.text = row;
  media.blob = "/mem/blobs/" + l.ref + ".jpg";
  rig.addRow(media);
  const std::string routed = b.render(0).c_str();
  agent::history::noteInbound(agent::history::inboundForBucket(b, 0, {l.ref}), routed);
  return routed;
}

}  // namespace

void storeFullPhotos(LabRig& rig, const Report& r) {
  const std::string chat = "storefull";
  rig.files().setFull(LabFiles::Full::Told);
  int replies = 0, oneDelivery = 0, saves = 0, asks = 0, longOnes = 0, repeats = 0, rawError = 0, splitTurns = 0;
  size_t bytes = 0;
  for (int k = 0; k < kPhotos; ++k) {
    const std::string turn = photoTurn(rig, chat, k);
    if (k == 0) r.check(turn.find("Not saved to the file store") != std::string::npos, "the turn carries the store fact");
    const int rowsBefore = assistantRows(rig, chat);
    const TurnRecord t = rig.say(chat, turn);
    const int rows = assistantRows(rig, chat) - rowsBefore;
    std::printf("    < [%d] %s\n", k + 1, t.reply.empty() ? "(no reply)" : t.reply.c_str());
    replies += !t.reply.empty() && t.tokensOut > 0;   // the head answered (not a device refusal)
    oneDelivery += t.deliveries.size() == 1;
    saves += triedSave(t);
    asks += deleteQuestions(t.reply);
    longOnes += t.reply.size() > kShortReply;
    repeats += storeSentences(t.reply) > 1;
    rawError += has(t.reply, "too many files") || has(t.reply, "remove some first");
    splitTurns += rows > 1;
    bytes += t.reply.size();
    r.note("turn " + std::to_string(k + 1) + ": " + std::to_string(t.reply.size()) + " B, " +
           std::to_string(storeSentences(t.reply)) + " store sentence(s), " + std::to_string(rows) +
           " history row(s), " + std::to_string(t.tokensIn) + " in / " + std::to_string(t.tokensOut) + " out (" +
           t.host + ")");
  }
  const std::string sum = std::to_string(replies) + " replies, " + std::to_string(saves) + " save attempt(s), " +
                          std::to_string(asks) + " question(s) about deleting, " + std::to_string(longOnes) +
                          " reply(ies) over " + std::to_string(kShortReply) + " B, " + std::to_string(repeats) +
                          " turn(s) with a second store sentence (a resend offer), " + std::to_string(rawError) +
                          " quoting the store error, " + std::to_string(splitTurns) +
                          " turn(s) captured as two history rows, mean " + std::to_string(bytes / kPhotos) + " B";
  r.note(sum);
  rig.files().setFull(LabFiles::Full::No);
  r.check(replies == kPhotos && oneDelivery == kPhotos, "every photo got exactly one reply from the head");
  r.check(saves == 0, "no turn tried to save into the full store");
  r.check(asks == 0, "no reply asked which files to delete");
  r.check(longOnes == 0, "no reply explained the store (every reply " + std::to_string(kShortReply) + " B or less)");
  r.check(rawError == 0, "no reply quoted the store's error");
}

}  // namespace storefull
}  // namespace lab
