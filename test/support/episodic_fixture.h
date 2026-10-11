#pragma once
// Recorded episodic history for host tests (CUM-515): a device day-stream JSONL
// file (one EpisodicMessage per line, the encodeEpisodicLine format) under
// test/support/fixtures/, loaded into rows a test can feed to any store.
//
// l4_persona_b_episodic_2026-10-10.jsonl is bench unit B's web chat from the
// L4-PERSONA-B run (fw v4.5.8, 2026-10-10, evidence/turns.jsonl): every prompt
// and reply from step 1 through 12d, then the step 9b question "Who is my PhD
// advisor again?" - the rows the device held when the model searched
// "advisor supervisor PhD" and was told "No matching episodic messages."
#include <cstdio>
#include <string>
#include <vector>

#include "nimbus/orch/episodic_log.h"

inline constexpr const char* kL4EpisodicFixture = "l4_persona_b_episodic_2026-10-10.jsonl";

// The other step-9 searches the bench model made (lastturn_recall2..5.txt), each
// with text from the fact row it must now reach in ONE call.
struct L4Recall { const char* text; const char* want; };
inline constexpr L4Recall kL4Recalls[] = {
    {"research boat boat vessel name", "Nereid II"},
    {"field site coordinates", "39.5694 N, 2.6502 E"},
    {"grant report due", "15 January 2027"},
    {"usual coffee order", "cortado with oat milk"},
};

inline std::vector<nimbus::orch::EpisodicMessage> loadEpisodicFixture(const char* name) {
  std::vector<nimbus::orch::EpisodicMessage> rows;
  const std::string path = std::string("test/support/fixtures/") + name;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return rows;
  std::string all;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n);
  std::fclose(f);
  size_t p = 0;
  while (p < all.size()) {
    size_t e = all.find('\n', p);
    if (e == std::string::npos) e = all.size();
    nimbus::orch::EpisodicMessage m;
    if (nimbus::orch::decodeEpisodicLine(all.substr(p, e - p), m)) rows.push_back(m);
    p = e + 1;
  }
  return rows;
}
