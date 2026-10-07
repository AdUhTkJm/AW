#ifndef AW_TEST_PLAN_SAMPLES_H
#define AW_TEST_PLAN_SAMPLES_H

// Sample .awr dumps shared by more than one test file, plus a subgraph probe.
// A sample a single test reads lives next to that test.

#include <cstddef>
#include <cstdint>

#include "aw/plan/CraftingGraph.h"
#include "aw/utils/PodVector.h"

namespace plan {

aw::vector<std::byte> buildZeroInputSample();
aw::vector<std::byte> buildPlanSample();
aw::vector<std::byte> buildPlanLeafSample();
aw::vector<std::byte> buildBatchRecycleSample();
aw::vector<std::byte> buildBlackCandleSample();

// True when `source` appears in the subgraph as a recipe origin.
bool subgraphHasRecipe(const aw::Subgraph &sub, uint32_t source);

}  // namespace plan

#endif
