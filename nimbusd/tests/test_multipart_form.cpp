// test_multipart_form - CUM-543: a hosted instance reads the web app's FormData posts.
//
// The page the device and a hosted instance share posts some settings as
// `new FormData(...)` (multipart/form-data): the Updates and battery toggles to
// /api/config, the cloud and sign-in forms. nimbusd parsed only urlencoded bodies,
// so a toggle was refused with its boundary line as the field name ("Not available
// on a hosted Nimbus: ------WebKitFormBoundary..."). The class rule: a multipart
// form reaches every handler as the same fields a urlencoded post carries.
//
//   (a) the parser: text parts become a urlencoded body whose decoded fields are
//       byte-identical to the parts (a value can never add a field), the boundary
//       is read from the Content-Type (quoted or not, any case), and a file part, a
//       form past the bounds or one that cannot be read is refused;
//   (b) POST /api/config with a real browser-shaped multipart body: a device-only
//       toggle is refused by its real name and a hosted field applies; the upload
//       route still takes its file part.
// The same over the real loopback HTTP surface, with the Content-Type header the
// browser sends, is in test_control.cpp.
// Offline: no network, no keys.
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "engine_thread.h"
#include "multipart_form.h"
#include "reply_buffer.h"
#include "rig.h"
#include "test_util.h"
#include "web_api.h"

using namespace nimbusd;

static bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

// The urlencoded fields of a body, decoded (what every /api/* handler reads).
static std::vector<std::pair<std::string, std::string>> fields(const std::string& body) {
  auto dec = [](const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
      if (s[i] == '%' && i + 2 < s.size()) {
        o += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
        i += 2;
      } else {
        o += s[i] == '+' ? ' ' : s[i];
      }
    }
    return o;
  };
  std::vector<std::pair<std::string, std::string>> out;
  for (size_t at = 0; at < body.size();) {
    size_t amp = body.find('&', at);
    if (amp == std::string::npos) amp = body.size();
    const std::string kv = body.substr(at, amp - at);
    const size_t eq = kv.find('=');
    out.push_back({dec(kv.substr(0, eq)), eq == std::string::npos ? std::string() : dec(kv.substr(eq + 1))});
    at = amp + 1;
  }
  return out;
}

static const char* kBoundary = "----WebKitFormBoundaryNimbusFormTest1";
static const std::string kType = std::string("multipart/form-data; boundary=") + kBoundary;

// A body exactly as a browser's FormData serializes it.
static std::string formData(const std::vector<std::pair<std::string, std::string>>& parts) {
  std::string b;
  for (const auto& f : parts)
    b += std::string("--") + kBoundary + "\r\nContent-Disposition: form-data; name=\"" + f.first +
         "\"\r\n\r\n" + f.second + "\r\n";
  return b + "--" + kBoundary + "--\r\n";
}

static std::string fileData(const std::string& name, const std::string& filename, const std::string& bytes) {
  return std::string("--") + kBoundary + "\r\nContent-Disposition: form-data; name=\"" + name +
         "\"; filename=\"" + filename + "\"\r\nContent-Type: application/octet-stream\r\n\r\n" + bytes +
         "\r\n--" + kBoundary + "--\r\n";
}

static void testParser(ndtest::Ctx& c) {
  std::printf("  -- (a) the parser --\n");
  c.eq(form::boundaryOf(kType), kBoundary, "the boundary is read from the Content-Type");
  c.eq(form::boundaryOf("Multipart/Form-Data; charset=utf-8; BOUNDARY=\"a b\""), "a b",
       "any case, a quoted boundary, other parameters first");
  c.eq(form::boundaryOf("application/x-www-form-urlencoded"), "", "a urlencoded post has none");
  c.ok(!form::isMultipart("application/x-www-form-urlencoded") && form::isMultipart(kType),
       "only multipart/form-data is a multipart form");

  std::string out;
  c.ok(form::toUrlencoded(formData({{"autoUpd", "1"}}), kBoundary, out) == form::Result::Ok, "one toggle reads");
  c.eq(out, "autoUpd=1", "as the urlencoded field the page would have posted");

  // A value with every character that means something in either encoding.
  const std::string tricky = "a&b=c+d%20e\r\nname=\"x\"\r\n--not-the-boundary \xC3\xA9";
  c.ok(form::toUrlencoded(formData({{"devName", tricky}, {"devTz", "Europe/London"}}), kBoundary, out) ==
           form::Result::Ok, "two fields read");
  const auto f = fields(out);
  c.eqi((long)f.size(), 2, "no value added a field of its own");
  if (f.size() == 2) {
    c.eq(f[0].first + "|" + f[0].second, "devName|" + tricky, "a value decodes byte-identical");
    c.eq(f[1].first + "|" + f[1].second, "devTz|Europe/London", "the next field is intact");
  }
  c.ok(form::toUrlencoded(formData({}), kBoundary, out) == form::Result::Ok && out.empty(),
       "an empty form reads as no fields");

  c.ok(form::toUrlencoded(fileData("file", "notes.txt", "x"), kBoundary, out) == form::Result::FilePart,
       "a file part is refused");
  std::vector<std::pair<std::string, std::string>> many;
  for (size_t i = 0; i <= form::kMaxParts; i++) many.push_back({"f" + std::to_string(i), "1"});
  c.ok(form::toUrlencoded(formData(many), kBoundary, out) == form::Result::TooLarge, "past the part bound");
  c.ok(form::toUrlencoded(formData({{"sysPrompt", std::string(form::kMaxValueBytes + 1, 'x')}}), kBoundary,
                          out) == form::Result::TooLarge, "past the value bound");
  c.ok(form::toUrlencoded(formData({{std::string(form::kMaxNameBytes + 1, 'n'), "1"}}), kBoundary, out) ==
           form::Result::TooLarge, "past the name bound");
  const std::string good = formData({{"autoUpd", "1"}});
  c.ok(form::toUrlencoded(good.substr(0, good.size() - 10), kBoundary, out) == form::Result::Malformed,
       "a cut body (no closing boundary) is refused");
  c.ok(form::toUrlencoded(good, "other-boundary", out) == form::Result::Malformed, "the wrong boundary");
  c.ok(form::toUrlencoded(std::string("--") + kBoundary + "\r\nX-Other: 1\r\n\r\nv\r\n--" + kBoundary + "--",
                          kBoundary, out) == form::Result::Malformed, "a part with no field name");
  c.ok(form::toUrlencoded("", kBoundary, out) == form::Result::Malformed, "an empty body");
}

// A rig + engine + web API with no keys (settings writes need no provider).
struct Web {
  explicit Web(const std::string& tag) : rig(cfg, opts(tag)), eng(&rig), api(&rig, &eng, &replies) {
    eng.start();
  }
  ~Web() { eng.stop(); }
  static NimbusdRig::Options opts(const std::string& tag) {
    NimbusdRig::Options o;
    o.dataDir = ndtest::scratchDir(tag) + "/data";
    ndtest::rmTree(ndtest::scratchDir(tag));
    o.embeddings = false;
    o.embedDims = 64;
    o.devName = "Before";
    return o;
  }
  ApiResp post(const std::string& path, const std::string& body, const std::string& type) {
    ApiResp out;
    api.handle("POST", path, body, out, type);
    return out;
  }
  Config cfg;
  NimbusdRig rig;
  EngineThread eng;
  ReplyBuffer replies;
  WebApi api;
};

static void testConfigPost(ndtest::Ctx& c) {
  std::printf("  -- (b) POST /api/config with the page's FormData --\n");
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY", "CUMULO_API_KEY", "Z_AI_TOKEN"})
    unsetenv(k);
  Web w("mp-cfg");
  // Every device-only toggle the page posts as FormData is refused by its own name.
  for (const char* f : {"autoUpd", "usbCfm", "lbRing", "lbSaver", "battMon", "scrFlip", "saverMin"}) {
    const ApiResp r = w.post("/api/config", formData({{f, "1"}}), kType);
    c.eqi(r.status, 400, std::string(f) + ": refused");
    c.ok(has(r.body, std::string("\"error\":\"Not available on a hosted Nimbus: ") + f + "\""),
         std::string(f) + ": by its real name");
    c.ok(!has(r.body, "WebKitFormBoundary"), std::string(f) + ": never the boundary line");
  }
  const ApiResp ok = w.post("/api/config", formData({{"devName", "Studio & Co"}}), kType);
  c.eqi(ok.status, 200, "a hosted field posted as FormData applies");
  c.eq(w.rig.settings().devName, "Studio & Co", "the name is saved exactly as posted");
  const ApiResp mixed = w.post("/api/config", formData({{"devName", "Other"}, {"autoUpd", "0"}}), kType);
  c.eqi(mixed.status, 400, "a post that mixes in a device-only field is refused");
  c.eq(w.rig.settings().devName, "Studio & Co", "and nothing in it applies");

  const ApiResp file = w.post("/api/config", fileData("devName", "x.txt", "Evil"), kType);
  c.eqi(file.status, 400, "a file part outside the Files panel is refused");
  c.ok(has(file.body, "Files can be added only from the Files panel."), "with a plain sentence");
  const ApiResp cut = w.post("/api/config", "--" + std::string(kBoundary) + "\r\nbroken", kType);
  c.eqi(cut.status, 400, "a form that cannot be read is refused");
  c.ok(has(cut.body, "Couldn't read the form."), "with a plain sentence");
  c.eq(w.rig.settings().devName, "Studio & Co", "neither changed anything");

  const ApiResp up = w.post("/api/files/upload?project=chat&name=notes.txt", fileData("file", "notes.txt", "hello"),
                            kType);
  c.eqi(up.status, 200, "the upload route still takes its file part");
  std::string bytes;
  c.ok(w.rig.files().fetch("chat", "notes.txt", bytes) && bytes == "hello", "and stores the file's bytes");
}

int main() {
  ndtest::Ctx c;
  std::printf("=== multipart forms on a hosted Nimbus (CUM-543, offline) ===\n");
  testParser(c);
  testConfigPost(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
