#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "nimbus/harness/conversation.h"   // historyTurns / fitHistory / foldSlice - the histV2 pieces
#include "nimbus/orch/budget.h"            // deriveBudget - the device's history and fold budgets
#include "nimbus/orch/compact.h"           // FoldStore - the fold trigger, its thrash wait and backoff
#include "nimbus/orch/episodic.h"
#include "nimbus/orch/wire_limits.h"       // foldDigestCap

// The compaction false alarm Lumi raised in the first hour of v4.6.0 (26 Oct 2026):
// a fold completed, and right after the next owner turn the owner got "compaction
// paused - it kept refilling immediately". This suite replays that sequence with the
// device's own history and fold code (the same budgets, read limits and anchors the
// engine and compactTick use) and pins the class: a backlog larger than one fold is
// folded again, and nothing pauses. Lane F of the history fix (16 Nov 2026, owner
// ruling 2): a real refill only schedules the next try 15 min out, a failed cycle
// schedules a retry by the backoff, and every legacy pause lifts on load.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

namespace {

struct FakeIO : FoldStoreIO {
  std::string blob;
  std::string load() override { return blob; }
  void save(const std::string& b) override { blob = b; }
};

const char kChat[] = "6098532481";
constexpr uint32_t kCompactBytes = 48u * 1024;   // Lumi's compactKB

// One chat's rows, oldest first, with the device's row ids.
struct Chat {
  std::vector<EpisodicMessage> rows;
  int seq = 0;

  std::string add(const std::string& role, const std::string& text, const std::string& tags) {
    EpisodicMessage m;
    char id[16];
    snprintf(id, sizeof(id), "m%08x", (unsigned)(++seq));
    m.id = id;
    m.sessionId = kChat;
    m.tsHours = 490000;
    m.role = role;
    m.kind = MsgKind::Message;
    m.text = text;
    m.tags = tags;
    rows.push_back(m);
    return m.id;
  }
  // A food-log style exchange: a short owner line, a longer reply.
  std::string exchange(int i, size_t replyBytes) {
    const std::string u = add("user", "Breakfast " + std::to_string(i) + ": two eggs, toast, coffee", "from:R");
    std::string reply = "Logged " + std::to_string(i) + ". ";
    while (reply.size() < replyBytes) reply += "Day total so far about 1,240 kcal and 82 g protein. ";
    add("assistant", reply, "fx:file,out:reply,turn:" + u);
    return u;
  }
  // A quiet scheduled turn that stayed silent (the post-update awareness turn).
  void silentSystemTurn() {
    add("system", "[routine \"firmware-update\" ran]", "ev:trigger,src:system,silent:quiet-routine");
  }
  // What readHistory hands historyTurns: the newest rows after `startAfter`, at most
  // `maxRows`, newest first.
  std::vector<EpisodicMessage> read(const std::string& startAfter, size_t maxRows) const {
    std::vector<EpisodicMessage> out;
    for (auto it = rows.rbegin(); it != rows.rend() && out.size() < maxRows; ++it) {
      if (!startAfter.empty() && !rowIdAfter(it->id, startAfter)) break;
      out.push_back(*it);
    }
    return out;
  }
};

// The device around the store: a turn builds the history (engine historyMessages),
// a pump pass evaluates and runs the fold (orchestrator compactTick, histV2 path).
struct Device {
  Chat chat;
  FakeIO io;
  FoldStore folds;
  agent::HistoryBudget hist;
  size_t sliceCap = 0;
  uint32_t epoch = 1793026543;
  int folds_run = 0, skips = 0, thrash = 0, shifts = 0;
  size_t turnReply = 420;    // an owner turn's reply size
  size_t lastDigest = 0;     // the newest fold's digest bytes

  Device() {
    hist.highBytes = deriveBudget(200000, {}).recentConvBytes;   // "budget: ctx=200K" on Lumi
    folds.begin(&io);
  }
  void history() {
    const ChatFold f = folds.get(kChat);
    agent::HistoryOptions o;
    o.startAfter = rowIdAfter(f.softStart, f.thru) ? f.softStart : f.thru;
    const auto turns = agent::historyTurns(chat.read(o.startAfter, 2 * hist.highRows), o);
    const agent::HistoryWindow w = agent::fitHistory(turns, f.softStart, hist, 0);
    if (!w.shifted) return;
    folds.setSoftStart(kChat, w.softStart);
    shifts++;
  }
  void ownerTurn(int i) {
    const std::string text = "Breakfast " + std::to_string(i) + ": two eggs, toast, coffee";
    folds.noteOwnerMessage(kChat, text);
    history();
    const size_t before = chat.rows.size();
    chat.exchange(i, turnReply);
    folds.noteMessage(kChat, chat.rows[before + 1].text.size());   // the reply row
    folds.noteTurn(kChat);
  }
  void awarenessTurn() {
    history();
    chat.silentSystemTurn();
    folds.noteTurn(kChat);
  }
  FoldDue pump() {
    bool th = false;
    const FoldDue due = folds.evaluateDue(kChat, kCompactBytes, kFoldMsgsThreshold, epoch += 60, &th);
    if (th) thrash++;
    if (due != FoldDue::Yes) return due;
    const ChatFold f = folds.get(kChat);
    agent::HistoryOptions o;
    o.startAfter = f.thru;
    sliceCap = std::min(deriveBudget(200000, {}).foldSliceBytes, foldDigestCap(f.summary.size()));
    const std::vector<EpisodicMessage> read = chat.read(f.thru, 400);
    const agent::FoldSlice s = agent::foldSlice(agent::historyTurns(read, o), hist.lowBytes(), sliceCap, false, 0);
    if (s.digest.empty()) {
      folds.noteFoldSkipped(kChat, read.empty() ? std::string() : read.front().id);   // the newest row it read
      skips++;
      return due;
    }
    folds.applyFold(kChat, f.summary, ++epoch, s.thru);
    lastDigest = s.digest.size();
    folds_run++;
    return due;
  }
  bool hidden() const {
    const ChatFold f = folds.get(kChat);
    return !f.softStart.empty() && (f.thru.empty() || rowIdAfter(f.softStart, f.thru));
  }
};

// Lumi before the update: weeks of chat, a summary from an older fold, and no
// histV2 anchor yet (v4.5.16 never wrote one).
void seedLumi(Device& d, int exchanges = 360, size_t replyBase = 300, size_t replyStep = 60) {
  for (int i = 0; i < exchanges; i++) d.chat.exchange(i, replyBase + (size_t)(i % 5) * replyStep);
  std::string summary = "1. Decisions & preferences\n";
  while (summary.size() < 2942) summary += "- Logs meals with kcal and protein; UK time.\n";
  d.folds.applyFold(kChat, summary, 1792000000);   // the old image's fold: no anchor
  for (int i = 0; i < 12; i++) d.folds.noteTurn(kChat);
}

}  // namespace

// The replay. Before the v4.6.1 fix the pump after the first owner turn returned
// ThrashPaused (the owner notice) although the fold had just completed fine. Since
// lane F a thrash is only a 15 min wait; this backlog raises none at all.
static void test_lumi_first_histv2_hour_raises_no_thrash() {
  Device d;
  seedLumi(d);
  // uptime 120 s: the awareness turn's history shifts ("overflow shift to ...").
  d.awarenessTurn();
  TEST_ASSERT_FALSE(d.folds.get(kChat).softStart.empty());
  TEST_ASSERT_TRUE(d.hidden());
  // uptime 378-452 s: the fold is due on the hidden rows and runs at its digest cap.
  TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)d.pump());
  TEST_ASSERT_EQUAL_INT(1, d.folds_run);
  // The cause, pinned: the capped slice leaves the anchor before the shift, so the
  // hidden rows are still due (a backlog), not a fold that failed to shrink anything.
  TEST_ASSERT_TRUE(d.hidden());
  const std::string firstAnchor = d.folds.get(kChat).thru;
  TEST_ASSERT_TRUE(agent::rowIdAfter(d.folds.get(kChat).softStart, firstAnchor));
  // uptime 460-528 s: the owner's turn. The next pump folds the rest, no alarm.
  d.ownerTurn(1000);
  TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)d.pump());
  TEST_ASSERT_TRUE(agent::rowIdAfter(d.folds.get(kChat).thru, firstAnchor));   // progress
  // A day of normal use afterwards: never a thrash, and the backlog is gone.
  for (int i = 0; i < 60; i++) {
    d.ownerTurn(1001 + i);
    d.pump();
  }
  TEST_ASSERT_EQUAL_INT(0, d.thrash);
  TEST_ASSERT_FALSE(d.hidden());
  TEST_ASSERT_EQUAL_UINT32(0, d.folds.get(kChat).nextTry);
  // Bounded work: the catch-up (the slice read is 400 rows, so at most a few capped
  // folds) plus one fold per later history shift, never a fold per pump pass.
  printf("replay: folds=%d skips=%d shifts=%d cap=%u\n", d.folds_run, d.skips, d.shifts, (unsigned)d.sliceCap);
  TEST_ASSERT_TRUE(d.folds_run + d.skips <= d.shifts + 3);
}

// The second way a fold left rows hidden (found in review): with short exchanges the
// history window shifts on its ROW budget (64 high, 32 low) while the fold keeps a
// BYTE tail (half the history budget), so even an uncapped fold can end before the
// shift. v4.6.0 alarmed on the next turn here too; now there is no alarm and the
// hidden rows clear.
static void test_short_rows_shift_raises_no_thrash() {
  Device d;
  d.turnReply = 80;
  seedLumi(d, 120, 60, 15);
  d.awarenessTurn();
  TEST_ASSERT_TRUE(d.hidden());
  TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)d.pump());
  printf("short rows: digest=%u cap=%u hidden-after-fold=%d\n", (unsigned)d.lastDigest, (unsigned)d.sliceCap,
         (int)d.hidden());
  TEST_ASSERT_TRUE(d.lastDigest < d.sliceCap);   // not the capped-slice path
  for (int i = 0; i < 40; i++) {
    d.ownerTurn(2000 + i);
    d.pump();
  }
  TEST_ASSERT_EQUAL_INT(0, d.thrash);
  TEST_ASSERT_FALSE(d.hidden());
  TEST_ASSERT_EQUAL_UINT32(0, d.folds.get(kChat).nextTry);
}

// The same chat with no backlog: a fold, then ordinary turns, never an alarm and
// never a second fold until the history really grows again.
static void test_fold_then_normal_turns_no_notice() {
  Device d;
  for (int i = 0; i < 30; i++) d.ownerTurn(i);
  int dues = 0;
  for (int i = 0; i < 200; i++) {
    d.ownerTurn(100 + i);
    if (d.pump() == FoldDue::Yes) dues++;
  }
  printf("normal: dues=%d shifts=%d\n", dues, d.shifts);
  TEST_ASSERT_EQUAL_INT(0, d.thrash);
  TEST_ASSERT_TRUE(dues > 3);              // the generator really reaches the trigger
  TEST_ASSERT_TRUE(dues <= d.shifts + 1);  // at most one fold per history shift
}

// A turn that left out history to fit asks for a fold. Soon after a fold that is not a refill:
// no thrash wait, no alert, however many large turns come in a row.
static void test_trimmed_turns_never_trip_the_thrash_guard() {
  FakeIO io;
  FoldStore fs;
  fs.begin(&io);
  uint32_t now = 1793026543;
  fs.noteTurn("c");
  fs.noteHistoryTrimmed("c");   // no fold yet: it is due
  TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)fs.evaluateDue("c", kCompactBytes, kFoldMsgsThreshold, now));
  fs.applyFold("c", "summary", now, "m00000010");
  int thrash = 0, dues = 0;
  for (int turn = 0; turn < 40; turn++) {
    now += 60;
    fs.noteTurn("c");
    fs.noteHistoryTrimmed("c");
    bool th = false;
    const FoldDue due = fs.evaluateDue("c", kCompactBytes, kFoldMsgsThreshold, now, &th);
    thrash += th ? 1 : 0;
    if (due == FoldDue::Yes) {
      dues++;
      fs.applyFold("c", "summary", now, "m" + std::to_string(10000000 + turn));
    }
  }
  TEST_ASSERT_EQUAL_INT(0, thrash);
  TEST_ASSERT_TRUE_MESSAGE(dues > 0, "a chat that keeps losing history is still folded");
  TEST_ASSERT_TRUE_MESSAGE(dues <= 40 / ((int)kThrashTurns + 1), "at most one paid fold per thrash window");
}

// Counter-test (the dimension the guard no longer pauses on): a real refill - the
// restarted counters over the threshold again right after a fold - folds once and
// waits 15 minutes for the next try, however often it is asked; never a pause.
static void test_real_refill_only_schedules() {
  FakeIO io;
  FoldStore fs;
  fs.begin(&io);
  const uint32_t t0 = 1793026543;
  fs.noteMessage("c", 60000);
  fs.noteTurn("c");
  TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)fs.evaluateDue("c", kCompactBytes, kFoldMsgsThreshold, t0));
  fs.applyFold("c", "sum", t0, "m00000010");
  fs.noteMessage("c", 60000);   // one huge turn refills it at once
  fs.noteTurn("c");
  fs.setSoftStart("c", "m00000040");   // hidden rows on top do not skip the wait
  int dues = 0, thrash = 0;
  for (int i = 0; i < 20; i++) {
    bool th = false;
    dues += fs.evaluateDue("c", kCompactBytes, kFoldMsgsThreshold, t0 + 10 + (uint32_t)i, &th) == FoldDue::Yes;
    thrash += th ? 1 : 0;
  }
  TEST_ASSERT_EQUAL_INT(1, dues);
  TEST_ASSERT_EQUAL_INT(1, thrash);
  TEST_ASSERT_EQUAL_UINT32(t0 + 10 + kFoldThrashRetryS, fs.get("c").nextTry);
  TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes,
                        (int)fs.evaluateDue("c", kCompactBytes, kFoldMsgsThreshold, t0 + 10 + kFoldThrashRetryS));
  // The message-count trigger is a refill too.
  FoldStore fm;
  FakeIO io2;
  fm.begin(&io2);
  fm.applyFold("m", "sum", t0, "m00000010");
  for (uint32_t i = 0; i < kFoldMsgsThreshold; i++) fm.noteMessage("m", 10);
  fm.noteTurn("m");
  bool th = false;
  TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)fm.evaluateDue("m", kCompactBytes, kFoldMsgsThreshold, t0 + 5, &th));
  TEST_ASSERT_TRUE(th);
  TEST_ASSERT_EQUAL_INT((int)FoldDue::No, (int)fm.evaluateDue("m", kCompactBytes, kFoldMsgsThreshold, t0 + 6));
}

// One slot as v4.6.0 wrote it (the blob format is unchanged): chatId, bytes, msgs,
// epoch;thru, turns;softStart, fails, paused, summary. v4.6.0 wrote every pause as
// paused=1; a v4.6.1 thrash pause is paused=2, which v4.6.0 still reads as paused.
std::string legacySlot(const std::string& chat, uint32_t bytes, uint32_t turns, const std::string& paused) {
  return chat + "\x1F" + std::to_string(bytes) + "\x1F" "3\x1F" "1000;m00000010\x1F" + std::to_string(turns) +
         ";m00000040\x1F" "0\x1F" + paused + "\x1F" "sum\x1E";
}

// Every pause an older image left (v4.6.0 paused=1, v4.6.1+ paused=2) lifts at the
// first evaluation after the update, with the counters under the threshold and over
// it, and the next write stores it unpaused.
static void test_legacy_pauses_heal_on_load() {
  for (const char* paused : {"1", "2"}) {
    for (uint32_t bytes : {500u, 60000u}) {
      FakeIO io;
      io.blob = legacySlot("c", bytes, 12, paused);
      FoldStore fs;
      fs.begin(&io);
      TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes,
                            (int)fs.evaluateDue("c", kCompactBytes, kFoldMsgsThreshold, 1793026543));
      fs.noteTurn("c");   // the next write
      TEST_ASSERT_TRUE(io.blob.find("\x1F" "0\x1F" "0\x1F" "sum") != std::string::npos);
    }
  }
}

// ...and within kThrashTurns of its fold a refilled legacy pause is a thrash: it folds
// once and waits 15 min, with no notice and no pause.
static void test_legacy_pause_within_thrash_turns_only_waits() {
  FakeIO io;
  io.blob = legacySlot("c", 60000, kThrashTurns, "1");
  FoldStore fs;
  fs.begin(&io);
  const uint32_t t0 = 1793026543;
  int dues = 0;
  for (int i = 0; i < 5; i++) dues += fs.evaluateDue("c", kCompactBytes, kFoldMsgsThreshold, t0) == FoldDue::Yes;
  TEST_ASSERT_EQUAL_INT(1, dues);
  TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes,
                        (int)fs.evaluateDue("c", kCompactBytes, kFoldMsgsThreshold, t0 + kFoldThrashRetryS));
}

// A thrash wait survives a reboot and ends on time; a failed cycle schedules a retry
// by the backoff (this replaced v5.1.1's failure pause, which never lifted).
static void test_waits_survive_reboot_and_a_failure_schedules_a_retry() {
  FakeIO io;
  FoldStore fs;
  fs.begin(&io);
  const uint32_t t0 = 1793026543;
  fs.applyFold("c", "sum", t0, "m00000010");
  fs.noteMessage("c", 60000);
  fs.noteTurn("c");
  TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)fs.evaluateDue("c", kCompactBytes, kFoldMsgsThreshold, t0 + 1));
  fs.setSoftStart("c", "m00000040");
  for (int i = 0; i < 6; i++) fs.noteTurn("c");
  for (int boot = 0; boot < 3; boot++) {
    FakeIO next;
    next.blob = io.blob;
    FoldStore after;
    after.begin(&next);
    TEST_ASSERT_EQUAL_INT((int)FoldDue::No, (int)after.evaluateDue("c", kCompactBytes, kFoldMsgsThreshold, t0 + 2));
    TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes,
                          (int)after.evaluateDue("c", kCompactBytes, kFoldMsgsThreshold, t0 + 1 + kFoldThrashRetryS));
    io.blob = next.blob;
  }
  // A failure schedules a retry: three failed cycles wait 15 min, 1 h, then 4 h.
  FakeIO io4;
  FoldStore failed;
  failed.begin(&io4);
  failed.setSoftStart("f", "m00000040");
  uint32_t now = t0;
  for (int i = 0; i < 3; i++) now = failed.noteCycleFailed("f", now);
  TEST_ASSERT_EQUAL_UINT32(t0 + 15 * 60 + 3600 + 4 * 3600, now);
  for (int i = 0; i < 4; i++) failed.noteTurn("f");
  TEST_ASSERT_EQUAL_INT((int)FoldDue::No, (int)failed.evaluateDue("f", kCompactBytes, kFoldMsgsThreshold, now - 1));
  TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)failed.evaluateDue("f", kCompactBytes, kFoldMsgsThreshold, now));
}

// A fold with no anchor (the pre-histV2 path) leaves no hidden rows behind, so the
// hidden trigger can never re-fire on folds that cannot move the anchor (the loop the
// old guard used to cut short).
static void test_anchorless_fold_clears_the_shift() {
  FakeIO io;
  FoldStore fs;
  fs.begin(&io);
  fs.setSoftStart("c", "m00000040");
  TEST_ASSERT_EQUAL_INT((int)FoldDue::Yes, (int)fs.evaluateDue("c", kCompactBytes, kFoldMsgsThreshold, 1000));
  fs.applyFold("c", "a summary of the whole live conversation", 1000);
  TEST_ASSERT_EQUAL_STRING("", fs.get("c").softStart.c_str());
  int dues = 0;
  for (int i = 0; i < 10; i++) {
    fs.noteTurn("c");
    dues += fs.evaluateDue("c", kCompactBytes, kFoldMsgsThreshold, 1000 + (uint32_t)i) != FoldDue::No;
  }
  TEST_ASSERT_EQUAL_INT(0, dues);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_lumi_first_histv2_hour_raises_no_thrash);
  RUN_TEST(test_short_rows_shift_raises_no_thrash);
  RUN_TEST(test_fold_then_normal_turns_no_notice);
  RUN_TEST(test_real_refill_only_schedules);
  RUN_TEST(test_legacy_pauses_heal_on_load);
  RUN_TEST(test_legacy_pause_within_thrash_turns_only_waits);
  RUN_TEST(test_waits_survive_reboot_and_a_failure_schedules_a_retry);
  RUN_TEST(test_anchorless_fold_clears_the_shift);
  RUN_TEST(test_trimmed_turns_never_trip_the_thrash_guard);
  return UNITY_END();
}
