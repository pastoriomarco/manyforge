// Source-level relocation gate for matched contact-grasp timing. The old
// harness depended on NativeKernelBundle for FK, scene mutation, observed
// state, and worker construction. Its fixed geometry contract and percentile
// reducer remain compiled in collision_qualification_support.hpp.

#include "collision_qualification_support.hpp"

#include <array>

namespace {

using manyforge::qualification::AcceptanceDisposition;

static_assert(
    manyforge::qualification::ContactGraspFixtureContract::kEnvelopeRadiusM ==
    0.15);
static_assert(
    manyforge::qualification::ContactGraspFixtureContract::kEnvelopeHeightM ==
    0.30);
static_assert(manyforge::qualification::ContactGraspFixtureContract::
                  kMaximumPenetrationM == 0.50);

constexpr std::array<AcceptanceDisposition, 5> kAcceptance{{
    {"matched_contact_grasp_fixture", "I6/N",
     "resolve FK and immutable attachment geometry from addressed products"},
    {"selected_pair_validation", "I8/P5",
     "validate the full trajectory while restricting optional distance work"},
    {"contact_envelope_policy", "I8/P5",
     "enforce the exact radius, height, touch links, and penetration bound"},
    {"mechanism_counter_nonregression", "I8/P5",
     "measure collision enumeration before selected distance evaluation"},
    {"matched_latency_evidence", "I8/P5",
     "recreate pinned/current comparison only on the new owner composition"},
}};

}  // namespace

int main() {
  return manyforge::qualification::emit_unavailable(
      "perf_contact_grasp_validation_nonregression", "I6/N+I8/P5",
      kAcceptance);
}
