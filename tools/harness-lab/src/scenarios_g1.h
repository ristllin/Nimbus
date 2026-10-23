#pragma once
#include "lab_rig.h"
#include "scenarios_g0.h"   // history::Report - the check/note pair every scenario prints through

// Live legs for message-level caching (the cacheable head request).
//
//   cache-gaps   a chat with weeks-style history (seeded rows, then short owner
//                turns through the real engine) at a fixed gap between turns:
//                NIMBUS_G1_GAP_S seconds (default 60), NIMBUS_G1_TURNS live turns
//                (default 4), NIMBUS_G1_SEED_TURNS seeded exchanges (default 16).
//                Prints each turn's provider-reported tokens and its
//                input-equivalent cost (uncached input x1, cache reads x0.1,
//                5-minute writes x1.25, 1-hour writes x2) and the largest request
//                body, so the same run on two builds compares their cost per
//                turn at gaps under 5 minutes, 5-60 minutes and over 60 minutes.
//                Use a distinct --dev-name per concurrent run so runs do not read
//                each other's cached system prompt.
namespace lab {
namespace caching {

void cacheGaps(LabRig& rig, const history::Report& r);

// Input-equivalent tokens of one usage total: what the turn cost in units of
// uncached input tokens. `excludesCache` = the provider reports cache reads and
// writes outside its input count (Anthropic).
double inputEquivalent(uint32_t in, uint32_t cacheRead, uint32_t cacheWrite, uint32_t cacheWrite1h,
                       bool excludesCache);

}  // namespace caching
}  // namespace lab
