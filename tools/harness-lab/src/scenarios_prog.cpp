#include "scenarios_prog.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "nimbus/harness/providers.h"        // kWireBodyMax
#include "nimbus/harness/turn_progress.h"    // planTurnProgress - the portable plan
#include "scenarios_ovf.h"                  // shared wire watchers: isHead, printCalls

namespace lab {
namespace prog {
namespace {

namespace orch = nimbus::orch;

// One bulk record the replay's lookup tool returns: a fixed number of bytes, so
// every tool round grows the next request by a known amount - the way the real
// case's memory searches did.
constexpr size_t kBulkRecord = 4000;

// "headloop: round 2 body 91322 B over the 85000 B budget - folded 6322 B of ..."
// -> the round that crossed, or -1 when the line is not a budget fold.
int foldRound(const std::string& line) {
  if (line.find("B budget - folded") == std::string::npos) return -1;
  int round = -1;
  if (std::sscanf(line.c_str(), "headloop: round %d", &round) != 1) return -1;
  return round;
}

}  // namespace

void longTurnReplay(LabRig& rig, const Report& r) {
  // BEFORE - the real case this replay is shaped from (Lumi v4.9.0, 4 Nov,
  // evidence/lumi-v490-watch): asked at 16:17:59, the head ran 8 tool rounds
  // with budget refits and answered at 16:23:11 - 5 min 12 s with no visible
  // sign of work, and a second message sent at 16:18:29 waited behind it until
  // 16:23:39.
  r.note("before: the real case ran 8 tool rounds for 5 min 12 s with no signal; "
         "a mid-turn message waited 5 min 10 s for its acknowledgement");

  // The lab's own round-0 body size, measured with one cheap turn, so the
  // seeded history lands the budget crossing mid-turn (after tools ran)
  // instead of drifting to round 0 as the prompt changes.
  size_t base = 0;
  rig.shim().onRequest = [&base](agent::HttpRequest& q) {
    if (ovf::isHead(q) && q.body.size() > base) base = q.body.size();
  };
  rig.say("prog-probe", "Reply with exactly the word: ok");
  rig.shim().onRequest = nullptr;
  r.note("probe: a bare chat's round-0 body is " + std::to_string(base) + " B");
  r.check(base > 0 && base + 3 * kBulkRecord < agent::providers::kWireBodyMax,
          "the probe measured a round-0 body with room to grow past the budget");

  // The replay chat: no seeded history (the window folds it); the body is grown
  // the way the real case grew it - tool schemas that ride every request, then
  // tool results that accumulate round by round until a round crosses the
  // 85 KB wire budget. The extra schemas make up the lab's smaller tool list.
  const std::string chat = "prog-long-turn";
  for (int i = 0; i < 40; i++)
    rig.registry().add("prog.extra_" + std::to_string(i), std::string(700, 'd'),
                      [](ArduinoJson::JsonObjectConst, const orch::Principal&) { return orch::ToolResult::ok("ok"); },
                      "{\"type\":\"object\",\"properties\":{\"q\":{\"type\":\"string\"}}}");
  // The lookup the prompt drives: one keyword per round, a fixed-size record
  // back. The figures exist only here, so the head must run the tool.
  rig.registry().add("prog_bulk_lookup",
                    "Returns the store's bulk record for one grocery keyword. Call it once per keyword.",
                    [](ArduinoJson::JsonObjectConst, const orch::Principal&) {
                      return orch::ToolResult::ok("bulk record: skyr, Waitrose, per 100 g: " +
                                                  std::string(kBulkRecord - 45, 'k'));
                    },
                    "{\"type\":\"object\",\"properties\":{\"q\":{\"type\":\"string\"}}}");
  size_t largest = 0;
  rig.shim().onRequest = [&largest](agent::HttpRequest& q) {
    if (ovf::isHead(q) && q.body.size() > largest) largest = q.body.size();
  };
  auto t = rig.say(chat,
                   "The figures are only in the store's bulk records. Look up the bulk record for each of these "
                   "keywords with prog_bulk_lookup, one call per keyword, one keyword per round (not all in one "
                   "round): skyr, oats, honey, almonds, raisins, apples. After the last one, answer in one short "
                   "line per keyword.");
  rig.shim().onRequest = nullptr;
  ovf::printCalls(rig, {r.check, r.note});
  r.note("reply: " + t.reply.substr(0, 300));

  // AFTER - the timeline the owner now sees: every boundary, when it fired, and
  // what the portable plan would show there (keep-alive, the ONE line, the
  // queue check). Folded the way a surface folds it: lineAlreadySent carries
  // across boundaries.
  const auto& marks = t.progress;
  std::string timeline;
  int keepAlives = 0, queueChecks = 0, sayLines = 0;
  std::string line;
  bool lineSent = false;
  for (const auto& m : marks) {
    orch::HeadProgress hp;
    hp.round = m.round;
    hp.elapsedMs = m.elapsedMs;
    hp.lastRound = m.lastRound;
    hp.tools = m.tools;
    const auto plan = nimbus::harness::planTurnProgress(hp, m.maxRounds, lineSent);
    if (plan.keepAlive) keepAlives++;
    if (plan.checkQueue) queueChecks++;
    if (plan.sayLine) {
      sayLines++;
      line = plan.line;
      lineSent = true;
    }
    timeline += "round " + std::to_string(m.round) + " at " + std::to_string(m.elapsedMs / 1000) + "s" +
                (m.lastRound ? " (final)" : "") + "; ";
  }
  r.note("after: boundaries at " + timeline);
  if (!line.empty()) r.note("after: the one progress line the owner would see: \"" + line + "\"");

  r.check(marks.size() >= 2, "the turn ran multiple rounds, so the boundary seam had something to signal");
  bool consecutive = true;
  for (size_t i = 0; i < marks.size(); ++i)
    if (marks[i].round != (int)i) consecutive = false;
  r.check(consecutive, "one boundary per round, round 0 included");
  const int finalMarks = marks.empty() ? 0 : (marks.back().lastRound ? 1 : 0);
  r.check(keepAlives == (int)marks.size() - finalMarks,
          "a keep-alive at every boundary except the imminent answer");
  r.check(queueChecks == (int)marks.size() - finalMarks,
          "every boundary looks for messages that arrived mid-turn");
  r.check(sayLines <= 1, "never more than one progress line per turn");
  r.check(marks.size() <= 2 || sayLines == 1,
          "a turn past its third round says the one plain line");
  if (sayLines == 1) {
    r.check(!line.empty() && line.find("prog_bulk_lookup") == std::string::npos &&
                line.find("memory_search") == std::string::npos && line.find('_') == std::string::npos,
            "the line is plain words, no machine names");
  }

  // The refit, on the real wire: each crossing folded enough in ONE cycle (the
  // real case needed two refusals per crossing), every request that left the
  // lab was inside the budget, and the provider accepted the refitted replay.
  int folds = 0;
  std::set<int> foldedRounds;
  for (const auto& l : LabRig::harnessLog()) {
    const int round = foldRound(l);
    if (round < 0) continue;
    folds++;
    foldedRounds.insert(round);
    r.note("fold: " + l);
  }
  r.check(folds >= 1, "the replay crossed the wire budget mid-turn, after tools ran");
  r.check(folds == (int)foldedRounds.size(),
          "each crossing folded enough in one cycle, no round refused twice");
  r.check(largest <= agent::providers::kWireBodyMax, "every request sent was inside the wire budget");
  r.check(ovf::lastCallServed(rig), "the provider accepted the refitted replay");
  r.check(!t.reply.empty() && t.reply.find("didn't finish") == std::string::npos,
          "the turn answered after the folds");
}

}  // namespace prog
}  // namespace lab
