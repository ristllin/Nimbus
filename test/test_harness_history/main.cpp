#include <unity.h>

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "nimbus/harness/conversation.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/turn_inbox.h"
#include "nimbus/orch/episodic.h"
#include "nimbus/orch/media_label.h"   // the one label the receive path writes (lane D)
#include "nimbus/orch/msg_batch.h"     // the Telegram batch a turn comes from (lane D)
#include "../support/fake_config.h"
#include "../support/fake_platform.h"

// histV2 through the real TurnEngine: the owner row's tags and stored text (H1,
// photo and voice stored once), exactly-once capture by source (H7), the trigger
// row of a seedless turn (row 58), out: kinds on assistant rows, the engine's
// RECENT CONVERSATION (H2) and its overflow shift (H3), and the switch off being
// byte-identical to an engine that never heard of histV2.

using namespace nimbus::orch;
using agent::HistoryMarks;
using agent::TurnEngine;
using harness_test::FakeConfig;
using harness_test::FakePlatform;

void setUp() {}
void tearDown() {}

namespace {

constexpr uint32_t kH = 489600 + 9;   // 2025-11-08 09:00 UTC: a synced clock

struct Rig {
  FakePlatform plat;
  FakeConfig cfg;
  InMemoryEpisodicStore store;
  int seq = 0;
  bool v2 = true;
  bool wireV2 = true;
  std::string reply = "{\"reply\":\"ok\",\"memory\":\"\",\"ask\":\"\"}";
  std::vector<std::string> instructions;   // what each provider call received
  std::vector<std::pair<std::string, std::string>> legacyCaptures;   // episodicCaptureUser
  std::string turnTag;                     // the in-flight turn's id (rows' turn: tag)
  HistoryMarks marks;
  int shifts = 0;
  uint32_t hour = kH;          // the hour rows are stored at
  int32_t offMin = 0;          // the device's zone now: minutes east of UTC
  agent::OffsetAt zone;        // ... or the offset valid at a unix time (DST); replaces offMin
  std::unique_ptr<TurnEngine> eng;

  std::string add(const std::string& chat, const std::string& role, MsgKind kind,
                  const std::string& text, const std::string& tags, const std::string& blob = "") {
    EpisodicMessage m;
    char id[16];
    snprintf(id, sizeof(id), "m%08x", (unsigned)(++seq));
    m.id = id;
    m.sessionId = chat;
    m.tsHours = hour;
    m.role = role;
    m.kind = kind;
    m.text = text;
    m.tags = tags;
    m.blobPath = blob;
    store.addMessage(m);
    return m.id;
  }
  std::vector<EpisodicMessage> rows(const std::string& chat, size_t limit, const std::string& before) const {
    MsgQuery q;
    q.sessionId = chat;
    q.haveKind = true;
    q.kind = MsgKind::Message;
    q.alsoKinds = {MsgKind::Image, MsgKind::File, MsgKind::Audio};
    q.limit = (int)limit;
    q.before = before;
    return store.query(q);
  }
  std::vector<EpisodicMessage> all(const std::string& chat) const { return rows(chat, 500, ""); }

  explicit Rig(bool wire = true) : wireV2(wire) {
    cfg.toolLoop = false;
    cfg.tts = true;
    TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    d.deliver = [](const std::string&, const std::string&) {};
    d.utcOffsetMin = [this] { return zone ? zone((uint32_t)plat.epoch) : offMin; };
    d.utcOffsetAt = [this](uint32_t t) { return zone ? zone(t) : offMin; };
    d.composeInputs = [](const std::string&) {
      agent::ComposeInputs in;
      in.devName = "Nimbus";
      in.recentConversation = "\n## RECENT CONVERSATION (device window)\n";
      return in;
    };
    d.episodicCaptureUser = [this](const std::string& c, const std::string& t, const std::string& f) {
      legacyCaptures.push_back({t, f});
      turnTag = add(c, "user", MsgKind::Message, t, f);
    };
    d.hooks.onTurnStart = [this](const agent::TurnStartEv& ev) {
      if (ev.source != agent::TurnSource::Owner && ev.source != agent::TurnSource::Serial) {
        char id[16];
        snprintf(id, sizeof(id), "m%08x", (unsigned)(++seq));   // minted from the same counter
        turnTag = id;
      }
    };
    d.apply.deliver = d.deliver;
    d.apply.principalFor = [](const std::string& chat) { return principalForRole(chat, Role::Admin); };
    d.apply.speak = [](const std::string&) {};
    d.apply.ttsEnabled = true;
    d.apply.captureAssistant = [this](const std::string& c, const std::string& t) {
      add(c, "assistant", MsgKind::Message, t, eng->assistantRowTags() + ",turn:" + turnTag);
    };
    d.hosts.add("anthropic", [this](std::string&, const std::string& ins, const std::string&, std::string& out,
                                    std::string&, const agent::HeadTools*, TokenUsage*) {
      instructions.push_back(ins);
      out = reply;
      return true;
    });
    if (wireV2) {
      d.histV2 = [this] { return v2; };
      d.historyRows = [this](const std::string& c, size_t limit, const std::string& before) {
        return rows(c, limit, before);
      };
      d.captureUserRow = [this](const std::string& c, const std::string& t, const std::string& tags) {
        turnTag = add(c, "user", MsgKind::Message, t, tags);
        return turnTag;
      };
      d.captureTrigger = [this](const std::string& c, const std::string& t, const std::string& tags) {
        add(c, "system", MsgKind::Message, t, tags + ",turn:" + turnTag);
      };
      d.historyMarks = [this](const std::string&) { return marks; };
      d.noteHistoryShift = [this](const std::string&, const std::string& s) {
        marks.softStart = s;
        shifts++;
      };
    }
    eng.reset(new TurnEngine(std::move(d)));
  }

  std::string lastWindow() const {
    const std::string& ins = instructions.back();
    const size_t at = ins.find("\n## RECENT CONVERSATION");
    if (at != std::string::npos) {   // histV1: the text window rides the dyn part
      const size_t end = ins.find("\n## ", at + 5);
      return ins.substr(at, end == std::string::npos ? std::string::npos : end - at);
    }
    // histV2/R3: the history rides as real messages; render them in the
    // window's line shape so the same assertions keep reading them.
    const agent::layout::PromptParts p = agent::layout::split(ins);
    std::string out;
    for (const auto& m : p.history) {
      out += m.user ? "- user: " : "- assistant: ";
      out += m.text;
      if (!out.empty() && out.back() != '\n') out += '\n';
    }
    return out;
  }
};

size_t count(const std::string& hay, const std::string& needle) {
  size_t n = 0;
  for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) n++;
  return n;
}

const char* kRef = "00000000abc00001";

}  // namespace

// H1 + stored once: the channel's note gives the owner row what the sender sent
// and the turn's media refs; the media row keeps the description.
static void test_owner_row_tags_and_stored_text() {
  Rig r;
  r.add("123", "user", MsgKind::Image, "[image] a bowl of ramen", "", std::string("/mem/blobs/") + kRef + ".jpg");
  agent::history::Inbound in;
  in.chatId = "123";
  in.said = "[image]\n\nThey wrote: lunch";
  in.refs = {kRef};
  in.src = "tg:9001x1";
  in.sentUnix = kH * 3600u + 120;
  const std::string routed = "[image] a bowl of ramen\n\nThey wrote: lunch";
  agent::history::noteInbound(in, routed);
  r.eng->handleMessage(routed, "Roy", "123");
  const auto rows = r.all("123");
  // newest first: assistant, user, image
  TEST_ASSERT_EQUAL(3, (int)rows.size());
  const EpisodicMessage& user = rows[1];
  TEST_ASSERT_EQUAL_STRING("[image]\n\nThey wrote: lunch", user.text.c_str());
  TEST_ASSERT_EQUAL_STRING(kRef, agent::tagValue(user.tags, agent::kAttTag).c_str());
  TEST_ASSERT_EQUAL_STRING("tg:9001x1", agent::tagValue(user.tags, agent::kSrcTag).c_str());
  TEST_ASSERT_TRUE(agent::tagValue(user.tags, "from:") == "Roy");
  TEST_ASSERT_TRUE(r.legacyCaptures.empty());
  // [USER] still carries the description this turn; the window left the photo out.
  TEST_ASSERT_TRUE(r.instructions.back().find("RECENT CONVERSATION (device window)") == std::string::npos);
  TEST_ASSERT_EQUAL(0, (int)count(r.lastWindow(), "bowl of ramen"));
  // The assistant row says it is the reply.
  TEST_ASSERT_EQUAL_STRING("reply", agent::tagValue(rows[0].tags, agent::kOutTag).c_str());
  // Next turn: the photo renders once, in place, in the history.
  r.eng->handleMessage("and dinner?", "Roy", "123");
  TEST_ASSERT_EQUAL(1, (int)count(r.lastWindow(), "a bowl of ramen"));
  TEST_ASSERT_TRUE(r.lastWindow().find("[image] a bowl of ramen\n\nThey wrote: lunch") != std::string::npos);
}

// Critique B2 (G1): an image row in the window is capped far below a text
// message - the ref names the photo and image.look reads it again, so a
// machine description only has to identify and summarize it.
static void test_image_row_is_capped_to_about_300_chars() {
  Rig r;
  const char* ref = "00000000abc00099";
  const std::string longDesc = std::string(2000, 'd');
  r.add("123", "user", MsgKind::Image, "[image] " + longDesc, "",
        std::string("/mem/blobs/") + ref + ".jpg");
  agent::history::Inbound in;
  in.chatId = "123";
  in.said = "[image]\n\nwhat was in that photo?";
  in.refs = {ref};
  in.src = "tg:9002x1";
  const std::string routed = "[image] " + longDesc + "\n\nwhat was in that photo?";
  agent::history::noteInbound(in, routed);
  r.reply = "{\"reply\":\"ok\",\"memory\":\"\",\"ask\":\"\",\"device\":[]}";
  r.eng->handleMessage(routed, "Roy", "123");
  r.eng->handleMessage("thanks", "Roy", "123");
  const std::string w = r.lastWindow();
  // The image row's user line: the label plus at most about 300 chars of the
  // description, its cut visible.
  const size_t at = w.find("[image] ");
  TEST_ASSERT_TRUE(at != std::string::npos);
  const size_t eol = w.find('\n', at);
  TEST_ASSERT_TRUE(eol - at <= 8 + nimbus::orch::kImageRowMaxChars + 16);
  TEST_ASSERT_TRUE(w.find(std::string(400, 'd')) == std::string::npos);  // the bulk is gone
  TEST_ASSERT_TRUE(w.find("what was in that photo?") != std::string::npos);  // the text after
}

// H7 / T8: random replay sequences (the same source delivered k times among
// others) give exactly one owner row per source, and a duplicate makes no
// provider call.
static void test_exactly_once_capture_by_source() {
  uint32_t rng = 7;
  auto rnd = [&](uint32_t n) { rng = rng * 1103515245u + 12345u; return (rng >> 8) % n; };
  for (int run = 0; run < 20; run++) {
    Rig r;
    std::map<std::string, int> delivered;
    for (int i = 0; i < 30; i++) {
      const std::string src = "tg:" + std::to_string(100 + rnd(8)) + "x1";
      agent::history::Inbound in;
      in.chatId = "555";
      in.src = src;
      agent::history::noteInbound(in, "message " + src);
      r.eng->handleMessage("message " + src, "Roy", "555");
      delivered[src]++;
    }
    std::map<std::string, int> rowsPerSrc;
    for (const auto& m : r.all("555"))
      if (m.role == "user") rowsPerSrc[agent::tagValue(m.tags, agent::kSrcTag)]++;
    for (const auto& kv : delivered) TEST_ASSERT_EQUAL_MESSAGE(1, rowsPerSrc[kv.first], kv.first.c_str());
    TEST_ASSERT_EQUAL((int)delivered.size(), (int)r.instructions.size());
  }
  // A note for another chat is stale: never applied to this one.
  Rig r;
  agent::history::Inbound other;
  other.chatId = "999";
  other.src = "tg:1x1";
  agent::history::noteInbound(other, "hello");
  r.eng->handleMessage("hello", "Roy", "555");
  TEST_ASSERT_EQUAL_STRING("", agent::tagValue(r.all("555")[1].tags, agent::kSrcTag).c_str());
}

// A note whose turn never reached the engine (a refused caption, a command) is
// stale: the chat's next turn, carrying other text, stores its own words and
// none of the note's refs, source or text.
static void test_stale_note_never_applies_to_another_turn() {
  Rig r;
  agent::history::Inbound in;
  in.chatId = "123";
  in.said = "[image]\n\nThey wrote: a refused caption";
  in.refs = {kRef};
  in.src = "tg:7x1";
  agent::history::noteInbound(in, "[image] something\n\nThey wrote: a refused caption");
  // ... the gate refused that turn, so the engine never took the note ...
  r.eng->handleMessage("ok thanks", "Roy", "123");
  const EpisodicMessage user = r.all("123")[1];
  TEST_ASSERT_EQUAL_STRING("ok thanks", user.text.c_str());
  TEST_ASSERT_EQUAL_STRING("", agent::tagValue(user.tags, agent::kAttTag).c_str());
  TEST_ASSERT_EQUAL_STRING("", agent::tagValue(user.tags, agent::kSrcTag).c_str());
  // The slot is empty afterwards: a later matching turn finds nothing either.
  agent::history::Inbound left;
  TEST_ASSERT_FALSE(agent::history::takeInbound("123", "[image] something\n\nThey wrote: a refused caption", left));
}

// Row 58: a routine that replies leaves a trigger row ahead of its reply; a
// quiet routine that stays silent now leaves its trace too (22 Oct amendment,
// SILENT-ROUTINE-COST): the still-pending trigger row lands, tagged with why the
// run stayed silent, so the fire never reads as a lost turn.
static void test_seedless_turn_trigger_row() {
  Rig r;
  r.eng->injectScheduledTurn("123", "check the trains", "commute", "l7", false, false, false);
  auto rows = r.all("123");
  TEST_ASSERT_EQUAL(2, (int)rows.size());
  TEST_ASSERT_EQUAL_STRING("[routine \"commute\" ran]", rows[1].text.c_str());
  TEST_ASSERT_TRUE(agent::hasTagItem(rows[1].tags, agent::kTriggerTag));
  TEST_ASSERT_EQUAL_STRING("loop:l7", agent::tagValue(rows[1].tags, agent::kSrcTag).c_str());
  TEST_ASSERT_EQUAL_STRING(agent::tagValue(rows[1].tags, "turn:").c_str(),
                           agent::tagValue(rows[0].tags, "turn:").c_str());
  r.reply = "{\"reply\":\"\",\"memory\":\"\",\"ask\":\"\"}";
  r.eng->injectScheduledTurn("123", "only if needed", "quiet check", "l8", true, false, false);
  auto quiet = r.all("123");
  TEST_ASSERT_EQUAL(3, (int)quiet.size());   // the silent run leaves its row
  TEST_ASSERT_EQUAL_STRING("[routine \"quiet check\" ran]", quiet[0].text.c_str());
  TEST_ASSERT_TRUE(agent::hasTagItem(quiet[0].tags, agent::kTriggerTag));
  TEST_ASSERT_TRUE_MESSAGE(agent::hasTagItem(quiet[0].tags, "silent:quiet-routine"),
                           quiet[0].tags.c_str());   // why it stayed silent is recorded
  // The next owner turn's history shows the routine as its own exchange.
  r.reply = "{\"reply\":\"ok\",\"memory\":\"\",\"ask\":\"\"}";
  r.eng->handleMessage("thanks", "Roy", "123");
  TEST_ASSERT_TRUE(r.lastWindow().find("[routine \"commute\" ran]\n- assistant: [did: nothing] ok") !=
                   std::string::npos);
}

// out: kinds: the spoken line, the reply and the question each say what they are.
// One row per message the owner received (lane FSN): an appended question rides the
// reply's row, so it is tagged reply; only a question sent on its own is tagged ask.
// The model-facing history reads the same as with two rows.
static void test_assistant_rows_carry_out_kinds() {
  Rig r;
  r.reply = "{\"reply\":\"Market at 9.\",\"memory\":\"\",\"ask\":\"Set a reminder?\","
            "\"device\":[{\"type\":\"tts\",\"text\":\"Market at nine.\"}]}";
  r.eng->handleMessage("plan Saturday", "Roy", "123");
  r.reply = "{\"reply\":\"\",\"memory\":\"\",\"ask\":\"Which stall first?\"}";
  r.eng->handleMessage("and Sunday?", "Roy", "123");
  std::map<std::string, std::string> kinds;
  int assistantRows = 0;
  for (const auto& m : r.all("123"))
    if (m.role == "assistant") {
      kinds[m.text] = agent::tagValue(m.tags, agent::kOutTag);
      assistantRows++;
    }
  TEST_ASSERT_EQUAL(3, assistantRows);   // tts, reply+question, question
  TEST_ASSERT_EQUAL_STRING("tts", kinds["Market at nine."].c_str());
  TEST_ASSERT_EQUAL_STRING("reply", kinds["Market at 9.\n\nSet a reminder?"].c_str());
  TEST_ASSERT_EQUAL_STRING("ask", kinds["Which stall first?"].c_str());
  r.reply = "{\"reply\":\"ok\",\"memory\":\"\",\"ask\":\"\"}";
  r.eng->handleMessage("thanks", "Roy", "123");
  const std::string w = r.lastWindow();
  // Lane G1's message-level history rides the [did: ...] tag inside the
  // assistant message ("[did: spoke] Market at 9." - conversation.cpp
  // renderHistory); the reply and its appended question are still ONE
  // assistant message, and the written reply still beats the spoken one.
  TEST_ASSERT_TRUE(w.find("- assistant: [did: spoke] Market at 9.\n\nSet a reminder?\n") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(w.find("Market at nine.") == std::string::npos);   // written beats spoken
  TEST_ASSERT_TRUE(w.find("- assistant: [did: nothing] Which stall first?") != std::string::npos);
}

// The switch off is byte-identical to an engine that never heard of histV2: the
// same prompt, the same legacy capture, no new tags.
static void test_switch_off_is_byte_identical() {
  Rig off;
  off.v2 = false;
  Rig never(false);
  for (Rig* r : {&off, &never}) {
    agent::history::Inbound in;
    in.chatId = "123";
    in.src = "tg:5x1";
    agent::history::noteInbound(in, "hello");
    r->eng->handleMessage("hello", "Roy", "123");
    r->eng->handleMessage("again", "Roy", "123");
  }
  TEST_ASSERT_EQUAL(2, (int)off.instructions.size());
  TEST_ASSERT_EQUAL_STRING(never.instructions[1].c_str(), off.instructions[1].c_str());
  TEST_ASSERT_TRUE(off.instructions[1].find("(device window)") != std::string::npos);
  TEST_ASSERT_EQUAL(2, (int)off.legacyCaptures.size());
  const auto a = off.all("123"), b = never.all("123");
  TEST_ASSERT_EQUAL(b.size(), a.size());
  for (size_t i = 0; i < a.size(); i++) {
    TEST_ASSERT_EQUAL_STRING(b[i].text.c_str(), a[i].text.c_str());
    TEST_ASSERT_EQUAL_STRING(b[i].tags.c_str(), a[i].tags.c_str());
    TEST_ASSERT_TRUE(a[i].tags.find("out:") == std::string::npos);
  }
}

// H3: past high water the oldest turns drop once (the shift is recorded and
// persisted by the device), and the next turn's history starts after it: the
// window is stable across turns between shifts and across a restart (a fresh
// engine over the same rows and marks renders the same block).
static void test_overflow_shift_is_recorded_and_stable() {
  Rig r;
  const std::string longText(700, 'w');
  for (int i = 0; i < 14; i++) r.eng->handleMessage("q" + std::to_string(i) + " " + longText, "Roy", "123");
  TEST_ASSERT_TRUE(r.shifts >= 1);
  TEST_ASSERT_TRUE(r.shifts <= 4);   // steps of about high - low bytes, not one per turn
  const std::string soft = r.marks.softStart;
  TEST_ASSERT_FALSE(soft.empty());
  // The window holds only rows after the shift.
  TEST_ASSERT_TRUE(r.lastWindow().find("q0 ") == std::string::npos);
  TEST_ASSERT_TRUE(r.lastWindow().find("q12 ") != std::string::npos);
  // A restart: a new engine over the same store and marks renders the same history.
  Rig fresh;
  fresh.store = r.store;
  fresh.seq = r.seq;
  fresh.marks = r.marks;
  const std::string before = r.lastWindow();
  r.eng->handleMessage("probe", "Roy", "123");
  fresh.eng->handleMessage("probe", "Roy", "123");
  const std::string a = r.lastWindow(), b = fresh.lastWindow();
  TEST_ASSERT_EQUAL_STRING(a.c_str(), b.c_str());
  TEST_ASSERT_TRUE(a.compare(0, before.size() - 1, before, 0, before.size() - 1) == 0 || r.marks.softStart != soft);
}

// The fold slice reads after the anchor: rows the summary already covers are
// never digested again.
static void test_fold_slice_starts_after_the_anchor() {
  Rig r;
  for (int i = 0; i < 40; i++) r.eng->handleMessage("note " + std::to_string(i) + std::string(240, 'n'), "Roy", "123");
  const agent::FoldSlice first = r.eng->historyFoldSlice("123", 65536, false);
  TEST_ASSERT_FALSE(first.digest.empty());
  TEST_ASSERT_TRUE(first.digest.find("note 0n") != std::string::npos);
  r.marks.thru = first.thru;
  r.marks.softStart.clear();
  const agent::FoldSlice second = r.eng->historyFoldSlice("123", 65536, true);
  TEST_ASSERT_TRUE(second.digest.find("note 0n") == std::string::npos);
  // Off: no slice at all (the device keeps its previous fold input).
  Rig off;
  off.v2 = false;
  off.eng->handleMessage("x", "Roy", "123");
  TEST_ASSERT_TRUE(off.eng->historyFoldSlice("123", 65536, true).digest.empty());
}


// An album turn from lane D's batcher (three photos, a caption, a text in one bucket)
// is captured once with its refs: the media rows keep the descriptions, the owner row
// keeps the labels and the words with att: refs in album order and the bucket's
// source, the next turn's history shows each description once, and the same page
// served again is dropped before any provider call. An item that left no media row
// keeps its description in the owner row (its only home).
static void test_album_from_the_batcher_is_captured_once_with_its_refs() {
  Rig r;
  nimbus::orch::MessageBatcher b;
  const char* refs[] = {"00000000abc00011", "00000000abc00012", "00000000abc00013"};
  const char* descs[] = {"a red bicycle by a wall", "a bowl of ramen", "a cat on a sofa"};
  for (int k = 0; k < 3; k++) b.addMedia("123", "Roy", k == 0 ? "our weekend" : "", k, 700 + k);
  b.add("123", "Roy", "which one do you like?", 703);
  std::vector<std::string> stored;
  for (int k = 0; k < 3; k++) {
    nimbus::orch::MediaLabel l;
    l.k = k + 1;
    l.n = 3;
    l.ref = refs[k];
    l.detail = descs[k];
    const std::string seg = nimbus::orch::mediaSegment(l);
    b.setMediaText(0, (size_t)k, seg);
    if (k < 2) {   // the third item left no media row (its card write failed)
      r.add("123", "user", MsgKind::Image, seg, "", std::string("/mem/blobs/") + refs[k] + ".jpg");
      stored.push_back(refs[k]);
    } else {
      stored.push_back("");
    }
  }
  const nimbus::orch::PsString routed = b.render(0);
  const std::string text(routed.c_str(), routed.size());
  const agent::history::Inbound in = agent::history::inboundForBucket(b, 0, stored);
  TEST_ASSERT_EQUAL_STRING("tg:703x4", in.src.c_str());
  TEST_ASSERT_EQUAL(2, (int)in.refs.size());
  TEST_ASSERT_TRUE(in.said.find("a red bicycle") == std::string::npos);
  TEST_ASSERT_TRUE(in.said.find("[image 1/3 ref=00000000abc00011]") != std::string::npos);
  TEST_ASSERT_TRUE(in.said.find("a cat on a sofa") != std::string::npos);   // not stored: kept
  TEST_ASSERT_TRUE(in.said.find("which one do you like?") != std::string::npos);
  agent::history::noteInbound(in, text);
  r.eng->handleMessage(text, "Roy", "123");
  TEST_ASSERT_EQUAL(1, (int)r.instructions.size());
  int owner = 0;
  for (const auto& m : r.all("123"))
    if (m.role == "user" && m.kind == MsgKind::Message) {
      owner++;
      TEST_ASSERT_EQUAL_STRING("00000000abc00011.00000000abc00012", agent::tagValue(m.tags, agent::kAttTag).c_str());
      TEST_ASSERT_EQUAL_STRING("tg:703x4", agent::tagValue(m.tags, agent::kSrcTag).c_str());
      TEST_ASSERT_TRUE(m.text.find("a bowl of ramen") == std::string::npos);   // stored once
    }
  TEST_ASSERT_EQUAL(1, owner);
  // The page served again (the same bucket after a restart): dropped, no paid call.
  agent::history::noteInbound(agent::history::inboundForBucket(b, 0, stored), text);
  r.eng->handleMessage(text, "Roy", "123");
  TEST_ASSERT_EQUAL(1, (int)r.instructions.size());
  // The next turn's history: every description once, in place.
  r.eng->handleMessage("thanks", "Roy", "123");
  const std::string w = r.lastWindow();
  for (const char* d : descs) TEST_ASSERT_EQUAL_MESSAGE(1, (int)count(w, d), d);
  TEST_ASSERT_TRUE(w.find("[image 1/3 ref=00000000abc00011] a red bicycle by a wall") != std::string::npos);
  TEST_ASSERT_EQUAL(1, (int)count(w, "- user: "));
}

// A text-only page from the batcher: its source rides the owner row (H7 on the
// device for text), and an inject with no update ids has none.
static void test_text_bucket_source_and_inject_without_ids() {
  nimbus::orch::MessageBatcher b;
  b.add("555", "Roy", "hello", 9001);
  agent::history::Inbound in = agent::history::inboundForBucket(b, 0, {});
  TEST_ASSERT_EQUAL_STRING("tg:9001x1", in.src.c_str());
  TEST_ASSERT_TRUE(in.said.empty());   // nothing stored: the routed text is what was sent
  TEST_ASSERT_TRUE(in.refs.empty());
  nimbus::orch::MessageBatcher web;
  web.add("web", "web", "from the web chat");
  TEST_ASSERT_TRUE(agent::history::inboundForBucket(web, 0, {}).src.empty());
}

namespace {
constexpr uint32_t kSatEve = (kH - 9 + 22) * 3600u + 59 * 60 + 30;   // Sat 8 Nov 2025 22:59:30 UTC

// Two identical owner rows 50 s apart across 23:00 UTC, no reply between (a failed turn),
// each stamped the way the engine stamps it (at:), then one more owner turn.
void dinnerEitherSideOf2300Utc(Rig& r) {
  r.plat.epoch = kSatEve + 3600u;
  for (uint32_t sec : {kSatEve, kSatEve + 50u}) {
    r.hour = sec / 3600u;
    r.add("123", "user", MsgKind::Message, "dinner 600", "from:Roy,at:" + std::to_string(sec));
  }
  r.hour = (kSatEve + 3600u) / 3600u;
  r.eng->handleMessage("and now?", "Roy", "123");
}
}  // namespace

// CUM-779: the history reader folds identical owner messages with the owner's local day,
// not the UTC day. 22:59:30 and 23:00:20 UTC are 23:59:30 and 00:00:20 at UTC+1: two
// messages under two days, never "(x2)" under one.
static void test_identical_messages_either_side_of_local_midnight_are_two_messages() {
  Rig plus1;
  plus1.offMin = 60;
  dinnerEitherSideOf2300Utc(plus1);
  const std::string w = plus1.lastWindow();
  TEST_ASSERT_EQUAL_MESSAGE(2, (int)count(w, "dinner 600"), w.c_str());
  TEST_ASSERT_EQUAL(0, (int)count(w, "(x2)"));
  TEST_ASSERT_TRUE_MESSAGE(w.find("(Sat 8 Nov) dinner 600") != std::string::npos, w.c_str());
  TEST_ASSERT_TRUE_MESSAGE(w.find("(Sun 9 Nov) dinner 600") != std::string::npos, w.c_str());
  // Counter-test: at UTC the same two rows share a day and are one repeated message.
  Rig utc;
  dinnerEitherSideOf2300Utc(utc);
  const std::string u = utc.lastWindow();
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)count(u, "dinner 600"), u.c_str());
  TEST_ASSERT_TRUE(u.find("(x2) dinner 600") != std::string::npos);
  // The per-time function is what the reader asks: "now" says UTC, the time says UTC+1.
  Rig dst;
  dst.zone = [](uint32_t) { return 60; };
  dst.offMin = 0;
  dinnerEitherSideOf2300Utc(dst);
  TEST_ASSERT_EQUAL(2, (int)count(dst.lastWindow(), "dinner 600"));
}

// CUM-781 through the engine: the owner's clocks go back at 01:00 UTC on Sun 9 Nov (a
// stand-in for a DST change). A row at 23:30 UTC on Saturday was 00:30 on Sunday when it was
// stored, and the history still says so once the zone reads UTC.
static void test_history_days_use_the_zone_in_force_when_the_row_was_stored() {
  const uint32_t change = (kH - 9 + 25) * 3600u;   // Sun 9 Nov 2025 01:00 UTC
  Rig r;
  r.zone = [change](uint32_t t) { return t < change ? 60 : 0; };
  const uint32_t a = change - 2 * 3600u - 1800u;   // 22:30 UTC Sat = 23:30 Sat local
  const uint32_t early = change - 3600u - 1800u;   // 23:30 UTC Sat = 00:30 Sun local
  const uint32_t b = change - 1u;                  // 00:59:59 UTC Sun = 01:59:59 Sun local
  for (const auto& p : {std::make_pair(a, "first"), std::make_pair(early, "second"), std::make_pair(b, "third")}) {
    r.hour = p.first / 3600u;
    r.add("123", "user", MsgKind::Message, p.second, "from:Roy,at:" + std::to_string(p.first));
  }
  r.plat.epoch = change + 8 * 3600u;
  r.hour = r.plat.epoch / 3600u;
  r.eng->handleMessage("later", "Roy", "123");
  const std::string w = r.lastWindow();
  TEST_ASSERT_TRUE_MESSAGE(w.find("(Sat 8 Nov) first") != std::string::npos, w.c_str());
  TEST_ASSERT_TRUE_MESSAGE(w.find("(Sun 9 Nov) second") != std::string::npos, w.c_str());
  TEST_ASSERT_EQUAL(1, (int)count(w, "(Sun 9 Nov)"));
}

// The whole chain with nothing seeded by hand: web-chat turns arrive while the clock reads a
// chosen second, the engine stamps each owner row, and the next turn's history tells the days
// with the zone in force at each stamp (the clocks go back at 01:00 UTC on Sun 9 Nov here).
static void test_engine_stamped_turns_tell_their_own_days() {
  const uint32_t change = (kH - 9 + 25) * 3600u;   // Sun 9 Nov 2025 01:00 UTC
  Rig r;
  r.zone = [change](uint32_t t) { return t < change ? 60 : 0; };
  const std::pair<uint32_t, const char*> turns[] = {{change - 9000u, "first turn"},     // 22:30 UTC Sat = 23:30 local
                                                    {change - 5400u, "second turn"},    // 23:30 UTC Sat = 00:30 local Sun
                                                    {change + 3600u, "third turn"}};    // 02:00 UTC Sun = 02:00 local
  for (const auto& t : turns) {
    r.plat.epoch = t.first;
    r.hour = t.first / 3600u;
    r.eng->handleMessage(t.second, "Roy", "web");
  }
  r.plat.epoch = change + 8 * 3600u;
  r.hour = r.plat.epoch / 3600u;
  r.eng->handleMessage("and now?", "Roy", "web");
  const std::string w = r.lastWindow();
  TEST_ASSERT_TRUE_MESSAGE(w.find("(Sat 8 Nov) first turn") != std::string::npos, w.c_str());
  TEST_ASSERT_TRUE_MESSAGE(w.find("(Sun 9 Nov) second turn") != std::string::npos, w.c_str());
  TEST_ASSERT_EQUAL(1, (int)count(w, "(Sun 9 Nov)"));   // the third turn opens no new day
  TEST_ASSERT_TRUE(w.find("third turn") != std::string::npos);
  // The same turns in a zone that never changes put the second turn on Saturday.
  Rig fixedZone;
  fixedZone.offMin = 0;
  for (const auto& t : turns) {
    fixedZone.plat.epoch = t.first;
    fixedZone.hour = t.first / 3600u;
    fixedZone.eng->handleMessage(t.second, "Roy", "web");
  }
  fixedZone.plat.epoch = change + 8 * 3600u;
  fixedZone.hour = fixedZone.plat.epoch / 3600u;
  fixedZone.eng->handleMessage("and now?", "Roy", "web");
  TEST_ASSERT_TRUE(fixedZone.lastWindow().find("(Sun 9 Nov) second turn") == std::string::npos);
}

// The engine stamps when it stored an owner or trigger row, to the second, once the clock is
// set; a boot-relative clock stamps nothing, and the history switch off stays untouched.
static void test_engine_stamps_the_second_a_row_was_stored() {
  Rig r;
  r.plat.epoch = kH * 3600u + 1234u;
  r.eng->handleMessage("hello", "Roy", "123");
  auto rows = r.all("123");
  TEST_ASSERT_EQUAL_STRING(std::to_string(kH * 3600u + 1234u).c_str(), agent::tagValue(rows[1].tags, agent::kAtTag).c_str());
  agent::history::Inbound in;   // a Telegram turn keeps the sender's time too
  in.chatId = "123";
  in.src = "tg:77x1";
  in.sentUnix = kH * 3600u + 1200u;
  agent::history::noteInbound(in, "from telegram");
  r.plat.epoch = kH * 3600u + 1300u;
  r.eng->handleMessage("from telegram", "Roy", "123");
  rows = r.all("123");
  TEST_ASSERT_EQUAL_STRING(std::to_string(kH * 3600u + 1200u).c_str(), agent::tagValue(rows[1].tags, agent::kSentTag).c_str());
  TEST_ASSERT_EQUAL_STRING(std::to_string(kH * 3600u + 1300u).c_str(), agent::tagValue(rows[1].tags, agent::kAtTag).c_str());
  r.plat.epoch = kH * 3600u + 1400u;
  r.eng->injectScheduledTurn("123", "check the trains", "commute", "l7", false, false, false);
  rows = r.all("123");
  size_t trigger = 0;
  while (trigger < rows.size() && !agent::hasTagItem(rows[trigger].tags, agent::kTriggerTag)) trigger++;
  TEST_ASSERT_TRUE(trigger < rows.size());
  TEST_ASSERT_EQUAL_STRING(std::to_string(kH * 3600u + 1400u).c_str(), agent::tagValue(rows[trigger].tags, agent::kAtTag).c_str());
  // No clock yet (0), or a boot-relative one: no stamp at all.
  for (uint64_t epoch : {uint64_t(0), uint64_t(4000)}) {
    Rig early;
    early.plat.epoch = epoch;
    early.eng->handleMessage("hello", "Roy", "123");
    TEST_ASSERT_TRUE(early.all("123")[1].tags.find("at:") == std::string::npos);
  }
  // The switch off: the legacy capture gets the bare sender, as before.
  Rig off;
  off.v2 = false;
  off.eng->handleMessage("hello", "Roy", "123");
  TEST_ASSERT_EQUAL(1, (int)off.legacyCaptures.size());
  TEST_ASSERT_TRUE(off.legacyCaptures[0].second.find("at:") == std::string::npos);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_identical_messages_either_side_of_local_midnight_are_two_messages);
  RUN_TEST(test_history_days_use_the_zone_in_force_when_the_row_was_stored);
  RUN_TEST(test_engine_stamped_turns_tell_their_own_days);
  RUN_TEST(test_engine_stamps_the_second_a_row_was_stored);
  RUN_TEST(test_owner_row_tags_and_stored_text);
  RUN_TEST(test_image_row_is_capped_to_about_300_chars);
  RUN_TEST(test_exactly_once_capture_by_source);
  RUN_TEST(test_stale_note_never_applies_to_another_turn);
  RUN_TEST(test_album_from_the_batcher_is_captured_once_with_its_refs);
  RUN_TEST(test_text_bucket_source_and_inject_without_ids);
  RUN_TEST(test_seedless_turn_trigger_row);
  RUN_TEST(test_assistant_rows_carry_out_kinds);
  RUN_TEST(test_switch_off_is_byte_identical);
  RUN_TEST(test_overflow_shift_is_recorded_and_stable);
  RUN_TEST(test_fold_slice_starts_after_the_anchor);
  UNITY_END();
  return 0;
}
