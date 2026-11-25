// test_plain_files_tools - engine 2's files_admin and media tools over engine 1's handlers (lane E2-P-OUT2,
// src/agent/tools_plain_files.cpp). A scripted Claude head on the real wire (txb::Battery) calls each tool
// through `more`; the engine-1 handlers are fakes that record the engine-1 arguments they were given and
// answer as engine 1 does. One test per tool:
//   - the call reaches engine 1's handler with engine 1's arguments (path split into project and name);
//   - the handler's answer reaches the head, engine 1's own tool names renamed to engine 2's;
//   - no tool is in the always-on array: each is listed only behind more, in its group;
//   - a write to a path the file tools may not write (inbox/) never reaches the handler.
#include <unity.h>

#include <ArduinoJson.h>

#include <map>
#include <string>
#include <vector>

#include "../support/tx_battery/driver.h"
#include "../../src/agent/tools_plain_send.h"   // E2-P-OUT1: send, the one way a stored file reaches the person
#include "nimbus/orch/tool_groups.h"

// The registration is portable; its device part is compiled only with ARDUINO.
#include "../../src/agent/tools_plain_files.cpp"

namespace plain = agent::plain;
namespace sp = harness_test::scripted;
namespace orch = nimbus::orch;
namespace pf = agent::plainfiles;

void setUp() {}
void tearDown() {}

namespace {

bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
size_t count(const std::string& s, const std::string& part) {
  size_t n = 0;
  for (size_t at = s.find(part); at != std::string::npos; at = s.find(part, at + 1)) n++;
  return n;
}

// One fake engine-1 handler per tool: the engine-1 arguments of each call, and the answer it gives.
struct Fakes {
  std::map<std::string, std::string> got;   // engine-1 name -> the arguments of its last call (JSON)
  std::map<std::string, std::string> answer;
  int calls = 0;
  orch::ToolHandler h(const char* old) {
    return [this, old](ArduinoJson::JsonObjectConst a, const orch::Principal&) {
      std::string json;
      serializeJson(a, json);
      got[old] = json;
      calls++;
      return orch::ToolResult::ok(answer.count(old) ? answer[old] : std::string("ok ") + old);
    };
  }
  pf::OldHandlers all() {
    pf::OldHandlers o;
    o.stat = h("files.stat");
    o.share = h("files.share");
    o.fetch = h("files.fetch");
    o.pdf = h("files.pdf");
    o.image = h("image.generate");
    o.play = h("media.play");
    o.pause = h("media.pause");
    o.stop = h("media.stop");
    o.list = h("media.list");
    return o;
  }
};

struct Rig {
  txb::Battery b;
  Fakes fakes;
  Rig() {
    b.registryDispatch = true;
    orch::registerMore(b.reg);
    pf::registerTools(b.reg, fakes.all());
  }
  // One owner turn: the head calls `name` through more with `args`, then answers.
  void callThroughMore(const std::string& name, const std::string& args) {
    b.model.push(sp::call("more", "{\"name\":\"" + name + "\",\"args\":" + args + "}"));
    b.model.push(sp::say("Done."));
    plain::TurnOutcome out;
    TEST_ASSERT_TRUE(b.owner("please", out).ok());
    TEST_ASSERT_EQUAL(2, (int)b.model.requests().size());
  }
  const std::string& firstBody() { return b.model.requests()[0].body; }
  const std::string& resultBody() { return b.model.requests()[1].body; }
};

// Not in the always-on array (the first request's tools), and listed in its group behind more.
void behindMore(const char* name, const char* group) {
  TEST_ASSERT_EQUAL_STRING(group, orch::toolClassFor(name).group);
  Rig r;
  r.b.model.push(sp::call("more", std::string("{\"group\":\"") + group + "\"}"));
  r.b.model.push(sp::say("Listed."));
  plain::TurnOutcome out;
  TEST_ASSERT_TRUE(r.b.owner("what can you do", out).ok());
  TEST_ASSERT_FALSE_MESSAGE(has(r.firstBody(), std::string("\"name\":\"") + name + "\""), name);
  // The listing (the group result) names it once more than the request before it (the tools index).
  TEST_ASSERT_TRUE_MESSAGE(count(r.resultBody(), name) > count(r.firstBody(), name), r.resultBody().c_str());
}

}  // namespace

void test_image_generate_reaches_image_generate_and_returns_the_stored_path() {
  behindMore("image_generate", "media");
  Rig r;
  r.fakes.answer["image.generate"] =
      "saved art/cat.png (1234 bytes)\nThe owner opens it from the Files list on the device web page.";
  r.callThroughMore("image_generate", R"({"prompt":"a cat","path":"art/cat.png","quality":"low"})");
  TEST_ASSERT_EQUAL_STRING(R"({"prompt":"a cat","quality":"low","project":"art","name":"cat.png"})",
                           r.fakes.got["image.generate"].c_str());
  TEST_ASSERT_TRUE_MESSAGE(has(r.resultBody(), "saved art/cat.png (1234 bytes)"), r.resultBody().c_str());
  // No path: engine 1 names the file from the prompt.
  Rig n;
  n.callThroughMore("image_generate", R"({"prompt":"a dog"})");
  TEST_ASSERT_EQUAL_STRING(R"({"prompt":"a dog"})", n.fakes.got["image.generate"].c_str());
}

void test_pdf_write_reaches_files_pdf_and_never_writes_a_read_only_path() {
  behindMore("pdf_write", "files_admin");
  Rig r;
  r.fakes.answer["files.pdf"] = "[file saved: reports/q3.pdf] (2 pages)";
  r.callThroughMore("pdf_write", R"({"path":"reports/q3.pdf","title":"Q3","text":"# Sales"})");
  TEST_ASSERT_EQUAL_STRING(R"({"title":"Q3","text":"# Sales","project":"reports","name":"q3.pdf"})",
                           r.fakes.got["files.pdf"].c_str());
  TEST_ASSERT_TRUE(has(r.resultBody(), "[file saved: reports/q3.pdf] (2 pages)"));
  // A path the file tools may not write never reaches the writer.
  Rig ro;
  ro.callThroughMore("pdf_write", R"({"path":"inbox/x.pdf","text":"t"})");
  TEST_ASSERT_EQUAL(0, ro.fakes.calls);
  TEST_ASSERT_TRUE_MESSAGE(has(ro.resultBody(), "inbox/x.pdf is read-only here"), ro.resultBody().c_str());
}

void test_share_file_reaches_files_share() {
  behindMore("share_file", "files_admin");
  Rig r;
  r.fakes.answer["files.share"] = "docs/plan.md is no longer shared";
  r.callThroughMore("share_file", R"({"path":"docs/plan.md","share":false})");
  TEST_ASSERT_EQUAL_STRING(R"({"project":"docs","name":"plan.md","share":false})", r.fakes.got["files.share"].c_str());
  TEST_ASSERT_TRUE(has(r.resultBody(), "docs/plan.md is no longer shared"));
}

void test_file_stat_reaches_files_stat() {
  behindMore("file_stat", "files_admin");
  Rig r;
  r.fakes.answer["files.stat"] = "docs/plan.md: 812 bytes, kind=doc, createdAt=1763000000";
  r.callThroughMore("file_stat", R"({"path":"docs/plan.md"})");
  TEST_ASSERT_EQUAL_STRING(R"({"project":"docs","name":"plan.md"})", r.fakes.got["files.stat"].c_str());
  TEST_ASSERT_TRUE(has(r.resultBody(), "docs/plan.md: 812 bytes, kind=doc"));
  // One segment is no store path: one true line, no handler call.
  Rig bad;
  bad.callThroughMore("file_stat", R"({"path":"plan.md"})");
  TEST_ASSERT_EQUAL(0, bad.fakes.calls);
  TEST_ASSERT_TRUE_MESSAGE(has(bad.resultBody(), "plan.md is not a path (project/name)"), bad.resultBody().c_str());
}

void test_fetch_url_queues_through_files_fetch_for_the_owners_approval() {
  behindMore("fetch_url", "files_admin");
  Rig r;
  r.fakes.answer["files.fetch"] =
      "queued for download (request 3, policy open). It runs in the background - check files.list shortly";
  r.callThroughMore("fetch_url", R"({"url":"https://example.com/a.pdf","path":"docs/a.pdf"})");
  TEST_ASSERT_EQUAL_STRING(R"({"url":"https://example.com/a.pdf","project":"docs","name":"a.pdf"})",
                           r.fakes.got["files.fetch"].c_str());
  TEST_ASSERT_TRUE_MESSAGE(has(r.resultBody(), "check list_files shortly"), r.resultBody().c_str());
  TEST_ASSERT_FALSE(has(r.resultBody(), "files.list"));
}

void test_media_play_reaches_media_play() {
  behindMore("media_play", "media");
  Rig r;
  r.fakes.answer["media.play"] = R"({"state":"playing","track":"song.wav"})";
  r.callThroughMore("media_play", R"({"track":"song.wav","queue":true})");
  TEST_ASSERT_EQUAL_STRING(R"({"path":"song.wav","queue":true})", r.fakes.got["media.play"].c_str());
  TEST_ASSERT_TRUE(has(r.resultBody(), "playing"));
}

void test_media_pause_reaches_media_pause() {
  behindMore("media_pause", "media");
  Rig r;
  r.fakes.answer["media.pause"] = R"({"state":"paused"})";
  r.callThroughMore("media_pause", "{}");
  TEST_ASSERT_EQUAL_STRING("{}", r.fakes.got["media.pause"].c_str());
  TEST_ASSERT_TRUE(has(r.resultBody(), "paused"));
}

void test_media_stop_reaches_media_stop() {
  behindMore("media_stop", "media");
  Rig r;
  r.fakes.answer["media.stop"] = R"({"state":"stopped"})";
  r.callThroughMore("media_stop", "{}");
  TEST_ASSERT_EQUAL_STRING("{}", r.fakes.got["media.stop"].c_str());
  TEST_ASSERT_TRUE(has(r.resultBody(), "stopped"));
}

void test_media_list_reaches_media_list() {
  behindMore("media_list", "media");
  Rig r;
  r.fakes.answer["media.list"] = R"({"available":["song.wav"],"player":{"state":"idle"}})";
  r.callThroughMore("media_list", "{}");
  TEST_ASSERT_EQUAL_STRING("{}", r.fakes.got["media.list"].c_str());
  TEST_ASSERT_TRUE(has(r.resultBody(), "song.wav"));
}

// The two lanes meet (E2-P-OUT1): the path image_generate returns is the path `send` kind file takes; the
// image reaches the person only through send's own delivery (sendFile), never through this lane.
void test_a_generated_image_path_is_accepted_by_send() {
  Rig r;
  r.fakes.answer["image.generate"] = "saved images/a-cat.png (1234 bytes)";
  std::vector<std::string> sent;   // project/name -> chat, as send's delivery saw it
  agent::plainsend::SendDeps d;
  d.inTurn = [] { return true; };
  d.target = [](const std::string&, const orch::Principal&, bool) { return std::string("1001"); };
  d.turnChat = [] { return std::string("1001"); };
  d.voiceOn = [] { return true; };
  d.speakerMuted = [] { return false; };
  d.screen = [](const std::string&, std::string&) { return true; };
  d.sendFile = [&sent](const std::string& project, const std::string& name, const std::string& chat,
                       const orch::Principal&, bool photoOnly, const std::string&) {
    sent.push_back(project + "/" + name + " -> " + chat + (photoOnly ? " photo" : " file"));
    return agent::plainsend::FileSent::Queued;
  };
  TEST_ASSERT_TRUE(agent::plainsend::registerSendTools(r.b.reg, d));
  r.b.model.push(sp::call("more", R"({"name":"image_generate","args":{"prompt":"a cat"}})"));
  r.b.model.push(sp::call("send", R"({"kind":"file","path":"images/a-cat.png"})"));
  r.b.model.push(sp::say("Here it is."));
  plain::TurnOutcome out;
  TEST_ASSERT_TRUE(r.b.owner("draw a cat and send it", out).ok());
  TEST_ASSERT_EQUAL(3, (int)r.b.model.requests().size());
  TEST_ASSERT_TRUE(has(r.b.model.requests()[1].body, "saved images/a-cat.png (1234 bytes)"));
  TEST_ASSERT_EQUAL(1, (int)sent.size());
  TEST_ASSERT_EQUAL_STRING("images/a-cat.png -> 1001 file", sent[0].c_str());
  TEST_ASSERT_FALSE(has(r.b.model.requests()[2].body, "is not a path"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_image_generate_reaches_image_generate_and_returns_the_stored_path);
  RUN_TEST(test_pdf_write_reaches_files_pdf_and_never_writes_a_read_only_path);
  RUN_TEST(test_share_file_reaches_files_share);
  RUN_TEST(test_file_stat_reaches_files_stat);
  RUN_TEST(test_fetch_url_queues_through_files_fetch_for_the_owners_approval);
  RUN_TEST(test_media_play_reaches_media_play);
  RUN_TEST(test_media_pause_reaches_media_pause);
  RUN_TEST(test_media_stop_reaches_media_stop);
  RUN_TEST(test_media_list_reaches_media_list);
  RUN_TEST(test_a_generated_image_path_is_accepted_by_send);
  return UNITY_END();
}
