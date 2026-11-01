#pragma once
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "nimbus/harness/turn_inbox.h"
#include "nimbus/harness/vision_caption.h"
#include "nimbus/orch/blob_store.h"
#include "nimbus/orch/media_label.h"
#include "nimbus/orch/msg_batch.h"
#include "nimbus/orch/row_overlay.h"
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/turn_images.h"
#include "posix_fs.h"

// hosted_photos - photos on a hosted instance: the same turn the device builds (owner
// ruling R1: a photo goes to the head model as an image, a transcription is the
// fallback). The channel side the device keeps in telegram.cpp and
// adapters/image_vision.cpp, over the shared pieces: TurnImages, the media label, the
// message batcher's render, the turn inbox, the row overlay and the caption walk.
//
// A photo's bytes stay on the instance volume by their content hash, so image.look can
// show it again. Its row is written with a pending label and settled when the turn is
// over (settledRow): what the conversation keeps of it is the head's own note, or a
// blind head's reading. Single-context: engine thread only.
namespace nimbusd {

// One photo for a turn: its bytes, or the Files entry they are read from at turn time.
struct PhotoIn {
  std::string project, name;
  std::string bytes;
  std::string why;   // set when the channel could not fetch it: what the head is told
};

class HostedPhotos {
 public:
  struct Hooks {
    // Store one photo row of `chat` (kind Image, its blob path) and return its id.
    std::function<void(const std::string& chat, const std::string& text, const std::string& blobPath)> captureRow;
    // The stored copy of a Files entry; false when it is not there.
    std::function<bool(const std::string& project, const std::string& name, std::string& out)> fetchFile;
    std::function<std::string(const std::string& provider)> key;
    std::function<std::string()> priority;
    std::function<std::string()> routerHost;
    std::function<std::string()> routerSelection;
    agent::HttpTransport* http = nullptr;
    std::function<void(const std::string& line)> log;
  };

  HostedPhotos(std::string dir, std::string overlayPath, nimbus::orch::RowTextOverlay* overlay, Hooks h)
      : dir_(std::move(dir)), overlayPath_(std::move(overlayPath)), overlay_(overlay), h_(std::move(h)) {
    fsutil::mkdirs(dir_);
    std::string lines;
    if (fsutil::readFile(overlayPath_, lines)) overlay_->load(lines);
  }

  // Build the turn's images and return the text the turn runs with. The photos are
  // offered to the engine through the shared slot and taken when the turn starts.
  std::string offerTurn(const std::string& chat, const std::string& from, const std::string& caption,
                        const std::vector<PhotoIn>& photos) {
    auto set = std::unique_ptr<nimbus::orch::TurnImages>(new nimbus::orch::TurnImages());
    adopt(*set);
    nimbus::orch::MessageBatcher b;
    b.addMedia(chat, from, caption, 0, (int)photos.size());
    std::vector<std::string> refs;
    for (size_t i = 0; i < photos.size(); i++) {
      std::string ref;
      nimbus::orch::MediaLabel label;
      label.k = (int)i + 1;
      label.n = (int)photos.size();
      set->chatId = chat;
      const std::string seg = takePhoto(*set, caption, photos[i], label, ref);
      b.setMediaText(0, i, seg);
      refs.push_back(ref);
    }
    const std::string routed(b.render(0).c_str());
    agent::history::Inbound note = agent::history::inboundForBucket(b, 0, refs);
    note.src.clear();   // no channel update behind it: two equal uploads are two messages
    agent::history::noteInbound(note, routed);
    nimbus::orch::offerTurnImages(chat, routed, std::move(set));
    return routed;
  }

  void registerTools(nimbus::orch::ToolRegistry& reg) {
    using nimbus::orch::Principal;
    using nimbus::orch::ToolResult;
    reg.add("image.look",
            "View a photo sent earlier yourself: it comes back to you as an image. ref = the 16 hex characters in "
            "its [image ... ref=...] label in the conversation; question = what you want to check in it (used only "
            "when you cannot see images). Use it whenever the photo's note or your earlier reply does not answer "
            "what you are asked.",
            [this](ArduinoJson::JsonObjectConst a, const Principal& who) -> ToolResult {
              std::string err;
              const std::string answer = look(a["ref"] | "", a["question"] | "", who.owner, err);
              return answer.empty() ? ToolResult::fail(err) : ToolResult::ok(answer);
            },
            R"({"type":"object","properties":{"ref":{"type":"string"},"question":{"type":"string"}},"required":["ref","question"]})");
    reg.add("image.note",
            "Write the record of a photo of this message that you saw: one short line of what matters in it (the "
            "dish and portion, the numbers on a label, what the document says). It is what the conversation keeps "
            "of the photo; call it with your answer for every photo you see. ref = the 16 hex characters in its "
            "[image ... ref=...] label.",
            [](ArduinoJson::JsonObjectConst a, const Principal&) -> ToolResult {
              const std::string ref = a["ref"] | "";
              nimbus::orch::TurnImages* set = nimbus::orch::activeTurnImages();
              if (!set || !set->holds(ref))
                return ToolResult::fail("image.note is only for a photo of the current message; an earlier photo keeps its note");
              return set->setNote(ref, a["note"] | "") ? ToolResult::ok("noted") : ToolResult::fail("the note is empty");
            },
            R"({"type":"object","properties":{"ref":{"type":"string"},"note":{"type":"string"}},"required":["ref","note"]})");
  }

 private:
  static uint8_t* heapCopy(const std::string& b) {
    uint8_t* buf = (uint8_t*)std::malloc(b.size() ? b.size() : 1);
    if (buf) std::memcpy(buf, b.data(), b.size());
    return buf;
  }
  static std::string newNonce() {
    static std::mt19937_64 rng{std::random_device{}()};
    char n[17];
    snprintf(n, sizeof n, "%016llx", (unsigned long long)rng());
    return n;
  }
  void say(const std::string& line) const {
    if (h_.log) h_.log(line);
  }
  // Keep one photo and write its row; returns its segment in the turn's text.
  std::string takePhoto(nimbus::orch::TurnImages& set, const std::string& caption, const PhotoIn& p,
                        nimbus::orch::MediaLabel label, std::string& ref) {
    std::string bytes = p.bytes;
    if (bytes.empty() && (!p.why.empty() || !h_.fetchFile || !h_.fetchFile(p.project, p.name, bytes))) {
      label.cls = nimbus::orch::MediaClass::FetchFailed;
      label.detail = p.why.empty() ? "not found in Files" : p.why;
      return nimbus::orch::mediaSegment(label);
    }
    ref = nimbus::orch::blobHash(bytes);
    label.ref = ref;
    const std::string path = nimbus::orch::blobPath(dir_, ref, "bin");
    const bool kept = fsutil::writeFileAtomic(path, bytes);
    prune();
    const char* why = "";
    const char* real = nimbus::orch::admitImage(set, (const uint8_t*)bytes.data(), bytes.size(), why);
    if (*real) {
      uint8_t* buf = heapCopy(bytes);
      set.items.push_back(nimbus::orch::attachedImage(label, buf, bytes.size(), real));
      set.items.back().context = caption;
      label.cls = nimbus::orch::MediaClass::Attached;
    } else {
      label.cls = nimbus::orch::MediaClass::NotRead;
      label.detail = why;
      say(std::string("photo: ") + ref + " not attached (" + why + ")");
    }
    const std::string seg = nimbus::orch::mediaSegment(label);
    if (kept && h_.captureRow) {
      nimbus::orch::MediaLabel stored = label;
      if (*real) stored.cls = nimbus::orch::MediaClass::Pending;
      h_.captureRow(set.chatId, nimbus::orch::mediaSegment(stored), path);
    }
    return seg;
  }
  // The stored copies are bounded: past kKeep photos the oldest goes (image.look then
  // says it has no stored copy; its row and note stay).
  static constexpr size_t kKeep = 512;
  void prune() {
    DIR* d = opendir(dir_.c_str());
    if (!d) return;
    std::vector<std::pair<time_t, std::string>> files;
    while (struct dirent* e = readdir(d)) {
      if (e->d_name[0] == '.') continue;
      const std::string path = dir_ + "/" + e->d_name;
      struct stat st{};
      if (::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) files.push_back({st.st_mtime, path});
    }
    closedir(d);
    if (files.size() <= kKeep) return;
    std::sort(files.begin(), files.end());
    for (size_t i = 0; i < files.size() - kKeep; i++) ::unlink(files[i].second.c_str());
  }
  agent::vision::CaptionDeps captionDeps() const {
    agent::vision::CaptionDeps d;
    d.http = h_.http;
    d.key = h_.key;
    d.priority = h_.priority;
    d.routerHost = h_.routerHost;
    d.routerSelection = h_.routerSelection;
    return d;
  }
  agent::vision::CaptionResult read(const nimbus::orch::TurnImage& it, agent::vision::CaptionKind kind,
                                    const std::string& question) const {
    agent::vision::CaptionRequest in;
    in.bytes = it.bytes;
    in.len = it.len;
    in.mime = it.mime;
    in.context = it.context;
    in.question = question;
    in.kind = kind;
    const agent::vision::CaptionResult r = agent::vision::caption(captionDeps(), in);
    for (const auto& s : r.spends) nimbus::orch::addVisionSpend(s.provider.c_str(), s.model.c_str(), s.usage);
    return r;
  }
  // The set's hooks: a blind head's reading, and the end of the turn.
  void adopt(nimbus::orch::TurnImages& set) {
    if (set.nonce.empty()) set.nonce = newNonce();
    if (!set.transcribe)
      set.transcribe = [this](nimbus::orch::TurnImage& it) {
        const agent::vision::CaptionResult r = read(it, agent::vision::CaptionKind::Fallback, std::string());
        nimbus::orch::MediaLabel l;
        l.cls = r.text.empty() ? nimbus::orch::MediaClass::NotRead : nimbus::orch::MediaClass::Transcribed;
        l.detail = r.text.empty() ? agent::vision::notReadWords(r) : r.text;
        l.by = r.provider;
        return it.tag + nimbus::orch::mediaSuffix(l);
      };
    if (!set.answer)
      set.answer = [this](const nimbus::orch::TurnImage& it, const std::string& q) {
        const agent::vision::CaptionResult r = read(it, agent::vision::CaptionKind::Question, q);
        return r.text.empty() ? agent::vision::notReadWords(r) : r.text;
      };
    if (!set.release) set.release = [this](nimbus::orch::TurnImages& s) { endOfTurn(s); };
  }
  // The turn is over: each photo's row says what the head had of it, then the bytes go.
  void endOfTurn(nimbus::orch::TurnImages& set) {
    for (size_t i = 0; i < set.items.size(); ++i) {
      const std::string row = nimbus::orch::settledRow(set, i);
      if (!row.empty() && !set.chatId.empty()) settleRow(set.items[i].ref, row);
    }
    for (auto& it : set.items) std::free(const_cast<uint8_t*>(it.bytes));
    for (auto& l : set.looked) std::free(const_cast<uint8_t*>(l.img.bytes));
    set.items.clear();
    set.looked.clear();
  }
  void settleRow(const std::string& ref, const std::string& text) {
    nimbus::orch::MsgQuery q;
    q.haveKind = true;
    q.kind = nimbus::orch::MsgKind::Image;
    q.limit = 48;
    for (const auto& m : overlay_->query(q)) {
      if (nimbus::orch::blobHashOf(m.blobPath) != ref) continue;
      overlay_->put(m.id, text);
      fsutil::writeFileAtomic(overlayPath_, overlay_->dump());
      return;
    }
  }
  std::string look(const std::string& ref, const std::string& question, bool owner, std::string& err) {
    if (!agent::vision::lookArgsValid(ref, question, err)) return std::string();
    nimbus::orch::TurnImages* set = nimbus::orch::activeTurnImages();
    if (set && set->holds(ref))
      return "This photo came with the current message and is already in front of you as an image: "
             "answer from what you see in it.";
    if (set && set->lookedIndex(ref) >= 0) return nimbus::orch::lookResultText(ref);
    if (!owner) {
      err = "no photo with that ref in this chat";   // photos belong to the owner, like Files here
      return std::string();
    }
    std::string bytes;
    if (!fsutil::readFile(nimbus::orch::blobPath(dir_, ref, "bin"), bytes)) {
      err = "no stored photo with that ref";
      return std::string();
    }
    const char* why = "";
    const char* real = set ? nimbus::orch::admitLook(*set, (const uint8_t*)bytes.data(), bytes.size(), why) : "";
    if (*real) {
      adopt(*set);
      nimbus::orch::LookedImage l;
      l.img.ref = ref;
      l.img.mime = real;
      l.img.bytes = heapCopy(bytes);
      l.img.len = bytes.size();
      l.img.tag = "[image ref=" + ref + "]";
      l.question = question;
      set->looked.push_back(l);
      return nimbus::orch::lookResultText(ref);
    }
    nimbus::orch::TurnImage it;
    it.bytes = (const uint8_t*)bytes.data();
    it.len = bytes.size();
    it.mime = nimbus::orch::sniffImageMime(it.bytes, it.len);
    if (it.mime.empty()) {
      err = "that ref is not a photo";
      return std::string();
    }
    const agent::vision::CaptionResult r = read(it, agent::vision::CaptionKind::Question, question);
    if (r.text.empty()) err = "the photo could not be read: " + r.words;
    return r.text;
  }

  std::string dir_, overlayPath_;
  nimbus::orch::RowTextOverlay* overlay_;
  Hooks h_;
};

}  // namespace nimbusd
