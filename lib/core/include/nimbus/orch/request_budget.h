#pragma once
#include <cstdint>
#include <string>

// request_budget - what one request to a head may carry: its model's window, the prompt
// it aims at, the bytes per token that turn tokens into body bytes, the body budget it
// is refused over, the round's deadline and the rates its time fits at. Declared by lane
// P0 of the history fix (16 Nov 2026) with TODAY's figures: every wire refuses a body
// over 85,000 B (providers::kWireBodyMax), bytes per token is the budget's convention
// (4, kBudgetBytesPerToken), and no rate is modelled. Lane B derives the figures per
// host and model; the host and model that send a request decide its budget.
// Arduino-free, host-tested (test_engine_seams).
namespace nimbus {
namespace orch {

// The one body budget every wire has today (providers::kWireBodyMax; test_engine_seams
// pins the two equal).
inline constexpr uint32_t kLegacyWireBodyBytes = 85000;

struct RequestBudget {
  uint32_t windowTokens = 0;     // the head model's input window (modelCtxTokens today)
  uint32_t targetTokens = 0;     // the prompt the budget aims at: wireBodyBytes / bytes per token
  uint16_t rNomX100 = 400;       // nominal body bytes per token, x100
  uint16_t rSafeX100 = 400;      // dense (worst-case) body bytes per token, x100
  uint32_t wireBodyBytes = kLegacyWireBodyBytes;   // a body over this is refused before the send
  uint32_t roundMs = 0;          // the round's deadline (the head's socket ceiling)
  uint32_t prefillTokPerS = 0;   // the prompt's slowest read rate; 0 = not modelled
  uint32_t decodeTokPerS = 0;    // the answer's slowest write rate; 0 = not modelled
  bool routed = false;           // the Cumulo router relays the request
  bool flat = false;             // the flat single-string wire (the custom endpoint)
};

// The budget of a request `host` sends on `model` (`routed`: through the router).
// Today's body: the window from modelCtxTokens, the 85,000 B body budget, the head's
// round deadline (Mistral 65 s, Z.ai 50 s, every other head 120 s) and the custom
// endpoint on the flat wire.
RequestBudget requestBudgetFor(const std::string& host, const std::string& model, bool routed);

}  // namespace orch
}  // namespace nimbus
