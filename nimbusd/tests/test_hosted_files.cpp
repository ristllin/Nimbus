// test_hosted_files - CUM-543: the Files panel and the not-yet-hosted surfaces
// answer honestly.
//
//   (a) an upload from the page lands on the instance disk, lists, downloads byte
//       for byte (binary-safe, served as octet-stream) and is readable by the
//       assistant's own files.read tool - it used to answer {"ok":true} and store
//       nothing (list count 0);
//   (b) delete and delete-project work, and a missing file is a 404;
//   (c) a traversal name is refused by the same gate the tools use;
//   (d) routines, skills, wake-ups and downloads refuse a write by name (the page
//       printed "Created" for a routine that never existed), and still answer an
//       honest empty GET.
// Offline: no provider traffic at all.
#include <cstdio>
#include <string>

#include "engine_thread.h"
#include "reply_buffer.h"
#include "rig.h"
#include "test_util.h"
#include "web_api.h"

using namespace nimbusd;

static bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

static NimbusdRig::Options opts(const std::string& tag) {
  NimbusdRig::Options o;
  o.dataDir = ndtest::scratchDir(tag) + "/data";
  ndtest::rmTree(ndtest::scratchDir(tag));
  o.embeddings = false;
  o.embedDims = 64;
  return o;
}

struct Web {
  explicit Web(NimbusdRig& rig) : eng(&rig), api(&rig, &eng, &replies) { eng.start(); }
  ~Web() { eng.stop(); }
  ApiResp call(const std::string& m, const std::string& path, const std::string& body = "") {
    ApiResp out;
    api.handle(m, path, body, out);
    return out;
  }
  EngineThread eng;
  ReplyBuffer replies;
  WebApi api;
};

static std::string multipart(const std::string& filename, const std::string& bytes) {
  const std::string b = "----NimbusTestBoundary7";
  return "--" + b + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" + filename +
         "\"\r\nContent-Type: application/octet-stream\r\n\r\n" + bytes + "\r\n--" + b + "--\r\n";
}

static void testUploadListDownload(ndtest::Ctx& c) {
  std::printf("  -- (a) upload, list, download, and the assistant can read it --\n");
  Config cfg;
  NimbusdRig rig(cfg, opts("hf-a"));
  const std::string bin = std::string("line one\r\n--not-a-boundary\r\n") + std::string(1, '\0') + "tail";
  {
    Web web(rig);
    const ApiResp up = web.call("POST", "/api/files/upload?project=chat&name=pots-notes.md",
                                multipart("pots-notes.md", "# Pots\n- launch week 6\n"));
    c.eqi(up.status, 200, "the upload answers 200");
    const ApiResp bin1 = web.call("POST", "/api/files/upload?project=chat&name=blob.bin", multipart("blob.bin", bin));
    c.eqi(bin1.status, 200, "a binary upload answers 200");
    const ApiResp list = web.call("GET", "/api/files/list");
    c.ok(has(list.body, "\"count\":2"), "the list counts both files");
    c.ok(has(list.body, "\"name\":\"pots-notes.md\""), "the list names the upload");
    const ApiResp dl = web.call("GET", "/api/files/dl?project=chat&name=blob.bin");
    c.eqi(dl.status, 200, "the download answers 200");
    c.ok(dl.body == bin, "the download is byte for byte (binary-safe)");
    c.eq(dl.ctype, "application/octet-stream", "served as octet-stream (never rendered on the page origin)");
  }
  c.ok(has(rig.callTool("files.read", R"({"project":"chat","name":"pots-notes.md"})"), "launch week 6"),
       "the assistant's files.read sees the uploaded file");
}

static void testRemove(ndtest::Ctx& c) {
  std::printf("  -- (b) delete and delete-project --\n");
  Config cfg;
  NimbusdRig rig(cfg, opts("hf-b"));
  Web web(rig);
  web.call("POST", "/api/files/upload?project=p1&name=a.txt", multipart("a.txt", "A"));
  web.call("POST", "/api/files/upload?project=p1&name=b.txt", multipart("b.txt", "B"));
  web.call("POST", "/api/files/upload?project=p2&name=c.txt", multipart("c.txt", "C"));
  c.eqi(web.call("POST", "/api/files/rm", "project=p2&name=c.txt").status, 200, "delete answers 200");
  c.eqi(web.call("POST", "/api/files/rm", "project=p2&name=c.txt").status, 404, "a missing file is a 404");
  c.eqi(web.call("POST", "/api/files/rmproject", "project=p1").status, 400, "delete-project needs the confirm phrase");
  const ApiResp rp = web.call("POST", "/api/files/rmproject", "project=p1&confirm=DELETE");
  c.ok(rp.status == 200 && has(rp.body, "\"removed\":2"), "delete-project removes the project's files");
  c.ok(has(web.call("GET", "/api/files/list").body, "\"count\":0"), "the store is empty");
}

static void testTraversalRefused(ndtest::Ctx& c) {
  std::printf("  -- (c) a traversal name is refused --\n");
  Config cfg;
  NimbusdRig rig(cfg, opts("hf-c"));
  Web web(rig);
  const ApiResp r = web.call("POST", "/api/files/upload?project=..&name=passwd", multipart("passwd", "x"));
  c.eqi(r.status, 400, "a '..' project is refused");
  c.ok(has(web.call("GET", "/api/files/list").body, "\"count\":0"), "nothing was stored");
}

static void testNotYetHosted(ndtest::Ctx& c) {
  std::printf("  -- (d) routines, skills, wake-ups and downloads refuse writes by name --\n");
  Config cfg;
  NimbusdRig rig(cfg, opts("hf-d"));
  Web web(rig);
  const ApiResp loop = web.call("POST", "/api/loops", "action=create&name=weekday+brief&prompt=hi");
  c.eqi(loop.status, 400, "creating a routine is refused");
  c.ok(has(loop.body, "\"ok\":false") && has(loop.body, "Not available on a hosted Nimbus yet: routines"),
       "with the feature named (the page shows x.error)");
  c.ok(web.call("GET", "/api/loops").body == "[]", "the routine list stays an honest empty list");
  for (const char* p : {"/api/skills/save", "/api/wakeups", "/api/fetchq"}) {
    const ApiResp r = web.call("POST", p, "id=x");
    c.ok(r.status == 400 && has(r.body, "Not available on a hosted Nimbus yet"), std::string(p) + " refuses a write");
  }
}

int main() {
  ndtest::Ctx c;
  std::printf("=== hosted files + not-yet-hosted surfaces (CUM-543, offline) ===\n");
  testUploadListDownload(c);
  testRemove(c);
  testTraversalRefused(c);
  testNotYetHosted(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
