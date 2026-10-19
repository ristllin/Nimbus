#include "scenarios_turn_record.h"

#include <chrono>
#include <cstdio>
#include <map>
#include <set>
#include <string>

#include "nimbus/orch/failure_chain.h"
#include "nimbus/orch/failure_rows.h"

namespace lab {
namespace turnrec {
namespace {

std::string lower(std::string s) {
  for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
  return s;
}
bool has(const std::string& hay, const char* needle) {
  return lower(hay).find(lower(needle)) != std::string::npos;
}
size_t countLogged(const char* needle) {
  size_t n = 0;
  for (const auto& l : LabRig::harnessLog())
    if (l.find(needle) != std::string::npos) n++;
  return n;
}
void show(const TurnRecord& t) {
  std::printf("    > %s\n", t.userText.substr(0, 110).c_str());
  for (const auto& c : t.toolCalls) std::printf("      [tool] %s\n", c.substr(0, 160).c_str());
  std::printf("    < %s\n", t.reply.empty() ? "(no reply)" : t.reply.c_str());
}

// The rows metered since `from`: printed for metering_check.py, and checked here:
// one row per (host, model) for the turn, and the round-0 host among them.
void meteredRows(LabRig& rig, const Report& r, size_t from) {
  const auto& rows = rig.spendRows();
  std::map<std::string, int> per;
  std::set<std::string> hosts;
  for (size_t i = from; i < rows.size(); i++) {
    const auto& x = rows[i];
    per[x.host + "/" + x.model]++;
    hosts.insert(x.host);
    std::printf("lab-ledger: {\"host\":\"%s\",\"model\":\"%s\",\"tag\":\"%s\",\"in\":%u,\"out\":%u,"
                "\"cr\":%u,\"cw\":%u}\n",
                x.host.c_str(), x.model.c_str(), x.tag.c_str(), (unsigned)x.in, (unsigned)x.out,
                (unsigned)x.cr, (unsigned)x.cw);
  }
  bool once = !per.empty();
  for (const auto& kv : per) once = once && kv.second == 1;
  r.check(once, "each billing (host, model) is filed exactly once for the turn");
  r.check(hosts.count("anthropic") == 1, "the round-0 host (anthropic) billed and was filed");
  r.check(countLogged("turn spend:") >= 1, "the turn logged its 'turn spend:' line");
  for (const auto& l : LabRig::harnessLog())
    if (l.find("fabric-spend:") != std::string::npos || l.find("turn spend:") != std::string::npos)
      r.note(l.substr(0, 300));
}

}  // namespace

// Run with --fault=anthropic:2-:529:overloaded: round 0 is real (and writes the
// prompt cache), every later Anthropic request fails, the next host finishes.
void meteringSwitch(LabRig& rig, const Report& r) {
  const size_t from = rig.spendRows().size();
  auto t = rig.say("metering",
                   "Save a note to memory that the metering check ran, then confirm in one short sentence.");
  show(t);
  r.check(!t.reply.empty(), "the switched turn finished with a reply");
  meteredRows(rig, r, from);
  bool other = false;
  for (size_t i = from; i < rig.spendRows().size(); i++)
    other = other || rig.spendRows()[i].host != "anthropic";
  r.check(other, "the host that finished the turn billed under its own name");
  r.check(countLogged("fabric-spend:") >= 1, "the fabric logged its per-host 'fabric-spend:' line");
}

// Run with --fault=anthropic:2-:400:badreq and every other host faulted: round 0
// bills, then the turn fails everywhere.
void meteringFail(LabRig& rig, const Report& r) {
  const size_t from = rig.spendRows().size();
  auto t = rig.say("metering",
                   "Save a note to memory that the metering failure check ran, then confirm in one short sentence.");
  show(t);
  r.check(has(t.reply, "didn't finish"), "the turn failed and said so");
  r.check(has(t.reply, "anthropic:"), "the notice names the first failing provider");
  meteredRows(rig, r, from);
}

// Failure-cause L1, variant A (the rows carry the chain): the 18 Oct dream failure
// seeded as the rows the device now writes, then the owner's question. Pin the head
// with NIMBUS_LAB_MODEL; fault every other host so the leg stays on one provider.
void diagnoseFromEvidence(LabRig& rig, const Report& r) {
  using namespace nimbus;
  orch::FailureChain ch;
  const std::string ant =
      "messages HTTP 400: The request body is not valid JSON: invalid unicode code point at line 1 column 55142";
  const std::string oai =
      "resp HTTP 400: Invalid body: encountered a unicode decode error while parsing the request";
  ch.note("anthropic", ant);
  ch.note("anthropic", ant);
  ch.note("openai", oai);
  ch.note("openai", oai);
  ch.note("mistral", "chat HTTP 400");
  ch.note("mistral", "chat HTTP 400");
  const uint32_t now = (uint32_t)std::chrono::duration_cast<std::chrono::hours>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
  auto& epi = rig.episodic();
  epi.addSession({"system", now - 260, "", "Device timeline", "active"});
  epi.addSession({"diag", now - 260, "", "Owner chat", "active"});
  int seq = 0x5f0;
  auto row = [&](const std::string& sess, uint32_t ts, const std::string& text, const std::string& tags) {
    char id[16];
    std::snprintf(id, sizeof id, "m%08x", seq++);
    orch::EpisodicMessage m;
    m.id = id;
    m.sessionId = sess;
    m.tsHours = ts;
    m.role = "system";
    m.kind = orch::MsgKind::Log;
    m.text = text;
    m.tags = tags;
    epi.addMessage(m);
  };
  for (int d = 10; d >= 2; d--)
    row("system", now - 24 * d + 21, orch::dreamRowText(true, "", 14000 + 300 * d, 0, 0), "ev:dream");
  orch::TurnEndRowIn te;
  te.ok = false;
  te.src = "dream";
  te.host = "mistral";
  te.model = "claude-haiku-5-5";
  te.fallback = true;
  te.tools = 4;
  te.in = 15074;
  te.out = 364;
  te.chain = &ch;
  te.error = "chat HTTP 400";
  row("diag", now - 3, orch::turnEndRowJson(te), "ev:turnend,turn:m00000600");
  orch::ErrorEventIn ein;
  ein.src = "dream";
  ein.requestedHost = "anthropic";
  ein.finalHost = "mistral";
  ein.chain = &ch;
  ein.error = "chat HTTP 400";
  ein.toolCalls = 4;
  row("system", now - 3, orch::errorEventText(ein), "ev:error,turn:m00000600");
  row("system", now - 3, orch::dreamRowText(false, ch.render(160), 15438, 0, 0), "ev:dream");

  auto t = rig.say("diag", "Did dreaming work? What seems to be the problem with these?");
  show(t);
  r.check(!t.reply.empty() && !has(t.reply, "didn't finish"), "the owner got an answer");
  r.check(has(t.reply, "unicode") || has(t.reply, "not valid json") || has(t.reply, "request body") ||
              has(t.reply, "invalid"),
          "the answer names the recorded cause (an invalid request body)");
  r.check(!has(t.reply, "repeatedly") && !has(t.reply, "every night") && !has(t.reply, "keeps failing"),
          "the answer does not say it failed repeatedly (9 of 10 nights were ok)");
  r.check(!has(t.reply, "60 s") && !has(t.reply, "60-second") && !has(t.reply, "60 second") &&
              !has(t.reply, "sixty"),
          "the answer does not blame a 60 second cap");
  r.check(!has(t.reply, "off mistral") && !has(t.reply, "mistral is the problem") &&
              !has(t.reply, "mistral caused"),
          "the answer does not blame mistral (it failed last, not first)");
  r.note(std::string("served by ") + t.host + ", " + std::to_string(t.tokensIn) + " in / " +
         std::to_string(t.tokensOut) + " out");
}

// Failure-cause L2: one invalid UTF-8 byte in the owner text reaches the provider
// (until the device cleans every provider-bound string). Pin one host with --host
// and fault the others.
void providerErrorReason(LabRig& rig, const Report& r) {
  const size_t from = rig.spendRows().size();
  auto t = rig.say("l2", std::string("Reply with the word ok. Note: caf\xC3\x28 and \xFF end."));
  show(t);
  const size_t at = t.reply.find("HTTP 400");
  if (at == std::string::npos) {
    r.note("no 400: the body was cleaned before it was sent (nothing to check)");
    return;
  }
  std::string after = t.reply.substr(at + 8);
  while (!after.empty() && (after[0] == ':' || after[0] == ' ')) after.erase(0, 1);
  r.check(after.size() > 3 && after[0] != '(' && after[0] != '.',
          "a reason follows \"HTTP 400\"");
  r.check(rig.spendRows().size() == from, "nothing billed, nothing metered");
  for (const auto& l : LabRig::harnessLog())
    if (l.find("HTTP 400") != std::string::npos) r.note(l.substr(0, 300));
}

}  // namespace turnrec
}  // namespace lab
