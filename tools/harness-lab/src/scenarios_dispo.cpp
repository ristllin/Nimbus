// Lane DISPO rate scenarios. See scenarios_dispo.h.
#include "scenarios_dispo.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace lab {
namespace dispolegs {
namespace {

int envInt(const char* name, int fallback) {
  const char* v = std::getenv(name);
  return (v && v[0]) ? std::max(1, std::atoi(v)) : fallback;
}

bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

bool calledTool(const TurnRecord& t, const std::string& name) {
  for (const auto& c : t.toolCalls)
    if (c.rfind(name, 0) == 0) return true;
  return false;
}

// True when the CSV has a data row whose metric column (field 2) names the
// metric. The source column cites the plan, whose text contains the word
// "kcal", so a whole-file substring search would count the protein row's
// citation as a calorie target.
bool metricRow(const std::string& csv, const std::string& metric) {
  size_t lineStart = 0;
  bool firstLine = true;
  while (lineStart < csv.size()) {
    const size_t lineEnd = csv.find('\n', lineStart);
    const std::string line = csv.substr(lineStart, lineEnd == std::string::npos
                                                    ? std::string::npos
                                                    : lineEnd - lineStart);
    if (!firstLine) {
      const size_t field2 = line.find(',');
      if (field2 != std::string::npos) {
        const size_t field2End = line.find(',', field2 + 1);
        const std::string metricCol = line.substr(
            field2 + 1, field2End == std::string::npos ? std::string::npos : field2End - field2 - 1);
        if (has(metricCol, metric)) return true;
      }
    }
    firstLine = false;
    if (lineEnd == std::string::npos) break;
    lineStart = lineEnd + 1;
  }
  return false;
}

void showTurn(const TurnRecord& t) {
  std::printf("    > %s\n", t.userText.substr(0, 110).c_str());
  for (const auto& c : t.toolCalls) std::printf("      [tool] %s\n", c.c_str());
  std::printf("    < %s\n", t.reply.empty() ? "(no reply)" : t.reply.substr(0, 300).c_str());
}

// The bench F03 plan file, verbatim in shape (a daily target the model can cite).
const char* kF03Plan = "Daily target: 2,400 kcal and 155 g protein\n";
const char* kF03TargetsHeader = "date,metric,target,source\n";

// The bench H09 seeded ledger: Saturday's breakfast and lunch, nothing else.
const char* kH09Seed =
    "date,meal,kcal,protein_g\n"
    "2026-10-24,breakfast,420,28\n"
    "2026-10-24,lunch,650,40\n";
const char* kLedgerHeader = "date,meal,kcal,protein_g\n";

}  // namespace

void f03Targets(LabRig& rig, const Report& r) {
  const int reps = envInt("NIMBUS_DISPO_REPS", 5);
  int both = 0, proteinOnly = 0, neither = 0, corrections = 0, answered = 0;
  for (int i = 1; i <= reps; i++) {
    const std::string chat = "f03-r" + std::to_string(i);
    rig.files().seed("food/plan.md", kF03Plan);
    rig.files().seed("food/food-targets.csv", kF03TargetsHeader);
    std::printf("  rep %d/%d\n", i, reps);
    auto t1 = rig.say(chat, "how much protein is left today?");
    showTurn(t1);
    auto t2 = rig.say(chat, "and calories?");
    showTurn(t2);
    const std::string targets = rig.files().get("food/food-targets.csv");
    const bool protein = metricRow(targets, "protein");
    const bool kcal = metricRow(targets, "kcal") || metricRow(targets, "calori");
    const bool corrected = has(t1.reply, "(Correction:") || has(t2.reply, "(Correction:");
    if (!t1.reply.empty() && !t2.reply.empty()) answered++;
    if (protein && kcal) both++;
    else if (protein) proteinOnly++;
    else neither++;
    if (corrected) corrections++;
    std::printf("    rep %d: protein=%d kcal=%d correction=%d\n", i, (int)protein, (int)kcal,
                (int)corrected);
  }
  std::printf("  F03 rate over %d reps: both targets %d, protein only %d, neither %d, "
              "(Correction: line %d, both turns answered %d\n",
              reps, both, proteinOnly, neither, corrections, answered);
  r.check(answered == reps, "every rep answered both turns (the write path itself works)");
  r.note("the rate line above is the disposition evidence: model choice, not a write-path fault");
}

void h09Dinner(LabRig& rig, const Report& r) {
  const int reps = envInt("NIMBUS_DISPO_REPS", 5);
  int requested = 0, mainLog = 0, nowhere = 0, answered = 0;
  for (int i = 1; i <= reps; i++) {
    const std::string chat = "h09-r" + std::to_string(i);
    rig.files().seed("qw3-bench/food-log.csv", kH09Seed);
    rig.files().seed("food/food-log.csv", kLedgerHeader);
    std::printf("  rep %d/%d\n", i, reps);
    auto t = rig.say(chat,
                     "Log my dinner to qw3-bench/food-log.csv: beef fillet 100 g, skyr with "
                     "berries, chicken 90 g, roast eggplant, carrot, tofu and salsa - 990 kcal "
                     "and 88 g protein in total.");
    showTurn(t);
    const std::string requestedFile = rig.files().get("qw3-bench/food-log.csv");
    const std::string mainLogFile = rig.files().get("food/food-log.csv");
    const bool landedRequested = requestedFile.find("990") != std::string::npos;
    const bool landedMain = mainLogFile.find("990") != std::string::npos;
    if (!t.reply.empty()) answered++;
    if (landedRequested) requested++;
    else if (landedMain) mainLog++;
    else nowhere++;
    std::printf("    rep %d: requested-file=%d main-log=%d nowhere=%d\n", i, (int)landedRequested,
                (int)landedMain, (int)!landedRequested && !landedMain);
  }
  std::printf("  H09 rate over %d reps: requested file %d, main-log redirect %d, nowhere %d, "
              "answered %d\n",
              reps, requested, mainLog, nowhere, answered);
  r.check(answered == reps, "every rep answered (the turn itself works)");
  r.note("the rate line above is the disposition evidence: where the head logs a named file");
}

void h20Search(LabRig& rig, const Report& r) {
  const int reps = envInt("NIMBUS_DISPO_REPS", 5);
  int searched = 0, sourced = 0, answered = 0;
  for (int i = 1; i <= reps; i++) {
    const std::string chat = "h20-r" + std::to_string(i);
    std::printf("  rep %d/%d\n", i, reps);
    auto t = rig.say(chat, "What is the latest Python 3 release?");
    showTurn(t);
    const bool used = calledTool(t, "web.search");
    const bool names = has(t.reply, "python.org") || has(t.reply, "Python 3.1");
    if (!t.reply.empty()) answered++;
    if (used) searched++;
    if (names) sourced++;
    std::printf("    rep %d: web.search=%d names-a-source=%d\n", i, (int)used, (int)names);
  }
  std::printf("  H20 rate over %d reps: web.search ran %d, reply names a source %d, "
              "answered %d\n",
              reps, searched, sourced, answered);
  r.check(answered == reps, "every rep answered (the turn itself works)");
  r.note("the rate line above is the disposition evidence: the head's choice to search");
}

}  // namespace dispolegs
}  // namespace lab
