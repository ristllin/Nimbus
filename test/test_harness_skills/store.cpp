// Skills the assistant uses and edits: the portable SkillStore rules, the attach
// block and the owner's /skill commands (UNIFIED 4.4 T1-T10). Registered from
// main.cpp's runner (runStoreTests).
#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../support/utf8_check.h"
#include "nimbus/harness/builtin_skills.h"
#include "nimbus/harness/skill_attach.h"
#include "nimbus/harness/skill_md.h"
#include "nimbus/harness/skill_store.h"
#include "nimbus/orch/command.h"
#include "nimbus/orch/msg_batch.h"

using agent::SkillCaller;
using agent::SkillMd;
using agent::SkillOutcome;
using agent::SkillResult;
using agent::SkillStore;

namespace {

// ---- an in-memory SkillFs ------------------------------------------------------

struct MemFs : agent::SkillFs {
  std::map<std::string, std::string> files;
  int writes = 0;
  bool failWrites = false;
  bool read(const std::string& path, size_t cap, std::string& out) override {
    auto it = files.find(path);
    if (it == files.end()) return false;
    out = it->second.substr(0, cap);
    return true;
  }
  bool writeAtomic(const std::string& path, const std::string& text) override {
    if (failWrites) return false;
    files[path] = text;
    writes++;
    return true;
  }
  std::vector<std::string> listDirs(const std::string& dir) override {
    std::set<std::string> names;
    const std::string pre = dir + "/";
    for (const auto& kv : files) {
      if (kv.first.compare(0, pre.size(), pre) != 0) continue;
      const size_t slash = kv.first.find('/', pre.size());
      if (slash != std::string::npos) names.insert(kv.first.substr(pre.size(), slash - pre.size()));
    }
    return std::vector<std::string>(names.begin(), names.end());
  }
  bool exists(const std::string& path) override { return files.count(path) > 0; }
  bool remove(const std::string& path) override { return files.erase(path) > 0; }
  bool rename(const std::string& from, const std::string& to) override {
    auto it = files.find(from);
    if (it == files.end()) return false;
    files[to] = it->second;
    files.erase(it);
    return true;
  }
  void removeDir(const std::string&) override {}
};

const std::string kRoot = "/mem/skills";
std::string livePath(const std::string& id) { return kRoot + "/" + id + "/SKILL.md"; }
std::string nextPath(const std::string& id) { return kRoot + "/" + id + "/SKILL.next.md"; }

int g_clock = 0;
std::string clockNow() { return "2026-10-23 06:" + std::to_string(10 + (g_clock++ % 40)); }

SkillStore storeOn(MemFs& fs, int pendingCap = 4) {
  agent::SkillStoreConfig cfg;
  cfg.root = kRoot;
  cfg.pendingCap = pendingCap;
  return SkillStore(fs, cfg, clockNow);
}

std::string md(const std::string& createdBy, bool approved, int rev, const std::string& body,
               int baseRev = 0, const std::string& inject = "spawn") {
  SkillMd d;
  d.title = "T " + body;
  d.desc = "read BEFORE doing " + body;
  d.inject = inject;
  d.createdBy = createdBy;
  d.createdAt = "2026-10-01 10:00";
  d.approved = approved;
  d.rev = rev;
  d.baseRev = baseRev;
  d.body = body;
  return agent::composeSkillMd(d);
}

// ---- T1: every state x actor x op ----------------------------------------------

enum class State : uint8_t {
  None = 0,
  PendingAgent,
  ApprovedAgent,
  ApprovedUser,
  ApprovedAgentNext,
  ApprovedUserNext,
  ApprovedUserStaleNext,
  kCount
};
enum class Actor : uint8_t { Owner = 0, AgentAdmin, AgentAdminTainted, AgentMember, AgentScheduled, kCount };
enum class Op : uint8_t { Save = 0, SaveBodyOnly, Approve, Deny, Discard, Delete, kCount };

const char* kId = "meal-log";

void seed(MemFs& fs, State s) {
  switch (s) {
    case State::None: break;
    case State::PendingAgent: fs.files[livePath(kId)] = md("agent", false, 1, "LIVE"); break;
    case State::ApprovedAgent: fs.files[livePath(kId)] = md("agent", true, 2, "LIVE"); break;
    case State::ApprovedUser: fs.files[livePath(kId)] = md("user", true, 2, "LIVE"); break;
    case State::ApprovedAgentNext:
      fs.files[livePath(kId)] = md("agent", true, 2, "LIVE");
      fs.files[nextPath(kId)] = md("agent", false, 3, "NEXT", 2);
      break;
    case State::ApprovedUserNext:
      fs.files[livePath(kId)] = md("user", true, 2, "LIVE");
      fs.files[nextPath(kId)] = md("agent", false, 3, "NEXT", 2);
      break;
    case State::ApprovedUserStaleNext:
      fs.files[livePath(kId)] = md("user", true, 4, "LIVE");
      fs.files[nextPath(kId)] = md("agent", false, 3, "NEXT", 2);
      break;
    case State::kCount: break;
  }
}

SkillCaller callerFor(Actor a) {
  switch (a) {
    case Actor::Owner: return SkillCaller::ownerCall();
    case Actor::AgentAdmin: return SkillCaller::agentCall(true, false, false);
    case Actor::AgentAdminTainted: return SkillCaller::agentCall(true, false, true);
    case Actor::AgentMember: return SkillCaller::agentCall(false, false, false);
    case Actor::AgentScheduled: return SkillCaller::agentCall(true, true, false);
    case Actor::kCount: break;
  }
  return SkillCaller{};
}

bool hasLive(State s) { return s != State::None; }
bool hasNext(State s) {
  return s == State::ApprovedAgentNext || s == State::ApprovedUserNext || s == State::ApprovedUserStaleNext;
}
bool userMade(State s) {
  return s == State::ApprovedUser || s == State::ApprovedUserNext || s == State::ApprovedUserStaleNext;
}

// The owner's ops. kCount = "no row" (the test fails on it).
SkillOutcome expectOwner(State s, Op op) {
  switch (op) {
    case Op::Save:
    case Op::SaveBodyOnly: return hasLive(s) ? SkillOutcome::Replaced : SkillOutcome::Created;
    case Op::Approve:
      if (!hasLive(s) || s == State::ApprovedUserStaleNext) return SkillOutcome::Refused;
      if (hasNext(s)) return SkillOutcome::EditApplied;
      return s == State::PendingAgent ? SkillOutcome::Approved : SkillOutcome::Unchanged;
    case Op::Deny:
      if (hasNext(s)) return SkillOutcome::EditDiscarded;
      return s == State::PendingAgent ? SkillOutcome::Deleted : SkillOutcome::Refused;
    case Op::Discard: return hasNext(s) ? SkillOutcome::EditDiscarded : SkillOutcome::Refused;
    case Op::Delete: return hasLive(s) ? SkillOutcome::Deleted : SkillOutcome::Refused;
    case Op::kCount: break;
  }
  return SkillOutcome::kCount;
}

SkillOutcome expectAgent(State s, Op op, bool live) {
  switch (op) {
    case Op::Save:
    case Op::SaveBodyOnly:
      if (op == Op::SaveBodyOnly && !hasLive(s)) return SkillOutcome::Refused;   // no desc
      if (live) return SkillOutcome::WentLive;
      if (!hasLive(s)) return SkillOutcome::Created;
      return s == State::PendingAgent ? SkillOutcome::Replaced : SkillOutcome::EditProposed;
    case Op::Approve:
    case Op::Deny:
    case Op::Discard: return SkillOutcome::Refused;   // only the owner decides
    case Op::Delete:
      return (hasLive(s) && !userMade(s)) ? SkillOutcome::Deleted : SkillOutcome::Refused;
    case Op::kCount: break;
  }
  return SkillOutcome::kCount;
}

SkillOutcome expected(State s, Actor a, Op op, bool selfApprove) {
  switch (a) {
    case Actor::Owner: return expectOwner(s, op);
    case Actor::AgentAdmin: return expectAgent(s, op, selfApprove);
    case Actor::AgentAdminTainted: return expectAgent(s, op, false);
    case Actor::AgentMember:
    case Actor::AgentScheduled: return SkillOutcome::Refused;
    case Actor::kCount: break;
  }
  return SkillOutcome::kCount;
}

SkillResult runOp(SkillStore& st, Op op, const SkillCaller& who, bool selfApprove) {
  // The assistant's save carries forged server fields: they must never land.
  const std::string full = "---\ntitle: New\ndesc: when X\napproved: true\ncreated_by: user\n---\nNEW";
  switch (op) {
    case Op::Save: return st.save(kId, full, who, selfApprove);
    case Op::SaveBodyOnly: return st.save(kId, "NEW BODY ONLY", who, selfApprove);
    case Op::Approve: return st.approve(kId, who);
    case Op::Deny: return st.deny(kId, who);
    case Op::Discard: return st.discard(kId, who);
    case Op::Delete: return st.remove(kId, who);
    case Op::kCount: break;
  }
  return SkillResult{};
}

bool inIndexAsActive(SkillStore& st, const std::string& id) {
  return st.indexText().find("- " + id + ":") != std::string::npos;
}

std::string label(State s, Actor a, Op op, bool sa) {
  char b[96];
  std::snprintf(b, sizeof(b), "state=%d actor=%d op=%d self=%d", (int)s, (int)a, (int)op, (int)sa);
  return b;
}

void checkInvariants(State s, Actor a, Op op, bool sa, const MemFs& before, MemFs& fs, const SkillResult& r) {
  const std::string tag = label(s, a, op, sa);
  const bool agent = a != Actor::Owner;
  const auto liveAfter = fs.files.find(livePath(kId));
  const SkillMd liveBefore =
      hasLive(s) ? agent::parseSkillMd(before.files.at(livePath(kId))) : SkillMd{};
  // I5: a refusal leaves the store byte-identical.
  if (!r.ok) TEST_ASSERT_TRUE_MESSAGE(before.files == fs.files, ("I5 " + tag).c_str());
  if (!agent) return;
  // I1: an assistant op changes an approved live file only when the switch made it live.
  if (hasLive(s) && liveBefore.approved && r.outcome != SkillOutcome::WentLive &&
      r.outcome != SkillOutcome::Deleted)
    TEST_ASSERT_TRUE_MESSAGE(liveAfter != fs.files.end() && liveAfter->second == before.files.at(livePath(kId)),
                             ("I1 " + tag).c_str());
  // I2: an assistant op never changes who wrote an owner's skill, never deletes one.
  if (userMade(s)) {
    TEST_ASSERT_TRUE_MESSAGE(liveAfter != fs.files.end(), ("I2 delete " + tag).c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE("user", agent::parseSkillMd(liveAfter->second).createdBy.c_str(),
                                     ("I2 origin " + tag).c_str());
  }
  // I3: a skill approved before an assistant op is still approved and indexed after it.
  SkillStore st = storeOn(fs);
  if (hasLive(s) && liveBefore.approved && r.outcome != SkillOutcome::Deleted)
    TEST_ASSERT_TRUE_MESSAGE(inIndexAsActive(st, kId), ("I3 " + tag).c_str());
  // I4: forged approval or origin never lands: a new assistant skill is the assistant's,
  // and waiting unless the switch made it live.
  if (!hasLive(s) && r.ok) {
    const SkillMd d = agent::parseSkillMd(liveAfter->second);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("agent", d.createdBy.c_str(), ("I4 origin " + tag).c_str());
    TEST_ASSERT_EQUAL_MESSAGE(r.outcome == SkillOutcome::WentLive, d.approved, ("I4 approval " + tag).c_str());
  }
}

// T6: what skill.list reports equals what is stored, and the index's tail names
// every waiting skill and proposed edit.
void checkListMatchesStore(MemFs& fs, const std::string& tag) {
  SkillStore st = storeOn(fs);
  agent::SkillMeta m;
  const bool have = fs.files.count(livePath(kId)) > 0;
  TEST_ASSERT_EQUAL_MESSAGE(have, st.find(kId, m), ("T6 find " + tag).c_str());
  if (!have) return;
  const SkillMd live = agent::parseSkillMd(fs.files[livePath(kId)]);
  const bool next = fs.files.count(nextPath(kId)) > 0;
  agent::SkillStatus want = !live.approved ? agent::SkillStatus::Pending
                            : next         ? agent::SkillStatus::ApprovedEditPending
                                           : agent::SkillStatus::Approved;
  TEST_ASSERT_EQUAL_STRING_MESSAGE(agent::skillStatusSlug(want), agent::skillStatusSlug(m.status()),
                                   ("T6 status " + tag).c_str());
  const std::string idx = st.indexText();
  const bool tailNames = idx.find("(waiting for the owner's approval") != std::string::npos &&
                         idx.find(kId, idx.find("(waiting")) != std::string::npos;
  TEST_ASSERT_EQUAL_MESSAGE(!live.approved || next, tailNames, ("T6 tail " + tag).c_str());
}

}  // namespace

static void test_store_actor_state_matrix() {
  int combos = 0;
  for (int s = 0; s < (int)State::kCount; ++s)
    for (int a = 0; a < (int)Actor::kCount; ++a)
      for (int op = 0; op < (int)Op::kCount; ++op)
        for (int sa = 0; sa < 2; ++sa) {
          MemFs fs;
          seed(fs, (State)s);
          const MemFs before = fs;
          SkillStore st = storeOn(fs);
          const SkillOutcome want = expected((State)s, (Actor)a, (Op)op, sa != 0);
          const std::string tag = label((State)s, (Actor)a, (Op)op, sa != 0);
          TEST_ASSERT_TRUE_MESSAGE(want != SkillOutcome::kCount, ("no table row: " + tag).c_str());
          const SkillResult r = runOp(st, (Op)op, callerFor((Actor)a), sa != 0);
          TEST_ASSERT_EQUAL_STRING_MESSAGE(agent::skillOutcomeSlug(want), agent::skillOutcomeSlug(r.outcome),
                                           (tag + " err=" + r.err).c_str());
          TEST_ASSERT_EQUAL_MESSAGE(want != SkillOutcome::Refused, r.ok, tag.c_str());
          checkInvariants((State)s, (Actor)a, (Op)op, sa != 0, before, fs, r);
          checkListMatchesStore(fs, tag);
          combos++;
        }
  TEST_ASSERT_EQUAL(7 * 5 * 6 * 2, combos);
}

// I6: a stale edit is never promoted, and the promoted edit keeps the live origin.
static void test_store_stale_edit_never_promoted_and_promotion_keeps_origin() {
  MemFs fs;
  SkillStore st = storeOn(fs);
  const SkillCaller owner = SkillCaller::ownerCall(), agent = SkillCaller::agentCall(true, false, false);
  TEST_ASSERT_TRUE(st.save("food", "---\ndesc: d\n---\nv1", owner, false).ok);
  const SkillResult e = st.save("food", "v2 by the assistant", agent, false);
  TEST_ASSERT_EQUAL_STRING("edit-proposed", agent::skillOutcomeSlug(e.outcome));
  TEST_ASSERT_EQUAL(2, e.rev);
  TEST_ASSERT_TRUE(st.save("food", "---\ndesc: d\n---\nv2 by the owner", owner, false).ok);   // now stale
  const auto snapshot = fs.files;
  const SkillResult a = st.approve("food", owner);
  TEST_ASSERT_FALSE(a.ok);
  TEST_ASSERT_TRUE(a.err.find("out of date") != std::string::npos);
  TEST_ASSERT_TRUE(snapshot == fs.files);
  TEST_ASSERT_TRUE(st.deny("food", owner).ok);   // the stale edit is dropped
  TEST_ASSERT_TRUE(st.save("food", "v3 by the assistant", agent, false).ok);
  const SkillResult ok = st.approve("food", owner);
  TEST_ASSERT_EQUAL_STRING("edit-applied", agent::skillOutcomeSlug(ok.outcome));
  const SkillMd live = agent::parseSkillMd(st.raw("food"));
  TEST_ASSERT_EQUAL_STRING("user", live.createdBy.c_str());   // an edit never hands over the skill
  TEST_ASSERT_EQUAL_STRING("v3 by the assistant", live.body.c_str());
  TEST_ASSERT_EQUAL_STRING("d", live.desc.c_str());           // inherited by the body-only edit
  TEST_ASSERT_EQUAL(3, live.rev);
  TEST_ASSERT_FALSE(fs.exists(nextPath("food")));
  TEST_ASSERT_FALSE(st.remove("food", agent).ok);   // still the owner's
}

// The owner-origin guard over two calls (the RC4 bypass): an assistant save over an
// owner's skill is a proposal, never an overwrite that would let it delete the skill.
static void test_store_owner_skill_cannot_be_overwritten_then_deleted() {
  MemFs fs;
  SkillStore st = storeOn(fs);
  const SkillCaller owner = SkillCaller::ownerCall(), agent = SkillCaller::agentCall(true, false, false);
  TEST_ASSERT_TRUE(st.save("mine", "---\ndesc: d\n---\nowner text", owner, false).ok);
  for (bool sa : {false, true}) {
    TEST_ASSERT_TRUE(st.save("mine", "---\ncreated_by: agent\n---\nassistant text", agent, sa).ok);
    TEST_ASSERT_EQUAL_STRING("user", agent::parseSkillMd(st.raw("mine")).createdBy.c_str());
    const SkillResult del = st.remove("mine", agent);
    TEST_ASSERT_FALSE(del.ok);
    TEST_ASSERT_TRUE(del.err.find("owner") != std::string::npos);
    TEST_ASSERT_TRUE(fs.exists(livePath("mine")));
  }
}

// The switch: off by default (absent or garbage file), on only for "1"; on, the change
// goes live and is reported as such; a turn that read outside content still waits.
static void test_store_self_approve_switch_default_off() {
  TEST_ASSERT_FALSE(agent::skillSelfApproveFromFile(""));
  TEST_ASSERT_FALSE(agent::skillSelfApproveFromFile("0\n"));
  TEST_ASSERT_FALSE(agent::skillSelfApproveFromFile("yes"));
  TEST_ASSERT_FALSE(agent::skillSelfApproveFromFile("10"));
  TEST_ASSERT_TRUE(agent::skillSelfApproveFromFile("1"));
  TEST_ASSERT_TRUE(agent::skillSelfApproveFromFile(" 1\n"));
  MemFs fs;
  SkillStore st = storeOn(fs);
  const SkillCaller agent = SkillCaller::agentCall(true, false, false);
  const SkillCaller tainted = SkillCaller::agentCall(true, false, true);
  const std::string text = "---\ndesc: when coffee\n---\nCoffee rules";
  TEST_ASSERT_EQUAL_STRING("created", agent::skillOutcomeSlug(st.save("c1", text, agent, false).outcome));
  TEST_ASSERT_FALSE(agent::parseSkillMd(st.raw("c1")).approved);
  TEST_ASSERT_EQUAL_STRING("went-live", agent::skillOutcomeSlug(st.save("c2", text, agent, true).outcome));
  TEST_ASSERT_TRUE(agent::parseSkillMd(st.raw("c2")).approved);
  TEST_ASSERT_EQUAL_STRING("created", agent::skillOutcomeSlug(st.save("c3", text, tainted, true).outcome));
  TEST_ASSERT_FALSE(agent::parseSkillMd(st.raw("c3")).approved);
  // A live change supersedes an edit that was waiting from before the switch.
  TEST_ASSERT_TRUE(st.approve("c1", SkillCaller::ownerCall()).ok);
  TEST_ASSERT_TRUE(st.save("c1", "edit one", agent, false).ok);
  TEST_ASSERT_TRUE(fs.exists(nextPath("c1")));
  TEST_ASSERT_EQUAL_STRING("went-live", agent::skillOutcomeSlug(st.save("c1", "edit two", agent, true).outcome));
  TEST_ASSERT_FALSE(fs.exists(nextPath("c1")));
  TEST_ASSERT_EQUAL_STRING("edit two", agent::parseSkillMd(st.raw("c1")).body.c_str());
}

// The waiting queue holds at most pendingCap items (waiting skills + proposed edits);
// re-saving an item already in it needs no room.
static void test_store_pending_cap_counts_skills_and_edits() {
  MemFs fs;
  SkillStore st = storeOn(fs, 2);
  const SkillCaller agent = SkillCaller::agentCall(true, false, false), owner = SkillCaller::ownerCall();
  TEST_ASSERT_TRUE(st.save("owned", "---\ndesc: d\n---\nx", owner, false).ok);
  TEST_ASSERT_TRUE(st.save("p1", "---\ndesc: d\n---\nx", agent, false).ok);
  TEST_ASSERT_TRUE(st.save("owned", "edit", agent, false).ok);
  TEST_ASSERT_EQUAL(2, st.pendingCount());
  const SkillResult full = st.save("p2", "---\ndesc: d\n---\nx", agent, false);
  TEST_ASSERT_FALSE(full.ok);
  TEST_ASSERT_TRUE(full.err.find("waiting for the owner") != std::string::npos);
  TEST_ASSERT_TRUE(st.save("p1", "---\ndesc: d2\n---\ny", agent, false).ok);   // replaces its own
  TEST_ASSERT_TRUE(st.save("owned", "edit again", agent, false).ok);         // replaces the edit
}

// T5: every invalid inject value, unknown key, missing desc, empty or over-cap body is
// refused on the assistant's save (the error says what to fix); the owner's save is
// accepted with warnings; the valid sets are accepted (counter-test).
static void test_lint_rejects_every_invalid_inject() {
  std::mt19937 rng(23);
  std::vector<std::string> bad = {
      "Estimate meal weight, calories, and macros from photos using a blinded, auditable, "
      "uncertainty-aware method",
      "true", "Spawn", "context,both", "SPAWN", "head", "none", "spawn context"};
  const std::string alphabet = "abcdefghijklmnopqrstuvwxyz ,-_ABCXYZ";
  for (int i = 0; i < 200; ++i) {
    std::string v;
    const int n = 1 + (int)(rng() % 24);
    for (int k = 0; k < n; ++k) v += alphabet[rng() % alphabet.size()];
    const size_t b = v.find_first_not_of(' '), e = v.find_last_not_of(' ');
    if (b == std::string::npos) continue;
    v = v.substr(b, e - b + 1);
    if (!agent::validInject(v)) bad.push_back(v);
  }
  const SkillCaller agent = SkillCaller::agentCall(true, false, false), owner = SkillCaller::ownerCall();
  for (const auto& v : bad) {
    MemFs fs;
    SkillStore st = storeOn(fs);
    const std::string text = "---\ntitle: T\ndesc: d\ninject: " + v + "\n---\nbody";
    const SkillResult r = st.save("lint", text, agent, false);
    TEST_ASSERT_FALSE_MESSAGE(r.ok, v.c_str());
    TEST_ASSERT_TRUE_MESSAGE(r.err.find("desc:") != std::string::npos, v.c_str());
    TEST_ASSERT_TRUE(fs.files.empty());
    const SkillResult o = st.save("lint", text, owner, false);
    TEST_ASSERT_TRUE_MESSAGE(o.ok && !o.warnings.empty(), v.c_str());
  }
  for (const char* v : {"spawn", "context", "both"}) {
    MemFs fs;
    SkillStore st = storeOn(fs);
    const SkillResult r = st.save("ok", std::string("---\ntitle: T\ndesc: d\ninject: ") + v + "\n---\nbody", agent, false);
    TEST_ASSERT_TRUE_MESSAGE(r.ok, v);
    TEST_ASSERT_EQUAL_STRING(v, agent::parseSkillMd(st.raw("ok")).inject.c_str());
  }
  MemFs fs;
  SkillStore st = storeOn(fs);
  TEST_ASSERT_TRUE(st.save("k", "---\ndesc: d\ncolor: teal\n---\nb", agent, false).err.find("color") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(st.save("k", "---\ntitle: no desc\n---\nb", agent, false).err.find("desc:") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(st.save("k", "---\ndesc: d\n---\n   \n", agent, false).err.find("empty") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(st.save("k", "---\ndesc: d\n---\n" + std::string(4097, 'x'), agent, false)
                       .err.find("4096") != std::string::npos);
  TEST_ASSERT_TRUE(st.save("k", "---\ndesc: d\n---\n" + std::string(4096, 'x'), agent, false).ok);
}

// The 17 Oct request ("add to your estimation skill") made a sibling skill; a new
// assistant skill whose id looks like an existing one names the existing one.
static void test_store_sibling_skill_is_named() {
  MemFs fs;
  SkillStore st = storeOn(fs);
  const SkillCaller agent = SkillCaller::agentCall(true, false, false);
  TEST_ASSERT_TRUE(st.save("meal-photo-science", "---\ndesc: d\n---\nx", SkillCaller::ownerCall(), false).ok);
  const SkillResult r = st.save("meal-photo-addenda", "---\ndesc: d\n---\ny", agent, false);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_STRING("meal-photo-science", r.similar.c_str());
  TEST_ASSERT_EQUAL_STRING("", st.save("coffee", "---\ndesc: d\n---\ny", agent, false).similar.c_str());
}

// T2: front matter survives get/edit/save round trips; rev rises by exactly one per
// accepted change; created_at never changes.
static void test_frontmatter_roundtrip_property() {
  std::mt19937 rng(7);
  const char* titles[] = {"Plain", "With: colon", "\xE6\x97\xA5\xE6\x9C\xAC CJK", "Trailing  "};
  const char* injects[] = {"spawn", "context", "both"};
  for (int i = 0; i < 60; ++i) {
    MemFs fs;
    SkillStore st = storeOn(fs);
    const std::string title = titles[rng() % 4];
    const std::string inject = injects[rng() % 3];
    const std::string desc = "read before task " + std::to_string(i);
    std::string text = "---\ntitle: " + title + "\ndesc: " + desc + "\ninject: " + inject + "\n---\n";
    text += (rng() % 2) ? "---\nbody opens with a rule\r\n" : "body " + std::to_string(i);
    TEST_ASSERT_TRUE(st.save("rt", text, SkillCaller::ownerCall(), false).ok);
    const SkillMd first = agent::parseSkillMd(st.raw("rt"));
    TEST_ASSERT_EQUAL(1, first.rev);
    const SkillCaller agent = SkillCaller::agentCall(true, false, false);
    TEST_ASSERT_TRUE(st.save("rt", "edited body only", agent, false).ok);
    TEST_ASSERT_TRUE(st.approve("rt", SkillCaller::ownerCall()).ok);
    const SkillMd second = agent::parseSkillMd(st.raw("rt"));
    TEST_ASSERT_EQUAL_STRING(agent::parseSkillMd(text).title.c_str(), second.title.c_str());
    TEST_ASSERT_EQUAL_STRING(desc.c_str(), second.desc.c_str());
    TEST_ASSERT_EQUAL_STRING(inject.c_str(), second.inject.c_str());
    TEST_ASSERT_EQUAL(2, second.rev);
    TEST_ASSERT_EQUAL_STRING(first.createdAt.c_str(), second.createdAt.c_str());
    TEST_ASSERT_TRUE(st.save("rt", "---\ntitle: New title\n---\nfull", agent, false).ok);
    TEST_ASSERT_TRUE(st.approve("rt", SkillCaller::ownerCall()).ok);
    const SkillMd third = agent::parseSkillMd(st.raw("rt"));
    TEST_ASSERT_EQUAL_STRING("New title", third.title.c_str());
    TEST_ASSERT_EQUAL_STRING(desc.c_str(), third.desc.c_str());
    TEST_ASSERT_EQUAL(3, third.rev);
  }
}

// Row 59: skill_get labels a waiting skill as not active and never serves it as an
// approved one; an approved skill with a proposed edit serves the approved text.
static void test_skill_get_labels_pending_and_serves_live_under_edit() {
  MemFs fs;
  SkillStore st = storeOn(fs);
  fs.files[livePath("wait")] = md("agent", false, 1, "WAITING BODY");
  fs.files[livePath("run")] = md("agent", true, 2, "LIVE BODY");
  fs.files[nextPath("run")] = md("agent", false, 3, "NEXT BODY", 2);
  bool trusted = true;
  const std::string w = st.get("wait", agent::SkillSide{}, &trusted);
  TEST_ASSERT_FALSE(trusted);
  TEST_ASSERT_TRUE(w.find("WAITING FOR THE OWNER'S APPROVAL - not active") != std::string::npos);
  TEST_ASSERT_TRUE(w.find("approved -") == std::string::npos);
  const std::string r = st.get("run", agent::SkillSide{}, &trusted);
  TEST_ASSERT_TRUE(trusted);
  TEST_ASSERT_TRUE(r.find("LIVE BODY") != std::string::npos);
  TEST_ASSERT_TRUE(r.find("NEXT BODY") == std::string::npos);
  TEST_ASSERT_TRUE(r.find("rev 3, is waiting") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING("", st.liveBody("wait", agent::SkillSide{}).c_str());
  TEST_ASSERT_EQUAL_STRING("", st.spawnCapsule("wait").c_str());
  TEST_ASSERT_EQUAL_STRING("LIVE BODY", st.spawnCapsule("run").c_str());
  const std::string b = st.get("tools-guide", agent::SkillSide{}, &trusted);
  TEST_ASSERT_TRUE(trusted);
  TEST_ASSERT_TRUE(b.find("[skill tools-guide - built-in]") == 0);
  TEST_ASSERT_EQUAL_STRING("", st.get("nope", agent::SkillSide{}, &trusted).c_str());
}

// T10: a fresh device (no skills directory at all) and a legacy file.
static void test_store_fresh_device_and_legacy_file() {
  MemFs fs;
  SkillStore st = storeOn(fs);
  const auto metas = st.list();
  TEST_ASSERT_EQUAL((int)agent::builtinSkillCount(), (int)metas.size());
  for (const auto& m : metas) TEST_ASSERT_TRUE(m.builtin());
  const std::string idx = st.indexText();
  TEST_ASSERT_EQUAL_STRING(idx.c_str(), st.indexText().c_str());
  TEST_ASSERT_TRUE(idx.find("(waiting") == std::string::npos);
  TEST_ASSERT_EQUAL(0, st.pendingCount());
  TEST_ASSERT_TRUE(st.save("first", "---\ndesc: d\n---\nb", SkillCaller::agentCall(true, false, false), false).ok);
  TEST_ASSERT_TRUE(fs.exists(livePath("first")));
  MemFs legacy;
  legacy.files[livePath("old")] = "---\ntitle: Old\n---\nold body";
  SkillStore ls = storeOn(legacy);
  agent::SkillMeta m;
  TEST_ASSERT_TRUE(ls.find("old", m));
  TEST_ASSERT_EQUAL(1, m.rev);
  TEST_ASSERT_TRUE(m.approved);
  ls.list();
  ls.get("old", agent::SkillSide{});
  ls.indexText();
  TEST_ASSERT_EQUAL(0, legacy.writes);   // a read never rewrites
}

// A crash between an atomic write's remove and rename leaves only the tmp file: the
// store adopts it instead of losing the skill.
static void test_store_adopts_a_tmp_file_left_by_a_crash() {
  MemFs fs;
  fs.files[livePath("crash") + ".tmp"] = md("user", true, 3, "SURVIVED");
  SkillStore st = storeOn(fs);
  agent::SkillMeta m;
  TEST_ASSERT_TRUE(st.find("crash", m));
  TEST_ASSERT_EQUAL(3, m.rev);
  TEST_ASSERT_TRUE(fs.exists(livePath("crash")));
  TEST_ASSERT_FALSE(fs.exists(livePath("crash") + ".tmp"));
}

// A failed write reports it and changes nothing.
static void test_store_write_failure_is_reported() {
  MemFs fs;
  SkillStore st = storeOn(fs);
  fs.failWrites = true;
  const SkillResult r = st.save("w", "---\ndesc: d\n---\nb", SkillCaller::ownerCall(), false);
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_TRUE(r.err.find("could not be written") != std::string::npos);
  TEST_ASSERT_TRUE(fs.files.empty());
}

// The flash tier holds a bounded number of skills.
static void test_store_flash_tier_limit() {
  MemFs fs;
  agent::SkillStoreConfig cfg;
  cfg.root = "/data/skills";
  cfg.sourceSlug = "flash";
  cfg.maxStored = 2;
  SkillStore st(fs, cfg, clockNow);
  const SkillCaller owner = SkillCaller::ownerCall();
  TEST_ASSERT_TRUE(st.save("a", "x", owner, false).ok);
  TEST_ASSERT_TRUE(st.save("b", "x", owner, false).ok);
  TEST_ASSERT_TRUE(st.save("a", "y", owner, false).ok);   // replacing needs no room
  TEST_ASSERT_FALSE(st.save("c", "x", owner, false).ok);
  agent::SkillMeta m;
  TEST_ASSERT_TRUE(st.find("a", m));
  TEST_ASSERT_EQUAL_STRING("flash", m.source.c_str());
}

// ---- attach --------------------------------------------------------------------

namespace {
struct AttachRig {
  MemFs fs;
  agent::AttachTable table;
  uint32_t now = 1000;
  SkillStore st = storeOn(fs);
  AttachRig() {
    fs.files[livePath("s-ctx")] = md("user", true, 1, "CTX BODY", 0, "context");
    fs.files[livePath("s-a")] = md("agent", true, 2, "A BODY");
    fs.files[livePath("s-b")] = md("agent", false, 1, "B BODY");
  }
  void get(const std::string& chat, const std::string& id) {
    bool trusted = false;
    if (!st.get(id, agent::SkillSide{}, &trusted).empty() && trusted) table.noteUsed(chat, id, now);
  }
  std::string turn(const std::string& chat) {
    table.noteTurn(chat, now);
    const auto picks = agent::selectAttach(table.stickyFor(chat), st.list());
    return agent::skillsAttachText(picks, [this](const std::string& id) {
      return st.liveBody(id, agent::SkillSide{});
    });
  }
};
std::vector<std::string> attachedIds(const std::string& block) {
  std::vector<std::string> ids;
  for (size_t p = block.find("[SKILL "); p != std::string::npos; p = block.find("[SKILL ", p + 1)) {
    const size_t e = block.find_first_of(" ]", p + 7);
    ids.push_back(block.substr(p + 7, e - (p + 7)));
  }
  return ids;
}
}  // namespace

// T3: the attach state machine over random events, with a thrash check.
static void test_attach_selection_property() {
  std::mt19937 rng(11);
  const std::vector<std::string> chats = {"c1", "c2", "c3"};
  const std::vector<std::string> ids = {"tools-guide", "log-rows", "s-ctx", "s-a", "s-b"};
  for (int run = 0; run < 40; ++run) {
    AttachRig r;
    std::map<std::string, std::set<std::string>> readIn;   // what each chat read (an upper bound)
    for (int step = 0; step < 120; ++step) {
      const std::string c = chats[rng() % 3];
      switch (rng() % 8) {
        case 0: {
          r.get(c, ids[rng() % ids.size()]);
          const std::vector<std::string> sticky = r.table.stickyFor(c);
          readIn[c].insert(sticky.begin(), sticky.end());
          break;
        }
        case 1: r.st.approve("s-b", SkillCaller::ownerCall()); break;
        case 2: r.st.save("s-a", "EDITED A", SkillCaller::agentCall(true, false, false), false); break;
        case 3: r.st.discard("s-a", SkillCaller::ownerCall()); break;
        case 4: r.table.dropChat(c); readIn[c].clear(); break;
        case 5: r.now += (rng() % 2) ? 60u * 1000u : agent::kSkillStickyIdleMs + 1; break;
        default: break;
      }
      const std::string block = r.turn(c);
      TEST_ASSERT_TRUE(block.size() <= agent::kSkillAttachCap);
      TEST_ASSERT_TRUE(testutf8::valid(block));
      TEST_ASSERT_TRUE(block.find("B BODY") == std::string::npos || agent::parseSkillMd(r.fs.files[livePath("s-b")]).approved);
      TEST_ASSERT_TRUE(block.find("EDITED A") == std::string::npos);   // an edit under review never rides
      TEST_ASSERT_TRUE(block.find("CTX BODY") != std::string::npos);    // pinned rides every chat
      TEST_ASSERT_TRUE(r.table.stickyFor(c).size() <= agent::kSkillStickyPerChat);
      const auto shown = attachedIds(block);
      TEST_ASSERT_EQUAL_STRING("s-ctx", shown.empty() ? "" : shown[0].c_str());
      TEST_ASSERT_TRUE(std::is_sorted(shown.begin() + 1, shown.end()));
      for (size_t k = 1; k < shown.size(); ++k)   // isolation: a chat sees only what it read
        TEST_ASSERT_TRUE_MESSAGE(readIn[c].count(shown[k]) > 0, shown[k].c_str());
    }
  }
}

static void test_attach_lifecycle_and_no_thrash() {
  AttachRig r;
  r.fs.files.erase(livePath("s-ctx"));   // no pinned skill: only reads attach
  TEST_ASSERT_EQUAL_STRING("", r.turn("c1").c_str());
  r.get("c1", "s-b");   // waiting: never attached
  TEST_ASSERT_EQUAL_STRING("", r.turn("c1").c_str());
  r.get("c1", "s-a");
  const std::string one = r.turn("c1");
  TEST_ASSERT_TRUE(one.find("[ACTIVE SKILLS]") == 0);
  TEST_ASSERT_TRUE(one.find("[SKILL s-a rev 2 - approved - attached because it was read") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING("", r.turn("c2").c_str());   // another chat sees nothing
  // 1000 plain turns inside the window: the block never changes (no thrash).
  for (int i = 0; i < 1000; ++i) {
    r.now += 5000;
    TEST_ASSERT_EQUAL_STRING(one.c_str(), r.turn("c1").c_str());
  }
  // A third read in the chat evicts the least recently read (at most 2 per chat).
  r.get("c1", "tools-guide");
  r.now += 10;
  r.get("c1", "log-rows");
  const auto ids = attachedIds(r.turn("c1"));
  TEST_ASSERT_EQUAL(2, (int)ids.size());
  TEST_ASSERT_EQUAL_STRING("log-rows", ids[0].c_str());
  TEST_ASSERT_EQUAL_STRING("tools-guide", ids[1].c_str());
  // Idle past the window: gone. /clear: gone. Delete: gone.
  r.now += agent::kSkillStickyIdleMs + 1;
  TEST_ASSERT_EQUAL_STRING("", r.turn("c1").c_str());
  r.get("c1", "s-a");
  r.table.dropChat("c1");
  TEST_ASSERT_EQUAL_STRING("", r.turn("c1").c_str());
  r.get("c1", "s-a");
  TEST_ASSERT_TRUE(r.st.remove("s-a", SkillCaller::ownerCall()).ok);
  TEST_ASSERT_EQUAL_STRING("", r.turn("c1").c_str());
  // The wrap of the millisecond clock does not expire a fresh entry.
  AttachRig w;
  w.fs.files.erase(livePath("s-ctx"));
  w.now = 0xFFFFFFF0u;
  w.get("c1", "s-a");
  w.now += 0x20;
  TEST_ASSERT_TRUE(w.turn("c1").find("s-a") != std::string::npos);
}

// T9 + UTF-8: the block never exceeds its cap, never cuts a character, and a body over
// the cap is cut with a visible note, never dropped silently.
static void test_attach_cap_is_utf8_safe_property() {
  std::mt19937 rng(5);
  const char* glyphs[] = {"a", "\xC3\xA9", "\xE6\x97\xA5", "\xF0\x9F\x8D\x95", " ", "\n"};
  for (int i = 0; i < 300; ++i) {
    std::string body;
    const size_t want = 1 + rng() % 6000;
    while (body.size() < want) body += glyphs[rng() % 6];
    std::vector<agent::AttachPick> picks = {{"one", 1, false, true}};
    if (rng() % 2) picks.push_back({"two", 4, false, false});
    const size_t cap = 300 + rng() % 4000;
    const std::string block = agent::skillsAttachText(picks, [&](const std::string&) { return body; }, cap);
    TEST_ASSERT_TRUE(block.size() <= cap);
    TEST_ASSERT_TRUE(testutf8::valid(block));
    // Never a silent drop: the whole body is there, or the cut / not-shown note is.
    TEST_ASSERT_TRUE(block.find(body) != std::string::npos ||
                     block.find("[skill truncated at 4096 B") != std::string::npos ||
                     block.find("not shown") != std::string::npos);
    const std::string inj = agent::composeSkillInjection("id", body, "task");
    TEST_ASSERT_TRUE(testutf8::valid(inj));
  }
  // A body of exactly the cap plus a pinned one: still within the cap, both named.
  const std::string exact(agent::kSkillAttachCap, 'x');
  const std::vector<agent::AttachPick> two = {{"pin", 1, false, true}, {"big", 2, false, false}};
  const std::string b = agent::skillsAttachText(two, [&](const std::string& id) {
    return id == "pin" ? std::string("short pinned rules") : exact;
  });
  TEST_ASSERT_TRUE(b.size() <= agent::kSkillAttachCap);
  TEST_ASSERT_TRUE(b.find("[SKILL pin") != std::string::npos);
  TEST_ASSERT_TRUE(b.find("[SKILL big") != std::string::npos);
  TEST_ASSERT_TRUE(b.find("skill_get big for the rest") != std::string::npos);
}

// ---- the owner's /skill commands -----------------------------------------------

// T8 for /skill: every sub-command, anywhere in a burst, runs as its own command
// (the 14 Oct "/skill approve" that reached the model inside "Message 1 of 2").
static void test_skill_commands_burst_and_parse() {
  for (size_t i = 0; i < agent::kSkillCmdCount; ++i) {
    const std::string cmd = std::string("/skill ") + agent::kSkillCmds[i].word + " meal-log";
    nimbus::orch::MessageBatcher b;
    b.add("9", "Ann", "one");
    TEST_ASSERT_TRUE_MESSAGE(nimbus::orch::BatchAdd::CommandBoundary == b.add("9", "Ann", cmd), cmd.c_str());
    b.add("9", "Ann", "three");
    TEST_ASSERT_EQUAL_STRING(cmd.c_str(), b.render(1).c_str());
    const nimbus::orch::Command c = nimbus::orch::parseCommand(cmd);
    TEST_ASSERT_TRUE(nimbus::orch::isOwnerVerb(c.verb));
    std::string arg;
    TEST_ASSERT_EQUAL((int)agent::kSkillCmds[i].cmd, (int)agent::parseSkillCmd(c.args, arg));
    TEST_ASSERT_TRUE(agent::skillCmdUsage().find(agent::kSkillCmds[i].word) != std::string::npos ||
                     agent::kSkillCmds[i].cmd == agent::SkillCmd::Help);
  }
  std::string arg;
  TEST_ASSERT_EQUAL((int)agent::SkillCmd::Help, (int)agent::parseSkillCmd("", arg));
  TEST_ASSERT_EQUAL((int)agent::SkillCmd::Help, (int)agent::parseSkillCmd("approve", arg));
  TEST_ASSERT_EQUAL((int)agent::SkillCmd::Help, (int)agent::parseSkillCmd("frobnicate x", arg));
  TEST_ASSERT_EQUAL((int)agent::SkillCmd::Auto, (int)agent::parseSkillCmd("AUTO On", arg));
  TEST_ASSERT_EQUAL_STRING("on", arg.c_str());
  TEST_ASSERT_EQUAL((int)agent::SkillCmd::Show, (int)agent::parseSkillCmd("  show   meal-log  ", arg));
  TEST_ASSERT_EQUAL_STRING("meal-log", arg.c_str());
}

// Every owner-facing text a skill change produces is plain: no em dash, no ticket
// numbers, valid UTF-8 - over every outcome and every state of the matrix.
static void test_skill_owner_texts_are_plain() {
  std::vector<std::string> texts = {agent::skillCmdUsage(), agent::skillSelfApproveNotice("x", 3, true),
                                    agent::skillSelfApproveNotice("x", 3, false)};
  for (int o = 0; o < (int)SkillOutcome::kCount; ++o) {
    SkillResult r;
    r.ok = o != (int)SkillOutcome::Refused;
    r.outcome = (SkillOutcome)o;
    r.rev = 2;
    r.err = "a reason";
    texts.push_back(agent::skillResultText("x", r));
    texts.push_back(agent::skillProposalNotice("x", r));
  }
  for (int s = 0; s < (int)State::kCount; ++s) {
    MemFs fs;
    seed(fs, (State)s);
    SkillStore st = storeOn(fs);
    texts.push_back(agent::skillShowText(st, kId));
    texts.push_back(agent::skillListText(st, s % 2 == 0));
  }
  for (const auto& t : texts) {
    TEST_ASSERT_TRUE_MESSAGE(t.find("\xE2\x80\x94") == std::string::npos, t.c_str());
    TEST_ASSERT_TRUE_MESSAGE(t.find("CUM-") == std::string::npos, t.c_str());
    TEST_ASSERT_TRUE(testutf8::valid(t));
    TEST_ASSERT_FALSE(t.empty());
  }
}

// T7: the tool texts frame skills as playbooks and document every key an author can
// write (one table: kSkillMdKeys); the composer never writes a key outside the table.
static std::string readFile(const char* path) {
  std::string out;
  FILE* f = std::fopen(path, "rb");
  if (!f) return out;
  char buf[8192];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

static void test_skill_tool_texts_are_playbook_framed() {
  const std::string doc = agent::skillAuthorKeysDoc();
  for (size_t i = 0; i < agent::kSkillMdKeyCount; ++i)
    if (agent::kSkillMdKeys[i].author)
      TEST_ASSERT_TRUE_MESSAGE(doc.find(std::string(agent::kSkillMdKeys[i].key) + " (") != std::string::npos,
                               agent::kSkillMdKeys[i].key);
  SkillMd all;
  all.title = "t";
  all.desc = "d";
  all.version = "v";
  all.inject = "both";
  all.createdBy = "agent";
  all.createdAt = "c";
  all.approved = false;
  all.rev = 2;
  all.updatedAt = "u";
  all.baseRev = 1;
  all.body = "b";
  const std::string canon = agent::composeSkillMd(all);
  size_t p = 4;   // past "---\n"
  int keys = 0;
  while (canon.compare(p, 4, "---\n") != 0) {
    const size_t colon = canon.find(':', p), eol = canon.find('\n', p);
    TEST_ASSERT_TRUE_MESSAGE(agent::isSkillMdKey(canon.substr(p, colon - p)), canon.substr(p, colon - p).c_str());
    p = eol + 1;
    keys++;
  }
  TEST_ASSERT_EQUAL((int)agent::kSkillMdKeyCount, keys);   // every key the table names is written
  const std::string src = readFile("src/agent/memory_subsystem.cpp");
  TEST_ASSERT_FALSE(src.empty());
  const size_t save = src.find("\"skill.save\"");
  TEST_ASSERT_TRUE(save != std::string::npos);
  TEST_ASSERT_TRUE_MESSAGE(src.find("skillAuthorKeysDoc()", save) < src.find("\"skill.delete\"", save),
                           "skill.save's description must come from the key table");
  for (const char* name : {"\"skill.list\"", "\"skill.get\"", "\"skill.save\"", "\"skill.delete\""}) {
    const size_t at = src.find(name);
    TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos, name);
    const std::string region = src.substr(at, 700);
    TEST_ASSERT_TRUE_MESSAGE(region.find("about yourself") == std::string::npos, name);
  }
  TEST_ASSERT_TRUE(agent::builtinSkillBody("tools-guide", true).find("about yourself") == std::string::npos);
  TEST_ASSERT_TRUE(agent::builtinSkillBody("tools-guide", true).find("task playbooks") != std::string::npos);
}

void runStoreTests() {
  RUN_TEST(test_store_actor_state_matrix);
  RUN_TEST(test_store_stale_edit_never_promoted_and_promotion_keeps_origin);
  RUN_TEST(test_store_owner_skill_cannot_be_overwritten_then_deleted);
  RUN_TEST(test_store_self_approve_switch_default_off);
  RUN_TEST(test_store_pending_cap_counts_skills_and_edits);
  RUN_TEST(test_lint_rejects_every_invalid_inject);
  RUN_TEST(test_store_sibling_skill_is_named);
  RUN_TEST(test_frontmatter_roundtrip_property);
  RUN_TEST(test_skill_get_labels_pending_and_serves_live_under_edit);
  RUN_TEST(test_store_fresh_device_and_legacy_file);
  RUN_TEST(test_store_adopts_a_tmp_file_left_by_a_crash);
  RUN_TEST(test_store_write_failure_is_reported);
  RUN_TEST(test_store_flash_tier_limit);
  RUN_TEST(test_attach_selection_property);
  RUN_TEST(test_attach_lifecycle_and_no_thrash);
  RUN_TEST(test_attach_cap_is_utf8_safe_property);
  RUN_TEST(test_skill_commands_burst_and_parse);
  RUN_TEST(test_skill_owner_texts_are_plain);
  RUN_TEST(test_skill_tool_texts_are_playbook_framed);
}
