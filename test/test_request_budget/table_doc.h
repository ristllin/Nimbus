#pragma once
// table_doc - writes docs/reference/request-budgets.md from the request budget code
// (test_request_budget compares the file with it; GOLDEN_UPDATE=1 rewrites it). Included by
// main.cpp after its Head rows and helpers.
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include "nimbus/orch/router_route.h"

// The adapter a head's requests go out on, and the model it sends there.
struct Wire {
  std::string host;
  std::string model;
};
static Wire wireOf(const Head& h) {
  if (h.host != "cumulo") return {h.host, h.model};
  const RouterRoute r = resolveRouterRoute(h.model);
  return {r.upstream, r.model};
}

static std::string num(uint64_t v) { return std::to_string(v); }
static std::string ratio(uint16_t x100) {
  char b[16];
  std::snprintf(b, sizeof b, "%u.%u", (unsigned)(x100 / 100), (unsigned)(x100 % 100 / 10));
  return b;
}

// "first / retry" of a final round at the budget's own load, "none" for an uncapped host.
static std::string capsCell(const Head& h, const RequestBudget& b, bool fragmented) {
  const Wire w = wireOf(h);
  providers::RoundLoad load;
  load.bodyBytes = b.wireBodyBytes;
  load.promptTokens = b.targetTokens;
  load.routed = b.routed;
  load.fragmented = fragmented;
  const providers::OutputCap c = providers::outputCapFor(w.host, w.model, providers::RoundKind::FinalRound, b.roundMs, load);
  if (!c.maxTokens) return "none";
  return num(c.maxTokens) + " / " + (c.retryTokens ? num(c.retryTokens) : std::string("-"));
}

static std::string sourceOf(const Head& h) {
  const Wire w = wireOf(h);
  return listedWindowTokens(w.host, w.model) ? "listed" : "table";
}

static std::string cells(const std::vector<std::string>& v) {
  std::string out = "|";
  for (const std::string& c : v) out += " " + c + " |";
  return out + "\n";
}

static std::string headRow(const Head& h, BudgetLink link) {
  const RequestBudget b = requestBudgetFor(h.host, h.model, h.routed, link);
  const ContextBudget c = deriveRequestContext(b, {});
  return cells({h.host, h.model, num(b.windowTokens), sourceOf(h), budgetBoundSlug(b.bound), num(b.targetTokens),
                num(b.wireBodyBytes), ratio(b.rNomX100), ratio(b.rSafeX100), num(b.readWindowMs / 1000),
                num(b.prefillTokPerS), num(b.decodeTokPerS), num(b.minFinalTokens), num((uint32_t)c.systemPromptBytes),
                num(c.toolResultBytes), num(c.recallBytes), num(c.replayBytes), num(c.chatSummaryBytes),
                num(c.historyMessageCap), num(c.historyMaxBytes), num(c.historySoftBytes), num(c.historyKeepBytes),
                num(c.foldStepBytes), num(c.foldSliceBytes), num(c.historyRows), num(c.foldTriggerBytes),
                capsCell(h, b, false), capsCell(h, b, true)});
}

static const char* kHeadColumns =
    "| Provider | Model | Window | Source | Bound | Target | Bytes | rNom | rSafe | Read window | Prefill | Decode "
    "| minFinal | System cap | Tool result | Recall | Replay | Summary | Message cap | Hist max | Hist soft "
    "| Hist keep | Fold step | Fold slice | Rows | Trigger | Caps healthy | Caps fragmented |\n";

static std::string separator(size_t n) {
  std::string s = "|";
  for (size_t i = 0; i < n; i++) s += "---|";
  return s + "\n";
}

// The caps read the installed link, so the table installs its own while it is written.
static std::string tableOf(const std::vector<Head>& heads, BudgetLink link) {
  const BudgetLink was = budgetLink();
  installBudgetLink(link);
  std::string t = std::string(kHeadColumns) + separator(28);
  for (const Head& h : heads) t += headRow(h, link);
  installBudgetLink(was);
  return t;
}

// The heads a routed device or a Virtual Nimbus would run, for the two side tables.
static std::vector<Head> mainHeads() {
  return {{"mistral", "mistral-large-4-0"},  {"mistral", "mistral-medium-latest"}, {"openai", "gpt-5.6-luna"},
          {"openai", "gpt-5.6"},             {"anthropic", "claude-sonnet-5-5"},   {"anthropic", "claude-haiku-5-5"},
          {"anthropic", "claude-opus-5-5"},  {"zai", "glm-5.3"},                   {"custom", "gpt-5.6-luna"}};
}

static std::string liftTable() {
  std::string t = "| Provider | Model | Window | Bound | Target | Bytes | Read window | Caps healthy | Caps fragmented |\n" +
                  separator(9);
  for (const Head& h : kRouted) {
    const RequestBudget b = requestBudgetFor(h.host, h.model, true, BudgetLink::Device, true);
    t += cells({h.host, h.model, num(b.windowTokens), budgetBoundSlug(b.bound), num(b.targetTokens),
                num(b.wireBodyBytes), num(b.readWindowMs / 1000), capsCell(h, b, false), capsCell(h, b, true)});
  }
  return t;
}

static std::vector<Head> recordedHeads() {
  std::vector<Head> out;
  for (const recorded::Model& m : recorded::models()) out.push_back({m.provider, m.id, false});
  return out;
}

static std::vector<Head> routedAndCustomHeads() {
  std::vector<Head> out(std::begin(kRouted), std::end(kRouted));
  out.insert(out.end(), std::begin(kCustom), std::end(kCustom));
  return out;
}

static std::string budgetTableDoc() {
  std::string md =
      "# Request budgets\n\n"
      "Generated by `test_request_budget` from the request budget code (`lib/core/src/orch_request_budget.cpp`,\n"
      "`deriveRequestContext` in `lib/core/src/orch_budget.cpp`) and the load-aware output caps\n"
      "(`lib/harness/src/providers/output_caps.cpp`). Do not edit by hand: re-run\n"
      "`GOLDEN_UPDATE=1 pio test -e native -f test_request_budget` and read the diff.\n\n"
      "Each head's budget comes out of one formula: its window (the provider's listed window, else the\n"
      "family table), the bytes per token of its wire (rNom nominal, rSafe dense), the read window of its host,\n"
      "and the slowest measured prefill and decode rates. Bytes is the body the head may send: the minimum of\n"
      "100,000 tokens, the latency bound, 0.75 of the window and 0.9 of a price tier, never under the floor of\n"
      "85,000 B (or 0.85 of a tiny window). Bound names which one decided. Target is bytes over rNom, in tokens.\n"
      "Read window is in seconds, prefill and decode in tokens per second (decode at the target's context),\n"
      "and every other figure in bytes. No figure is below the head's v5.1.1 value. A head at the floor keeps\n"
      "the v5.1.1 history window and its 48 KB fold trigger. Caps are the first attempt and the retry of a\n"
      "final round at the budget, on a healthy and on a fragmented heap (none: the host sends no output cap).\n\n"
      "## Every recorded model\n\n"
      "One row per model the recorded provider lists name (`test/support/fixtures/models_2026-10` and\n"
      "`models_2026-11`), with the windows those lists give.\n\n";
  md += tableOf(recordedHeads(), BudgetLink::Device);
  md += "\n## Routed heads and the custom endpoint\n\n"
        "In v5.1.2 every routed head stays on the 85,000 B floor until the Cumulo router bills cached tokens\n"
        "at their own rate, and the custom endpoint keeps the floor on its flat wire.\n\n";
  md += tableOf(routedAndCustomHeads(), BudgetLink::Device);
  md += "\n## Routed heads once the router meters cached tokens\n\n"
        "What the same formula gives the routed heads once that ships, inside the 90 s the device waits for\n"
        "the router.\n\n";
  md += liftTable();
  md += "\n## Virtual Nimbus\n\n"
        "A Virtual Nimbus sends from a server: no upload rate and no heap pacing enter its fits.\n\n";
  md += tableOf(mainHeads(), BudgetLink::Server);
  return md;
}
