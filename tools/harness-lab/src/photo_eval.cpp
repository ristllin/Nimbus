#include "photo_eval.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "nimbus/harness/vision_caption.h"
#include "nimbus/orch/blob_store.h"
#include "nimbus/orch/media_label.h"
#include "nimbus/orch/msg_batch.h"
#include "nimbus/orch/turn_images.h"

namespace lab {
namespace {

namespace orch = nimbus::orch;

struct ManifestPhoto {
  std::string file, kind, prompt;
  std::vector<uint8_t> bytes;
};

bool loadManifest(const std::string& dir, const std::string& promptOverride,
                  std::vector<ManifestPhoto>& out, std::string& why) {
  std::ifstream t(dir + "/photos.json");
  const std::string json((std::istreambuf_iterator<char>(t)), std::istreambuf_iterator<char>());
  JsonDocument d;
  if (json.empty() || deserializeJson(d, json)) {
    why = "no readable photos.json in " + dir;
    return false;
  }
  for (JsonObjectConst p : d["photos"].as<JsonArrayConst>()) {
    ManifestPhoto ph;
    ph.file = p["file"] | "";
    ph.kind = p["kind"] | "scene";
    ph.prompt = promptOverride.empty() ? std::string(p["prompt"] | "") : promptOverride;
    std::ifstream f(dir + "/" + ph.file, std::ios::binary);
    ph.bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (ph.bytes.empty()) {
      why = "cannot read " + ph.file;
      return false;
    }
    out.push_back(std::move(ph));
  }
  if (out.empty()) {
    why = "the manifest lists no photos";
    return false;
  }
  return true;
}

// One photo the way the device renders a single-photo owner turn: the media
// segment in the batched turn text, the bytes offered to the head as an image.
std::string singlePhotoTurn(const std::string& chat, const ManifestPhoto& ph, orch::TurnImages& set) {
  orch::MessageBatcher b;
  b.addMedia(chat, "Owner", ph.prompt, 0, 1);
  set.nonce = "lab0nonce";
  set.uploadOnce = true;
  orch::MediaLabel l;
  l.k = 1;
  l.n = 1;
  l.ref = orch::blobHash(std::string(ph.bytes.begin(), ph.bytes.end()));
  l.cls = orch::MediaClass::Attached;
  b.setMediaText(0, 0, orch::mediaSegment(l));
  set.items.push_back(orch::attachedImage(l, ph.bytes.data(), ph.bytes.size(), "image/jpeg"));
  return b.render(0).c_str();
}

std::string jsonEscape(const std::string& s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((unsigned char)c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else
          out += c;
    }
  }
  return out;
}

}  // namespace

int cmdPhotoEval(const Env& env, const LabRig::Options& opt, const PhotoEvalOptions& pe) {
  if (pe.dir.empty() || pe.out.empty()) {
    std::fprintf(stderr, "photo-eval needs --dir=<manifest dir> and --out=<results dir>\n");
    return 2;
  }
  std::vector<ManifestPhoto> photos;
  std::string why;
  if (!loadManifest(pe.dir, pe.prompt, photos, why)) {
    std::fprintf(stderr, "%s\n", why.c_str());
    return 2;
  }
  std::error_code ec;
  std::filesystem::create_directories(pe.out, ec);
  if (ec) {
    std::fprintf(stderr, "cannot create %s\n", pe.out.c_str());
    return 2;
  }
  const std::string resultsPath = pe.out + "/results.jsonl";
  std::ofstream results(resultsPath, std::ios::app);
  if (!results) {
    std::fprintf(stderr, "cannot write %s\n", resultsPath.c_str());
    return 2;
  }
  LabRig rig(env, opt);
  std::printf("photo-eval: %zu photos, priority=%s, out=%s\n", photos.size(), opt.priority.c_str(),
              pe.out.c_str());
  int n = 0;
  for (const ManifestPhoto& ph : photos) {
    // A fresh chat per photo: one turn, no history, exactly the device's first
    // look at a photo. A shared chat would leak one photo's answer into the next.
    const std::string chat = "pe-" + std::to_string(n + 1);
    auto set = std::unique_ptr<orch::TurnImages>(new orch::TurnImages());
    const std::string turn = singlePhotoTurn(chat, ph, *set);
    // The fallback for a head without image input, exactly as the device wires it.
    set->transcribe = [&rig](const orch::TurnImage& it) {
      agent::vision::CaptionDeps d;
      d.http = &rig.http();
      d.key = [&rig](const std::string& p) { return rig.env().providerKey(p); };
      d.priority = [] { return std::string("anthropic,openai,mistral"); };
      agent::vision::CaptionRequest in;
      in.bytes = it.bytes;
      in.len = it.len;
      in.mime = it.mime;
      const agent::vision::CaptionResult res = agent::vision::caption(d, in);
      orch::MediaLabel l;
      l.cls = res.text.empty() ? orch::MediaClass::NotRead : orch::MediaClass::Transcribed;
      l.detail = res.text.empty() ? agent::vision::notReadWords(res) : res.text;
      l.by = res.provider;
      return it.tag + orch::mediaSuffix(l);
    };
    orch::offerTurnImages(chat, turn, std::move(set));
    const TurnRecord t = rig.say(chat, turn);
    std::string line = "{\"file\":\"" + jsonEscape(ph.file) + "\",\"kind\":\"" + jsonEscape(ph.kind) +
                       "\",\"prompt\":\"" + jsonEscape(ph.prompt) + "\",\"host\":\"" + jsonEscape(t.host) +
                       "\",\"model\":\"" + jsonEscape(rig.modelFor(t.host)) + "\",\"tokensIn\":" +
                       std::to_string(t.tokensIn) + ",\"tokensOut\":" + std::to_string(t.tokensOut) +
                       ",\"cacheRead\":" + std::to_string(t.cacheRead) + ",\"seconds\":" +
                       std::to_string(t.seconds) + ",\"ok\":" + (t.ok ? "true" : "false") +
                       ",\"toolCalls\":" + std::to_string(t.toolCalls.size()) + ",\"reply\":\"" +
                       jsonEscape(t.reply) + "\"}";
    results << line << "\n";
    results.flush();
    std::printf("  [%d] %s (%s): %s, %.1fs, %u in / %u out\n", ++n, ph.file.c_str(), ph.kind.c_str(),
                t.ok ? "ok" : "FAILED", t.seconds, t.tokensIn, t.tokensOut);
    if (n == 1 && !t.systemPrompt.empty()) {
      std::ofstream sp(pe.out + "/system_prompt.txt");
      sp << t.systemPrompt;
    }
  }
  std::printf("done: %s\n", resultsPath.c_str());
  return 0;
}

}  // namespace lab
