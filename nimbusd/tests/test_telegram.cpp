// test_telegram - offline (T2) proof of the Telegram channel against a FAKE
// Telegram server (an injected HttpTransport that returns canned bodies). No
// network. Covers the load-bearing behaviours: getMe validation, long-poll
// parse + chat-id auth gate, offset advance, send, and offset DURABILITY across
// a restart (the property that stops a handled message being re-answered).
#include <string>
#include <vector>

#include "nimbus/harness/http.h"
#include "telegram.h"
#include "test_util.h"

using namespace nimbusd;

// A scripted transport: each exec() returns the next queued response and records
// the request path/body so the test can assert what the channel sent.
class FakeTg : public agent::HttpTransport {
 public:
  struct Reply { int status; std::string body; };
  std::vector<Reply> queue;
  std::vector<std::string> paths;
  std::vector<std::string> bodies;
  size_t i = 0;

  bool exec(const agent::HttpRequest& req, agent::HttpResponse& out,
            std::string& err) override {
    paths.push_back(req.path);
    bodies.push_back(req.body);
    if (i >= queue.size()) { err = "no scripted reply"; return false; }
    out.status = queue[i].status;
    out.body = queue[i].body;
    i++;
    return true;
  }
};

int main() {
  ndtest::Ctx c;
  std::printf("=== telegram channel (T2, offline, fake server) ===\n");

  const std::string dir = ndtest::scratchDir("tg");
  ndtest::rmTree(dir);
  const std::string offsetPath = dir + "/tg_offset";

  // ---- 1. getMe validation --------------------------------------------------
  {
    FakeTg tg;
    tg.queue.push_back({200, R"({"ok":true,"result":{"id":42,"is_bot":true,"username":"nimbus_test_bot"}})"});
    TelegramChannel ch("123:ABC", &tg, offsetPath);
    std::string user, err;
    c.ok(ch.getMe(user, err), "getMe succeeds on a valid token");
    c.eq(user, "nimbus_test_bot", "getMe extracts the bot username");
    c.ok(tg.paths[0].find("/bot123:ABC/getMe") != std::string::npos,
         "getMe hit the right path with the token");
  }

  // ---- 2. a bad token fails, naming the cause -------------------------------
  {
    FakeTg tg;
    tg.queue.push_back({401, R"({"ok":false,"error_code":401,"description":"Unauthorized"})"});
    TelegramChannel ch("bad", &tg, offsetPath);
    std::string user, err;
    c.ok(!ch.getMe(user, err), "getMe fails on a bad token");
    c.ok(err.find("401") != std::string::npos, "the failure names the HTTP cause");
  }

  // ---- 3. poll parses, applies the chat-id gate, advances the offset --------
  ndtest::rmTree(dir);
  {
    FakeTg tg;
    // Two updates: one from the allowed chat, one from a stranger's chat.
    tg.queue.push_back({200, R"({"ok":true,"result":[
      {"update_id":1001,"message":{"chat":{"id":555},"from":{"first_name":"Roy"},"text":"hello nimbus"}},
      {"update_id":1002,"message":{"chat":{"id":999},"from":{"first_name":"Stranger"},"text":"let me in"}}
    ]})"});
    TelegramChannel ch("t", &tg, offsetPath, /*allowChatId=*/"555");
    std::vector<nimbus::tg::Update> ups;
    std::string err;
    c.ok(ch.poll(0, ups, err), "poll succeeds");
    c.eqi((long)ups.size(), 1, "only the allow-listed chat's message is returned");
    if (!ups.empty()) {
      c.eq(ups[0].text, "hello nimbus", "the message text parsed correctly");
      c.eq(ups[0].chatId, "555", "the auth gate is on message.chat.id");
    }
    c.eqi(ch.offset(), 1003, "offset advanced past the last whole update (1002 + 1)");
  }

  // ---- 4. offset DURABILITY: a restart does not re-deliver handled updates --
  {
    // A fresh channel on the same offset file must resume at 1003, so the next
    // getUpdates asks the server for >= 1003 (the handled ones are not replayed).
    FakeTg tg;
    tg.queue.push_back({200, R"({"ok":true,"result":[]})"});
    TelegramChannel ch("t", &tg, offsetPath, "555");
    c.eqi(ch.offset(), 1003, "offset restored from disk after restart");
    std::vector<nimbus::tg::Update> ups;
    std::string err;
    ch.poll(0, ups, err);
    c.ok(tg.paths[0].find("offset=1003") != std::string::npos,
         "the resumed poll asks only for updates newer than the last handled one");
  }

  // ---- 5. sendMessage builds a well-formed request --------------------------
  {
    FakeTg tg;
    tg.queue.push_back({200, R"({"ok":true})"});
    TelegramChannel ch("t", &tg, offsetPath, "555");
    std::string err;
    c.ok(ch.sendMessage("555", "line one\nline \"two\"", err), "sendMessage succeeds");
    c.ok(tg.paths[0].find("/sendMessage") != std::string::npos, "sendMessage path");
    c.ok(tg.bodies[0].find("\\n") != std::string::npos &&
             tg.bodies[0].find("\\\"two\\\"") != std::string::npos,
         "the body JSON-escapes newlines and quotes");
    c.ok(tg.bodies[0].find("\"chat_id\":555") != std::string::npos,
         "a numeric chat id is sent unquoted");
  }

  ndtest::rmTree(dir);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
