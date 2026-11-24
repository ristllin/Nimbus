#pragma once
#include <string>
#include <vector>

#include "transcripts.h"

// cases.h - the request fixtures (router design section 11, layer L1): one Case per stored
// state the wire must keep byte for byte. Each case renders one request. Names are the golden
// file names: renaming one is a golden change.
namespace route_goldens {

inline Case make(const char* name, Tx tx, Facts f = Facts(), const char* sig = kDirect) {
  Case c;
  c.name = name;
  c.signature = sig;
  c.tx = std::move(tx);
  c.facts = f;
  return c;
}

// A first owner message and nothing else.
inline Tx firstMessage() {
  Tx t;
  t.open();
  t.owner("Hello. What can you help me with today?");
  return t;
}

// One round with one call and its result; the request of round 2.
inline Tx oneCall() {
  Tx t;
  t.open();
  t.owner("What is in notes/beds.md?");
  t.call("toolu_01A", "read_note", R"({"path":"notes/beds.md"})");
  t.receipt("toolu_01A", "read_note", "# Beds\nbed 1: beans\nbed 2: leeks\nbed 3: empty");
  t.next();
  return t;
}

// Two calls in one round; the results stored in the other order.
inline Tx twoParallelCalls() {
  Tx t;
  t.open();
  t.owner("Compare notes/beds.md with notes/seeds.md.");
  t.say("Reading both notes.", false);
  t.call("toolu_02A", "read_note", R"({"path":"notes/beds.md"})");
  t.call("toolu_02B", "read_note", R"({"path":"notes/seeds.md"})");
  t.receipt("toolu_02B", "read_note", "seeds: beans, leeks, chard");
  t.receipt("toolu_02A", "read_note", "bed 1: beans\nbed 2: leeks");
  t.next();
  return t;
}

// Three rounds; text between calls; a device note stored between a call and its result.
inline Tx threeRounds() {
  Tx t;
  t.open();
  t.owner("Check the three bed notes one by one.");
  for (int i = 1; i <= 3; ++i) {
    const std::string id = "toolu_03" + std::to_string(i);
    if (i == 2) t.say("Bed 1 is fine. Next one.", false);
    t.call(id, "read_note", R"({"path":"notes/bed)" + std::to_string(i) + R"(.md"})");
    if (i == 3) t.note("[device] the card was busy for 2 s");
    t.receipt(id, "read_note", "bed " + std::to_string(i) + ": watered on Monday");
    t.next();
    t.advance(4);
  }
  return t;
}

// A tool turn closed with its answer, then a second owner turn.
inline Tx secondTurnAfterTools() {
  Tx t = oneCall();
  t.say("Bed 1 has beans, bed 2 leeks, bed 3 is empty.");
  t.close();
  t.advance(600);
  t.open();
  t.owner("Plant chard in bed 3 then.");
  return t;
}

// A cut answer as the engine stores it: its text not final, the call's arguments cut, the
// not-run result; then the round after it.
inline Tx storedCutAnswer() {
  Tx t;
  t.open();
  t.owner("Read the long planting note.");
  t.say("Let me read the planting no", false);
  t.call("toolu_04A", "read_note", R"({"path":"notes/plan)");
  t.receipt("toolu_04A", "read_note", "not run: the answer was cut", false);
  t.next();
  return t;
}

// A photo shown in the open turn.
inline Tx photoInView() {
  Tx t;
  t.open({"pa1b2c3d4"});
  t.owner("What plant is this?", {"pa1b2c3d4"});
  return t;
}

// The same photo three owner turns later: its stub line (with its stored note) only.
inline Tx photoThreeTurnsLater() {
  Tx t;
  t.open({"pa1b2c3d4"});
  t.owner("What plant is this?", {"pa1b2c3d4"}, "a chard seedling");
  t.say("It looks like a chard seedling.");
  t.close();
  for (int i = 0; i < 2; ++i) {
    t.advance(300);
    t.chatTurn("And when do I water it?", "Every second day.");
  }
  t.advance(300);
  t.open();
  t.owner("Thanks, all clear.");
  return t;
}

// An open turn at the photo caps: the owner's photo plus view results that bring back four
// more; the caps (tx::kInlinePhotosInViewMax) stop the last one, which renders as its line.
inline Tx nearCapPhotoTurn() {
  Tx t;
  const std::vector<std::string> refs = {"pb0000001", "pb0000002", "pb0000003", "pb0000004"};
  t.open({"pa1b2c3d4"});
  t.owner("Compare this one with the four from last week.", {"pa1b2c3d4"});
  for (size_t i = 0; i < refs.size(); ++i) {
    const std::string id = "toolu_05" + std::to_string(i);
    t.call(id, "view", R"({"id":")" + refs[i] + R"("})");
    t.receipt(id, "view", "photo " + refs[i] + " Tue 10 Nov 09:1" + std::to_string(i) + ": shown here");
  }
  t.next();
  return t;
}

// A closed turn's bulk result stubbed by a stored pointer move (x item; without it the result
// would render whole), outside content of a closed turn (a stub by its class), and the open
// turn's bulk result whole.
inline Tx stubAndBulk() {
  Tx t;
  t.open();
  t.owner("Load the seed catalogue.");
  t.call("toolu_06A", "read_note", R"({"path":"notes/catalogue.md"})");
  const uint64_t first = t.bulkResult("toolu_06A", "read_note", payload("CATALOGUE", 3000));
  t.call("toolu_06B", "read_note", R"({"path":"web/frost-dates"})");
  t.bulkResult("toolu_06B", "read_note", payload("OUTSIDE-FROST", 1200), tx::ResultClass::Outside);
  t.next();
  t.say("The catalogue lists 40 seeds; frost ends in April.");
  t.close();
  t.advance(400);
  t.open();
  t.move(first, "cold");
  t.owner("Now load the order list.");
  t.call("toolu_06C", "read_note", R"({"path":"notes/orders.md"})");
  t.bulkResult("toolu_06C", "read_note", payload("ORDERS", 2500));
  t.next();
  return t;
}

// Three closed tool turns and an open one whose first round stored signed thinking: the
// state each ladder rung renders.
inline Tx ladderState() {
  Tx t;
  for (int i = 1; i <= 3; ++i) {
    t.open();
    t.owner("Load list " + std::to_string(i) + ".");
    const std::string id = "toolu_07" + std::to_string(i);
    t.call(id, "read_note", R"({"path":"notes/list)" + std::to_string(i) + R"(.md"})");
    t.bulkResult(id, "read_note", payload("LIST-" + std::to_string(i), 1500));
    t.next();
    t.say("List " + std::to_string(i) + " is loaded.");
    t.close();
    t.advance(500);
  }
  t.open();
  t.owner("Merge the three lists.");
  t.meta(R"({"type":"thinking","thinking":"Merge by name.","signature":"c2lnbmF0dXJlLTA3"})");
  t.call("toolu_074", "read_note", R"({"path":"notes/merged.md"})");
  t.bulkResult("toolu_074", "read_note", payload("MERGED", 1800));
  t.next();
  return t;
}

// The q of the last item of turn `turn`.
inline uint64_t lastQOf(const Tx& t, uint32_t turn) {
  uint64_t q = 0;
  for (const tx::Item& it : t.items)
    if (it.turn == turn) q = it.q;
  return q;
}
inline uint64_t lastResultQ(const Tx& t) {
  uint64_t q = 0;
  for (const tx::Item& it : t.items)
    if (it.kind == tx::ItemKind::Result) q = it.q;
  return q;
}

// Signed thinking in a closed turn (dropped) and in the open turn (sent where it sat).
inline Tx openTurnThinking() {
  Tx t;
  t.open();
  t.owner("Which bed gets the most sun?");
  t.meta(R"({"type":"thinking","thinking":"Old reasoning.","signature":"b2xkLXNpZw"})");
  t.say("Bed 2 gets the most sun.");
  t.close();
  t.advance(200);
  t.open();
  t.owner("Then put the tomatoes there and check the note.");
  t.meta(R"({"type":"thinking","thinking":"Read the note first.","signature":"c2lnLW9wZW4"})");
  t.call("toolu_08A", "read_note", R"({"path":"notes/bed2.md"})");
  t.meta(R"({"type":"redacted_thinking","data":"cmVkYWN0ZWQ"})");
  t.receipt("toolu_08A", "read_note", "bed 2: empty, full sun");
  t.next();
  return t;
}

// A k item another dialect wrote (a Responses reasoning item) and a broken one, in the open turn.
inline Tx foreignMeta() {
  Tx t;
  t.open();
  t.owner("Check bed 1.");
  t.meta(R"({"type":"reasoning","id":"rs_0001","encrypted_content":"ZW5jcnlwdGVk","summary":[]})");
  t.meta("not json at all");
  t.call("toolu_09A", "read_note", R"({"path":"notes/bed1.md"})");
  t.receipt("toolu_09A", "read_note", "bed 1: beans");
  t.next();
  return t;
}

// Ids and tool names another provider or engine wrote: dotted, empty and repeated ids, a
// dotted and an over-long tool name; one result answers no call.
inline Tx foreignIds() {
  Tx t;
  t.open();
  t.owner("Read both notes.");
  t.call("call_1.a", "files.read", R"({"path":"notes/a.md"})");
  t.call("call_1.a", "files.read", R"({"path":"notes/b.md"})");
  t.call("", std::string(70, 'n'), R"({"path":"notes/c.md"})");
  t.receipt("call_1.a", "files.read", "note a");
  t.receipt("call_1.a", "files.read", "note b");
  t.receipt("", std::string(70, 'n'), "note c");
  t.receipt("call_orphan", "read_note", "an orphan result");
  t.next();
  return t;
}

// Owner messages after a local midnight, before and after the day start (UTC+1, day start 04:00).
inline Tx dayChange() {
  Tx t;
  t.tz = 60;
  t.t = kT0 + 10 * 3600 + 48 * 60;   // 00:50 local, 18 Nov: still the running day of 17 Nov
  t.chatTurn("Late check on the beds.", "All three are fine.");
  t.advance(100 * 60);                // 02:30 local: still the day before
  t.chatTurn("Cannot sleep. Is frost due?", "Not tonight.");
  t.advance(220 * 60);                // 06:10 local: the new day
  t.open();
  t.owner("Good morning. Water today?");
  return t;
}

// A digest covers the first two turns: only what follows its thru is sent.
inline Tx digestCovers() {
  Tx t;
  t.chatTurn("First question.", "First answer.");
  t.chatTurn("Second question.", "Second answer.");
  t.digest("Tue 17 Nov: two questions about the beds.", "2026-11-17", t.q);
  t.advance(900);
  t.chatTurn("Third question.", "Third answer.");
  t.open();
  t.owner("Fourth question.");
  return t;
}

// The guard's last round: tool choice none after several rounds.
inline Tx guardRound() {
  Tx t;
  t.open();
  t.owner("Keep reading notes until you find the seed list.");
  for (int i = 1; i <= 3; ++i) {
    const std::string id = "toolu_10" + std::to_string(i);
    t.call(id, "read_note", R"({"path":"notes/n)" + std::to_string(i) + R"(.md"})");
    t.receipt(id, "read_note", "no seed list here");
    t.next();
  }
  return t;
}

// The summarizer's one-message request (plain_compact.cpp askRung): the slice as owner text.
inline Tx summarizerSlice() {
  Tx t;
  t.open();
  t.owner("[slice] Tue 17 Nov: the owner asked about beds 1 to 3; bed 3 gets chard.");
  return t;
}

// A restart in the middle of a turn: an effect call with its intent and no result, a device
// note written at the restart, then the request that resumes. The wire leaves the dangling call
// out (pairing: a call is sent only with its one result), so it appears nowhere in the golden.
inline Tx restartMidTurn() {
  Tx t;
  t.open();
  t.owner("Add chard to the order list.");
  t.call("toolu_11A", "read_note", R"({"path":"notes/orders.md"})");
  t.receipt("toolu_11A", "read_note", "orders: beans, leeks");
  t.next();
  t.call("toolu_11B", "read_note", R"({"path":"notes/orders.md","add":"chard"})", true);
  t.intent("toolu_11B", "read_note");
  t.advance(30);
  t.note("[device] restarted during this turn; the last call has no result", tx::NoteClass::Gap);
  t.next();
  return t;
}

// Lane E2-R6: reasoning as the chat rows store it, in the open turn: a Mistral thinking chunk before the
// round's text, and Z.ai's reasoning_content, each tagged as written on `sig`'s row (the planner sends a
// k item only to its writer's tag; the codec then sends only its row's kind). A closed turn's as well:
// the chat rows replay every rendered turn (replayScope allTurns).
inline Tx chatReasoning(const char* meta, const char* sig) {
  const RowFacts row = rowOf(sig, false);
  const std::string tag = plain::replayTag(row.dialect, row.routed, row.model);
  Tx t;
  t.open();
  t.owner("Which bed gets the most sun?");
  t.meta(meta);
  t.items.back().wr = tag;
  t.say("Bed 2.");
  t.close();
  t.advance(200);
  t.open();
  t.owner("Check the note for bed 2.");
  t.meta(meta);
  t.items.back().wr = tag;
  t.say("Reading the note.", false);
  t.call("toolu_12A", "read_note", R"({"path":"notes/bed2.md"})");
  t.receipt("toolu_12A", "read_note", "bed 2: empty, full sun");
  t.next();
  return t;
}
inline constexpr const char* kMistralThinking =
    R"({"type":"thinking","thinking":[{"type":"text","text":"The note says which bed."}]})";
inline constexpr const char* kZaiReasoning = R"({"reasoning_content":"The note says which bed."})";

// The chat rows' requests (lane E2-R6): the common fixtures on each direct row, two on each routed
// row, and a custom endpoint in text mode (its stored calls and results as text).
inline void chatCases(std::vector<Case>& out) {
  for (const char* sig : {kMistral, kZai}) {
    Facts f;
    out.push_back(make("text_only", firstMessage(), f, sig));
    out.push_back(make("one_call", oneCall(), f, sig));
    out.push_back(make("two_parallel_calls", twoParallelCalls(), f, sig));
    out.push_back(make("three_rounds", threeRounds(), f, sig));
    out.push_back(make("second_turn_after_tools", secondTurnAfterTools(), f, sig));
    out.push_back(make("stub_and_bulk_result", stubAndBulk(), f, sig));
    out.push_back(make("foreign_k_item", foreignMeta(), f, sig));
    out.push_back(make("open_turn_thinking", openTurnThinking(), f, sig));
    out.push_back(make("mistral_thinking_chunk", chatReasoning(kMistralThinking, sig), f, sig));
    out.push_back(make("zai_reasoning_content", chatReasoning(kZaiReasoning, sig), f, sig));
    f.toolChoiceNone = true;
    out.push_back(make("guard_round", guardRound(), f, sig));
    f = Facts();
    f.photos = Photos::Inline;
    out.push_back(make("photo_in_view_inline", photoInView(), f, sig));
    f = Facts();
    f.system = Sys::Summarizer;
    f.noTools = true;
    f.schema = true;
    out.push_back(make("summarizer_structured", summarizerSlice(), f, sig));
  }
  for (const char* sig : {kMistralRouted, kZaiRouted}) {
    out.push_back(make("text_only", firstMessage(), Facts(), sig));
    out.push_back(make("one_call", oneCall(), Facts(), sig));
  }
  Facts f;
  f.effort = "";
  out.push_back(make("text_only", firstMessage(), f, kCustomChat));
  out.push_back(make("second_turn_after_tools", secondTurnAfterTools(), f, kCustomChat));   // calls as text (R4b)
  f.system = Sys::Summarizer;
  f.noTools = true;
  out.push_back(make("summarizer_lines", summarizerSlice(), f, kCustomChat));
}

// Lane E2-R7: stored Responses reasoning items, tagged as the engine stamps them for a direct
// gpt-5.6 head, in a closed turn and in the open turn (before the call it came with); then one
// tagged for the routed head and one with no tag (Anthropic's reading): both left out.
inline Tx openTurnReasoning() {
  const std::string tag = plain::replayTag(plain::Dialect::OpenAIResponses, false, "gpt-5.6");
  Tx t;
  t.open();
  t.owner("Which bed gets the most sun?");
  t.meta(R"({"type":"reasoning","id":"rs_old01","summary":[],"encrypted_content":"b2xkLXJlYXNvbmluZw"})");
  t.items.back().wr = tag;
  t.say("Bed 2 gets the most sun.");
  t.close();
  t.advance(200);
  t.open();
  t.owner("Then put the tomatoes there and check the note.");
  t.meta(R"({"type":"reasoning","id":"rs_open01","summary":[{"type":"summary_text","text":"Read the note."}],"encrypted_content":"b3Blbi1yZWFzb25pbmc"})");
  t.items.back().wr = tag;
  t.call("call_R7a", "read_note", R"({"path":"notes/bed2.md"})");
  t.receipt("call_R7a", "read_note", "bed 2: empty, full sun");
  t.next();
  t.meta(R"({"type":"reasoning","id":"rs_routed1","summary":[],"encrypted_content":"cm91dGVk"})");
  t.items.back().wr = plain::replayTag(plain::Dialect::OpenAIResponses, true, "gpt-5.6");
  t.meta(R"({"type":"reasoning","id":"rs_untag1","summary":[],"encrypted_content":"dW50YWdnZWQ"})");
  t.call("call_R7b", "read_note", R"({"path":"notes/bed3.md"})");
  t.receipt("call_R7b", "read_note", "bed 3: shade");
  t.next();
  return t;
}

// The Responses rows' requests (lane E2-R7): the common fixtures direct, two routed.
inline void responsesCases(std::vector<Case>& out) {
  Facts f;
  f.maxTokens = 0;   // the exchange's cap for an OpenAI head (tableCap 0); the pre-flight decides
  const Facts base = f;
  const char* sig = kResponsesDirect;
  out.push_back(make("text_only", firstMessage(), f, sig));
  f.effort = "";
  out.push_back(make("text_only_no_effort", firstMessage(), f, sig));
  f = base;
  out.push_back(make("one_call", oneCall(), f, sig));
  out.push_back(make("two_parallel_calls", twoParallelCalls(), f, sig));
  out.push_back(make("three_rounds", threeRounds(), f, sig));
  out.push_back(make("second_turn_after_tools", secondTurnAfterTools(), f, sig));
  out.push_back(make("stored_cut_answer", storedCutAnswer(), f, sig));
  out.push_back(make("photo_in_view", photoInView(), f, sig));
  out.push_back(make("near_cap_photo_turn", nearCapPhotoTurn(), f, sig));
  f.viewBound = true;
  out.push_back(make("photo_three_turns_later", photoThreeTurnsLater(), f, sig));
  f = base;
  out.push_back(make("stub_and_bulk_result", stubAndBulk(), f, sig));
  const Tx ladder = ladderState();
  f.ladder.omitThrough = lastQOf(ladder, 2);
  f.ladder.omissionLine = kOmission;
  out.push_back(make("ladder_rung3_omit_through", ladder, f, sig));
  f = base;
  out.push_back(make("open_turn_reasoning", openTurnReasoning(), f, sig));
  out.push_back(make("foreign_k_item", foreignMeta(), f, sig));
  out.push_back(make("foreign_ids", foreignIds(), f, sig));
  out.push_back(make("digest_covers_earlier_turns", digestCovers(), f, sig));
  out.push_back(make("restart_mid_turn", restartMidTurn(), f, sig));
  f.toolChoiceNone = true;
  out.push_back(make("guard_round", guardRound(), f, sig));
  f = base;
  f.system = Sys::Summarizer;
  f.noTools = true;
  f.schema = true;
  out.push_back(make("summarizer_structured", summarizerSlice(), f, sig));
  f.schema = false;
  out.push_back(make("summarizer_lines", summarizerSlice(), f, sig));
  out.push_back(make("text_only", firstMessage(), base, kResponsesRouted));
  out.push_back(make("one_call", oneCall(), base, kResponsesRouted));
}

inline std::vector<Case> requestCases() {
  std::vector<Case> out;
  Facts f;
  out.push_back(make("text_only", firstMessage()));
  f.effort = "";
  out.push_back(make("text_only_no_effort", firstMessage(), f));
  out.push_back(make("one_call", oneCall()));
  out.push_back(make("two_parallel_calls", twoParallelCalls()));
  out.push_back(make("three_rounds", threeRounds()));
  out.push_back(make("second_turn_after_tools", secondTurnAfterTools()));
  f = Facts();
  f.longCache = true;
  out.push_back(make("second_turn_after_tools_long_cache", secondTurnAfterTools(), f));
  out.push_back(make("stored_cut_answer", storedCutAnswer()));
  out.push_back(make("photo_in_view_file_ref", photoInView()));
  f = Facts();
  f.photos = Photos::Inline;
  out.push_back(make("photo_in_view_inline", photoInView(), f));
  f = Facts();
  f.viewBound = true;
  out.push_back(make("photo_three_turns_later", photoThreeTurnsLater(), f));
  f = Facts();
  f.photos = Photos::Inline;
  out.push_back(make("near_cap_photo_turn", nearCapPhotoTurn(), f));
  out.push_back(make("stub_and_bulk_result", stubAndBulk()));
  const Tx ladder = ladderState();
  out.push_back(make("ladder_none", ladder));
  f = Facts();
  f.pointer = lastQOf(ladder, 1);   // RenderInput::stubPointer: turn 1's bulk result as its stub
  out.push_back(make("stub_by_input_pointer", ladder, f));
  f = Facts();
  f.ladder.closedThrough = lastQOf(ladder, 2);
  out.push_back(make("ladder_rung1_closed_through", ladder, f));
  f = Facts();
  f.ladder.stubThrough = lastResultQ(ladder);
  out.push_back(make("ladder_rung2_stub_through", ladder, f));
  f = Facts();
  f.ladder.omitThrough = lastQOf(ladder, 2);
  f.ladder.omissionLine = kOmission;
  out.push_back(make("ladder_rung3_omit_through", ladder, f));
  out.push_back(make("open_turn_thinking", openTurnThinking()));
  out.push_back(make("foreign_k_item", foreignMeta()));
  out.push_back(make("foreign_ids", foreignIds()));
  f = Facts();
  f.dayStartMin = 240;
  out.push_back(make("day_change", dayChange(), f));
  out.push_back(make("digest_covers_earlier_turns", digestCovers()));
  f = Facts();
  f.toolChoiceNone = true;
  out.push_back(make("guard_round", guardRound(), f));
  f = Facts();
  f.system = Sys::Summarizer;
  f.noTools = true;
  f.schema = true;
  out.push_back(make("summarizer_structured", summarizerSlice(), f));
  f.schema = false;
  out.push_back(make("summarizer_lines", summarizerSlice(), f));
  out.push_back(make("restart_mid_turn", restartMidTurn()));
  f = Facts();
  f.routed = true;
  out.push_back(make("text_only", firstMessage(), f, kRouted));
  out.push_back(make("one_call", oneCall(), f, kRouted));
  chatCases(out);
  responsesCases(out);
  return out;
}

}  // namespace route_goldens
