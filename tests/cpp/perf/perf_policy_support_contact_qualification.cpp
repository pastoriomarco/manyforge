// Source-level relocation gate for support-contact policy performance.
//
// The owner-neutral transition/partition reference behavior is already active
// in manyforge_core tests (collision_policy_evaluator,
// ordered_contact_validation, collision_validation_coordinator, and
// parallel_collision_validation_worker_pool). The exact historical fixture
// constants remain below; only model/scene/runtime composition is gated.

#include "collision_qualification_support.hpp"

#include <array>

namespace {

using manyforge::qualification::AcceptanceDisposition;

static_assert(manyforge::qualification::SupportContactFixtureContract::
                  kPositivePenetrationCapM == 0.03);
static_assert(manyforge::qualification::SupportContactFixtureContract::
                  kReleaseToleranceM == 0.001);
static_assert(manyforge::qualification::SupportContactFixtureContract::
                  kDistractorCount == 10);

constexpr std::array<AcceptanceDisposition, 7> kAcceptance{{
    {"precise_reference_state_machine", "retained/manyforge_core",
     "exhaustive transition and partition-order behavior remains in core tests"},
    {"support_fixture_snapshot_and_discovery", "I6/N",
     "resolve configured/unconfigured/far pairs from owner-issued identities"},
    {"binary_contact_mode", "I8/P5",
     "retain collision-only release semantics without numeric overclaim"},
    {"signed_penetration_release_mode", "I8/P5",
     "retain ordered release, plateau, increase, separation, and recontact rules"},
    {"bounded_penetration_mode", "I8/P5",
     "enforce every colliding sample against the exact positive cap"},
    {"parallel_completion_order_equivalence", "retained/manyforge_core+I8/P5",
     "retain pure equivalence now and remeasure on final worker composition"},
    {"same_geometry_latency_and_mechanism_evidence", "I8/P5",
     "collect policy-mode evidence only after addressed runtime restoration"},
}};

}  // namespace

int main() {
  return manyforge::qualification::emit_unavailable(
      "perf_policy_support_contact_qualification", "I6/N+I8/P5",
      kAcceptance);
}
