// Source-level relocation gate for the historical collision-strategy probe.
//
// The former body is pinned at
// manyforge@08222188293df8e12f90dc72e204c21f22f92ea1. It constructed
// NativeKernelBundle to obtain FK, observed state, mutable scene snapshots,
// and planner comparison. Those owners are intentionally absent. Distribution
// and fixture constants were extracted to collision_qualification_support.hpp;
// this target now fails before model, filesystem, clock, thread, or query work.

#include "collision_qualification_support.hpp"

#include <array>

namespace {

using manyforge::qualification::AcceptanceDisposition;

constexpr std::array<AcceptanceDisposition, 5> kAcceptance{{
    {"fixture_model_fk_and_snapshot", "I6/N",
     "build an addressed fixture from owner-issued model and scene products"},
    {"legacy_planner_parity", "I6/N",
     "compare through a diagnostic observer without planner scene authority"},
    {"selected_and_all_active_distance_modes", "I8/P5",
     "retain collision coverage while policy selects bounded distance work"},
    {"runtime_mechanism_counters", "I6/N+I8/P5",
     "record collision, distance, FK, allocation, and latency facts"},
    {"percentile_mad_and_json_projection", "retained/current",
     "use owner-neutral distribution helpers without claiming qualification"},
}};

}  // namespace

int main() {
  return manyforge::qualification::emit_unavailable(
      "perf_collision_strategy", "I6/N+I8/P5", kAcceptance);
}
