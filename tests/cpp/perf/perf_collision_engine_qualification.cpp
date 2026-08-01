// Source-level relocation gate for the historical collision-engine evidence
// harness. Pure distribution logic is retained in
// collision_qualification_support.hpp. Runtime fixture construction remains
// unavailable rather than silently recreating NativeKernelBundle.

#include "collision_qualification_support.hpp"

#include <array>

namespace {

using manyforge::qualification::AcceptanceDisposition;

constexpr std::array<AcceptanceDisposition, 7> kAcceptance{{
    {"primitive_mesh_and_dynamic_fixture_snapshots", "I6/N",
     "consume exact owner-issued bodies, revisions, freshness, and model state"},
    {"manyforge_engine_correctness_and_fail_closed_geometry", "I6/N",
     "qualify the authoritative engine without a planner-owned scene"},
    {"diagnostic_planner_comparison", "I6/N+I5/H0",
     "emit source-owned mismatch facts through the new observer contract"},
    {"serial_parallel_validation", "I8/P5",
     "requalify bounded worker selection, ordering, and failure equivalence"},
    {"warm_cold_and_revision_change_runtime_queries", "I8/P5",
     "measure explicit changed-snapshot and warm-query budgets separately"},
    {"stale_dynamic_source_failure", "I6/N+I8/P5",
     "fail closed on owner-issued freshness evidence without local synthesis"},
    {"latency_allocation_counter_and_json_evidence", "retained/current+I8/P5",
     "retain metric shapes; collect evidence only after runtime composition"},
}};

}  // namespace


int main() {
  return manyforge::qualification::emit_unavailable(
      "perf_collision_engine_qualification", "I6/N+I8/P5", kAcceptance);
}
