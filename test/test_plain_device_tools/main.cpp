// test_plain_device_tools - engine 2's device, people, models and docs groups (lane E2-P-TOOLS,
// src/agent/tools_plain_device.h) over the device's one-chat binding (chat_binding.h) with a scripted
// Claude head on the real Anthropic wire and a fake card. The handlers are fakes standing for engine
// 1's (the device binds main.cpp's and memory_subsystem.cpp's); the device_control applier is the
// shared one (device_apply.cpp) over fake executors. One test per family:
//   - device: device_status, device_log, ota_status, device_selftest, system_health are listed in the
//     device group behind more and reached through it;
//   - device_control: a volume change applies inside the turn; a restart and a head switch are not
//     run inside it but come back as the turn's deferred effects (run after the reply is sent); a
//     member is refused;
//   - people: the tenant tools reach an admin and are neither listed nor callable for a member;
//   - models: models_list reaches its handler with the model's arguments;
//   - docs: docs_search and docs_read reach their handlers.
#include <unity.h>

#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../../src/agent/tools_plain_device.h"
#include "../support/scripted_provider/scripted_provider.h"
#include "nimbus/harness/plain/chat_binding.h"
#include "nimbus/orch/tool_groups.h"
#include "nimbus/orch/tx/card_files.h"

namespace plain = agent::plain;
namespace pd = agent::plaindevice;
namespace sp = harness_test::scripted;
namespace tx = nimbus::orch::tx;
namespace orch = nimbus::orch;

namespace {

constexpr uint64_t kEpoch0 = 1794920520ULL;   // Tue 17 Nov 2026 13:02 UTC

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// A card that keeps files and folders.
class Card : public tx::CardOps {
 public:
  std::map<std::string, std::string> files;
  std::set<std::string> dirs{"/"};
  bool present() override { return true; }
  bool mayWrite() override { return true; }
  uint32_t largestInternal() override { return 64 * 1024; }
  uint32_t nowMs() override { return 0; }
  void lock() override {}
  void unlock() override {}
  bool exists(const std::string& p) override { return files.count(p) || dirs.count(p); }
  bool isDir(const std::string& p) override { return dirs.count(p) > 0; }
  bool mkdir(const std::string& p) override { return dirs.insert(p).second; }
  size_t append(const std::string& p, const char* d, size_t n) override {
    files[p].append(d, n);
    return n;
  }
  bool readAt(const std::string& p, size_t off, char* buf, size_t want, size_t& got) override {
    got = 0;
    auto it = files.find(p);
    if (it == files.end()) return false;
    if (off < it->second.size()) got = it->second.copy(buf, want, off);
    return true;
  }
  bool fileSize(const std::string& p, size_t& out) override {
    auto it = files.find(p);
    if (it == files.end()) return false;
    out = it->second.size();
    return true;
  }
  size_t writeWhole(const std::string& p, const char* d, size_t n) override {
    files[p].assign(d, n);
    return n;
  }
  bool rename(const std::string& a, const std::string& b) override {
    if (!files.count(a) || files.count(b)) return false;
    files[b] = files[a];
    files.erase(a);
    return true;
  }
  bool remove(const std::string& p) override { return files.erase(p) > 0; }
  bool listFiles(const std::string& dir, std::vector<std::string>& names) override {
    const std::string pre = dir + "/";
    for (const auto& kv : files)
      if (kv.first.rfind(pre, 0) == 0 && kv.first.find('/', pre.size()) == std::string::npos)
        names.push_back(kv.first.substr(pre.size()));
    return true;
  }
  bool listNames(const std::string& dir, std::vector<std::string>& names) override {
    names.clear();
    if (!dirs.count(dir)) return false;
    const std::string pre = dir == "/" ? std::string("/") : dir + "/";
    auto child = [&](const std::string& p) {
      if (p.size() > pre.size() && p.rfind(pre, 0) == 0 && p.find('/', pre.size()) == std::string::npos)
        names.push_back(p.substr(pre.size()));
    };
    for (const auto& kv : files) child(kv.first);
    for (const std::string& d : dirs) child(d);
    return true;
  }
};

constexpr char kChat[] = "1001";

// The fake handlers: each answers with its name and the arguments it got, and counts its calls.
struct Fakes {
  std::map<std::string, int> calls;
  std::map<std::string, std::string> args;
  pd::Handler named(const std::string& n) {
    return [this, n](ArduinoJson::JsonObjectConst a, const orch::Principal&) {
      calls[n]++;
      std::string s;
      serializeJson(a, s);
      args[n] = s;
      return orch::ToolResult::ok("fake " + n + " " + s);
    };
  }
  pd::Handlers handlers() {
    pd::Handlers h;
    h.status = named("status");
    h.log = named("log");
    h.ota = named("ota");
    h.selftest = named("selftest");
    h.health = named("health");
    h.docsList = named("docsList");
    h.docsSearch = named("docsSearch");
    h.docsRead = named("docsRead");
    h.tenantList = named("tenantList");
    h.tenantSetRole = named("tenantSetRole");
    h.tenantSetQuota = named("tenantSetQuota");
    h.modelsList = named("modelsList");
    return h;
  }
};

// The executors device_control gets: what reached them.
struct Executed {
  int configs = 0, staged = 0, switches = 0;
  int sfxVol = -1;
};

struct Device {
  explicit Device(orch::Role role_ = orch::Role::Admin) : files(card), role(role_) {
    platform.nowMs = [this] { return ms; };
    platform.nowEpoch = [this] { return kEpoch0 + ms / 1000u; };
    platform.delayMs = [this](uint32_t n) { ms += n; };
    platform.freeHeap = [] { return 200000u; };
    platform.allocLarge = [](size_t n) { return std::malloc(n); };
    platform.freeLarge = [](void* p) { std::free(p); };
    model.clock = &ms;
    pd::ControlDeps c;
    c.unattended = [] { return false; };
    c.chat = [] { return std::string(kChat); };
    c.apply = [this] {
      agent::DeviceApplyDeps d;
      d.execConfig = [this](const orch::ValidatedAction& va) {
        exec.configs++;
        if (va.hasSfxVol) exec.sfxVol = va.sfxVol;
        return true;
      };
      d.stageDevice = [this](const orch::ValidatedAction&) { exec.staged++; };
      d.modelIsValid = [](const std::string& p, const std::string& m) { return p == "anthropic" && m == "claude-haiku-4-5"; };
      d.providerHasKey = [](const std::string& p) { return p == "anthropic"; };
      d.setOrchHostModel = [this](const std::string&, const std::string&) {
        exec.switches++;
        return true;
      };
      return d;
    };
    pd::registerDeviceFamilies(reg, fakes.handlers(), c);
    orch::registerMore(reg);
  }

  plain::Status boot() {
    plain::ChatBindingConfig c;
    c.provider = "anthropic";   // E2-R8a: the head slot has no default
    c.host = "api.anthropic.com";
    c.chatKey = kChat;
    c.chatId = kChat;
    c.model = "claude-haiku-5-5";
    c.summarizerModel = "claude-haiku-5-5";
    c.timeZone = "Europe/London";
    c.role = role;
    plain::ChatBindingDeps d;
    d.platform = &platform;
    d.transport = &model;
    d.files = &files;
    d.registry = &reg;
    d.sendPart = [this](const std::string& chat, const std::string& part) {
      sent.push_back(chat + "|" + part);
      return true;
    };
    d.key = [] { return std::string("sk-fake-binding"); };
    binding.reset(new plain::ChatBinding(c, d));
    return binding->boot();
  }

  plain::Status say(const std::string& text) {
    plain::ChatMessage m;
    m.text = text;
    out = plain::TurnOutcome{};
    return binding->turn(m, out);
  }

  // The tool result the head read in its last request.
  const std::string& lastBody() const { return model.last().body; }

  uint32_t ms = 1000;
  Card card;
  agent::Platform platform;
  sp::ScriptedProvider model;
  tx::CardTxFiles files;
  orch::ToolRegistry reg;
  orch::Role role;
  Fakes fakes;
  Executed exec;
  std::unique_ptr<plain::ChatBinding> binding;
  std::vector<std::string> sent;
  plain::TurnOutcome out;
};


// One more call in a turn of its own; the head's next request carries the result.
std::string moreCall(Device& dev, const std::string& argsJson) {
  dev.model.push(sp::call("more", argsJson));
  dev.model.push(sp::say("Done."));
  TEST_ASSERT_TRUE(dev.say("go").ok());
  return dev.lastBody();
}

}  // namespace

void setUp() {}
void tearDown() {}

// The device group: listed behind more and reached through it, engine 1's handlers answering.
static void test_device_family_is_behind_more() {
  Device dev;
  TEST_ASSERT_TRUE(dev.boot().ok());
  const std::string listing = moreCall(dev, R"({"group":"device"})");
  for (const char* n : {"device_status(", "device_log(", "ota_status(", "device_selftest(", "system_health(", "device_control("})
    TEST_ASSERT_TRUE_MESSAGE(has(listing, n), n);
  TEST_ASSERT_TRUE(has(moreCall(dev, R"({"name":"device_status"})"), "fake status"));
  TEST_ASSERT_TRUE(has(moreCall(dev, R"({"name":"device_log","args":{"match":"nvs","lines":5}})"), "fake log"));
  TEST_ASSERT_EQUAL_STRING(R"({"match":"nvs","lines":5})", dev.fakes.args["log"].c_str());
  TEST_ASSERT_TRUE(has(moreCall(dev, R"({"name":"ota_status"})"), "fake ota"));
  TEST_ASSERT_TRUE(has(moreCall(dev, R"({"name":"device_selftest","args":{"audible":false}})"), "fake selftest"));
  TEST_ASSERT_TRUE(has(moreCall(dev, R"({"name":"system_health"})"), "fake health"));
  TEST_ASSERT_EQUAL(1, dev.fakes.calls["status"]);
  TEST_ASSERT_EQUAL(1, dev.fakes.calls["health"]);
  TEST_ASSERT_EQUAL(0, (int)dev.out.deferred.mask);   // a read asks for nothing after the turn
}

// device_control: a setting now; a restart and a head switch only after the reply is sent.
static void test_device_control_applies_now_and_defers_restart_and_switch() {
  Device dev;
  TEST_ASSERT_TRUE(dev.boot().ok());
  const std::string vol = moreCall(dev, R"({"name":"device_control","args":{"action":{"type":"config","sfxVol":10}}})");
  TEST_ASSERT_TRUE_MESSAGE(has(vol, "applied: cfg"), vol.c_str());
  TEST_ASSERT_EQUAL(10, dev.exec.sfxVol);
  TEST_ASSERT_EQUAL(0, (int)dev.out.deferred.mask);

  const std::string rb = moreCall(dev, R"({"name":"device_control","args":{"action":{"type":"reboot"}}})");
  TEST_ASSERT_TRUE_MESSAGE(has(rb, "applied: reboot") && has(rb, "runs after this reply is sent"), rb.c_str());
  TEST_ASSERT_EQUAL(0, dev.exec.staged);   // never staged inside the turn
  TEST_ASSERT_TRUE(dev.out.deferred.has(plain::Deferred::Restart));
  TEST_ASSERT_EQUAL_STRING("Done.", dev.out.text.c_str());   // the reply still goes first

  moreCall(dev, R"({"name":"device_control","args":{"action":{"type":"orch_model","provider":"anthropic","model":"claude-haiku-4-5"}}})");
  TEST_ASSERT_TRUE(dev.out.deferred.has(plain::Deferred::HeadSwitch));
  TEST_ASSERT_EQUAL_STRING("anthropic/claude-haiku-4-5",
                           dev.out.deferred.arg[static_cast<size_t>(plain::Deferred::HeadSwitch)].c_str());
  TEST_ASSERT_EQUAL(0, dev.exec.switches);   // written after the turn by the device, not here

  const std::string bad = moreCall(dev, R"({"name":"device_control","args":{"action":{"type":"orch_model","provider":"anthropic","model":"no-such-model"}}})");
  TEST_ASSERT_TRUE_MESSAGE(has(bad, "orch_model(invalid)"), bad.c_str());
  TEST_ASSERT_EQUAL(0, (int)dev.out.deferred.mask);
}

// E2-REP1 (E2-INT1's finding): the switch after the turn reports what the store did. A refused write
// is not a switch, so the chat is never told a model answers now that does not.
static void test_a_head_switch_the_store_refuses_is_not_reported_done() {
  agent::DeviceApplyDeps d;
  std::vector<std::string> written;
  bool stores = true;
  d.setOrchHostModel = [&](const std::string& p, const std::string& m) {
    written.push_back(p + "/" + m);
    return stores;
  };
  TEST_ASSERT_TRUE(pd::switchHeadWith("openai/gpt-5.6-luna", d));
  stores = false;   // NVS full: the host or the model write refused
  TEST_ASSERT_FALSE(pd::switchHeadWith("openai/gpt-5.6-luna", d));
  TEST_ASSERT_EQUAL(2, (int)written.size());
  TEST_ASSERT_FALSE(pd::switchHeadWith("no-slash", d));   // no write for another shape
  TEST_ASSERT_EQUAL(2, (int)written.size());
  TEST_ASSERT_FALSE(pd::switchHeadWith("openai/gpt-5.6-luna", agent::DeviceApplyDeps{}));
}

static void test_device_control_refuses_a_member() {
  Device dev(orch::Role::User);
  TEST_ASSERT_TRUE(dev.boot().ok());
  const std::string r = moreCall(dev, R"({"name":"device_control","args":{"action":{"type":"reboot"}}})");
  TEST_ASSERT_FALSE(has(r, "applied: reboot"));
  TEST_ASSERT_EQUAL(0, (int)dev.out.deferred.mask);
  TEST_ASSERT_EQUAL(0, dev.exec.staged);
}

// people: the admin manages roles by conversation; a member sees no people tool and cannot call one.
static void test_people_family_is_admin_only() {
  Device owner;
  TEST_ASSERT_TRUE(owner.boot().ok());
  TEST_ASSERT_TRUE(has(moreCall(owner, R"({"group":"people"})"), "tenant_set_role("));
  const std::string set = moreCall(owner, R"({"name":"tenant_set_role","args":{"chat":"2002","role":"guest"}})");
  TEST_ASSERT_TRUE_MESSAGE(has(set, "fake tenantSetRole"), set.c_str());
  TEST_ASSERT_TRUE(has(moreCall(owner, R"({"name":"tenant_list"})"), "fake tenantList"));

  Device member(orch::Role::User);
  TEST_ASSERT_TRUE(member.boot().ok());
  TEST_ASSERT_FALSE(has(moreCall(member, R"({"group":"people"})"), "tenant_"));
  moreCall(member, R"({"name":"tenant_set_role","args":{"chat":"2002","role":"admin"}})");
  TEST_ASSERT_EQUAL(0, member.fakes.calls["tenantSetRole"]);
}

static void test_models_family_reaches_models_list() {
  Device dev;
  TEST_ASSERT_TRUE(dev.boot().ok());
  TEST_ASSERT_TRUE(has(moreCall(dev, R"({"group":"models"})"), "models_list("));
  TEST_ASSERT_TRUE(has(moreCall(dev, R"({"name":"models_list","args":{"provider":"anthropic","query":"haiku"}})"), "fake modelsList"));
  TEST_ASSERT_EQUAL_STRING(R"({"provider":"anthropic","query":"haiku"})", dev.fakes.args["modelsList"].c_str());
}

static void test_docs_family_reaches_search_and_read() {
  Device dev;
  TEST_ASSERT_TRUE(dev.boot().ok());
  TEST_ASSERT_TRUE(has(moreCall(dev, R"({"group":"docs"})"), "docs_search("));
  TEST_ASSERT_TRUE(has(moreCall(dev, R"({"name":"docs_search","args":{"query":"volume"}})"), "fake docsSearch"));
  TEST_ASSERT_TRUE(has(moreCall(dev, R"({"name":"docs_read","args":{"id":"sound"}})"), "fake docsRead"));
  TEST_ASSERT_TRUE(has(moreCall(dev, R"({"name":"docs_list"})"), "fake docsList"));
}

// The deferral code: what a tool asks for and nothing else.
static void test_deferred_code_round_trip() {
  plain::Deferred d = plain::Deferred::None;
  std::string arg;
  TEST_ASSERT_TRUE(plain::deferredFromCode(plain::deferredCode(plain::Deferred::Restart), d, arg));
  TEST_ASSERT_TRUE(d == plain::Deferred::Restart && arg.empty());
  TEST_ASSERT_TRUE(plain::deferredFromCode(plain::deferredCode(plain::Deferred::HeadSwitch, "anthropic/x"), d, arg));
  TEST_ASSERT_TRUE(d == plain::Deferred::HeadSwitch && arg == "anthropic/x");
  for (const char* c : {"", "stored", "deferred:", "deferred:0", "deferred:9", "deferred:3x"})
    TEST_ASSERT_FALSE_MESSAGE(plain::deferredFromCode(c, d, arg), c);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_device_family_is_behind_more);
  RUN_TEST(test_device_control_applies_now_and_defers_restart_and_switch);
  RUN_TEST(test_device_control_refuses_a_member);
  RUN_TEST(test_people_family_is_admin_only);
  RUN_TEST(test_models_family_reaches_models_list);
  RUN_TEST(test_docs_family_reaches_search_and_read);
  RUN_TEST(test_deferred_code_round_trip);
  RUN_TEST(test_a_head_switch_the_store_refuses_is_not_reported_done);
  return UNITY_END();
}
