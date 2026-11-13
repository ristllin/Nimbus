// test_telegram - offline (T2) proof of the Telegram channel against a FAKE
// Telegram server (an injected HttpTransport that returns canned bodies). No
// network. Covers the load-bearing behaviours: getMe validation, long-poll
// parse, offset advance, send, and offset DURABILITY across a restart (the
// property that stops a handled message being re-answered). The chat-id auth
// gate is no longer the channel's job: every update is returned and routed
// through the device trust model (tests/test_tg_access.cpp, CUM-459).
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

// ---- 6. a token rotation resets the offset: the stored offset belongs to a bot --
static void checkTokenRotationResetsOffset(ndtest::Ctx& c, const std::string& dir,
                                           const std::string& offsetPath) {
  // Bot 111 confirms up to 5002, then the owner rotates to a different bot.
  ndtest::rmTree(dir);
  {
    FakeTg tg;
    tg.queue.push_back({200, R"({"ok":true,"result":[
      {"update_id":5001,"message":{"chat":{"id":555},"from":{"first_name":"Roy"},"text":"hi"}}
    ]})"});
    TelegramChannel ch("111:OLD-SECRET", &tg, offsetPath);
    std::vector<nimbus::tg::Update> ups;
    std::string err;
    c.ok(ch.poll(0, ups, err), "poll succeeds");
    c.eqi(ch.offset(), 5002, "offset advanced and persisted for bot 111");
  }
  // A restart with a DIFFERENT bot must not reuse bot 111's offset: getUpdates
  // with offset 5002 on the new bot answers ok with an empty result forever and
  // silently drops every message (the rotation failure this closes).
  {
    FakeTg tg;
    tg.queue.push_back({200, R"({"ok":true,"result":[
      {"update_id":1,"message":{"chat":{"id":555},"from":{"first_name":"Roy"},"text":"new bot"}}
    ]})"});
    TelegramChannel ch("222:NEW-SECRET", &tg, offsetPath);
    c.eqi(ch.offset(), 0, "a different bot starts at offset 0, not the old bot's offset");
    std::vector<nimbus::tg::Update> ups;
    std::string err;
    c.ok(ch.poll(0, ups, err), "poll succeeds");
    c.ok(tg.paths[0].find("offset=0") != std::string::npos,
         "the new bot's first poll asks from 0, so its messages are served");
    c.eqi(ch.offset(), 2, "the new bot's own update advances its own offset");
  }
  // The same bot with a regenerated secret keeps its offset: update ids continue
  // on that bot, so a reset here would needlessly re-serve its unconfirmed page.
  {
    FakeTg tg;
    tg.queue.push_back({200, R"({"ok":true,"result":[]})"});
    TelegramChannel ch("222:ROTATED-SECRET", &tg, offsetPath);
    c.eqi(ch.offset(), 2, "the same bot id keeps the offset across a secret rotation");
  }
  // A pre-fingerprint offset file (bare number, no bot id) belongs to no known
  // bot: an upgrade must not trust it against today's token.
  ndtest::rmTree(dir);
  fsutil::writeFileAtomic(offsetPath, "4821");
  {
    FakeTg tg;
    tg.queue.push_back({200, R"({"ok":true,"result":[]})"});
    TelegramChannel ch("333:SECRET", &tg, offsetPath);
    c.eqi(ch.offset(), 0, "a pre-fingerprint offset file is not trusted for any bot");
    std::vector<nimbus::tg::Update> ups;
    std::string err;
    ch.poll(0, ups, err);
    c.ok(tg.paths[0].find("offset=0") != std::string::npos,
         "the first poll after the upgrade asks from 0");
  }
}

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

  // ---- 3. poll parses every update (the caller gates), advances the offset ---
  ndtest::rmTree(dir);
  {
    FakeTg tg;
    // Two updates: one from the allowed chat, one from a stranger's chat.
    tg.queue.push_back({200, R"({"ok":true,"result":[
      {"update_id":1001,"message":{"chat":{"id":555},"from":{"first_name":"Roy"},"text":"hello nimbus"}},
      {"update_id":1002,"message":{"chat":{"id":999},"from":{"first_name":"Stranger"},"text":"let me in"}}
    ]})"});
    TelegramChannel ch("t", &tg, offsetPath);
    std::vector<nimbus::tg::Update> ups;
    std::string err;
    c.ok(ch.poll(0, ups, err), "poll succeeds");
    c.eqi((long)ups.size(), 2, "both updates are returned for the trust gate to route");
    if (ups.size() == 2) {
      c.eq(ups[0].text, "hello nimbus", "the message text parsed correctly");
      c.eq(ups[0].chatId, "555", "the chat id is message.chat.id (what the gate keys on)");
      c.eq(ups[1].chatId, "999", "the stranger's update is returned too, in wire order");
    }
    c.eqi(ch.offset(), 1003, "offset advanced past the last whole update (1002 + 1)");
  }

  // ---- 4. offset DURABILITY: a restart does not re-deliver handled updates --
  {
    // A fresh channel on the same offset file must resume at 1003, so the next
    // getUpdates asks the server for >= 1003 (the handled ones are not replayed).
    FakeTg tg;
    tg.queue.push_back({200, R"({"ok":true,"result":[]})"});
    TelegramChannel ch("t", &tg, offsetPath);
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
    TelegramChannel ch("t", &tg, offsetPath);
    std::string err;
    c.ok(ch.sendMessage("555", "line one\nline \"two\"", err), "sendMessage succeeds");
    c.ok(tg.paths[0].find("/sendMessage") != std::string::npos, "sendMessage path");
    c.ok(tg.bodies[0].find("\\n") != std::string::npos &&
             tg.bodies[0].find("\\\"two\\\"") != std::string::npos,
         "the body JSON-escapes newlines and quotes");
    c.ok(tg.bodies[0].find("\"chat_id\":555") != std::string::npos,
         "a numeric chat id is sent unquoted");
  }

  // ---- 5b. CUM-995: sends go out as parse_mode=HTML with the markdown converted
  {
    FakeTg tg;
    tg.queue.push_back({200, R"({"ok":true})"});
    TelegramChannel ch("t", &tg, offsetPath);
    std::string err;
    c.ok(ch.sendMessage("555", "**bold** and `code`", err), "html send succeeds");
    c.ok(tg.bodies[0].find("\"parse_mode\":\"HTML\"") != std::string::npos,
         "the body carries parse_mode HTML");
    c.ok(tg.bodies[0].find("<b>bold</b>") != std::string::npos &&
             tg.bodies[0].find("<code>code</code>") != std::string::npos,
         "markdown markers convert to Telegram HTML entities");
    c.ok(tg.bodies[0].find("**") == std::string::npos && tg.bodies[0].find("`code`") == std::string::npos,
         "no raw markdown markers reach the wire");
  }

  // ---- 5c. CUM-995: markup in the text is escaped, never injected ------------
  {
    FakeTg tg;
    tg.queue.push_back({200, R"({"ok":true})"});
    TelegramChannel ch("t", &tg, offsetPath);
    std::string err;
    c.ok(ch.sendMessage("555", "<script>alert(1)</script> & <b>fake</b>", err), "send succeeds");
    c.ok(tg.bodies[0].find("&lt;script&gt;") != std::string::npos &&
             tg.bodies[0].find("&lt;b&gt;fake&lt;/b&gt;") != std::string::npos,
         "HTML in the text is entity-escaped");
    c.ok(tg.bodies[0].find("<script>") == std::string::npos &&
             tg.bodies[0].find("<b>fake</b>") == std::string::npos,
         "no raw markup reaches the wire");
  }

  // ---- 5d. CUM-995: Telegram rejects the entities -> the original text is
  // resent plain (a reply is never lost), and only then does it fail --------
  {
    FakeTg tg;
    tg.queue.push_back({400, R"({"ok":false,"error_code":400,"description":"Bad Request: can't parse entities"})"});
    tg.queue.push_back({200, R"({"ok":true})"});
    TelegramChannel ch("t", &tg, offsetPath);
    std::string err;
    c.ok(ch.sendMessage("555", "**bold** <b>x</b>", err), "the plain resend succeeds after a 400");
    c.eqi((long)tg.bodies.size(), 2, "two sendMessage calls were made");
    c.ok(tg.bodies[0].find("\"parse_mode\":\"HTML\"") != std::string::npos,
         "the first attempt was the HTML one");
    c.ok(tg.bodies[1].find("parse_mode") == std::string::npos,
         "the fallback carries no parse_mode");
    c.ok(tg.bodies[1].find("**bold**") != std::string::npos &&
             tg.bodies[1].find("<b>x</b>") != std::string::npos,
         "the fallback resends the ORIGINAL text verbatim");
  }
  {
    FakeTg tg;
    tg.queue.push_back({400, R"({"ok":false})"});
    tg.queue.push_back({400, R"({"ok":false})"});
    TelegramChannel ch("t", &tg, offsetPath);
    std::string err;
    c.ok(!ch.sendMessage("555", "hi", err), "both sends failing reports failure");
    c.ok(!err.empty(), "the failure carries a reason");
    c.eqi((long)tg.bodies.size(), 2, "the plain resend was attempted before failing");
  }

  checkTokenRotationResetsOffset(c, dir, offsetPath);

  ndtest::rmTree(dir);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
