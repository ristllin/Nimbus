#pragma once
// The LAN mock's claim-check head turns, byte for byte as tests/hil/mock_llm.py
// (CLAIM_TURNS) answers them, so the host suites check the exact turns the bench
// legs run (tests/hil/test_claim_check_bench_host.py pins the two together), and
// the owner lines those legs expect.
namespace harness_test {

// "mock-claim-saved": a "Saved it" reply with one memory write.
inline constexpr char kMockClaimSavedTurn[] = R"({"reply": "Saved it to memory.", "mem_write": [{"content": "bench claim check note", "importance": 0.5, "permanent": false, "ttl": "days"}]})";
// "mock-claim-durable": a durability claim with one scratchpad item.
inline constexpr char kMockClaimDurableTurn[] = R"({"reply": "Put it in my notes, so it will survive a restart.", "scratchpad": {"active": null, "short": ["bench claim check note"], "mid": null, "long": null}})";

inline constexpr char kNotSavedLine[] = "(Correction: that has not been saved.)";
inline constexpr char kRamOnlyLine[] =
    "(Note: this is kept only until the next restart; saving it permanently failed.)";

}  // namespace harness_test
