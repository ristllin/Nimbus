#pragma once
#include <string>
#include <vector>

#include "../scripted_provider/scripted_provider.h"
#include "driver.h"
#include "items.h"
#include "nimbus/harness/plain/turn.h"

// tx_battery/fixtures.h - the battery's shared transcripts (lane RB-B0b).
//
// dayOfFourTurns: one stored day, snapshotted at every point a request is rendered. It has
// a receipt, a tier-2 read, two calls in one round, a photo, outside content and a closing
// turn after it, so the wire-level properties meet every result class and the outside
// change point. runTurns: a scripted multi-turn chat through the engine with real gaps.
namespace txb {

struct Snapshot {
  std::string name;     // "turn 2 round 1", ...
  Convo c;              // what is stored when the request is rendered
  uint32_t openTurn = 0;
};

inline std::vector<Snapshot> dayOfFourTurns(plain::Dialect d) {
  std::vector<Snapshot> out;
  Convo c;
  c.callPrefix = d == plain::Dialect::AnthropicMessages ? "toolu_" : "call_";
  auto snap = [&](const char* name) { out.push_back(Snapshot{name, c, c.turn}); };

  c.openTurn();
  c.owner("What is in notes/boat.md?");
  snap("turn 1 open");
  std::string id = c.call("read_file", R"({"path":"notes/boat.md"})");
  c.receipt(id, "read_file", "# Boat\nNereid II, 9.5 m, Volvo D2-40");
  c.nextRound();
  snap("turn 1 round 1");
  c.say("Your boat is the Nereid II, 9.5 m with a Volvo D2-40.");
  c.close();

  c.advance(60);
  c.openTurn();
  c.items.back().photos = {"pab12cd34"};
  c.owner("Log this label please", {"pab12cd34"});
  snap("turn 2 open");
  id = c.call("big_read", R"({"tag":"label","bytes":20000})");
  c.bulkResult(id, "big_read", payload("BULK-LABEL", 20000));
  c.nextRound();
  snap("turn 2 round 1");
  const std::string a = c.call("ledger_day", R"({"day":"2026-11-17"})");
  const std::string b = c.call("ledger_write", R"({"day":"2026-11-17","add":[{"food":"skyr","g":150}]})", true);
  c.receipt(a, "ledger_day", "ledger 2026-11-17: empty");
  c.intent(b, "ledger_write");
  c.receipt(b, "ledger_write", "ledger 2026-11-17: row L00A3 added; total 150 g");
  c.nextRound();
  snap("turn 2 round 2");
  c.say("Logged skyr, 150 g (row L00A3).");
  c.close();

  c.advance(90);
  c.openTurn();
  c.items.back().photos = {"pab12cd34"};   // a file-reference head keeps it for two owner turns
  c.owner("Anything on train times to Leeds?");
  snap("turn 3 open");
  id = c.call("web_search", R"({"q":"train times Leeds"})");
  c.outside(id, "web_search", R"({"q":"train times Leeds"})", payload("OUTSIDE-TRAINS", 3000));
  c.nextRound();
  snap("turn 3 round 1");
  c.say("The 17:05 from York reaches Leeds at 17:31.");
  c.close();

  c.advance(45);
  c.openTurn();
  c.items.back().photos = {"pab12cd34"};
  c.owner("Thanks!");
  snap("turn 4 open (outside now stubbed)");
  return out;
}

struct TurnScript {
  uint32_t gapS = 60;                // virtual seconds before this owner message
  std::string ask;
  std::vector<sp::Step> steps;       // what the model answers, round by round
};

// Run each owner turn after its gap; the first failing turn ends the run.
inline tx::Status runTurns(Battery& b, const std::vector<TurnScript>& turns) {
  for (const TurnScript& t : turns) {
    b.advance(t.gapS);
    for (const sp::Step& s : t.steps) b.model.push(s);
    plain::TurnOutcome out;
    tx::Status st = b.owner(t.ask, out);
    if (!st.ok()) return st;
  }
  return tx::Status::success();
}

// A sane model for effect turns: it calls `tool` until a result (or 'outcome unknown') is in
// the request, then says `reply`. A second effect is then the harness's doing, never the model's.
inline bool carriesAResult(const sp::Recorded& r) {
  return r.body.find("tool_result") != std::string::npos || r.body.find("function_call_output") != std::string::npos ||
         r.body.find("\"role\":\"tool\"") != std::string::npos || r.body.find("outcome unknown") != std::string::npos;
}
inline void callUntilAnswered(Battery& b, const std::string& tool, const std::string& args, const std::string& reply) {
  b.model.respondWith([tool, args, reply](const sp::Recorded& r, size_t) {
    return carriesAResult(r) ? sp::say(reply) : sp::call(tool, args);
  });
}

// A digest the summarizer may return: over the 120 B a slice of 2 KB or more needs, under 768 B.
inline constexpr const char* kDigest =
    "Day digest: the owner asked about the boat, read files and searched the web; nothing is pending. "
    "He prefers short answers. Facts: Nereid II, 9.5 m.";

inline TurnScript chat(uint32_t gap, const std::string& ask, const std::string& answer) {
  return TurnScript{gap, ask, {sp::say(answer)}};
}

inline TurnScript withTool(uint32_t gap, const std::string& ask, const std::string& tool, const std::string& args,
                           const std::string& answer) {
  return TurnScript{gap, ask, {sp::call(tool, args), sp::say(answer)}};
}

}  // namespace txb
