#pragma once
#include <cstdint>

// nimbus::fault - a runtime capability-fault registry for RESILIENCE testing.
//
// It lets the HIL suite (and the FAULT test-console command / POST /api/fault)
// mark a capability as "simulated absent/broken" so the firmware's degraded
// paths run ON DEMAND, without physically unplugging hardware. Every capability
// seam checks fault::active(Cap) and, when set, behaves as if that peripheral
// were gone. The point is to PROVE the device keeps running - never a crash or a
// reboot - as capabilities drop out one by one (no SD, no mic, no LED, ...).
//
// Portable + Arduino-free so `pio test -e native` can exercise both the registry
// and the degraded logic it gates. The mask is a single word (a single aligned
// store is atomic on the ESP32 / the host), so the flag itself needs no lock.
//
// FAULT injection is a TEST affordance: only the NIMBUS_TEST console/endpoint can
// SET a fault, so a production build can never enter a simulated-fault state
// (active() then always returns false and every gate is a no-op branch).
namespace nimbus::fault {

// SD  = "card absent" (mount reports gone): drives effHaveSd() -> the degraded
//       tier caps, exactly as a no-card boot. SD_IO = "card present but writes
//       FAIL" (a mid-op pull the boot mount-latch misses): storeSD stays true
//       while appends/blob-writes return failure, so it exercises the demote
//       path that FAULT sd cannot. Append-only enum (COUNT sizes the mask).
// PROVIDER = "every LLM API host is unreachable" (transport returns the same
// "no response" a dead network produces, for every direct LLM API host:
// api.openai.com / api.anthropic.com / api.mistral.ai and the Z.ai base
// (api.z.ai | open.bigmodel.cn, whichever the device probed) - Telegram and
// other hosts stay up, so the alerts a failure path emits still deliver. This
// is the seam the 2026-08-11 field bug class (provider outage -> fold
// retry/notice behavior) is tested through. The host list is a function, not
// a comment: a provider added later (Z.ai, lane MG) joins it here, so "every
// LLM API host" stays true of the code, not just of the doc.
// RELAY_NOSLOT = "the cloud relay serves like v4.5.13": no TLS work slot for its dial
// and big frames, and no busy answer for a big tunneled page during a turn. The
// counter-leg of the concurrent-TLS bench test (it must reproduce the overlap).
// EMBED_FAIL = "only the embedding call fails" with a recorded out-of-credit refusal,
// so the memory-write queue runs on demand (FAULT embed_fail). It lives in this mask
// like every other injected fault, so FAULT clear (clearAll) ends it too.
enum Cap : uint8_t {
  SD = 0, MEMORY, MIC, SPEAKER, LED, SCREEN, SD_IO, PROVIDER, RELAY_NOSLOT, EMBED_FAIL, COUNT
};

bool        active(Cap c);                    // is this capability currently faulted?
void        set(Cap c, bool faulted);         // inject (true) / clear (false) one capability
void        clearAll();                       // clear every injected fault
uint16_t    mask();                           // bit i (i<COUNT) set == Cap i faulted
const char* name(Cap c);                      // "sd" / "memory" / "mic" / "speaker" / "led" / "screen"
bool        parse(const char* s, Cap& out);   // name (case-insensitive) -> Cap; false if unknown

// True when `host` is a direct LLM API host the PROVIDER fault cuts: the fixed
// openai/anthropic/mistral API hosts plus `zaiBase` (the probed Z.ai host,
// "" when the device never probed it). Every other host (Telegram, the relay,
// Tavily, the router) stays reachable so a failure path's owner-visible alerts
// still deliver. The transport seam calls this; the class (every LLM API host,
// no other host) is pinned by test_fault.
bool        providerOutageHost(const char* host, const char* zaiBase);

}  // namespace nimbus::fault
