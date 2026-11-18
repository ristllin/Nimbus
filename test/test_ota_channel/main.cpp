// RB-OTA2: the owner's switch for the update channel (nimbus/ota/ota_channel.h).
// The /ota command and the route GET/POST /api/ota/channel: owner only, and every
// failure ends on the stable channel.
#include <unity.h>

#include <cstring>
#include <string>

#include "nimbus/orch/command.h"
#include "nimbus/orch/rbac.h"
#include "nimbus/ota/ota_channel.h"

using namespace nimbus::ota;

// ---- a channel file that can fail at every step --------------------------------
namespace fake {
enum Fault : unsigned {
  WriteFails = 1u << 0,     // open or write returns false, nothing lands
  WriteShort = 1u << 1,     // returns false after landing a prefix of the text
  WriteLies = 1u << 2,      // returns true but the bytes did not land
  RemoveFails = 1u << 3,    // returns false, the file stays
  RemoveLies = 1u << 4,     // returns true, the file stays
  ReadFails = 1u << 5,      // the file cannot be opened: reads as stable
  kAllFaults = 1u << 6,
};
unsigned faults = 0;
std::string file;   // "" = missing
bool exists = false;
int writes = 0, removes = 0, reads = 0;

void reset(const char* initial) {
  faults = 0;
  exists = initial != nullptr;
  file = initial ? initial : "";
  writes = removes = reads = 0;
}
bool write(const char* t, size_t n) {
  writes++;
  if (faults & WriteFails) return false;
  if (faults & WriteShort) {
    exists = true;
    file.assign(t, n / 2);
    return false;
  }
  if (faults & WriteLies) return true;
  exists = true;
  file.assign(t, n);
  return true;
}
bool remove() {
  removes++;
  if (faults & RemoveFails) return !exists;
  if (faults & RemoveLies) return true;
  exists = false;
  file.clear();
  return true;
}
Channel read() {
  reads++;
  if (!exists || (faults & ReadFails)) return Channel::Stable;
  return parseChannel(file.data(), file.size());
}
const ChannelFile kFile{&write, &remove, &read};
}  // namespace fake

void setUp() { fake::reset(nullptr); }
void tearDown() {}

// ---- the ask ---------------------------------------------------------------------
static void test_parse_ask() {
  auto ask = [](const char* s) { return parseChannelAsk(s, std::strlen(s)); };
  TEST_ASSERT_TRUE(ChannelAsk::Show == ask(""));
  TEST_ASSERT_TRUE(ChannelAsk::Show == ask("   "));
  TEST_ASSERT_TRUE(ChannelAsk::Show == parseChannelAsk(nullptr, 0));
  TEST_ASSERT_TRUE(ChannelAsk::Candidate == ask("candidate"));
  TEST_ASSERT_TRUE(ChannelAsk::Candidate == ask(" Candidate\n"));
  TEST_ASSERT_TRUE(ChannelAsk::Stable == ask("stable"));
  TEST_ASSERT_TRUE(ChannelAsk::Stable == ask("STABLE"));
  for (const char* bad : {"cand", "candidates", "beta", "stable now", "candidate x", "on", "1"})
    TEST_ASSERT_TRUE_MESSAGE(ChannelAsk::Unknown == ask(bad), bad);
  // An embedded NUL is not the word (the route hands the body's bytes with their length).
  TEST_ASSERT_TRUE(ChannelAsk::Unknown == parseChannelAsk("candidate\0x", 11));
  TEST_ASSERT_TRUE(ChannelAsk::Unknown == parseChannelAsk("stable\0", 7));
}

// ---- the set ---------------------------------------------------------------------
static void test_set_candidate_and_back() {
  bool latch = false;
  TEST_ASSERT_TRUE(applyChannel(fake::kFile, Channel::Candidate, latch));
  TEST_ASSERT_TRUE(Channel::Candidate == channelInForce(fake::kFile, latch));
  TEST_ASSERT_EQUAL_STRING("candidate\n", fake::file.c_str());
  TEST_ASSERT_TRUE(applyChannel(fake::kFile, Channel::Stable, latch));
  TEST_ASSERT_TRUE(Channel::Stable == channelInForce(fake::kFile, latch));
  TEST_ASSERT_FALSE(fake::exists);   // stable is the missing file, as RB-OTA1 reads it
  TEST_ASSERT_FALSE(latch);
  // Stable on a device with no file: nothing to remove, still a success.
  TEST_ASSERT_TRUE(applyChannel(fake::kFile, Channel::Stable, latch));
}

// The class rule: for EVERY combination of file faults and every starting state, a
// switch that reports failure leaves the stable channel in force, and a switch that
// reports success leaves exactly the channel asked for.
static void test_every_failure_ends_on_stable() {
  int failures = 0;
  for (unsigned mask = 0; mask < fake::kAllFaults; mask++) {
    for (const char* start : {(const char*)nullptr, "candidate\n", "stable\n", "junk"}) {
      for (Channel want : {Channel::Stable, Channel::Candidate}) {
        for (bool latchBefore : {false, true}) {
          fake::reset(start);
          fake::faults = mask;
          bool latch = latchBefore;
          const bool ok = applyChannel(fake::kFile, want, latch);
          const Channel now = channelInForce(fake::kFile, latch);
          char msg[96];
          std::snprintf(msg, sizeof msg, "mask=%u start=%s want=%s latch=%d", mask,
                        start ? start : "(none)", channelSlug(want), (int)latchBefore);
          if (ok) {
            TEST_ASSERT_TRUE_MESSAGE(now == want, msg);
            TEST_ASSERT_FALSE_MESSAGE(latch, msg);
          } else {
            failures++;
            TEST_ASSERT_TRUE_MESSAGE(now == Channel::Stable, msg);
          }
          // The update check reads the same rule: never candidate after a failed switch.
          if (!ok) TEST_ASSERT_TRUE_MESSAGE(channelInForce(fake::kFile, latch) == Channel::Stable, msg);
        }
      }
    }
  }
  TEST_ASSERT_TRUE(failures > 100);   // the sweep did reach the failure paths
}

// A failed candidate set that could clear the file needs no latch: the file says stable
// after a restart too.
static void test_failed_candidate_clears_the_file() {
  bool latch = false;
  fake::faults = fake::WriteShort;   // "cand" landed
  TEST_ASSERT_FALSE(applyChannel(fake::kFile, Channel::Candidate, latch));
  TEST_ASSERT_FALSE(fake::exists);
  TEST_ASSERT_FALSE(latch);
  // A file that lies about the write and cannot be removed: latched.
  fake::reset("candidate\n");
  fake::faults = fake::WriteLies | fake::RemoveFails;
  TEST_ASSERT_FALSE(applyChannel(fake::kFile, Channel::Stable, latch));
  TEST_ASSERT_TRUE(latch);
  TEST_ASSERT_TRUE(Channel::Stable == channelInForce(fake::kFile, latch));
  // The next successful set lifts the latch.
  fake::faults = 0;
  TEST_ASSERT_TRUE(applyChannel(fake::kFile, Channel::Candidate, latch));
  TEST_ASSERT_FALSE(latch);
  TEST_ASSERT_TRUE(Channel::Candidate == channelInForce(fake::kFile, latch));
}

// A stable set whose remove fails falls back to overwriting the file with "stable".
static void test_stable_overwrites_when_remove_fails() {
  fake::reset("candidate\n");
  fake::faults = fake::RemoveFails;
  bool latch = false;
  TEST_ASSERT_TRUE(applyChannel(fake::kFile, Channel::Stable, latch));
  TEST_ASSERT_FALSE(latch);
  TEST_ASSERT_TRUE(Channel::Stable == fake::read());
}

// ---- the line --------------------------------------------------------------------
static void test_every_line_is_plain_and_true() {
  for (ChannelAsk a : {ChannelAsk::Show, ChannelAsk::Stable, ChannelAsk::Candidate, ChannelAsk::Unknown}) {
    for (bool ok : {false, true}) {
      for (Channel c : {Channel::Stable, Channel::Candidate}) {
        const char* line = channelLine(a, ok, c);
        TEST_ASSERT_NOT_NULL(line);
        const std::string s = line;
        TEST_ASSERT_TRUE(s.size() > 10 && s.size() < 160);
        TEST_ASSERT_TRUE(s.find('\n') == std::string::npos);    // one line
        for (unsigned char ch : s) TEST_ASSERT_TRUE(ch >= 0x20 && ch < 0x7f && ch != '"' && ch != '\\');
        TEST_ASSERT_TRUE(s.find('!') == std::string::npos);
        TEST_ASSERT_TRUE(s.find(" I ") == std::string::npos && s.find("I'm") == std::string::npos);
        if (a == ChannelAsk::Unknown) continue;   // the usage line
        // The line names the channel in force, never the other one as in force.
        TEST_ASSERT_TRUE_MESSAGE(s.find(channelSlug(c)) != std::string::npos, line);
        if (!ok) TEST_ASSERT_TRUE_MESSAGE(s.find("ouldn't") != std::string::npos, line);
      }
    }
  }
  TEST_ASSERT_TRUE(std::string(channelLine(ChannelAsk::Unknown, false, Channel::Stable)).find("/ota candidate") !=
                   std::string::npos);
}

// ---- the command ------------------------------------------------------------------
namespace dev {
Channel ch = Channel::Stable;
bool setOk = true;
int sets = 0, gets = 0;
Channel lastSet = Channel::Stable;
bool set(Channel c) {
  sets++;
  lastSet = c;
  if (setOk) ch = c;
  else ch = Channel::Stable;   // the device's set fails closed (proved above)
  return setOk;
}
Channel get() {
  gets++;
  return ch;
}
const ChannelOps kOps{&set, &get};
void reset(Channel c = Channel::Stable, bool ok = true) {
  ch = c;
  setOk = ok;
  sets = gets = 0;
}
}  // namespace dev

static void test_command_reply() {
  dev::reset();
  std::string r = channelCommandReply("", dev::kOps);
  TEST_ASSERT_EQUAL_INT(0, dev::sets);
  TEST_ASSERT_EQUAL_STRING(channelLine(ChannelAsk::Show, true, Channel::Stable), r.c_str());

  r = channelCommandReply("candidate", dev::kOps);
  TEST_ASSERT_EQUAL_INT(1, dev::sets);
  TEST_ASSERT_TRUE(dev::lastSet == Channel::Candidate);
  TEST_ASSERT_EQUAL_STRING(channelLine(ChannelAsk::Candidate, true, Channel::Candidate), r.c_str());

  r = channelCommandReply("Stable", dev::kOps);
  TEST_ASSERT_TRUE(dev::lastSet == Channel::Stable);
  TEST_ASSERT_EQUAL_STRING(channelLine(ChannelAsk::Stable, true, Channel::Stable), r.c_str());

  dev::reset(Channel::Candidate);
  r = channelCommandReply("beta", dev::kOps);
  TEST_ASSERT_EQUAL_INT(0, dev::sets);   // an unknown word changes nothing
  TEST_ASSERT_TRUE(dev::ch == Channel::Candidate);
  TEST_ASSERT_EQUAL_STRING(channelLine(ChannelAsk::Unknown, false, Channel::Candidate), r.c_str());

  dev::reset(Channel::Stable, /*ok=*/false);
  r = channelCommandReply("candidate", dev::kOps);
  TEST_ASSERT_TRUE(dev::ch == Channel::Stable);
  TEST_ASSERT_EQUAL_STRING(channelLine(ChannelAsk::Candidate, false, Channel::Stable), r.c_str());
}

// /ota is in the verb table and owner-only: the dispatcher's one gate
// (nimbus::orch::isOwnerVerb, then mayRunOwnerCommands on the chat's role) answers
// a member or guest with the standard refusal before any handler runs.
static void test_command_is_owner_only() {
  const nimbus::orch::Command c = nimbus::orch::parseCommand("/ota@Example_bot candidate");
  TEST_ASSERT_TRUE(c.isCommand);
  TEST_ASSERT_EQUAL_STRING("ota", c.verb.c_str());
  TEST_ASSERT_EQUAL_STRING("candidate", c.args.c_str());
  TEST_ASSERT_NOT_NULL(nimbus::orch::findCommandVerb("ota"));
  TEST_ASSERT_TRUE(nimbus::orch::isOwnerVerb("ota"));
  using nimbus::orch::Role;
  TEST_ASSERT_FALSE(nimbus::orch::mayRunOwnerCommands(Role::Unknown));
  TEST_ASSERT_FALSE(nimbus::orch::mayRunOwnerCommands(Role::Guest));
  TEST_ASSERT_FALSE(nimbus::orch::mayRunOwnerCommands(Role::User));
  TEST_ASSERT_TRUE(nimbus::orch::mayRunOwnerCommands(Role::Admin));
  // A forwarded "/ota candidate" never runs as the owner's command.
  TEST_ASSERT_FALSE(nimbus::orch::isCommandText(nimbus::orch::forwardedText("/ota candidate")));
}

// ---- the route --------------------------------------------------------------------
static void test_route_answers() {
  dev::reset();
  RouteReply g = channelRoute(true, false, nullptr, dev::kOps);
  TEST_ASSERT_EQUAL_INT(200, g.status);
  TEST_ASSERT_EQUAL_INT(0, dev::sets);
  TEST_ASSERT_TRUE(g.body.find("\"channel\":\"stable\"") != std::string::npos);
  TEST_ASSERT_TRUE(g.body.find(channelLine(ChannelAsk::Show, true, Channel::Stable)) != std::string::npos);

  RouteReply p = channelRoute(true, true, "candidate", dev::kOps);
  TEST_ASSERT_EQUAL_INT(200, p.status);
  TEST_ASSERT_EQUAL_INT(1, dev::sets);
  TEST_ASSERT_EQUAL_STRING(
      (std::string("{\"ok\":true,\"channel\":\"candidate\",\"line\":\"") +
       channelLine(ChannelAsk::Candidate, true, Channel::Candidate) + "\"}").c_str(),
      p.body.c_str());
  // GET shows it.
  g = channelRoute(true, false, nullptr, dev::kOps);
  TEST_ASSERT_TRUE(g.body.find("\"channel\":\"candidate\"") != std::string::npos);

  // A GET never sets, whatever it carries.
  const int before = dev::sets;
  g = channelRoute(true, false, "stable", dev::kOps);
  TEST_ASSERT_EQUAL_INT(before, dev::sets);
  TEST_ASSERT_TRUE(dev::ch == Channel::Candidate);

  // The failed switch: 500, the channel in force is stable.
  dev::reset(Channel::Candidate, /*ok=*/false);
  p = channelRoute(true, true, "candidate", dev::kOps);
  TEST_ASSERT_EQUAL_INT(500, p.status);
  TEST_ASSERT_TRUE(p.body.find("\"ok\":false") != std::string::npos);
  TEST_ASSERT_TRUE(p.body.find("\"channel\":\"stable\"") != std::string::npos);
}

// The class rule for the route: the channel is set iff the request is authenticated,
// a POST, and names a channel; an unauthenticated request reads nothing either.
static void test_route_sets_only_on_owner_post() {
  const char* values[] = {nullptr, "", "candidate", "stable", "Candidate", "beta", "candidate x"};
  for (bool authed : {false, true}) {
    for (bool post : {false, true}) {
      for (const char* v : values) {
        dev::reset(Channel::Stable);
        const RouteReply r = channelRoute(authed, post, v, dev::kOps);
        const ChannelAsk a = v ? parseChannelAsk(v, std::strlen(v)) : ChannelAsk::Unknown;
        const bool named = a == ChannelAsk::Stable || a == ChannelAsk::Candidate;
        char msg[64];
        std::snprintf(msg, sizeof msg, "authed=%d post=%d v=%s", (int)authed, (int)post, v ? v : "(none)");
        TEST_ASSERT_EQUAL_INT_MESSAGE((authed && post && named) ? 1 : 0, dev::sets, msg);
        if (!authed) {
          TEST_ASSERT_EQUAL_INT_MESSAGE(401, r.status, msg);
          TEST_ASSERT_EQUAL_INT_MESSAGE(0, dev::gets, msg);
          TEST_ASSERT_EQUAL_STRING_MESSAGE(kAuthRequiredJson, r.body.c_str(), msg);
          continue;
        }
        if (!post) TEST_ASSERT_EQUAL_INT_MESSAGE(200, r.status, msg);
        else TEST_ASSERT_EQUAL_INT_MESSAGE(named ? 200 : 400, r.status, msg);
        TEST_ASSERT_TRUE_MESSAGE(r.body.front() == '{' && r.body.back() == '}', msg);
      }
    }
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_ask);
  RUN_TEST(test_set_candidate_and_back);
  RUN_TEST(test_every_failure_ends_on_stable);
  RUN_TEST(test_failed_candidate_clears_the_file);
  RUN_TEST(test_stable_overwrites_when_remove_fails);
  RUN_TEST(test_every_line_is_plain_and_true);
  RUN_TEST(test_command_reply);
  RUN_TEST(test_command_is_owner_only);
  RUN_TEST(test_route_answers);
  RUN_TEST(test_route_sets_only_on_owner_post);
  return UNITY_END();
}
