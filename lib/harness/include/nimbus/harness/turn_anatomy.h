#pragma once
#include <cstddef>
#include <string>
#include <vector>

// turn_anatomy - the head of a turn anatomy page (/api/lastturn, the per-turn
// dossier file, the hosted daemon's last-turn page).
//
// The page must tell the truth about what the model saw. A stateless tool-loop
// turn holds no provider-side conversation, yet the device itself sends history
// in the system prompt: the conversation summary, the recent window, the running
// memory and the recalled memories. So the header names the provider chain and
// the device history separately, both MEASURED on the prompt as sent (never the
// inputs the composer was offered), and the legend lists every "## " section the
// prompt carries, with the cache block that holds it.
namespace agent {

struct TurnDebugEv;   // hooks.h

// The history the device put in this turn's system prompt.
struct HistoryFacts {
  size_t summaryBytes = 0;        // the conversation summary block as sent (0 = none sent)
  bool summaryClipped = false;    // the device held more summary than it sent (cut or dropped)
  int windowRows = 0;             // rows of the recent conversation window as sent
  size_t windowBytes = 0;         // the window section as sent
  size_t runningMemoryBytes = 0;  // the running memory note as sent
  int recalledItems = 0;          // recalled long-term memories as sent
};

// Measure `instructions` (the composed prompt: the joined layout string, or a
// legacy single string). `heldSummary` and `runningMemory` are what the device
// offered the composer (ComposeInputs), to tell a clipped summary from a whole one.
HistoryFacts measureHistory(const std::string& instructions, const std::string& heldSummary,
                            const std::string& runningMemory);

// True when a stored conversation id is a real provider-side conversation (an
// OpenAI response chain, a Mistral conversation). False for "" and for the
// stateless markers the adapters store ("messages" on Anthropic, "chat" on the
// Mistral loop and the custom head): those hold nothing on the provider.
bool providerChainHeld(const std::string& convId);

// Every "## " heading line of a prompt, in order (the line without its "## ").
std::vector<std::string> promptHeadings(const std::string& prompt);

// The legend under section 1: one line per "## " heading of `prompt` (the
// display form, layout::display), tagged with the block that carries it.
std::string sectionLegend(const std::string& prompt);

// The page head: title, host, provider chain, device history, result, the
// section list, and the section 1 heading with its legend. The caller appends
// the prompt itself (*ev.instructions) and the later sections.
std::string turnAnatomyHeader(const TurnDebugEv& ev);

}  // namespace agent
