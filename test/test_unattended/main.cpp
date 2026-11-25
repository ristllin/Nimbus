// test_unattended - isolated routine runs and held delivery on engine 2 (lane RB-U1a, PRD section 6).
//
// The class: a turn nobody asked for ran with the owner's whole chat and every tool (DIAGNOSIS
// RC-13); on engine 2 a routine fire runs on its own transcript with only its own tools, its
// output reaches the owner's transcript whole (owner ruling 81), a note for quiet hours waits and
// is sent as stored, and a routine that reaches a limit says so (ruling 84 item 9). Every case is
// a property over generated routines, transcripts, paths or texts, each with its counter-test.
#include <unity.h>

#include <algorithm>
#include <set>

#include "nimbus/orch/tx/item.h"
#include "nimbus/orch/unattended_policy.h"
#include "rig.h"

using namespace ut;

void setUp() {}
void tearDown() {}

namespace {

constexpr int kCases = 24;

const char* const kAnyTool[] = {"read_file", "write_file", "append_file", "edit_file", "delete_file", "web_search",
                                "send",      "schedule",   "note",        "list_files", "calc",       "search"};

plain::RoutineFire makeFire(std::mt19937& r, uint64_t at) {
  plain::RoutineFire f;
  f.def.id = "lp" + hexOf(r, 6);
  f.def.name = "Brief " + hexOf(r, 3);
  f.def.instruction = wordsOf(r, 3 + r() % 8);
  const size_t n = r() % 5;
  for (size_t i = 0; i < n; ++i) f.def.tools.push_back(kAnyTool[r() % 12]);
  if (r() % 2) f.def.paths.push_back("routine-out/");
  f.grants = r() % 2;
  f.fireAt = at;
  for (size_t i = 0, k = r() % 4; i < k; ++i)
    f.last.push_back(plain::RoutineOutput{at - (k - i) * kDay, "earlier output " + hexOf(r, 4)});
  return f;
}

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }
void seedFiles(Files& w);
void loopForever(Rig& rig, const std::string& lastText, uint32_t takesMs = 1000);

// The owner's chat, filled with marked messages, replies and tool work.
std::vector<std::string> fillOwner(Rig& rig, std::mt19937& r) {
  std::vector<std::string> marks;
  const size_t turns = 1 + r() % 4;
  for (size_t t = 0; t < turns; ++t) {
    const std::string m = "OWNMARK" + hexOf(r, 10), rep = "REPLYMARK" + hexOf(r, 10), arg = "ARGMARK" + hexOf(r, 10);
    marks.insert(marks.end(), {m, rep, arg});
    rig.world.files["owner/" + arg + ".md"] = "FILEMARK" + arg;
    marks.push_back("FILEMARK" + arg);
    if (r() % 2) rig.ownerModel.push(sp::call("read_file", "{\"path\":\"owner/" + arg + ".md\"}"));
    rig.ownerModel.push(sp::say(rep));
    plain::TurnOutcome out;
    TEST_ASSERT_TRUE(rig.ownerTurn(m + " " + wordsOf(r, 4), out).ok());
    rig.advance(60);
  }
  return marks;
}

// A routine model that calls one generated tool, then answers.
// A model offered no tools makes no call (the wire refuses a call in a request with no tools).
void scriptRoutine(Rig& rig, std::mt19937& r, const std::string& answer, bool offered) {
  const std::string tool = kAnyTool[r() % 12];
  if (offered) rig.routineModel.push(sp::call(tool, "{\"path\":\"routine-out/x.md\",\"content\":\"c\",\"text\":\"t\",\"q\":\"news\"}"));
  rig.routineModel.push(sp::say(answer));
}

// ---- 1. isolation ---------------------------------------------------------------------------
void test_a_routine_request_holds_zero_owner_bytes_and_only_its_allowed_tools() {
  for (int c = 0; c < kCases; ++c) {
    std::mt19937 r(1000 + c);
    Rig rig;
    TEST_ASSERT_TRUE(rig.boot().ok());
    const std::vector<std::string> marks = fillOwner(rig, r);
    const plain::RoutineFire f = makeFire(r, rig.epoch);
    const std::vector<std::string> allowed = plain::routineTools(f);
    bool offered = false;
    for (const std::string& n : allowed) offered = offered || (rig.reg.has(n) && rig.reg.classOf(n).alwaysOn());
    scriptRoutine(rig, r, "routine answer " + hexOf(r, 6), offered);
    plain::FireReport rep;
    rig.world.ran.clear();
    TEST_ASSERT_TRUE_MESSAGE(rig.fire(f, rep).ok(), rep.status.where);
    for (const std::string& t : rig.world.ran)   // a call to any other tool never runs
      TEST_ASSERT_TRUE_MESSAGE(std::find(allowed.begin(), allowed.end(), t) != allowed.end(), t.c_str());
    TEST_ASSERT_EQUAL(offered ? 2 : 1, (int)rig.routineModel.requests().size());
    for (const auto& req : rig.routineModel.requests()) {
      TEST_ASSERT_TRUE(contains(req.body, f.def.instruction));
      for (const std::string& m : marks) TEST_ASSERT_FALSE_MESSAGE(contains(req.body, m), m.c_str());
      for (const std::string& t : req.view.tools)
        TEST_ASSERT_TRUE_MESSAGE(std::find(allowed.begin(), allowed.end(), t) != allowed.end(), t.c_str());
      for (const auto& o : f.last) TEST_ASSERT_TRUE(contains(req.body, o.text));   // its state is its continuity
    }
    // A second fire starts from its state alone: nothing of the first fire's turn is in it.
    rig.routineModel.push(sp::say("second"));
    plain::RoutineFire f2 = f;
    f2.fireAt = rig.epoch + 3600;
    f2.last.clear();
    TEST_ASSERT_TRUE(rig.fire(f2, rep).ok());
    TEST_ASSERT_FALSE(contains(rig.routineModel.last().body, "routine answer"));
    TEST_ASSERT_EQUAL(1, (int)rig.routineModel.last().view.messages.size());
  }
}

// Counter-test: the same fire on the owner's own engine carries his transcript, and the full
// registry offers tools a routine is not given.
void test_counter_the_owners_engine_carries_his_transcript_and_every_tool() {
  std::mt19937 r(7);
  Rig rig;
  TEST_ASSERT_TRUE(rig.boot().ok());
  const std::vector<std::string> marks = fillOwner(rig, r);
  plain::RoutineFire f = makeFire(r, rig.epoch);
  f.def.tools = {"read_file"};
  rig.ownerModel.push(sp::say("on the owner's engine"));
  plain::TurnInput in;
  in.ctx = rig.headCtx("1001", plain::Trigger::Routine);
  in.text = plain::routineInputText(f, 0, plain::kDayStartDefaultMin);
  plain::TurnOutcome out;
  TEST_ASSERT_TRUE(rig.owner->runTurn(in, out).ok());
  const std::string& body = rig.ownerModel.last().body;
  TEST_ASSERT_TRUE(contains(body, marks.front()));
  const auto& tools = rig.ownerModel.last().view.tools;
  TEST_ASSERT_TRUE(std::find(tools.begin(), tools.end(), "write_file") != tools.end());
  TEST_ASSERT_EQUAL(1, (int)plain::routineTools(f).size());
}

void test_the_policy_row_cuts_the_definition_fail_closed() {
  plain::RoutineFire f;
  f.def.id = "lp000001";
  // The default list: send is never offered (its reply is the routine's message), web_search is
  // paid work (only with the owner's switch on), read_file is a read.
  std::vector<std::string> t = plain::routineTools(f);
  TEST_ASSERT_EQUAL(1, (int)t.size());
  TEST_ASSERT_EQUAL_STRING("read_file", t[0].c_str());
  f.grants = true;
  t = plain::routineTools(f);
  TEST_ASSERT_EQUAL(2, (int)t.size());
  TEST_ASSERT_EQUAL_STRING("web_search", t[0].c_str());
  // A write tool only with listed paths; settings, memory and unknown names never.
  f.def.tools = {"write_file", "note", "skill_save", "made_up_tool", "read_file", "read_file"};
  t = plain::routineTools(f);
  TEST_ASSERT_EQUAL(1, (int)t.size());
  f.def.paths = {"out/"};
  t = plain::routineTools(f);
  TEST_ASSERT_EQUAL(2, (int)t.size());
  TEST_ASSERT_EQUAL_STRING("write_file", t[0].c_str());
  // Every name it offers is one the policy table lets a routine be offered.
  const orch::UnattendedFacts facts = plain::routineFacts(f);
  for (const std::string& n : t) TEST_ASSERT_TRUE(orch::unattendedMayList(facts, n));
}

void test_the_routine_key_is_its_own_and_never_a_chat() {
  std::string k;
  TEST_ASSERT_TRUE(plain::routineChatKey("lp00ab12", k).ok());
  TEST_ASSERT_EQUAL_STRING("routine-lp00ab12", k.c_str());
  TEST_ASSERT_TRUE(plain::isRoutineChat(k));
  for (const char* bad : {"", "LP00", "lp:1", "../x", "lp 1", "a/b", "012345678901234567890123456789012"}) {
    TEST_ASSERT_FALSE_MESSAGE(plain::routineChatKey(bad, k).ok(), bad);
    TEST_ASSERT_TRUE(k.empty());
  }
  for (const char* chat : {"1001", "-1001234", "routine", "routine-"}) TEST_ASSERT_FALSE(plain::isRoutineChat(chat));
}

// A tool the policy allows a routine but its definition does not name is refused, never run; and
// after outside content (a search) the routine may not schedule (RB-Q2a: no wakeup chain from a page).
void test_a_routine_runs_only_its_own_tools_and_outside_content_closes_schedule() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.boot().ok());
  std::mt19937 r(31);
  plain::RoutineFire f = makeFire(r, rig.epoch);
  f.def.tools = {"read_file", "web_search", "schedule"};
  f.grants = true;
  rig.routineModel.push(sp::call("list_files", "{\"path\":\"\"}"));
  rig.routineModel.push(sp::call("schedule", "{\"op\":\"list\"}"));
  rig.routineModel.push(sp::call("web_search", "{\"q\":\"news\"}"));
  rig.routineModel.push(sp::call("schedule", "{\"op\":\"create\",\"kind\":\"routine\"}"));
  rig.routineModel.push(sp::say("done"));
  plain::FireReport rep;
  TEST_ASSERT_TRUE(rig.fire(f, rep).ok());
  const std::vector<std::string> want = {"schedule", "web_search"};
  TEST_ASSERT_TRUE_MESSAGE(rig.world.ran == want, "list_files and the schedule after the search never ran");
}

// The router `more` is never offered and never runs: through it a call would reach the device's
// whole registry (a write to an unlisted path, a listing of every tool).
void test_more_never_reaches_past_the_routine() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.boot().ok());
  seedFiles(rig.world);
  std::mt19937 r(37);
  plain::RoutineFire f = makeFire(r, rig.epoch);
  f.def.tools = {"read_file", "more", "delete_file", "write_file"};
  f.def.paths = {"out/"};
  rig.routineModel.push(sp::call("more", "{\"name\":\"write_file\",\"args\":{\"path\":\"secret.md\",\"content\":\"W\"}}"));
  rig.routineModel.push(sp::call("more", "{\"name\":\"write_file\",\"args\":{\"path\":\"out/a.md\",\"content\":\"W\"}}"));
  rig.routineModel.push(sp::call("more", "{}"));
  // A backslash may separate folders on the card: "out/..\\secret.md" is never under out/ (pathListed).
  rig.routineModel.push(sp::call("write_file", "{\"path\":\"out/..\\\\secret.md\",\"content\":\"W\"}"));
  rig.routineModel.push(sp::say("done"));
  plain::FireReport rep;
  TEST_ASSERT_TRUE(rig.fire(f, rep).ok());
  TEST_ASSERT_EQUAL_STRING("S", rig.world.files["secret.md"].c_str());
  TEST_ASSERT_EQUAL(0, (int)rig.world.files.count("out/a.md"));
  TEST_ASSERT_TRUE(rig.world.ran.empty());
  for (const auto& req : rig.routineModel.requests())
    TEST_ASSERT_TRUE(std::find(req.view.tools.begin(), req.view.tools.end(), "more") == req.view.tools.end());
}

// ---- 2. the whole append (ruling 81) ---------------------------------------------------------
void test_a_routine_output_reaches_the_owners_transcript_whole() {
  const size_t lengths[] = {1, 17, 4000, 4001, 12288, 12289, 20000, 30000};
  for (size_t li = 0; li < sizeof lengths / sizeof lengths[0]; ++li) {
    std::mt19937 r(300 + (int)li);
    Rig rig;
    TEST_ASSERT_TRUE(rig.boot().ok());
    (void)fillOwner(rig, r);
    plain::RoutineFire f = makeFire(r, rig.epoch);
    const std::string out = textOf(r, lengths[li]);
    rig.routineModel.push(sp::say(out));
    plain::FireReport rep;
    TEST_ASSERT_TRUE(rig.fire(f, rep).ok());
    TEST_ASSERT_EQUAL((int)plain::FireResult::Sent, (int)rep.result);
    TEST_ASSERT_FALSE(rig.sent.empty());
    plain::HeldNote n;
    n.name = f.def.name;
    n.at = f.fireAt;
    n.text = out;
    const std::string note = plain::heldNoteText(n, 0, plain::kDayStartDefaultMin);
    const std::vector<std::string> notes = rig.ownerNotes();
    TEST_ASSERT_EQUAL(1, (int)notes.size());
    TEST_ASSERT_TRUE_MESSAGE(notes[0] == note, "the stored note is the output whole");
    TEST_ASSERT_TRUE(contains(note, out));
    // The owner's next request carries it byte for byte.
    rig.ownerModel.push(sp::say("ok"));
    plain::TurnOutcome o;
    TEST_ASSERT_TRUE(rig.ownerTurn("what did the routine say", o).ok());
    bool found = false;
    for (const auto& m : rig.ownerModel.last().view.messages)
      for (const auto& b : m.blocks) found = found || b.text == note || contains(b.text, note);
    TEST_ASSERT_TRUE_MESSAGE(found, "the owner's request carries the note whole");
    TEST_ASSERT_TRUE(rig.held->notes().empty());
  }
}

void test_the_note_label_names_the_routine_with_the_year() {
  const std::string t = plain::routineNoteText("Morning\nbrief", kNoon - 6 * 3600, 0, plain::kDayStartDefaultMin, "hi");
  TEST_ASSERT_EQUAL_STRING("[routine Morning brief 2026-11-17 Tue 06:00] hi", t.c_str());
}

// ---- 3. held delivery --------------------------------------------------------------------
void test_a_quiet_hours_note_is_held_and_sent_as_stored_when_they_end() {
  for (int c = 0; c < 8; ++c) {
    std::mt19937 r(500 + c);
    Rig rig;
    rig.epoch = kNoon + 11 * 3600 + (uint64_t)(r() % 3600);   // 23:00 to 23:59
    TEST_ASSERT_TRUE(rig.boot().ok());
    plain::RoutineFire f = makeFire(r, rig.epoch);
    const std::string out = "overnight " + textOf(r, 1 + r() % 3000);
    rig.routineModel.push(sp::say(out));
    plain::FireReport rep;
    TEST_ASSERT_TRUE(rig.fire(f, rep).ok());
    TEST_ASSERT_EQUAL((int)plain::FireResult::Held, (int)rep.result);
    TEST_ASSERT_TRUE(rig.sent.empty());
    TEST_ASSERT_TRUE(rig.ownerNotes().empty());   // the counter: not in his transcript before it is sent
    TEST_ASSERT_EQUAL(1, (int)rig.held->notes().size());
    // P8 (lane E2-FIX2B): held for quiet hours, its 12 h count from their end (09:00).
    TEST_ASSERT_EQUAL((uint32_t)(kNoon + 21 * 3600 + plain::kHeldNoteExpiryS), (uint32_t)rig.held->notes()[0].expires);
    // Still quiet at 08:59: nothing.
    rig.epoch = kNoon + 20 * 3600 + 59 * 60;
    plain::IdleOutcome io;
    TEST_ASSERT_TRUE(rig.idle(io).ok());
    TEST_ASSERT_TRUE(rig.sent.empty());
    // 09:00: sent as stored, once, and appended then.
    rig.epoch = kNoon + 21 * 3600 + (uint64_t)(r() % 600);
    TEST_ASSERT_TRUE(rig.idle(io).ok());
    TEST_ASSERT_TRUE(io.worked);
    TEST_ASSERT_EQUAL(1, (int)rig.sent.size());
    TEST_ASSERT_TRUE(rig.sent[0].text == out);
    TEST_ASSERT_EQUAL(1, (int)rig.ownerNotes().size());
    TEST_ASSERT_TRUE(contains(rig.ownerNotes()[0], out));
    TEST_ASSERT_TRUE(rig.held->notes().empty());
    TEST_ASSERT_TRUE(rig.idle(io).ok());
    TEST_ASSERT_EQUAL(1, (int)rig.sent.size());
  }
}

void test_an_expired_note_is_dropped_unsent() {
  Rig rig;
  rig.epoch = kNoon + 10 * 3600 + 1800;   // 22:30
  TEST_ASSERT_TRUE(rig.boot().ok());
  std::mt19937 r(9);
  plain::RoutineFire f = makeFire(r, rig.epoch);
  f.def.holdS = 3600;   // the routine's own expiry: an hour
  rig.routineModel.push(sp::say("stale by morning"));
  plain::FireReport rep;
  TEST_ASSERT_TRUE(rig.fire(f, rep).ok());
  TEST_ASSERT_EQUAL((int)plain::FireResult::Held, (int)rep.result);
  rig.epoch = kNoon + 22 * 3600 + 300;   // 10:05: its hour after quiet hours ended (09:00) is over
  plain::IdleOutcome io;
  TEST_ASSERT_TRUE(rig.idle(io).ok());
  TEST_ASSERT_TRUE(io.worked);
  TEST_ASSERT_TRUE(rig.held->notes().empty());
  TEST_ASSERT_TRUE(rig.sent.empty());
  TEST_ASSERT_TRUE(rig.ownerNotes().empty());
}

// Lane E2-FIX2B, finding P8: quiet hours that last longer than the note's hold (the owner set their
// end at 11:00, a routine fired at 22:15). Before: the note expired at 10:15, still in quiet hours,
// and was dropped unsent. Now it waits for their end and goes then, once.
void test_quiet_hours_longer_than_the_hold_keep_the_note_until_they_end() {
  Rig rig;
  rig.quiet.endMinute = 11 * 60;
  rig.epoch = kNoon + 10 * 3600 + 900;   // 22:15
  TEST_ASSERT_TRUE(rig.boot().ok());
  std::mt19937 r(13);
  plain::RoutineFire f = makeFire(r, rig.epoch);
  rig.routineModel.push(sp::say("the nightly summary"));
  plain::FireReport rep;
  TEST_ASSERT_TRUE(rig.fire(f, rep).ok());
  TEST_ASSERT_EQUAL((int)plain::FireResult::Held, (int)rep.result);
  rig.epoch = kNoon + 22 * 3600 + 45 * 60;   // 10:45, still quiet, past fire + 12 h
  plain::IdleOutcome io;
  TEST_ASSERT_TRUE(rig.idle(io).ok());
  TEST_ASSERT_EQUAL(1, (int)rig.held->notes().size());
  TEST_ASSERT_TRUE(rig.sent.empty());
  rig.epoch = kNoon + 23 * 3600 + 60;   // 11:01
  TEST_ASSERT_TRUE(rig.idle(io).ok());
  TEST_ASSERT_EQUAL(1, (int)rig.sent.size());
  TEST_ASSERT_TRUE(rig.sent[0].text == "the nightly summary");
  TEST_ASSERT_TRUE(rig.held->notes().empty());
  TEST_ASSERT_TRUE(rig.idle(io).ok());
  TEST_ASSERT_EQUAL(1, (int)rig.sent.size());
}


// Lane E2-FIX2B, finding P14: a cut right after the channel took a held note. Before: the outbox on
// the card still said Ready, so the restart sent it again. Now Sent is on the card before the send.
void test_a_cut_after_the_send_never_sends_a_held_note_twice() {
  Rig rig;
  rig.epoch = kNoon + 11 * 3600;   // 23:00
  TEST_ASSERT_TRUE(rig.boot().ok());
  std::mt19937 r(17);
  rig.routineModel.push(sp::say("sent only once"));
  plain::FireReport rep;
  TEST_ASSERT_TRUE(rig.fire(makeFire(r, rig.epoch), rep).ok());
  TEST_ASSERT_EQUAL((int)plain::FireResult::Held, (int)rep.result);
  std::string atSend;   // the outbox on the card the moment the channel takes the note
  const auto send = rig.channel.send;
  rig.channel.send = [&rig, &atSend, send](const std::string& chat, const std::string& text) {
    atSend = rig.heldFile;
    return send(chat, text);
  };
  rig.epoch = kNoon + 21 * 3600 + 60;   // 09:01
  plain::IdleOutcome io;
  TEST_ASSERT_TRUE(rig.idle(io).ok());
  TEST_ASSERT_EQUAL(1, (int)rig.sent.size());
  rig.heldFile = atSend;   // the power is cut right after the send: the card holds what it held then
  TEST_ASSERT_TRUE(rig.boot().ok());
  for (int i = 0; i < 3; ++i) TEST_ASSERT_TRUE(rig.idle(io).ok());
  TEST_ASSERT_EQUAL(1, (int)rig.sent.size());
  TEST_ASSERT_TRUE(rig.held->notes().empty());
  // The neighbor: a send the channel refuses stays Ready and goes on the next pass.
  rig.routineModel.push(sp::say("refused once"));
  rig.epoch = kNoon + 11 * 3600 + 86400;
  TEST_ASSERT_TRUE(rig.fire(makeFire(r, rig.epoch), rep).ok());
  int refusals = 1;
  rig.channel.send = [&refusals, send](const std::string& chat, const std::string& text) {
    if (refusals > 0) return --refusals, false;
    return send(chat, text);
  };
  rig.epoch += 10 * 3600 + 60;
  TEST_ASSERT_TRUE(rig.idle(io).ok());
  TEST_ASSERT_EQUAL(1, (int)rig.sent.size());
  TEST_ASSERT_EQUAL(1, (int)rig.held->notes().size());
  TEST_ASSERT_TRUE(rig.held->notes()[0].state == plain::HeldState::Ready);
  TEST_ASSERT_TRUE(rig.idle(io).ok());
  TEST_ASSERT_EQUAL(2, (int)rig.sent.size());
  TEST_ASSERT_TRUE(rig.sent[1].text == "refused once");
}

void test_an_already_delivered_note_is_not_sent_again() {
  Rig rig;
  rig.epoch = kNoon + 11 * 3600;   // 23:00
  TEST_ASSERT_TRUE(rig.boot().ok());
  std::mt19937 r(11);
  plain::RoutineFire f = makeFire(r, rig.epoch);
  rig.routineModel.push(sp::say("the same news"));
  rig.routineModel.push(sp::say("the same news"));
  plain::FireReport rep;
  TEST_ASSERT_TRUE(rig.fire(f, rep).ok());
  f.fireAt += 3600;
  TEST_ASSERT_TRUE(rig.fire(f, rep).ok());
  TEST_ASSERT_EQUAL(2, (int)rig.held->notes().size());
  rig.epoch = kNoon + 21 * 3600 + 60;
  plain::IdleOutcome io;
  for (int i = 0; i < 4; ++i) TEST_ASSERT_TRUE(rig.idle(io).ok());
  TEST_ASSERT_EQUAL(1, (int)rig.sent.size());
  TEST_ASSERT_EQUAL(1, (int)rig.ownerNotes().size());
  TEST_ASSERT_TRUE(rig.held->notes().empty());
}

// A cut after the send and before the append: the boot keeps the note as Sent, the idle step
// appends it and never sends it again. A fire cut before it finished (Pending) is Ready at boot.
// "Already delivered" is this note going out again, never a later day's note that reads the same,
// and a stop-only note is its own.
void test_the_same_text_on_another_day_is_still_sent() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.boot().ok());
  std::mt19937 r(39);
  plain::RoutineFire f = makeFire(r, rig.epoch);
  rig.routineModel.push(sp::say("nothing new"));
  plain::FireReport rep;
  TEST_ASSERT_TRUE(rig.fire(f, rep).ok());   // noon: sent
  TEST_ASSERT_EQUAL(1, (int)rig.sent.size());
  rig.epoch = kNoon + 11 * 3600;   // 23:00: held
  f.fireAt = rig.epoch;
  rig.routineModel.push(sp::say("nothing new"));
  TEST_ASSERT_TRUE(rig.fire(f, rep).ok());
  TEST_ASSERT_EQUAL((int)plain::FireResult::Held, (int)rep.result);
  rig.epoch = kNoon + 21 * 3600 + 60;
  plain::IdleOutcome io;
  TEST_ASSERT_TRUE(rig.idle(io).ok());
  TEST_ASSERT_EQUAL(2, (int)rig.sent.size());
  // Two routines that say the same thing in one night are two notes.
  plain::HeldNote a, b;
  a.routine = "lp000001";
  b.routine = "lp000002";
  a.text = b.text = "same";
  TEST_ASSERT_TRUE(a.key() != b.key());
  b.routine = a.routine;
  b.stop = "rounds";
  TEST_ASSERT_TRUE(a.key() != b.key());
}

// The text is the note: a channel that took only the stop line leaves the note to send again.
void test_a_note_whose_text_did_not_go_is_not_marked_sent() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.boot().ok());
  loopForever(rig, "half done");
  rig.refuseIf = [](const std::string& t) { return t == "half done"; };
  std::mt19937 r(41);
  plain::FireReport rep;
  (void)rig.fire(makeFire(r, rig.epoch), rep);
  TEST_ASSERT_EQUAL(1, (int)rig.held->notes().size());
  TEST_ASSERT_EQUAL((int)plain::HeldState::Ready, (int)rig.held->notes()[0].state);
  TEST_ASSERT_TRUE(rig.ownerNotes().empty());
  rig.refuseIf = nullptr;
  plain::IdleOutcome io;
  TEST_ASSERT_TRUE(rig.idle(io).ok());
  TEST_ASSERT_TRUE(rig.held->notes().empty());
  TEST_ASSERT_EQUAL(1, (int)rig.ownerNotes().size());
}

void test_the_outbox_survives_a_restart_and_never_sends_twice() {
  Rig rig;
  rig.epoch = kNoon + 21 * 3600 + 60;
  TEST_ASSERT_TRUE(rig.boot().ok());
  plain::HeldNote sent, pending;
  sent.id = "lp000001-1";
  sent.name = "Brief";
  sent.chatId = "1001";
  sent.at = rig.epoch - 3600;
  sent.expires = rig.epoch + 3600;
  sent.text = "went out before the cut";
  sent.state = plain::HeldState::Sent;
  pending = sent;
  pending.id = "lp000002-1";
  pending.text = "stored by a fire that was cut";
  pending.state = plain::HeldState::Pending;
  TEST_ASSERT_TRUE(rig.held->put(sent).ok());
  TEST_ASSERT_TRUE(rig.held->put(pending).ok());
  TEST_ASSERT_TRUE(rig.boot().ok());   // a restart: the outbox loads from its file
  TEST_ASSERT_EQUAL(2, (int)rig.held->notes().size());
  TEST_ASSERT_EQUAL((int)plain::HeldState::Ready, (int)rig.held->find("lp000002-1")->state);
  plain::IdleOutcome io;
  for (int i = 0; i < 3; ++i) TEST_ASSERT_TRUE(rig.idle(io).ok());
  TEST_ASSERT_EQUAL(1, (int)rig.sent.size());
  TEST_ASSERT_TRUE(rig.sent[0].text == pending.text);
  TEST_ASSERT_EQUAL(2, (int)rig.ownerNotes().size());
  TEST_ASSERT_TRUE(rig.held->notes().empty());
}

// A send the channel refuses keeps the note for the next idle step.
void test_a_refused_send_keeps_the_note() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.boot().ok());
  rig.refuseSends = true;
  std::mt19937 r(13);
  rig.routineModel.push(sp::say("try again later"));
  plain::FireReport rep;
  (void)rig.fire(makeFire(r, rig.epoch), rep);
  TEST_ASSERT_EQUAL((int)plain::FireResult::Held, (int)rep.result);
  TEST_ASSERT_EQUAL(1, (int)rig.held->notes().size());
  TEST_ASSERT_TRUE(rig.ownerNotes().empty());
  plain::IdleOutcome io;
  TEST_ASSERT_TRUE(rig.idle(io).ok());
  TEST_ASSERT_FALSE_MESSAGE(io.worked, "a refused send is no work done: the later idle slots still run");
  rig.refuseSends = false;
  TEST_ASSERT_TRUE(rig.idle(io).ok());
  TEST_ASSERT_EQUAL(1, (int)rig.sent.size());
  TEST_ASSERT_EQUAL(1, (int)rig.ownerNotes().size());
}

// A full outbox never loses a note: the next one goes at once, quiet hours or not.
void test_a_full_outbox_sends_rather_than_loses() {
  Rig rig;
  rig.epoch = kNoon + 11 * 3600;
  TEST_ASSERT_TRUE(rig.boot().ok());
  std::mt19937 r(33);
  plain::FireReport rep;
  for (size_t i = 0; i <= plain::kHeldNotesMax; ++i) {
    rig.routineModel.push(sp::say("night note " + std::to_string(i)));
    plain::RoutineFire f = makeFire(r, rig.epoch + i);
    TEST_ASSERT_TRUE(rig.fire(f, rep).ok());
    TEST_ASSERT_EQUAL((int)(i < plain::kHeldNotesMax ? plain::FireResult::Held : plain::FireResult::Sent), (int)rep.result);
  }
  TEST_ASSERT_EQUAL(1, (int)rig.sent.size());
  TEST_ASSERT_EQUAL((int)plain::kHeldNotesMax, (int)rig.held->notes().size());
}

// A fire that read outside content says so on its note (for the rails, RB-RL).
void test_a_note_after_outside_content_is_marked() {
  Rig rig;
  rig.epoch = kNoon + 11 * 3600;
  TEST_ASSERT_TRUE(rig.boot().ok());
  std::mt19937 r(35);
  plain::RoutineFire f = makeFire(r, rig.epoch);
  f.def.tools = {"web_search"};
  f.grants = true;
  rig.routineModel.push(sp::call("web_search", "{\"q\":\"news\"}"));
  rig.routineModel.push(sp::say("the news"));
  plain::FireReport rep;
  TEST_ASSERT_TRUE(rig.fire(f, rep).ok());
  TEST_ASSERT_TRUE(rig.held->find(rep.noteId)->outside);
  f.fireAt += 60;
  f.def.tools = {"read_file"};
  rig.routineModel.push(sp::say("no outside"));
  TEST_ASSERT_TRUE(rig.fire(f, rep).ok());
  TEST_ASSERT_FALSE(rig.held->find(rep.noteId)->outside);
}

void test_counter_outside_quiet_hours_the_note_goes_at_once() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.boot().ok());
  std::mt19937 r(15);
  rig.routineModel.push(sp::say("at noon"));
  plain::FireReport rep;
  TEST_ASSERT_TRUE(rig.fire(makeFire(r, rig.epoch), rep).ok());
  TEST_ASSERT_EQUAL((int)plain::FireResult::Sent, (int)rep.result);
  TEST_ASSERT_EQUAL(1, (int)rig.sent.size());
  TEST_ASSERT_TRUE(rig.held->notes().empty());
  // Quiet hours off by the owner: a note at 23:00 goes at once too.
  Rig late;
  late.quiet.respected = false;
  late.epoch = kNoon + 11 * 3600;
  TEST_ASSERT_TRUE(late.boot().ok());
  late.routineModel.push(sp::say("late"));
  TEST_ASSERT_TRUE(late.fire(makeFire(r, late.epoch), rep).ok());
  TEST_ASSERT_EQUAL((int)plain::FireResult::Sent, (int)rep.result);
}

// A due fire runs from the owner engine's idle step, after the outbox has nothing to do.
void test_a_due_fire_runs_from_the_idle_step() {
  Rig rig;
  std::mt19937 r(17);
  plain::RoutineFire f = makeFire(r, kNoon);
  bool given = false;
  rig.due = [&](uint64_t, plain::RoutineFire& out) {
    if (given) return false;
    given = true;
    out = f;
    return true;
  };
  TEST_ASSERT_TRUE(rig.boot().ok());
  rig.routineModel.push(sp::say("from idle"));
  plain::IdleOutcome io;
  TEST_ASSERT_TRUE(rig.idle(io).ok());
  TEST_ASSERT_TRUE(io.worked);
  TEST_ASSERT_EQUAL(1, (int)rig.fires.size());
  TEST_ASSERT_EQUAL(1, (int)rig.sent.size());
  TEST_ASSERT_TRUE(rig.idle(io).ok());
  TEST_ASSERT_FALSE(io.worked);
}

void test_silence_reaches_nobody() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.boot().ok());
  std::mt19937 r(19);
  rig.routineModel.push(sp::say(""));
  plain::FireReport rep;
  TEST_ASSERT_TRUE(rig.fire(makeFire(r, rig.epoch), rep).ok());
  TEST_ASSERT_EQUAL((int)plain::FireResult::Silent, (int)rep.result);
  TEST_ASSERT_TRUE(rig.sent.empty());
  TEST_ASSERT_TRUE(rig.ownerNotes().empty());
  TEST_ASSERT_TRUE(rig.held->notes().empty());
}

// ---- 4. writes (property) -------------------------------------------------------------------
const char* const kPaths[] = {"notes/a.md",        "notes/sub/b.md", "notesX/c.md",   "secret.md",
                              "notes/../secret.md", "/secret.md",     "NOTES/a.md",    "log.csv",
                              "log.csv.bak",        "notes",          "notes/./a.md",  "x/../notes/a.md", "notes/..\\secret.md"};
const char* const kListed[] = {"notes/", "log.csv", "notes/a.md", "x/", "other/"};

bool listedByDef(const std::string& path, const std::vector<std::string>& listed) {
  if (path.empty() || contains("/" + path + "/", "/../") || contains(path, "\\")) return false;
  for (const std::string& l : listed) {
    if (l == path) return true;
    if (!l.empty() && l.back() == '/' && path.compare(0, l.size(), l) == 0 && path.size() > l.size()) return true;
  }
  return false;
}

void seedFiles(Files& w) {
  w.files = {{"notes/a.md", "A"}, {"notes/sub/b.md", "B"}, {"notesX/c.md", "C"}, {"secret.md", "S"}, {"log.csv", "L"}};
}

void test_a_routine_cannot_write_outside_its_definition() {
  for (int c = 0; c < 40; ++c) {
    std::mt19937 r(2000 + c);
    Rig rig;
    TEST_ASSERT_TRUE(rig.boot().ok());
    seedFiles(rig.world);
    const std::map<std::string, std::string> before = rig.world.files;
    plain::RoutineFire f;
    f.def.id = "lp" + hexOf(r, 6);
    f.def.name = "Writer";
    f.def.instruction = "write things";
    f.def.tools = {"write_file", "append_file", "edit_file", "delete_file", "read_file"};
    for (const char* l : kListed)
      if (r() % 3 == 0) f.def.paths.push_back(l);
    f.fireAt = rig.epoch;
    f.grants = r() % 2;
    std::vector<sp::Call> calls;
    std::vector<std::string> aimed;
    for (int i = 0, n = 1 + (int)(r() % 6); i < n; ++i) {
      const std::string p = kPaths[r() % 13];
      const char* tool = f.def.tools[r() % 4].c_str();
      aimed.push_back(p);
      calls.push_back(sp::Call{tool, "{\"path\":\"" + p + "\",\"content\":\"W\",\"text\":\"W\",\"old\":\"A\",\"new\":\"W\"}", ""});
    }
    rig.routineModel.push(sp::calls(calls));
    rig.routineModel.push(sp::say("done"));
    plain::FireReport rep;
    TEST_ASSERT_TRUE(rig.fire(f, rep).ok());
    for (const auto& kv : rig.world.files) {
      const bool listed = listedByDef(kv.first, f.def.paths);
      if (listed) continue;
      auto it = before.find(kv.first);
      TEST_ASSERT_TRUE_MESSAGE(it != before.end(), ("created outside the definition: " + kv.first).c_str());
      TEST_ASSERT_TRUE_MESSAGE(it->second == kv.second, ("changed outside the definition: " + kv.first).c_str());
    }
    for (const auto& kv : before)
      if (!listedByDef(kv.first, f.def.paths))
        TEST_ASSERT_TRUE_MESSAGE(rig.world.files.count(kv.first), ("deleted outside the definition: " + kv.first).c_str());
    // A listed path is reached: the policy allows what the definition lists.
    for (size_t i = 0; i < aimed.size(); ++i)
      if (listedByDef(aimed[i], f.def.paths) && calls[i].name == "write_file" &&
          std::count(aimed.begin(), aimed.end(), aimed[i]) == 1)   // no other call aimed at it
        TEST_ASSERT_EQUAL_STRING("W", rig.world.files[aimed[i]].c_str());
  }
}

void test_counter_the_owners_turn_writes_the_same_file() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.boot().ok());
  seedFiles(rig.world);
  rig.ownerModel.push(sp::call("write_file", "{\"path\":\"secret.md\",\"content\":\"W\"}"));
  rig.ownerModel.push(sp::say("done"));
  plain::TurnOutcome out;
  TEST_ASSERT_TRUE(rig.ownerTurn("write it", out).ok());
  TEST_ASSERT_EQUAL_STRING("W", rig.world.files["secret.md"].c_str());
}

// ---- 5. limits (ruling 84 item 9) ----------------------------------------------------------
// A routine that never stops calling tools.
void loopForever(Rig& rig, const std::string& lastText, uint32_t takesMs) {
  rig.routineModel.respondWith([lastText, takesMs](const sp::Recorded& req, size_t i) {
    sp::Step s = req.view.toolChoice == "none" ? sp::say(lastText)
                                               : sp::call("read_file", "{\"path\":\"p" + std::to_string(i) + ".md\"}");
    s.takesMs = takesMs;
    s.usage.input = 4000;
    s.usage.output = 100;
    return s;
  });
}

void expectHonestStop(Rig& rig, const plain::FireReport& rep, const char* slug, const std::string& text) {
  TEST_ASSERT_EQUAL_STRING(slug, plain::guardTripSlug(rep.outcome.trip));
  TEST_ASSERT_EQUAL((int)plain::FireResult::Sent, (int)rep.result);
  const std::string stop = std::string(plain::texts::device_notes::kLimitStopped) + slug;
  bool said = false;
  for (const Sent& s : rig.sent) said = said || contains(s.text, stop);
  TEST_ASSERT_TRUE_MESSAGE(said, "the owner is told the routine stopped at its limit");
  TEST_ASSERT_EQUAL(1, (int)rig.ownerNotes().size());
  TEST_ASSERT_TRUE(contains(rig.ownerNotes()[0], stop));
  if (!text.empty()) TEST_ASSERT_TRUE(contains(rig.ownerNotes()[0], text));
}

void test_a_routine_that_reaches_its_round_limit_ends_honestly() {
  for (const char* last : {"partial: two of five done", ""}) {
    Rig rig;
    TEST_ASSERT_TRUE(rig.boot().ok());
    loopForever(rig, last);
    std::mt19937 r(21);
    plain::RoutineFire f = makeFire(r, rig.epoch);
    plain::FireReport rep;
    TEST_ASSERT_TRUE(rig.fire(f, rep).ok());
    TEST_ASSERT_EQUAL(plain::kRoutineRounds + 1, (int)rig.routineModel.requests().size());
    TEST_ASSERT_EQUAL_STRING("none", rig.routineModel.last().view.toolChoice.c_str());
    expectHonestStop(rig, rep, "rounds", last);
  }
}

void test_a_routine_that_reaches_its_time_limit_ends_honestly() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.boot().ok());
  loopForever(rig, "ran out of time", 30000);
  std::mt19937 r(23);
  plain::FireReport rep;
  const uint32_t t0 = rig.ms;
  TEST_ASSERT_TRUE(rig.fire(makeFire(r, rig.epoch), rep).ok());
  TEST_ASSERT_TRUE((int)rig.routineModel.requests().size() <= 4);
  TEST_ASSERT_TRUE(rig.ms - t0 <= plain::kRoutineHardMs);
  expectHonestStop(rig, rep, "time", "ran out of time");
}

void test_a_routine_that_reaches_its_spend_limit_ends_honestly() {
  Rig rig;
  rig.cost.inMicroPerM = 3000000;    // 3 USD per 1M input tokens: 4,000 tokens = 0.012 USD a request
  rig.cost.outMicroPerM = 15000000;
  TEST_ASSERT_TRUE(rig.boot().ok());
  loopForever(rig, "spent enough");
  std::mt19937 r(25);
  plain::FireReport rep;
  TEST_ASSERT_TRUE(rig.fire(makeFire(r, rig.epoch), rep).ok());
  TEST_ASSERT_TRUE((int)rig.routineModel.requests().size() < plain::kRoutineRounds + 1);
  TEST_ASSERT_TRUE(rep.outcome.end.microUsd >= plain::kRoutineMaxMicroUsd);   // it stopped for spend
  // A request costs 13,500 micro-USD here: the trip is seen at the round boundary after the cap is
  // reached, then the one last round with no tools: at most two requests past it.
  TEST_ASSERT_TRUE(rep.outcome.end.microUsd <= plain::kRoutineMaxMicroUsd + 2 * 13500);
  expectHonestStop(rig, rep, "spend", "spent enough");
}

void test_counter_a_routine_inside_its_limits_has_no_stop_line() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.boot().ok());
  rig.routineModel.push(sp::call("read_file", "{\"path\":\"a.md\"}"));
  rig.routineModel.push(sp::say("complete"));
  std::mt19937 r(27);
  plain::FireReport rep;
  TEST_ASSERT_TRUE(rig.fire(makeFire(r, rig.epoch), rep).ok());
  TEST_ASSERT_EQUAL((int)plain::GuardTrip::None, (int)rep.outcome.trip);
  TEST_ASSERT_EQUAL(1, (int)rig.ownerNotes().size());
  TEST_ASSERT_FALSE(contains(rig.ownerNotes()[0], plain::texts::device_notes::kLimitStopped));
  // An attended owner turn keeps the head's own limits: the ceiling is the routine's only.
  plain::GuardLimits l;
  l.rounds = 16;
  l.softWallMs = 240000;
  l.maxSpendMicroUsd = 500000;
  uint32_t hard = plain::kHardWallMs;
  plain::applyCeiling(plain::TurnCeiling{}, l, hard);
  TEST_ASSERT_EQUAL(16, l.rounds);
  TEST_ASSERT_EQUAL(plain::kHardWallMs, hard);
  plain::applyCeiling(plain::routineCeiling(), l, hard);
  TEST_ASSERT_EQUAL(plain::kRoutineRounds, l.rounds);
  TEST_ASSERT_EQUAL(plain::kRoutineSoftMs, l.softWallMs);
  TEST_ASSERT_EQUAL(plain::kRoutineMaxMicroUsd, l.maxSpendMicroUsd);
  TEST_ASSERT_EQUAL(plain::kRoutineHardMs, hard);
}

// ---- the state ------------------------------------------------------------------------------
void test_the_state_keeps_the_last_three_outputs_in_2_kb() {
  std::vector<plain::RoutineOutput> last;
  for (uint64_t i = 1; i <= 5; ++i) plain::nextRoutineState(last, plain::RoutineOutput{i, "out " + std::to_string(i)});
  TEST_ASSERT_EQUAL(3, (int)last.size());
  TEST_ASSERT_EQUAL_STRING("out 3", last[0].text.c_str());
  plain::nextRoutineState(last, plain::RoutineOutput{6, ""});   // a silent fire adds nothing
  TEST_ASSERT_EQUAL(3, (int)last.size());
  plain::nextRoutineState(last, plain::RoutineOutput{7, std::string(1500, 'x')});
  size_t bytes = 0;
  for (const auto& o : last) bytes += o.text.size();
  TEST_ASSERT_TRUE(bytes <= plain::kRoutineStateMax);
  const std::vector<plain::RoutineOutput> back = plain::decodeRoutineState(plain::encodeRoutineState(last) + "junk\n");
  TEST_ASSERT_EQUAL((int)last.size(), (int)back.size());
  for (size_t i = 0; i < back.size(); ++i) TEST_ASSERT_TRUE(back[i].text == last[i].text && back[i].at == last[i].at);
  plain::nextRoutineState(last, plain::RoutineOutput{8, std::string(3000, 'y')});
  TEST_ASSERT_EQUAL(1, (int)last.size());
  TEST_ASSERT_EQUAL(plain::kRoutineStateMax, last[0].text.size());
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_routine_request_holds_zero_owner_bytes_and_only_its_allowed_tools);
  RUN_TEST(test_counter_the_owners_engine_carries_his_transcript_and_every_tool);
  RUN_TEST(test_the_policy_row_cuts_the_definition_fail_closed);
  RUN_TEST(test_the_routine_key_is_its_own_and_never_a_chat);
  RUN_TEST(test_more_never_reaches_past_the_routine);
  RUN_TEST(test_a_routine_runs_only_its_own_tools_and_outside_content_closes_schedule);
  RUN_TEST(test_a_routine_output_reaches_the_owners_transcript_whole);
  RUN_TEST(test_the_note_label_names_the_routine_with_the_year);
  RUN_TEST(test_a_quiet_hours_note_is_held_and_sent_as_stored_when_they_end);
  RUN_TEST(test_quiet_hours_longer_than_the_hold_keep_the_note_until_they_end);
  RUN_TEST(test_a_cut_after_the_send_never_sends_a_held_note_twice);
  RUN_TEST(test_an_expired_note_is_dropped_unsent);
  RUN_TEST(test_an_already_delivered_note_is_not_sent_again);
  RUN_TEST(test_the_same_text_on_another_day_is_still_sent);
  RUN_TEST(test_a_note_whose_text_did_not_go_is_not_marked_sent);
  RUN_TEST(test_the_outbox_survives_a_restart_and_never_sends_twice);
  RUN_TEST(test_a_refused_send_keeps_the_note);
  RUN_TEST(test_a_full_outbox_sends_rather_than_loses);
  RUN_TEST(test_a_note_after_outside_content_is_marked);
  RUN_TEST(test_counter_outside_quiet_hours_the_note_goes_at_once);
  RUN_TEST(test_a_due_fire_runs_from_the_idle_step);
  RUN_TEST(test_silence_reaches_nobody);
  RUN_TEST(test_a_routine_cannot_write_outside_its_definition);
  RUN_TEST(test_counter_the_owners_turn_writes_the_same_file);
  RUN_TEST(test_a_routine_that_reaches_its_round_limit_ends_honestly);
  RUN_TEST(test_a_routine_that_reaches_its_time_limit_ends_honestly);
  RUN_TEST(test_a_routine_that_reaches_its_spend_limit_ends_honestly);
  RUN_TEST(test_counter_a_routine_inside_its_limits_has_no_stop_line);
  RUN_TEST(test_the_state_keeps_the_last_three_outputs_in_2_kb);
  return UNITY_END();
}
