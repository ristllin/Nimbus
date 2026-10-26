// test_spend_ceiling_vn - the monthly spend ceiling on a Virtual Nimbus (lane DEF; ruling
// R7: the 20 USD ceiling applies to hosted instances too). Offline (T2):
//   * the instance's ceiling file: default 20 USD, a crossing freezes, the state and the
//     notices sent survive a restart, raising the ceiling lifts it at once;
//   * the process-wide paid-call gate answers from it (every paid seam reads that gate);
//   * an instance that already ran an earlier release owes its web chat one notice;
//   * a file that does not read back (cut short, foreign) starts paused, never at zero;
//   * the web surface: /api/state spend + setupDefaults, /api/orch usage.ceiling,
//     POST spendCeil (cents; 0 refused), POST /api/chat answered 409 while paused,
//     and the engine thread's tick delivers the freeze notice to the web chat once.
#include <ArduinoJson.h>

#include <chrono>
#include <mutex>
#include <vector>
#include <string>
#include <thread>

#include "daemon_http.h"
#include "engine_thread.h"
#include "http_control.h"
#include "reply_buffer.h"
#include "rig.h"
#include "spend_ceiling_vn.h"
#include "test_util.h"

using namespace nimbusd;
namespace orch = nimbus::orch;

static bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

// One Anthropic sonnet-5 turn of `inTok` input tokens ($2 per million).
static orch::TurnSpend sonnetTurn(uint32_t inTok) {
  orch::TokenUsage u;
  u.promptTokens = inTok;
  u.responses = 1;
  orch::TurnSpend s;
  s.add("anthropic", "claude-sonnet-5", u);
  return s;
}

static void checkFile(ndtest::Ctx& c, const std::string& dir) {
  const std::string path = dir + "/spend_ceiling.txt";
  {
    SpendCeilingVn v(path, false, false);
    v.install();
    c.ok(has(v.stateJson(), "\"limitCents\":2000"), "a new instance starts at the 20 USD default");
    c.ok(!v.frozen() && !orch::spendFrozen(), "under the ceiling: paid calls allowed");
    v.commit(sonnetTurn(9000000), true);   // $18
    c.ok(!v.frozen(), "$18 of $20: still allowed");
    std::string t;
    c.ok(v.nextNotice(orch::NoticeChannel::Web, t) == SpendCeilingVn::Kind::Warn,
         "80 percent: one early warning is owed");
    v.delivered(orch::NoticeChannel::Web, SpendCeilingVn::Kind::Warn);
    v.commit(sonnetTurn(1500000), true);   // $21
    c.ok(v.frozen() && orch::spendFrozen(), "the crossing freezes, and the gate says so");
    c.ok(!orch::paidCallAllowed(orch::SpendSeam::Head), "the head seam is refused");
    c.ok(v.nextNotice(orch::NoticeChannel::Web, t) == SpendCeilingVn::Kind::Freeze &&
             has(t, "Paid AI calls are paused") && has(t, "$20.00"),
         "the web chat owes one freeze notice naming the ceiling");
    v.delivered(orch::NoticeChannel::Web, SpendCeilingVn::Kind::Freeze);
    c.ok(v.nextNotice(orch::NoticeChannel::Web, t) == SpendCeilingVn::Kind::None,
         "told once: nothing more is owed");
  }
  c.ok(!orch::spendFrozen(), "a destroyed instance uninstalls the gate");
  {
    SpendCeilingVn v(path, false, false);   // a restart
    v.install();
    std::string t;
    c.ok(v.frozen(), "the freeze survives a restart");
    c.ok(v.nextNotice(orch::NoticeChannel::Web, t) == SpendCeilingVn::Kind::None,
         "a restart sends no second notice");
    c.ok(v.setLimitMicro(30000000) && !v.frozen() && !orch::spendFrozen(),
         "raising the ceiling lifts the freeze at once");
    c.ok(has(v.stateJson(), "\"ownerSet\":true") && has(v.stateJson(), "\"limitCents\":3000"),
         "the owner's ceiling is kept");
  }
  {
    SpendCeilingVn v(dir + "/other.txt", true, false);   // an instance from an earlier release
    std::string t;
    c.ok(v.nextNotice(orch::NoticeChannel::Web, t) == SpendCeilingVn::Kind::Migration &&
             has(t, "New in this update: a monthly spend ceiling"),
         "an instance that ran an earlier release owes one first-start notice");
    v.delivered(orch::NoticeChannel::Web, SpendCeilingVn::Kind::Migration);
    c.ok(v.nextNotice(orch::NoticeChannel::Web, t) == SpendCeilingVn::Kind::None,
         "the first-start notice is sent once");
  }
}

// Every paid call outside a turn (a sub-agent, an embedding, speech) counts toward the
// hosted ceiling too: the seam meter's sink is this instance.
static void checkSeamMeter(ndtest::Ctx& c, const std::string& dir) {
  SpendCeilingVn v(dir + "/meter.txt", false, false);
  v.install();
  orch::TokenUsage u;
  u.promptTokens = 1000000;   // $0.10 on mistral-embed
  u.responses = 1;
  orch::meterSeamUsage(orch::SpendSeam::Embed, "mistral", "mistral-embed", u);
  c.ok(has(v.stateJson(), "\"spentCents\":10,"), "an embedding's usage counts: " + v.stateJson());
  orch::meterSeamMicro(orch::SpendSeam::Tts, "openai", 50000);   // a $0.05 estimate
  c.ok(has(v.stateJson(), "\"spentCents\":15,"), "an estimate counts too: " + v.stateJson());
  orch::TokenUsage sub;
  sub.promptTokens = 1000000;   // $0.10 on claude-haiku-5-5
  sub.responses = 1;
  orch::meterSeamUsage(orch::SpendSeam::Sub, "anthropic", "claude-haiku-5-5", sub);
  c.ok(has(v.stateJson(), "\"spentCents\":25,"), "a sub-agent's usage counts: " + v.stateJson());
}

// Lane QW4: a ceiling file that does not read back (torn, foreign, unreadable) is never
// a zero count: the instance starts paused at the default ceiling, one notice owed, and
// raising the ceiling resumes it. A save leaves no temporary file behind.
static void checkUnreadable(ndtest::Ctx& c, const std::string& dir) {
  const std::string path = dir + "/torn.txt";
  {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    std::fputs("!v|1;20000", f);   // cut mid-way through the first field
    std::fclose(f);
  }
  {
    SpendCeilingVn v(path, false, false);
    v.install();
    std::string t;
    c.ok(v.frozen() && orch::spendFrozen(), "a torn file starts paused, never at zero: " + v.stateJson());
    c.ok(!orch::paidCallAllowed(orch::SpendSeam::Head), "the paid-call gate refuses");
    c.ok(v.nextNotice(orch::NoticeChannel::Web, t) == SpendCeilingVn::Kind::Freeze &&
             has(t, "Paid AI calls are paused"),
         "one freeze notice says how to resume");
    c.ok(v.setLimitMicro(25000000) && !v.frozen(), "raising the ceiling resumes paid calls");
    c.ok(!fsutil::readFile(path + ".tmp", t), "the save leaves no temporary file");
  }
  {
    SpendCeilingVn v(path, false, false);   // the repaired file reads back
    c.ok(!v.frozen() && has(v.stateJson(), "\"limitCents\":2500"), "the repaired state survives a restart");
  }
  const std::string garbage = dir + "/foreign.txt";
  {
    std::FILE* f = std::fopen(garbage.c_str(), "wb");
    std::fputs("not a ceiling file", f);
    std::fclose(f);
  }
  SpendCeilingVn g(garbage, false, false);
  c.ok(g.frozen(), "a foreign file starts paused too");
}

struct Out { int status = 0; std::string body; };
static Out req(DaemonHttpTransport& cl, int port, const std::string& method, const std::string& path,
               const std::string& tok, const std::string& body) {
  agent::HttpRequest r;
  r.method = method;
  r.host = "127.0.0.1";
  r.port = port;
  r.tls = false;
  r.path = path;
  r.timeoutMs = 8000;
  r.headers.push_back({"X-Nimbus-Token", tok});
  if (method != "GET") {
    r.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
    r.body = body;
  }
  agent::HttpResponse resp;
  std::string err;
  Out o;
  if (cl.exec(r, resp, err)) { o.status = resp.status; o.body = resp.body; }
  return o;
}

static void checkWeb(ndtest::Ctx& c, const std::string& dataDir) {
  // A bot is set and its owner chat is allowlisted: the freeze notice goes there too.
  setenv("TELEGRAM_BOT_TOKEN", "test-bot", 1);
  setenv("NIMBUSD_TG_CHAT_ID", "424242", 1);
  Config cfg;
  NimbusdRig::Options opt;
  opt.dataDir = dataDir;
  opt.embeddings = false;
  opt.embedDims = 64;
  NimbusdRig rig(cfg, opt);
  EngineThread eng(&rig);
  eng.start();
  const std::string tok = "vn-ceiling-token";
  ReplyBuffer replies;
  std::mutex chatsMu;
  std::vector<std::string> noticeChats;   // the chat of every freeze notice delivered
  rig.setDeliver([&](const std::string& chat, const std::string& t) {
    replies.push("assistant", t, chat, eng.currentWebTurn());
    std::lock_guard<std::mutex> lk(chatsMu);
    if (has(t, "Paid AI calls are paused")) noticeChats.push_back(chat);
  });
  HttpControl http(&eng, "127.0.0.1", 0, tok, &replies, &rig);
  const int port = http.start();
  DaemonHttpTransport cl;

  Out st = req(cl, port, "GET", "/api/state", tok, "");
  c.ok(has(st.body, "\"spend\":{\"limitCents\":2000") && has(st.body, "\"setupDefaults\":\"Each provider"),
       "/api/state carries the ceiling and the setup line");
  Out o = req(cl, port, "GET", "/api/orch", tok, "");
  c.ok(has(o.body, "\"ceiling\":{\"limitCents\":2000"), "/api/orch usage carries the ceiling meter");
  c.ok(req(cl, port, "POST", "/api/orch", tok, "spendCeil=0").status == 400, "a ceiling of 0 is refused");
  Out p = req(cl, port, "POST", "/api/orch", tok, "spendCeil=5000");
  c.ok(p.status == 200 && has(p.body, "\"applied\":1"), "POST spendCeil=5000 applies: " + p.body);
  c.ok(has(rig.spendCeiling().stateJson(), "\"limitCents\":5000"), "the ceiling is now 50 USD");

  rig.spendCeiling().commit(sonnetTurn(26000000), true);   // $52: over the ceiling
  Out chat = req(cl, port, "POST", "/api/chat", tok, "text=hello");
  c.ok(chat.status == 409 && has(chat.body, "spend_ceiling"), "a chat send while paused is answered 409");
  // The engine thread's tick (<= 1 s) delivers the freeze notice to the web chat once.
  int notices = 0;
  for (int i = 0; i < 40 && notices == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    notices = has(replies.sinceJsonArray(0), "Paid AI calls are paused") ? 1 : 0;
  }
  c.ok(notices == 1, "the web chat got the freeze notice");
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));
  const std::string all = replies.sinceJsonArray(0);
  // One notice per channel: the web chat ("owner") and the Telegram owner chat.
  size_t n = 0;
  for (size_t at = all.find("Paid AI calls are paused"); at != std::string::npos;
       at = all.find("Paid AI calls are paused", at + 1))
    ++n;
  c.ok(n == 2, "one notice on the web chat and one on Telegram, nothing repeated (" +
                   std::to_string(n) + ")");
  {
    std::lock_guard<std::mutex> lk(chatsMu);
    c.ok(noticeChats.size() == 2 && noticeChats[0] != noticeChats[1] &&
             (noticeChats[0] == "424242" || noticeChats[1] == "424242"),
         "the Telegram notice went to the owner chat");
  }
  // Lane DEF2: the owner can turn the ceiling off; paid calls are no longer paused by it.
  Out off = req(cl, port, "POST", "/api/orch", tok, "spendCeil=off");
  c.ok(off.status == 200 && has(rig.spendCeiling().stateJson(), "\"active\":false") &&
           !orch::spendFrozen(),
       "POST spendCeil=off turns the ceiling off: " + off.body);
  unsetenv("TELEGRAM_BOT_TOKEN");
  unsetenv("NIMBUSD_TG_CHAT_ID");
  http.stop();
  eng.stop();
}

int main() {
  ndtest::Ctx c;
  std::printf("=== virtual nimbus monthly spend ceiling (T2, offline) ===\n");
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY", "TAVILY_API_KEY"})
    unsetenv(k);
  setenv("TZ", "UTC0", 1);
  tzset();
  const std::string dir = ndtest::scratchDir("spendceil");
  ndtest::rmTree(dir);
  fsutil::mkdirs(dir);
  checkFile(c, dir);
  checkSeamMeter(c, dir);
  checkUnreadable(c, dir);
  checkWeb(c, dir + "/data");
  ndtest::rmTree(dir);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
