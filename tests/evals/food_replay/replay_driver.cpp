// The replay driver: one JSON case per input line, one JSON result per output line. Built against
// any tree, so the table can compare the code before and after a change (see README.md).
#include <ArduinoJson.h>

#include <iostream>
#include <string>

#include "nimbus/orch/claims.h"
#include "nimbus/orch/file_append.h"
#include "nimbus/orch/food_ledger.h"
#include "nimbus/orch/ledger_migrate.h"
#include "nimbus/orch/turn_effects.h"
#if __has_include("nimbus/orch/food_figures.h")
#define HAS_FOOD_FIGURES 1
#include "nimbus/orch/food_figures.h"
#endif

using namespace nimbus::orch;

static void runSum(JsonObjectConst in, JsonObject out) {
  const std::string file = in["file"] | "", day = in["day"] | "";
  const FoodLedger l = scanFoodLedger(file, true, LogDay{day, "GMT"}, kFoodLedgerProject, kFoodLedgerOwnerName);
  out["kcal"] = l.kcal;
  out["protein"] = l.protein;
  out["rows"] = l.kcalRows;
  out["unread"] = l.kcalUnread;
  const std::string block = renderLedgerBlock(l, true);
  out["block"] = block;
  double total = 0;
  if (csvDayTotal(file, "kcal", day, total)) out["receipt"] = total;
}

// A stored legacy log folded into the ledger, then the day read: what the owner sees after the first start.
static void runFold(JsonObjectConst in, JsonObject out) {
  const LedgerMigration m = planLedgerMigration(std::string(in["file"] | ""), std::string(in["ledger"] | ""));
  const std::string day = in["day"] | "";
  const FoodLedger l = scanFoodLedger(m.out, true, LogDay{day, "GMT"}, kFoodLedgerProject, kFoodLedgerOwnerName);
  out["kcal"] = l.kcal;
  out["protein"] = l.protein;
  out["rows"] = l.kcalRows;
  out["folded"] = m.folded;
  out["dropped"] = m.dropped;
}

static void runAppend(JsonObjectConst in, JsonObject out) {
  const std::string file = in["file"] | "", row = in["row"] | "", day = in["day"] | "";
  const CsvUpsert up = planCsvUpsert(file, row, "meal", "kcal", day);
  out["reply"] = up.ok ? renderUpsertReply(up, "food/food-log.csv", "kcal", "GMT") : "refused: " + up.err;
}

static void runReply(JsonObjectConst in, JsonObject out) {
  const std::string reply = in["reply"] | "", owner = in["owner"] | "";
  TurnEffects none;
  ClaimEvidence ev;
  ev.fx = &none;
#ifdef HAS_FOOD_FIGURES
  const ClaimScan scan = scanClaims(reply, ownerAskedHow(owner), ownerAskedKeep(owner));
#else
  const ClaimScan scan = scanClaims(reply, ownerAskedHow(owner));
#endif
  const std::vector<ClaimGap> gaps = claimGaps(reply, scan, ev);
  std::string tail = claimTail(gaps);
#ifdef HAS_FOOD_FIGURES
  FoodFacts facts;
  JsonObjectConst f = in["facts"];
  if (!f.isNull()) {
    facts.have = true;
    facts.kcal = f["kcal"] | 0.0;
    facts.protein = f["protein"] | 0.0;
    facts.rows = f["rows"] | 1;
    facts.proteinUnknown = f["protein_unknown"] | 0;
  }
  const std::string sources = figureNumbers(std::string(in["sources"] | "") + " " + ownerStatementsOf(owner), 4096);
  tail += foodGapTail(foodFigureGaps(reply, facts, sources), facts);
  const std::string routine = in["routine_day"] | "";
  int age = 0;
  if (!routine.empty() && !staleStatusIn(reply, routine, kStaleStatusDays, &age).empty())
    tail += "\n\n(Note: that comes from a record " + std::to_string(age) + " days old; the device has nothing newer.)";
#endif
  out["tail"] = tail;
}

int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    JsonDocument in, out;
    if (deserializeJson(in, line) != DeserializationError::Ok) {
      std::cout << "{\"error\":\"bad json\"}\n";
      continue;
    }
    const std::string kind = in["kind"] | "";
    JsonObject o = out.to<JsonObject>();
    o["id"] = in["id"] | "";
    if (kind == "sum") runSum(in.as<JsonObjectConst>(), o);
    else if (kind == "append") runAppend(in.as<JsonObjectConst>(), o);
    else if (kind == "fold") runFold(in.as<JsonObjectConst>(), o);
    else runReply(in.as<JsonObjectConst>(), o);
    serializeJson(out, std::cout);
    std::cout << "\n";
  }
  return 0;
}
