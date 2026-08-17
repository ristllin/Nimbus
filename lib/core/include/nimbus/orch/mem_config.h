#pragma once
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

// mem_config - the retrieval/decay knobs that tune how associative memory is
// injected each turn. The SAME config is adjustable by BOTH the model (a
// `memory.config` tool, clamped to these ranges) and the user (the web
// dashboard). Every setter clamps, so an out-of-range value from either side
// is coerced, never rejected - the model's tool and the API PUT both run
// through the clamp.
//
// Header-only + Arduino-free so it is host-tested and shared by the portable
// context assembler (which reads retrievalCount / maxContextBytes) and the
// device web layer (which reads/writes it via NVS).
namespace nimbus {
namespace orch {

struct MemConfig {
  // Defaults, with max_context in BYTES (device) not tokens.
  int   retrievalCount   = 10;      // vector memories injected per turn (1..100)
  float relevanceThreshold = 0.0f;  // min cosine similarity to include (0..1; 0=all)
  float decayFactor      = 0.95f;   // importance decay per maintenance cycle (0.5..1)
  int   maxContextBytes  = 0;       // assembled-prompt byte budget. 0 = AUTO - derived
                                    // per turn from the head model's context window
                                    // (nimbus::orch::deriveBudget; 32768 at the 200K
                                    // anchor, so auto == the old default on today's
                                    // fleet). Non-zero = owner override (4096..65536).
  int   maxVectors       = 5000;    // VDB capacity (working set is PSRAM-resident; ~550 B/vec (measured: struct + content strings) ≈ 1.3 MB of 8 MB). At the cap, add()
                                    // evicts the lowest importance*ttl-left score.
  // Composite-recall tunables (consumed by VectorMemory::recall via RecallParams).
  int   recencyHalfLifeHours = 168; // recency weight halves each half-life (24..2160)
  float mmrLambda        = 0.7f;    // MMR relevance-vs-diversity weight (0..1)

  // Clamp bounds, shared by the model tool and the user API.
  static constexpr int   kRetrievalMin = 1,   kRetrievalMax = 100;
  static constexpr float kRelevanceMin = 0.0f, kRelevanceMax = 1.0f;
  static constexpr float kDecayMin = 0.5f,    kDecayMax = 1.0f;
  // 65536 (not 128 KB): a 128 KB internal-heap prompt string is the OOM class the
  // device documents; 64 KB is already 2x the historical default.
  static constexpr int   kContextMin = 4096,  kContextMax = 65536;
  static constexpr int   kMaxVectorsMin = 0,  kMaxVectorsMax = 20000;
  static constexpr int   kRecencyHlMin = 24,  kRecencyHlMax = 2160;
  static constexpr float kMmrMin = 0.0f,      kMmrMax = 1.0f;

  void setRetrievalCount(int v) { retrievalCount = clampI(v, kRetrievalMin, kRetrievalMax); }
  void setRelevanceThreshold(float v) { relevanceThreshold = clampF(v, kRelevanceMin, kRelevanceMax); }
  void setDecayFactor(float v) { decayFactor = clampF(v, kDecayMin, kDecayMax); }
  // 0 (or negative) = auto/derived; a positive value clamps to the owner range.
  void setMaxContextBytes(int v) { maxContextBytes = v <= 0 ? 0 : clampI(v, kContextMin, kContextMax); }
  void setMaxVectors(int v) { maxVectors = clampI(v, kMaxVectorsMin, kMaxVectorsMax); }
  void setRecencyHalfLifeHours(int v) { recencyHalfLifeHours = clampI(v, kRecencyHlMin, kRecencyHlMax); }
  void setMmrLambda(float v) { mmrLambda = clampF(v, kMmrMin, kMmrMax); }

  // Apply one named field (the model tool + the web API both dispatch here).
  // Returns true if `key` was a known field (value is clamped on the way in).
  bool applyInt(const std::string& key, int v) {
    if (key == "retrieval_count")  { setRetrievalCount(v);  return true; }
    if (key == "max_context_bytes"){ setMaxContextBytes(v); return true; }
    if (key == "max_vectors")      { setMaxVectors(v);      return true; }
    if (key == "recency_half_life_hours") { setRecencyHalfLifeHours(v); return true; }
    return false;
  }
  bool applyFloat(const std::string& key, float v) {
    if (key == "relevance_threshold") { setRelevanceThreshold(v); return true; }
    if (key == "decay_factor")        { setDecayFactor(v);        return true; }
    if (key == "mmr_lambda")          { setMmrLambda(v);          return true; }
    return false;
  }

  // NVS persistence: key=value lines, tolerant (unknown keys skipped, missing
  // keys keep their default). This is the fix for the silent reset-on-reboot bug
  // - the device loads this in begin() and rewrites it on every config change.
  std::string serialize() const {
    std::string s;
    s += "retrieval_count=" + std::to_string(retrievalCount) + "\n";
    s += "relevance_threshold=" + fstr(relevanceThreshold) + "\n";
    s += "decay_factor=" + fstr(decayFactor) + "\n";
    s += "max_context_bytes=" + std::to_string(maxContextBytes) + "\n";
    s += "max_vectors=" + std::to_string(maxVectors) + "\n";
    s += "recency_half_life_hours=" + std::to_string(recencyHalfLifeHours) + "\n";
    s += "mmr_lambda=" + fstr(mmrLambda) + "\n";
    return s;
  }
  bool deserialize(const std::string& blob) {
    size_t i = 0;
    bool any = false;
    while (i < blob.size()) {
      size_t nl = blob.find('\n', i);
      if (nl == std::string::npos) nl = blob.size();
      size_t eq = blob.find('=', i);
      if (eq != std::string::npos && eq < nl) {
        std::string k = blob.substr(i, eq - i);
        std::string v = blob.substr(eq + 1, nl - eq - 1);
        if (!k.empty()) {
          // Try float then int keys; a float value coerces for int keys via atoi.
          if (applyFloat(k, (float)atof(v.c_str())) || applyInt(k, atoi(v.c_str())))
            any = true;
        }
      }
      i = nl + 1;
    }
    return any;
  }

 private:
  static int   clampI(int v, int lo, int hi)       { return std::max(lo, std::min(hi, v)); }
  static float clampF(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
  static std::string fstr(float f) {
    char b[24]; snprintf(b, sizeof(b), "%.4g", (double)f); return std::string(b);
  }
};

}  // namespace orch
}  // namespace nimbus
