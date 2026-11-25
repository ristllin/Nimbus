// lab_r10 - engine 2 against the real providers (lab_r10.h), lane E2-R10.
#include "lab_r10.h"

#include <ArduinoJson.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../../../nimbusd/src/plain_rig.h"
#include "../../../test/support/tx_battery/mem_files.h"
#include "../../../test/test_route_goldens/cases.h"
#include "../../../test/test_route_goldens/render_case.h"
#include "curl_transport.h"
#include "nimbus/harness/plain/request.h"
#include "nimbus/harness/plain/route.h"
#include "nimbus/harness/plain/texts.h"
#include "nimbus/orch/wire_errors.h"

namespace lab {
namespace {

namespace plain = agent::plain;
namespace rg = route_goldens;
namespace tx = nimbus::orch::tx;

// The key named by `var` in the dotenv file `path`, read here and never printed; the environment is
// not read, so a key exported in the shell cannot stand in for the one the run names.
std::string keyFrom(const std::string& path, const std::string& var) {
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("export ", 0) == 0) line = line.substr(7);
    const size_t eq = line.find('=');
    if (eq == std::string::npos || line.substr(0, eq) != var) continue;
    std::string v = line.substr(eq + 1);
    while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' ')) v.pop_back();
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
    return v;
  }
  return std::string();
}

bool readAll(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream s;
  s << f.rdbuf();
  out = s.str();
  return true;
}

void writeAll(const std::string& path, const std::string& bytes) {
  std::ofstream f(path, std::ios::binary);
  f << bytes;
}

struct Head {
  std::string provider, model, photoModel, key;
  std::string host;
  plain::Dialect dialect = plain::Dialect::AnthropicMessages;
  const char* sig = rg::kDirect;
};

bool headOf(const std::string& provider, Head& h) {
  h.provider = provider;
  if (provider == "anthropic") {
    h.host = "api.anthropic.com";
    h.dialect = plain::Dialect::AnthropicMessages;
    h.sig = rg::kDirect;
  } else if (provider == "openai") {
    h.host = "api.openai.com";
    h.dialect = plain::Dialect::OpenAIResponses;
    h.sig = rg::kResponsesDirect;
  } else if (provider == "mistral") {
    h.host = "api.mistral.ai";
    h.dialect = plain::Dialect::ChatCompletions;
    h.sig = rg::kMistral;
  } else if (provider == "zai") {
    h.host = "api.z.ai";
    h.dialect = plain::Dialect::ChatCompletions;
    h.sig = rg::kZai;
  } else {
    return false;
  }
  return true;
}

// Every exchange the transport made since `from`, written as <seq>-<label>.req.json, .resp.json and
// .meta (status, seconds). No header is written, so no key leaves the process.
struct Recorder {
  std::string dir;
  int seq = 0;
  size_t done = 0;
  // Numbering continues after the highest file already in the folder, so a second run never overwrites.
  void start(const std::string& d) {
    dir = d;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
      const int n = std::atoi(e.path().filename().string().c_str());
      if (n > seq) seq = n;
    }
  }
  void flush(const CurlHttpTransport& t, const std::string& label) {
    for (; done < t.calls.size(); ++done) {
      const auto& c = t.calls[done];
      char pre[16];
      std::snprintf(pre, sizeof pre, "%02d-", ++seq);
      const std::string base = dir + "/" + pre + label;
      writeAll(base + ".req.json", c.reqBody);
      writeAll(base + ".resp.json", c.respBody);
      writeAll(base + ".meta", "status " + std::to_string(c.status) + "\nseconds " + std::to_string(c.seconds) +
                                   "\npath " + c.path + "\nerr " + c.err + "\n");
      std::printf("    [%s] %s%s -> %d, %zu B out, %zu B in, %.1f s\n", label.c_str(), c.host.c_str(), c.path.c_str(),
                  c.status, c.reqBody.size(), c.respBody.size(), c.seconds);
    }
  }
};

std::string clip(const std::string& s, size_t n = 160) {
  std::string o = s.size() > n ? s.substr(0, n) + "..." : s;
  for (char& ch : o)
    if (ch == '\n') ch = ' ';
  return o;
}

// ---- the engine part: owner turns through the plain loop -------------------------------------

struct LiveChat {
  agent::Platform platform;
  txb::MemFiles files;
  std::unique_ptr<nimbusd::PlainRig> rig;
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

  LiveChat() {
    platform.nowMs = [this] {
      return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
                 .count() + 1000u;
    };
    platform.nowEpoch = [] { return (uint64_t)std::time(nullptr); };
    platform.delayMs = [](uint32_t n) { std::this_thread::sleep_for(std::chrono::milliseconds(n)); };
    platform.freeHeap = [] { return 200000u; };
    platform.largestFreeBlock = [] { return 200000u; };
    platform.allocLarge = [](size_t n) { return std::malloc(n); };
    platform.freeLarge = [](void* p) { std::free(p); };
  }
};

// The device's always-on tools as the engine declares them (texts.h, up to its 14): every one answers
// fixed made-up content, so the request carries the real tools array and system text.
void addTools(nimbus::orch::ToolRegistry& reg) {
  namespace td = plain::texts::tool_descriptions;
  size_t n = 0;
  for (const td::ToolDecl& d : td::kTools) {
    const std::string name = d.name;
    if (name == "more" || ++n > 13) continue;
    reg.add(
        name, d.description,
        [name](ArduinoJson::JsonObjectConst args, const nimbus::orch::Principal&) {
          nimbus::orch::ToolResult r;
          const std::string path = args["path"] | "";
          if (name == "read_file")
            r.output = path.find("seeds") != std::string::npos
                           ? "notes/seeds.txt 72 B, 1 line, sha8 1f2e3d4c\nseed list: 1. runner beans 2. rainbow chard "
                             "3. leeks 4. sweet peas"
                           : "no such file: " + path;
          else if (name == "list_files")
            r.output = "notes/seeds.txt (72 B)\nnotes/beds.txt (40 B)\n2 files";
          else
            r.output = "done";
          return r;
        },
        plain::declSchema(d, false));
  }
}

std::string saveFiles(const txb::MemFiles& f) {
  std::string o;
  for (const auto& kv : f.files)
    o += std::to_string(kv.first.size()) + " " + std::to_string(kv.second.size()) + "\n" + kv.first + kv.second;
  return o;
}

bool loadFiles(const std::string& bytes, txb::MemFiles& f) {
  size_t i = 0;
  while (i < bytes.size()) {
    const size_t nl = bytes.find('\n', i);
    if (nl == std::string::npos) return false;
    size_t a = 0, b = 0;
    if (std::sscanf(bytes.substr(i, nl - i).c_str(), "%zu %zu", &a, &b) != 2) return false;
    i = nl + 1;
    if (i + a + b > bytes.size()) return false;
    f.files[bytes.substr(i, a)] = bytes.substr(i + a, b);
    i += a + b;
  }
  return true;
}

struct EngineRun {
  const Head& head;
  CurlHttpTransport transport;
  Recorder rec;
  LiveChat chat;
  explicit EngineRun(const Head& h) : head(h) {}

  bool boot() {
    nimbusd::PlainRigConfig cfg;
    cfg.chatKey = "r10";
    cfg.host = head.host;
    cfg.model = head.model;
    cfg.dialect = head.dialect;
    cfg.effort = plain::headEffortFor(head.provider, head.model);
    const std::string key = head.key;
    cfg.key = [key] { return key; };
    cfg.noCompaction = "harness-lab r10: three short live turns, no compaction bound";
    chat.rig.reset(new nimbusd::PlainRig(cfg, nimbusd::PlainRigBindings{&chat.platform, &transport, &chat.files,
                                                                        nullptr, nullptr, nullptr}));
    addTools(chat.rig->registry());
    const plain::Status st = chat.rig->boot();
    if (!st.ok()) std::printf("  boot failed: %s at %s\n", tx::errSlug(st.err), st.where);
    return st.ok();
  }

  bool turn(const std::string& label, const std::string& text) {
    const nimbusd::PlainTurn t = chat.rig->say(text);
    rec.flush(transport, label);
    std::printf("  %s: ending=%s requests=%u calls=%u status=%s cause=%s\n    stated: %s\n", label.c_str(),
                nimbusd::plainEndingSlug(t.ending), (unsigned)t.out.headRequests, (unsigned)t.out.toolCalls,
                t.out.statusText.c_str(), t.out.end.cause.c_str(), clip(t.stated, 300).c_str());
    return t.ending == nimbusd::PlainEnding::Reply;
  }
};

int runEngine(const Head& h, const std::string& dir) {
  std::puts("engine: S1 text, S2 tool call and result, S3 a second turn");
  EngineRun run(h);
  run.rec.start(dir);
  if (!run.boot()) return 1;
  int fails = 0;
  fails += !run.turn("S1-text", "Hello. In one short sentence: what is two plus two?");
  fails += !run.turn("S2-tool", "Please read the file notes/seeds.txt with read_file and tell me the first seed it lists.");
  writeAll(dir + "/chat-after-S2.tx", saveFiles(run.chat.files));
  fails += !run.turn("S3-second", "Thanks. Which seed came second in that file?");
  writeAll(dir + "/chat.tx", saveFiles(run.chat.files));
  return fails;
}

int runHandoff(const Head& h, const std::string& dir, const std::string& from) {
  std::printf("handoff: one turn on %s/%s over the chat in %s\n", h.provider.c_str(), h.model.c_str(), from.c_str());
  EngineRun run(h);
  run.rec.start(dir);
  std::string bytes;
  if (!readAll(from, bytes) || !loadFiles(bytes, run.chat.files)) {
    std::puts("  cannot read the stored chat");
    return 1;
  }
  if (!run.boot()) return 1;
  const std::string tag = "S9-from-" + std::filesystem::path(from).parent_path().filename().string();
  return run.turn(tag, "Thanks. Which seed came second in that file, and which tool did you use to find out?") ? 0 : 1;
}

// ---- the codec part: the goldens' transcripts on the live head --------------------------------

// The PNG every photo ref shows (R10_PNG): a real image, so the provider decodes it.
std::string g_png;

struct Probe {
  std::string label;
  rg::Case c;
  uint32_t forceCap = 0;       // after the pre-flight (S5)
  std::string key;             // "" = the head's
  std::string model;           // "" = the head's
};

std::vector<Probe> probesFor(const Head& h) {
  static const std::set<std::string> wanted = {
      "guard_round", "summarizer_structured", "foreign_k_item", "foreign_ids", "stored_cut_answer",
      "second_turn_after_tools", "photo_in_view", "photo_in_view_inline", "near_cap_photo_turn",
      "mistral_thinking_chunk", "zai_reasoning_content", "two_parallel_calls", "restart_mid_turn"};
  std::vector<Probe> out;
  rg::Case text;
  for (const rg::Case& c : rg::requestCases()) {
    if (c.signature != h.sig) continue;
    if (c.name == "text_only") text = c;
    if (wanted.count(c.name) && !(c.name == "photo_in_view_inline" && h.provider == "anthropic"))
      out.push_back(Probe{c.name, c});
  }
  if ((h.provider == "mistral" || h.provider == "zai")) {   // S10 on a chat row: no golden names it
    rg::Facts f;
    f.photos = rg::Photos::Inline;
    out.push_back(Probe{"near_cap_photo_turn", rg::make("near_cap_photo_turn", rg::nearCapPhotoTurn(), f, h.sig)});
  }
  rg::Tx story;   // S5: an answer that cannot fit 64 tokens
  story.open();
  story.owner("Write a 400-word story about a vegetable garden in spring.");
  out.push_back(Probe{"S5-cap64", rg::make("S5_cut", story, rg::Facts(), h.sig), 64});
  out.push_back(Probe{"S7-wrong-key", text, 0, "sk-r10-wrong-key-0000000000"});
  out.push_back(Probe{"S7-unknown-model", text, 0, "", "r10-no-such-model"});
  return out;
}

bool hasPhoto(const rg::Case& c) { return c.name.find("photo") != std::string::npos; }

void describe(const plain::WireCodec& wire, const agent::HttpResponse& resp, const std::string& provider) {
  if (resp.status < 200 || resp.status >= 300) {
    const auto k = nimbus::orch::wire::classifyProvider(nimbus::orch::wire::markerSetFor(provider), resp.status, resp.body);
    std::printf("      error: class=%d marked=%d refused=%u body: %s\n", (int)k.cls, (int)k.marked, (unsigned)k.refused,
                clip(resp.body, 400).c_str());
    return;
  }
  plain::ParsedStep step;
  const plain::Status st = wire.parse(resp, step);
  size_t metaMax = 0;
  for (const auto& m : step.meta) metaMax = std::max(metaMax, m.bytes.size());
  std::printf("      parse=%s stop=%s calls=%zu meta=%zu (largest %zu B) in=%u out=%u read=%u write=%u\n",
              st.ok() ? "ok" : tx::errSlug(st.err), plain::stopSlug(step.stop), step.calls.size(), step.meta.size(),
              metaMax, (unsigned)step.usage.promptTokens, (unsigned)step.usage.completionTokens,
              (unsigned)step.usage.cacheReadTokens, (unsigned)step.usage.cacheWriteTokens);
  for (const auto& c : step.calls)
    std::printf("      call id=%s name=%s complete=%d args=%s\n", c.id.c_str(), c.name.c_str(), (int)c.complete,
                clip(c.argsJson, 120).c_str());
  if (!step.text.empty()) std::printf("      text: %s\n", clip(step.text, 300).c_str());
  if (!step.refusal.empty()) std::printf("      refusal: %s\n", clip(step.refusal).c_str());
}

int runProbe(const Head& h, const Probe& p, CurlHttpTransport& t, Recorder& rec) {
  rg::World w;
  tx::Status st = w.load(p.c);
  if (!st.ok()) return std::printf("  %s: load refused\n", p.label.c_str()), 1;
  w.turn.model = !p.model.empty() ? p.model : (hasPhoto(p.c) && !h.photoModel.empty() ? h.photoModel : h.model);
  w.head.host = h.host;
  const std::string key = p.key.empty() ? h.key : p.key;
  w.head.key = [key] { return key; };
  w.in.photo = [](const std::string&, plain::PhotoBytes& out) {
    out.bytes = (const uint8_t*)g_png.data();
    out.len = g_png.size();
    out.mime = "image/png";
    return tx::Status::success();
  };
  const plain::WireCodec* wire = nullptr;
  if (!(st = plain::wireFor(h.dialect, wire)).ok() || !wire) return std::printf("  %s: no wire\n", p.label.c_str()), 1;
  if (!(st = plain::resolveRoute(w.head, w.turn, w.route)).ok())
    return std::printf("  %s: route refused %s\n", p.label.c_str(), st.where), 1;
  const uint16_t dropped = plain::preflight(w.route, 0, w.own, w.in);
  if (p.forceCap) w.in.maxTokens = p.forceCap;
  agent::HttpRequest req;
  if (!(st = wire->render(w.in, req)).ok()) return std::printf("  %s: render refused %s\n", p.label.c_str(), st.where), 1;
  req.timeoutMs = 120000;
  std::printf("  %s (model %s, dropped %u, cap %u)\n", p.label.c_str(), w.turn.model.c_str(), (unsigned)dropped,
              (unsigned)w.in.maxTokens);
  agent::HttpResponse resp;
  std::string err;
  (void)t.exec(req, resp, err);
  rec.flush(t, p.label);
  describe(*wire, resp, h.provider);
  return 0;
}

int runCodec(const Head& h, const std::string& dir, const std::string& only) {
  std::puts("codec: the goldens' transcripts on the live head");
  CurlHttpTransport t;
  Recorder rec;
  rec.start(dir);
  int fails = 0;
  for (const Probe& p : probesFor(h)) {
    if (!only.empty() && only.find("," + p.label + ",") == std::string::npos) continue;
    fails += runProbe(h, p, t, rec);
  }
  return fails;
}

}  // namespace

int cmdR10(const std::vector<std::string>& a) {
  if (a.size() < 7) {
    std::fprintf(stderr, "usage: r10 <provider> <model> <out dir> <dotenv> <key var> <parts> [photo model]\n");
    return 2;
  }
  Head h;
  if (!headOf(a[1], h)) return std::fprintf(stderr, "unknown provider %s\n", a[1].c_str()), 2;
  h.model = a[2];
  h.photoModel = a.size() > 7 ? a[7] : "";
  h.key = keyFrom(a[4], a[5]);
  if (h.key.empty()) return std::fprintf(stderr, "no key %s in the named file\n", a[5].c_str()), 2;
  const std::string dir = a[3] + "/" + h.provider;
  std::filesystem::create_directories(dir);
  const char* png = std::getenv("R10_PNG");
  if (!png || !readAll(png, g_png)) return std::fprintf(stderr, "R10_PNG names no file\n"), 2;
  std::printf("r10: %s/%s (key %zu chars, read in process)\n", h.provider.c_str(), h.model.c_str(), h.key.size());
  int fails = 0;
  std::stringstream parts(a[6]);
  std::string part;
  while (std::getline(parts, part, ',')) {
    if (part == "engine") fails += runEngine(h, dir);
    else if (part.rfind("handoff:", 0) == 0) fails += runHandoff(h, dir, part.substr(8));
    else if (part.rfind("codec", 0) == 0) {
      const char* only = std::getenv("R10_ONLY");
      fails += runCodec(h, dir, only ? std::string(",") + only + "," : std::string());
    }
  }
  std::printf("r10 done: %d failed\n", fails);
  return fails ? 1 : 0;
}

}  // namespace lab
