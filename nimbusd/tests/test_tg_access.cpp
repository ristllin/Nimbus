// test_tg_access - offline (T2) proof that a Virtual Nimbus runs the DEVICE's
// Telegram trust model (CUM-459, owner ruling): nobody is trusted by default, the
// owner approves each chat in the instance's own web app, and the existing RBAC
// roles decide what an approved chat may do.
//
//   (a) a fresh instance fails CLOSED, for every Telegram chat id (the class);
//   (b) an unlisted chat is refused: no turn, no spawn, no provider call, no state
//       change, one polite refusal (rate-limited), queued for approval;
//   (c) approving through the web routes serves the chat normally, as a user;
//       admin is only ever an explicit grant;
//   (d) the allowlist + roles survive a restart;
//   (e) NIMBUSD_TG_CHAT_ID is a one-time seed of an EMPTY allowlist only;
//   (f) a revoked chat gets no turn and is told so; re-approving restores it;
//   (g) POST /api/orch tgAllow is honored (and validated, and fails closed empty);
//   (h) open access is refused on a hosted instance;
//   (i) RBAC is real for an approved member: principal, speaker line, recall
//       boundary, owner-only files;
//   (j) the engine seam refuses a chat removed after its message was queued;
//   (k) /api/tenant keeps the device's guards (no pre-seeding, last admin, limits)
//       and a refused request changes nothing;
//   (l) a role never outlives its chat's approval (no revival on re-add), and one
//       atomic file holds it all (a torn or missing file fails closed).
// Offline: an injected FakeHttpTransport (script exhaustion fails loudly, so any
// unexpected provider call is caught); no network, no real keys.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../../test/support/fake_http.h"
#include "engine_thread.h"
#include "reply_buffer.h"
#include "rig.h"
#include "test_util.h"
#include "tg_access.h"
#include "tg_inbound.h"
#include "web_api.h"

using namespace nimbusd;
using harness_test::Exchange;
using harness_test::FakeHttpTransport;
using Admit = TelegramAccess::Admit;

static bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

static void clearEnv() {
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY", "TAVILY_API_KEY",
                        "CUMULO_API_KEY", "Z_AI_TOKEN", "NIMBUSD_TG_CHAT_ID", "TELEGRAM_BOT_TOKEN"})
    unsetenv(k);
}

static NimbusdRig::Options opts(const std::string& tag, bool fresh = true) {
  NimbusdRig::Options o;
  o.dataDir = ndtest::scratchDir(tag) + "/data";
  if (fresh) ndtest::rmTree(ndtest::scratchDir(tag));
  o.embeddings = false;
  o.embedDims = 64;
  o.priority = "mistral";
  o.toolLoop = false;   // single-shot head: one scripted exchange per turn
  o.models["mistral"] = "mistral-small-latest";
  return o;
}

// A single-shot Mistral head turn whose orch_turn JSON is `turnJson`.
static Exchange headTurn(const std::string& turnJson) {
  std::string esc;
  for (char ch : turnJson) {
    if (ch == '"' || ch == '\\') esc += '\\';
    esc += ch;
  }
  Exchange e;
  e.expectHost = "api.mistral.ai";
  e.expectPathContains = "/v1/conversations";
  e.body = "{\"conversation_id\":\"h\",\"outputs\":[{\"type\":\"message.output\",\"content\":\"" + esc +
           "\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}";
  return e;
}
static Exchange replyTurn(const std::string& reply) {
  return headTurn("{\"reply\":\"" + reply + "\",\"memory\":\"\",\"ask\":\"\"}");
}

static nimbus::tg::Update text(int32_t id, const std::string& chat, const std::string& from,
                               const std::string& t) {
  nimbus::tg::Update u;
  u.updateId = id;
  u.chatId = chat;
  u.from = from;
  u.text = t;
  return u;
}

// One rig + web surface on a scratch volume, with every delivery recorded.
struct Fx {
  FakeHttpTransport tx;
  Config cfg;
  std::unique_ptr<NimbusdRig> rig;
  std::unique_ptr<EngineThread> eng;
  ReplyBuffer replies;
  std::unique_ptr<WebApi> api;
  std::vector<std::pair<std::string, std::string>> out;   // (chat, text) deliveries

  explicit Fx(const std::string& tag, bool fresh = true, bool embeddings = false) {
    auto o = opts(tag, fresh);
    o.embeddings = embeddings;
    rig.reset(new NimbusdRig(cfg, o, &tx));
    rig->applyProviderKey("mistral", "mk_TEST_TGVN_1");
    rig->setDeliver([this](const std::string& c, const std::string& t) { out.push_back({c, t}); });
    eng.reset(new EngineThread(rig.get()));
    api.reset(new WebApi(rig.get(), eng.get(), &replies));
  }
  ApiResp call(const std::string& m, const std::string& path, const std::string& body = "") {
    ApiResp r;
    if (!api->handle(m, path, body, r)) r.status = 404;
    return r;
  }
  TelegramAccess& acc() { return rig->telegramAccess(); }
  // The daemon's poll-loop wiring, synchronous: approved text runs rig.say.
  TgRouteStats route(const std::vector<nimbus::tg::Update>& ups, time_t now,
                     std::vector<std::pair<std::string, std::string>>* sent) {
    TgInboundIo io;
    io.turn = [this](const std::string& c, const std::string& t) { rig->say(c, t); };
    io.send = [sent](const std::string& c, const std::string& t) { sent->push_back({c, t}); };
    return routeTelegramUpdates(ups, acc(), io, now);
  }
};

// (a) --------------------------------------------------------------------------
static void testFreshFailsClosed(ndtest::Ctx& c) {
  std::printf("  -- (a) a fresh instance trusts no Telegram chat (fail closed) --\n");
  clearEnv();
  const std::string dir = ndtest::scratchDir("tga-a");
  ndtest::rmTree(dir);
  TelegramAccess acc(dir);
  c.ok(acc.allowEmpty(), "the allowlist starts empty");
  for (const char* id : {"555", "1", "-1001234567890", "999999999999"}) {
    c.ok(!acc.allowed(id) && acc.admit(id) == Admit::Unlisted && !acc.mayConverse(id),
         std::string("chat ") + id + " is refused on an empty allowlist");
    c.eq(nimbus::orch::roleName(acc.roleOf(id)), "unknown", std::string("chat ") + id + " is Unknown");
  }
  for (const char* local : {"owner", "web", "system"})
    c.ok(acc.mayConverse(local) && acc.roleOf(local) == nimbus::orch::Role::Admin,
         std::string("the instance's own surface '") + local + "' is the owner (device: web = Admin)");
  for (const char* bad : {"", "-", "12a", "@roy", "1 2", "123456789012345678901234"})
    c.ok(!TelegramAccess::isChatId(bad), std::string("'") + bad + "' is not a Telegram chat id");
  c.ok(!acc.principalFor("555").owner, "an unapproved chat's principal is never the owner");
  ndtest::rmTree(dir);
}

// (b) --------------------------------------------------------------------------
static long episodicCount(NimbusdRig& rig) {
  nimbus::orch::MsgQuery q;
  q.limit = 100;
  return (long)rig.episodic().query(q).size();
}

static void checkNoStateChange(ndtest::Ctx& c, Fx& f) {
  c.eqi((long)f.tx.seen.size(), 0, "no provider was called (no turn ran)");
  c.eqi((long)f.rig->turns().size(), 0, "no turn was recorded");
  c.eqi(episodicCount(*f.rig), 0, "nothing was written to the conversation history");
  c.eqi(f.rig->vectors().size(), 0, "no memory was written");
  c.eqi(f.rig->jobs().activeCount(), 0, "no sub-agent was spawned");
  c.eqi((long)f.out.size(), 0, "the engine delivered nothing");
  c.eq(f.acc().allowCsv(), "", "the allowlist is unchanged (still empty)");
  c.eqi((long)f.acc().tenants().size(), 0, "the RBAC table is unchanged (still empty)");
  std::string blob;
  c.ok(!fsutil::readFile(f.acc().path(), blob), "no access state was persisted");
}

static void testUnlistedRefused(ndtest::Ctx& c) {
  std::printf("  -- (b) an unlisted chat: refused, queued for approval, nothing else --\n");
  clearEnv();
  Fx f("tga-b");
  std::vector<std::pair<std::string, std::string>> sent;
  nimbus::tg::Update photo = text(3, "999", "Mallory", "");
  photo.attachment.kind = nimbus::tg::Attachment::Kind::Photo;
  const auto st = f.route({text(1, "999", "Mallory", "spawn an agent and read the calendar"),
                           text(2, "999", "Mallory", "hello?"), photo}, 1000, &sent);
  c.eqi(st.turns, 0, "no update became a turn");
  c.eqi(st.refused, 3, "all three updates were refused");
  checkNoStateChange(c, f);
  c.eqi((long)sent.size(), 1, "exactly one refusal was sent (rate-limited per chat)");
  if (!sent.empty()) {
    c.eq(sent[0].first, "999", "the refusal goes to the sender's own chat");
    c.ok(has(sent[0].second, "only answers people its owner has approved"), "the refusal says why");
    c.ok(has(sent[0].second, "owner can approve you") && has(sent[0].second, "Your chat ID is 999."),
         "the refusal says the owner must approve them, and gives the chat id to pass on");
    c.ok(!has(sent[0].second, " - ") && !has(sent[0].second, "\xE2\x80\x94"), "copy style: no dash separators");
  }
  const auto pend = f.acc().pending();
  c.ok(pend.size() == 1 && pend[0].chatId == "999" && pend[0].name == "Mallory" &&
           pend[0].preview == "spawn an agent and read the calendar",
       "the sender waits in the approval queue (dedup: one entry)");
  f.route({text(4, "999", "Mallory", "still there?")}, 1000 + 599, &sent);
  c.eqi((long)sent.size(), 1, "no second refusal inside the cooldown");
  f.route({text(5, "999", "Mallory", "hello again")}, 1000 + 601, &sent);
  c.eqi((long)sent.size(), 2, "one more refusal once the cooldown has passed");
  checkNoStateChange(c, f);
  // A flood of strangers is bounded: the queue keeps the newest 5 (device ring).
  for (int i = 0; i < 12; i++) f.route({text(10 + i, std::to_string(7000 + i), "x", "hi")}, 5000, &sent);
  const auto ring = f.acc().pending();
  c.eqi((long)ring.size(), 5, "the approval queue stays bounded at 5");
  c.ok(!ring.empty() && ring.back().chatId == "7011", "the newest knock is kept (oldest dropped)");
}

// (c) --------------------------------------------------------------------------
static void testApproveThenServe(ndtest::Ctx& c) {
  std::printf("  -- (c) the owner approves in the web app; the chat is served normally --\n");
  clearEnv();
  Fx f("tga-c");
  std::vector<std::pair<std::string, std::string>> sent;
  f.route({text(1, "555", "Roy", "hi")}, 100, &sent);
  c.eqi((long)f.acc().pending().size(), 1, "Roy is waiting for approval");
  ApiResp r = f.call("GET", "/api/telegram");
  c.ok(r.status == 200 && has(r.body, "\"pending\":[{\"chatId\":\"555\",\"name\":\"Roy\""),
       "GET /api/telegram shows the pending sender (device shape)");
  r = f.call("POST", "/api/telegram/approve", "id=555&name=Roy");
  c.eqi(r.status, 200, "POST /api/telegram/approve -> 200");
  c.eqi((long)f.acc().pending().size(), 0, "approval drops them from the queue");
  r = f.call("GET", "/api/telegram");
  c.ok(has(r.body, "\"allow\":[{\"id\":\"555\",\"name\":\"Roy\",\"owner\":false}]") &&
           has(r.body, "\"public\":false"),
       "Roy is listed as approved, NOT as an admin (approval never grants owner rights)");
  c.eq(nimbus::orch::roleName(f.acc().roleOf("555")), "user", "an approved chat is a user");
  c.eqi(f.call("POST", "/api/tenant", "id=555&role=admin").status, 200,
        "the owner makes their own chat an admin explicitly (the role chip)");
  r = f.call("GET", "/api/tenant");
  c.ok(has(r.body, "{\"id\":\"555\",\"role\":\"admin\"") && has(r.body, "\"admins\":1"),
       "GET /api/tenant has Roy's RBAC row as admin");
  c.ok(has(f.call("GET", "/api/telegram").body, "{\"id\":\"555\",\"name\":\"Roy\",\"owner\":true}"),
       "and the chip now shows the admin");
  c.ok(has(f.call("GET", "/api/orch").body, "\"tgAllow\":\"555\""), "GET /api/orch reports tgAllow");
  f.tx.script.push_back(replyTurn("Hello Roy."));
  const auto st = f.route({text(2, "555", "Roy", "hello")}, 200, &sent);
  c.eqi(st.turns, 1, "the approved chat's message became a turn");
  c.ok(f.out.size() == 1 && f.out[0].first == "555" && f.out[0].second == "Hello Roy.",
       "the reply was delivered to the approved chat");
  c.eqi((long)sent.size(), 1, "no refusal was sent after approval (only the first knock's)");
  r = f.call("POST", "/api/telegram/add", "id=777&name=Sam%3A%2CSmith");
  c.eqi(r.status, 200, "POST /api/telegram/add by chat id -> 200");
  c.ok(has(f.call("GET", "/api/telegram").body, "{\"id\":\"777\",\"name\":\"Sam  Smith\",\"owner\":false}"),
       "a second approval is a member, and the name's delimiters are sanitized");
  c.eq(nimbus::orch::roleName(f.acc().roleOf("777")), "user", "the second approved chat is a user");
  c.eqi(f.call("POST", "/api/telegram/add", "id=@sam").status, 400, "a non-numeric chat id is refused");
  c.eqi(f.call("POST", "/api/telegram/add", "name=x").status, 400, "a missing id is refused");
}

// (d) --------------------------------------------------------------------------
static void testPersistsAcrossRestart(ndtest::Ctx& c) {
  std::printf("  -- (d) the allowlist and roles survive a restart --\n");
  clearEnv();
  {
    Fx f("tga-d");
    f.call("POST", "/api/telegram/approve", "id=555&name=Roy");
    f.call("POST", "/api/telegram/add", "id=777&name=Sam");
    c.eqi(f.call("POST", "/api/tenant", "id=555&role=admin").status, 200, "Roy is made the admin");
    c.eqi(f.call("POST", "/api/tenant", "id=777&role=guest").status, 200, "Sam is made a guest");
  }
  Fx g("tga-d", /*fresh=*/false);
  c.eq(g.acc().allowCsv(), "555,777", "the allowlist came back from the instance volume");
  c.ok(g.acc().admit("555") == Admit::Serve && g.acc().admit("777") == Admit::Serve,
       "both chats are still served after the restart");
  c.eq(nimbus::orch::roleName(g.acc().roleOf("555")), "admin", "Roy is still the admin");
  c.eq(nimbus::orch::roleName(g.acc().roleOf("777")), "guest", "Sam is still a guest");
  c.eq(g.acc().labelOf("777"), "Sam", "display names survived");
  c.ok(g.acc().admit("999") == Admit::Unlisted, "an unlisted chat is still refused");
  c.eqi((long)g.acc().pending().size(), 0, "the approval queue is RAM only (device parity)");
}

// (e) --------------------------------------------------------------------------
static void testEnvSeedOnce(ndtest::Ctx& c) {
  std::printf("  -- (e) NIMBUSD_TG_CHAT_ID is a one-time seed of an EMPTY allowlist --\n");
  clearEnv();
  setenv("NIMBUSD_TG_CHAT_ID", "555", 1);
  {
    Fx f("tga-e");
    c.eq(f.acc().allowCsv(), "555", "an empty allowlist is seeded from the env");
    c.eq(nimbus::orch::roleName(f.acc().roleOf("555")), "admin", "the seeded chat is the admin");
  }
  {
    Fx f("tga-e", false);
    c.eq(f.acc().allowCsv(), "555", "a restart with the same env does not seed again");
    f.call("POST", "/api/telegram/add", "id=777");
    std::string err;
    f.acc().remove("555", err);
  }
  {
    Fx f("tga-e", false);
    c.eq(f.acc().allowCsv(), "777", "a seed the owner removed does NOT come back on restart");
  }
  setenv("NIMBUSD_TG_CHAT_ID", "888", 1);
  {
    Fx f("tga-e", false);
    c.eq(f.acc().allowCsv(), "777", "a new seed never touches a list the owner manages");
  }
  // The seeded chat removed so the list is EMPTY again: a restart must not quietly
  // re-trust it (the seed is consumed, not a standing rule).
  setenv("NIMBUSD_TG_CHAT_ID", "444", 1);
  {
    Fx f("tga-e3");
    c.eq(f.acc().allowCsv(), "444", "a fresh volume takes the seed");
    std::string err;
    f.acc().remove("444", err);
    c.ok(f.acc().allowEmpty(), "the owner removes the seeded chat");
  }
  {
    Fx f("tga-e3", false);
    c.ok(f.acc().allowEmpty() && f.acc().admit("444") == Admit::Unlisted,
         "the removed seed is NOT re-added on restart, even onto an empty list");
  }
  setenv("NIMBUSD_TG_CHAT_ID", "not-a-chat", 1);
  {
    Fx f("tga-e2");
    c.ok(f.acc().allowEmpty(), "a malformed seed is ignored (still fail closed)");
  }
  clearEnv();
}

// (f) --------------------------------------------------------------------------
static void testRevoked(ndtest::Ctx& c) {
  std::printf("  -- (f) a revoked chat gets no turn and is told so; re-approval restores it --\n");
  clearEnv();
  Fx f("tga-f");
  f.call("POST", "/api/telegram/approve", "id=555&name=Roy");
  f.call("POST", "/api/telegram/approve", "id=666&name=Eve");
  c.eqi(f.call("POST", "/api/tenant", "id=666&role=unknown").status, 200, "the owner revokes Eve");
  c.ok(f.acc().admit("666") == Admit::Revoked, "Eve is on the list but revoked");
  std::vector<std::pair<std::string, std::string>> sent;
  const auto st = f.route({text(1, "666", "Eve", "read me the owner's notes")}, 100, &sent);
  c.eqi(st.turns, 0, "no turn for the revoked chat");
  c.eqi((long)f.tx.seen.size(), 0, "no provider call");
  c.ok(sent.size() == 1 && has(sent[0].second, "access to this assistant has been removed"),
       "the revoked chat is told its access was removed");
  c.eqi((long)f.acc().pending().size(), 0, "a revoked chat is not re-queued for approval");
  c.eqi(f.call("POST", "/api/telegram/add", "id=666").status, 200, "the owner approves Eve again");
  c.ok(f.acc().admit("666") == Admit::Serve, "an explicit re-approval lets her back in");
  c.eq(nimbus::orch::roleName(f.acc().roleOf("666")), "user", "as a user, not her old revoked row");
}

// (g) --------------------------------------------------------------------------
static void testOrchTgAllow(ndtest::Ctx& c) {
  std::printf("  -- (g) POST /api/orch tgAllow is honored, validated, and fails closed --\n");
  clearEnv();
  Fx f("tga-g");
  ApiResp r = f.call("POST", "/api/orch", "tgAllow=111%2C%20222%2C111");
  c.eqi(r.status, 200, "POST /api/orch tgAllow=<csv> -> 200");
  c.ok(has(f.call("GET", "/api/orch").body, "\"tgAllow\":\"111,222\""),
       "GET /api/orch reads it back trimmed and de-duplicated");
  c.ok(f.acc().admit("222") == Admit::Serve, "a listed chat is served at once (no restart)");
  r = f.call("POST", "/api/orch", "tgAllow=111%2Cabc");
  c.ok(r.status == 400 && has(r.body, "Chat IDs are numbers"), "a malformed id refuses the whole write");
  c.eq(f.acc().allowCsv(), "111,222", "and nothing changed");
  c.eqi(f.call("POST", "/api/orch", "tgAllow=").status, 200, "an empty tgAllow clears the list");
  c.ok(f.acc().admit("111") == Admit::Unlisted && f.acc().admit("222") == Admit::Unlisted,
       "an emptied allowlist refuses everyone (fail closed)");
}

// (h) --------------------------------------------------------------------------
static void testNoOpenAccess(ndtest::Ctx& c) {
  std::printf("  -- (h) open access is refused on a hosted instance --\n");
  clearEnv();
  Fx f("tga-h");
  ApiResp r = f.call("POST", "/api/telegram/public", "on=1");
  c.ok(r.status == 400 && has(r.body, "isn't available on a hosted instance"), "public on -> 400, says why");
  c.eqi(f.call("POST", "/api/telegram/public", "on=0").status, 200, "public off is always fine");
  c.ok(has(f.call("GET", "/api/telegram").body, "\"public\":false"), "GET reports no open access");
  c.ok(f.acc().admit("999") == Admit::Unlisted, "a stranger is still refused");
}

// (i) --------------------------------------------------------------------------
static Exchange embedExchange() {
  Exchange e;
  e.expectHost = "api.mistral.ai";
  e.expectPathContains = "/v1/embeddings";
  std::string v = "[1";
  for (int i = 1; i < 64; i++) v += ",0";
  e.body = "{\"data\":[{\"embedding\":" + v + "]}]}";
  return e;
}

static void testMemberRbac(ndtest::Ctx& c) {
  std::printf("  -- (i) an approved member runs as a member, not as the owner --\n");
  clearEnv();
  Fx f("tga-i", true, /*embeddings=*/true);
  f.call("POST", "/api/telegram/approve", "id=555&name=Roy");
  f.call("POST", "/api/telegram/approve", "id=666&name=Sam");
  f.call("POST", "/api/tenant", "id=555&role=admin");   // Roy is the owner's own chat
  const auto who = f.acc().principalFor("666");
  c.ok(!who.owner && who.role == nimbus::orch::Role::User && who.ns == "chat:666",
       "the member's principal is User with its own namespace");
  { orch::VecEntry e; e.id = "s1"; e.content = "OWNER-SECRET-FACT"; e.ttlHours = -1;
    e.vec.assign(64, 0); e.vec[0] = 127; f.rig->vectors().add(e); }
  { orch::VecEntry e; e.id = "s2"; e.content = "EXPIRED-OWNER-FACT"; e.ttlHours = 24; e.createdAtHours = 1;
    e.vec.assign(64, 0); e.vec[0] = 127; f.rig->vectors().add(e, /*dedup=*/false); }   // expired long ago
  f.tx.script = {embedExchange(), replyTurn("ok"), embedExchange(), replyTurn("ok")};
  f.rig->say("666", "what do you remember?");
  f.rig->say("555", "what do you remember?");
  c.eqi((long)f.tx.seen.size(), 4, "each turn embedded its message and ran one head turn");
  if (f.tx.seen.size() == 4) {
    const std::string& member = f.tx.seen[1].body;
    const std::string& owner = f.tx.seen[3].body;
    c.ok(!has(member, "OWNER-SECRET-FACT"), "the member's turn never recalls the owner's memory");
    c.ok(has(owner, "OWNER-SECRET-FACT"), "the owner's own turn does recall it");
    c.ok(!has(owner, "EXPIRED-OWNER-FACT"), "an expired memory is not recalled (query-time TTL)");
    c.ok(has(member, "role: user") && has(member, "NOT your owner"), "the member's prompt names them a user");
    c.ok(has(owner, "role: admin"), "the owner's prompt names them the admin");
  }
  const std::string list = f.rig->registry().handleRpc(
      R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"files.list","arguments":{}}})", who);
  c.ok(has(list, "belong to its owner"), "the owner's files are refused to a member");
  bool advertised = false;
  for (const auto& s : f.rig->registry().toolSpecsFor(who)) advertised = advertised || s.name == "files.read";
  c.ok(!advertised, "and not advertised to them");
}

// (j) --------------------------------------------------------------------------
static void testEngineSeamRechecks(ndtest::Ctx& c) {
  std::printf("  -- (j) the engine seam refuses a chat removed after its message queued --\n");
  clearEnv();
  Fx f("tga-j");
  f.call("POST", "/api/telegram/approve", "id=555&name=Roy");
  f.call("POST", "/api/telegram/approve", "id=666&name=Sam");
  c.eqi(f.call("POST", "/api/telegram/remove", "id=666").status, 200, "the owner removes Sam");
  f.rig->say("666", "queued before the removal");   // what the engine thread would run next
  f.rig->say("12345", "posted over /api/message for an unknown chat");
  c.eqi((long)f.tx.seen.size(), 0, "neither reached a provider");
  c.eqi((long)f.out.size(), 0, "and nothing was delivered");
  f.call("POST", "/api/tenant", "id=555&role=admin");
  c.eqi(f.call("POST", "/api/telegram/remove", "id=555").status, 200,
        "even the only admin chat can be removed (the web app is always the owner)");
  c.ok(f.acc().admit("555") == Admit::Unlisted, "and it is refused at once");
}

// (k) --------------------------------------------------------------------------
static void testTenantGuards(ndtest::Ctx& c) {
  std::printf("  -- (k) /api/tenant keeps the device's guards --\n");
  clearEnv();
  Fx f("tga-k");
  f.call("POST", "/api/telegram/approve", "id=555");
  f.call("POST", "/api/telegram/approve", "id=666");
  f.call("POST", "/api/tenant", "id=555&role=admin");
  c.eqi(f.call("POST", "/api/tenant", "id=999&role=admin").status, 409,
        "no role for a chat that was never approved (no pre-seeding)");
  c.eqi(f.call("POST", "/api/tenant", "id=555&role=user").status, 409, "the last admin cannot be demoted");
  c.eqi(f.call("POST", "/api/tenant", "id=666&remove=1").status, 409, "an approved chat's row is not removed");
  c.eqi(f.call("POST", "/api/tenant", "id=666&vectors=50").status, 404,
        "a limit needs a tenant row first (device parity: the role chip creates it)");
  c.eqi(f.call("POST", "/api/tenant", "id=666&role=user").status, 200, "the role chip writes the row");
  c.eqi(f.call("POST", "/api/tenant", "id=666&ttl=-1").status, 400, "a negative limit is refused");
  c.eqi(f.call("POST", "/api/tenant", "id=666&vectors=50").status, 200, "a limit is set");
  c.ok(has(f.call("GET", "/api/tenant").body, "{\"id\":\"666\",\"role\":\"user\",\"vectors\":50"),
       "and read back");
  c.eqi(f.call("POST", "/api/tenant", "id=666&role=wizard").status, 400, "an unknown role is refused");
  c.eqi(f.call("POST", "/api/tenant", "role=user").status, 400, "a missing id is refused");
  c.eqi(f.call("POST", "/api/tenant", "id=12%1F3&role=unknown").status, 400, "a non-numeric id is refused");
  c.eqi(f.call("POST", "/api/tenant", "id=666&role=guest&ttl=-1").status, 400,
        "a request with one bad limit is refused whole");
  c.eq(nimbus::orch::roleName(f.acc().roleOf("666")), "user", "and its role part was NOT applied");
  c.eqi(f.call("PUT", "/api/tenant", "id=666&role=guest").status, 405, "only GET and POST are routes");
  c.eqi(f.call("POST", "/api/telegram/role", "id=666&owner=1").status, 200,
        "the legacy owner flag route maps onto the real role");
  c.eq(nimbus::orch::roleName(f.acc().roleOf("666")), "admin", "and it made them an admin");
}

// (l) --------------------------------------------------------------------------
static void testNoRoleRevival(ndtest::Ctx& c) {
  std::printf("  -- (l) a role never outlives its chat's approval; one file, fail closed --\n");
  clearEnv();
  Fx f("tga-l");
  f.call("POST", "/api/telegram/approve", "id=555");
  f.call("POST", "/api/telegram/approve", "id=666");
  f.call("POST", "/api/tenant", "id=666&role=admin");
  c.eqi(f.call("POST", "/api/orch", "tgAllow=555").status, 200, "the raw tgAllow write drops 666");
  c.ok(!has(f.call("GET", "/api/tenant").body, "\"666\""), "and its admin row went with it");
  c.eqi(f.call("POST", "/api/telegram/add", "id=666").status, 200, "666 is approved again later");
  c.eq(nimbus::orch::roleName(f.acc().roleOf("666")), "user", "as a user: the old admin grant is NOT revived");
  std::string blob;
  c.ok(fsutil::readFile(f.acc().path(), blob) && has(blob, "tgAllow=555,666") && has(blob, "\ntenants="),
       "the allowlist and the roles live in ONE atomically written file");
  fsutil::writeFileAtomic(f.acc().path(), "tgAllow=555\ntenants=\x01garbage\n");
  TelegramAccess torn(ndtest::scratchDir("tga-l") + "/data/mem");
  c.eq(nimbus::orch::roleName(torn.roleOf("555")), "user", "a torn roles line never promotes anyone");
  fsutil::writeFileAtomic(f.acc().path(), "\x7f\x7f not a config \x7f");
  TelegramAccess junk(ndtest::scratchDir("tga-l") + "/data/mem");
  c.ok(junk.allowEmpty() && junk.admit("555") == Admit::Unlisted, "an unreadable file is an empty allowlist");
}

int main() {
  ndtest::Ctx c;
  c.suite = "telegram trust (device model)";
  std::printf("=== %s ===\n", c.suite);
  testFreshFailsClosed(c);
  testUnlistedRefused(c);
  testApproveThenServe(c);
  testPersistsAcrossRestart(c);
  testEnvSeedOnce(c);
  testRevoked(c);
  testOrchTgAllow(c);
  testNoOpenAccess(c);
  testMemberRbac(c);
  testEngineSeamRechecks(c);
  testTenantGuards(c);
  testNoRoleRevival(c);
  for (const char* t : {"tga-b", "tga-c", "tga-d", "tga-e", "tga-e2", "tga-e3", "tga-f", "tga-g", "tga-h",
                        "tga-i", "tga-j", "tga-k", "tga-l"})
    ndtest::rmTree(ndtest::scratchDir(t));
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
