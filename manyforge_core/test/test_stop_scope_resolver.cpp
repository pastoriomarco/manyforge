// Stop target resolution (stop state model §3.2 S4, §5.4): exact, stale
// metadata, no effect, invalid. Scope capture over snapshots, aggregates and
// acceptance construction are gone with the transaction model; their
// subjects are covered by test_stop_state_core.cpp.

#include <manyforge_core/testing/request_key_fixture.hpp>
#include <gtest/gtest.h>
#include <manyforge_core/stop_scope_resolver.hpp>
#include <string>
#include <vector>

namespace manyforge::core {
namespace {


[[nodiscard]] StopOperationTargetDTO target(const std::uint64_t id, const std::uint64_t revision) {
  return StopOperationTargetDTO{OperationNumber(id), OperationRevision(revision)};
}

TEST(StopScopeResolverTest, CurrentRevisionOfLiveOperationIsExact) {
  EXPECT_EQ(classify_stop_operation_target(target(7U, 3U), OperationRevision(3U), true, false),
            StopTargetResolution::kExact);
}

TEST(StopScopeResolverTest, OldRevisionStopsTheCurrentScopeWithAStaleDiagnostic) {
  EXPECT_EQ(classify_stop_operation_target(target(7U, 2U), OperationRevision(3U), true, false),
            StopTargetResolution::kStaleMetadata);
}

TEST(StopScopeResolverTest, KnownTerminalTargetsHaveNoEffect) {
  // Resident but terminal and effect-certain.
  EXPECT_EQ(classify_stop_operation_target(target(7U, 3U), OperationRevision(3U), false, false),
            StopTargetResolution::kNoEffect);
  // Retired into history.
  EXPECT_EQ(classify_stop_operation_target(target(7U, 3U), std::nullopt, false, true),
            StopTargetResolution::kNoEffect);
}

TEST(StopScopeResolverTest, UnknownTargetIsInvalid) {
  EXPECT_EQ(classify_stop_operation_target(target(9U, 1U), std::nullopt, false, false),
            StopTargetResolution::kUnknown);
}

TEST(StopScopeResolverTest, TerminalButEffectUncertainOperationRequiresContainment) {
  EXPECT_TRUE(stop_requires_containment(OperationStateKind::kExecuting, false));
  EXPECT_TRUE(stop_requires_containment(OperationStateKind::kInterventionRequired, true));
  EXPECT_FALSE(stop_requires_containment(OperationStateKind::kStopped, false));
}

TEST(StopScopeResolverTest, ComponentsAndResourcesResolveAgainstTheComponentScope) {
  const std::vector<StopScopeComponentSnapshotDTO> components{
      {ControllableComponentId("arm.a"), ComponentLifecycleGeneration(1U),
       {ResourceId("arm.a.joints")}},
      {ControllableComponentId("arm.b"), ComponentLifecycleGeneration(1U),
       {ResourceId("arm.b.joints")}}};
  ASSERT_NE(find_stop_component(components, ControllableComponentId("arm.b")), nullptr);
  EXPECT_EQ(find_stop_component(components, ControllableComponentId("arm.c")), nullptr);
  const auto* owner = find_stop_resource_owner(components, ResourceId("arm.a.joints"));
  ASSERT_NE(owner, nullptr);
  EXPECT_EQ(owner->controllable_component_id, ControllableComponentId("arm.a"));
  EXPECT_EQ(find_stop_resource_owner(components, ResourceId("unknown")), nullptr);
}

TEST(StopScopeResolverTest, NormalizedIntentFieldErrorNamesTheFieldThatWidens) {
  StopIntentDTO intent{PublicRequestContextDTO{TestRequestKey("stop.1"), "test"},
                       std::vector<StopOperationTargetDTO>{target(1U, 1U)}, std::nullopt};
  EXPECT_FALSE(NormalizedStopIntentFieldError(intent).has_value());
  intent.selector = std::vector<StopOperationTargetDTO>{target(2U, 1U), target(1U, 1U)};
  EXPECT_TRUE(NormalizedStopIntentFieldError(intent).has_value());
  intent.selector = std::vector<StopOperationTargetDTO>{};
  EXPECT_TRUE(NormalizedStopIntentFieldError(intent).has_value());
}

}  // namespace
}  // namespace manyforge::core
