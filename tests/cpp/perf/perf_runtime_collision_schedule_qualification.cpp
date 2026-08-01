// Source-level relocation gate for the immediate/lookahead schedule campaign.
// Scheduler, monitor, lookahead-selection, policy-transition, and distribution
// algorithms remain in focused Core/RoboPlan tests. The historical campaign
// cannot run until one addressed I6 runtime and I8 Stop/policy composition
// supplies its snapshot, observed state, progress, and authority facts.

#include "collision_qualification_support.hpp"

#include <array>

namespace {

using manyforge::qualification::AcceptanceDisposition;
using Schedule = manyforge::qualification::RuntimeScheduleContract;

static_assert(Schedule::kMonitorPeriodNs == 50'000'000);
static_assert(Schedule::kImmediateQueryBudgetNs == 50'000'000);
static_assert(Schedule::kLookaheadCycleBudgetNs == 200'000'000);
static_assert(Schedule::kLookaheadHorizonNs == 2'000'000'000);
static_assert(Schedule::kLookaheadQueryBudgetNs == 75'000'000);
static_assert(Schedule::kLookaheadSamples == 32);
static_assert(Schedule::kLookaheadMinParallelWaypoints == 8);
static_assert(Schedule::kTrajectoryWaypointCount == 201);
static_assert(Schedule::kTrajectoryStepS == 0.02);

constexpr std::array<AcceptanceDisposition, 8> kAcceptance{{
    {"schedule_configuration_and_lookahead_selection", "retained/manyforge_core",
     "exact periods, budgets, horizon, sampling, and selection remain testable"},
    {"fixture_snapshot_policy_and_trajectory", "I6/N",
     "consume addressed model, scene, state, and trajectory products"},
    {"immediate_monitor_campaign", "I8/P5",
     "prove periodic progress, deadline behavior, and fail-closed outcomes"},
    {"lookahead_worker_campaign", "I8/P5",
     "prove bounded fanout, earliest failure, and revision authority fencing"},
    {"cold_snapshot_preparation", "I6/N+I8/P5",
     "separate changed-revision allocations and latency from warm queries"},
    {"warm_query_allocation_attribution", "I8/P5",
     "attribute framework and engine allocations without interposed owners"},
    {"binary_release_and_bounded_penetration_arms", "I8/P5",
     "requalify all policy arms on identical owner-issued geometry"},
    {"timing_observation_and_replay_evidence", "I8/P5",
     "restore the two-pass evidence schema only with final runtime authority"},
}};

}  // namespace

int main() {
  return manyforge::qualification::emit_unavailable(
      "perf_runtime_collision_schedule_qualification", "I6/N+I8/P5",
      kAcceptance);
}
