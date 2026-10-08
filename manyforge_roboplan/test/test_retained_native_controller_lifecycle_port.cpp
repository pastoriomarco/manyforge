#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <manyforge_core/controller_lifecycle_evaluator.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_roboplan/retained_externally_managed_lifecycle_ports.hpp>
#include <manyforge_roboplan/retained_native_controller_lifecycle_port.hpp>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

using namespace manyforge::core;

[[nodiscard]] ControllerLifecycleExpectedDomain MakeExpectedDomain() {
  const ControllerRoleId controller_role("native_execution");
  const ControlDomainId domain_id("domain.arm");
  const ControlDomainRevision domain_revision("domain-revision");
  const ControlBindingId binding_id("binding.arm");
  const ControlBindingRevision binding_revision("binding-revision");
  const ControlInterfaceContractId interface_id(
      "manyforge.native_arm_position");
  const ControlInterfaceRevision interface_revision("interface-revision");
  const ControllerTopologyId topology_id("manyforge.native_arm_direct");
  const TopologyRevision topology_revision("topology-revision");
  const ControlRouteId route_id("arm_position");
  const ActivationSetId activation_id("arm_direct");
  const ControllerInterfaceInstanceDTO interface{ResourceId("robot.joint.1"),
                                                 "position"};

  ControllerLifecycleExpectedDomainDTO value{
      domain_id,
      domain_revision,
      topology_id,
      topology_revision,
      ProductId("product.control.arm"),
      ProductContractId("manyforge.control-domain-control.v2"),
      ProductContractRevision("product-contract-revision"),
      ContentDigest("control-artifact-digest"),
      ContentDigest("source-conformance-digest"),
      RuntimeUnitId("runtime.native"),
      RuntimeEpoch("runtime-epoch.1"),
      ControllerTopologyDTO{
          topology_id,
          topology_revision,
          {ControllerTopologyControllerDTO{
              controller_role,
              {ControllerControlInterfaceClaimDTO{
                  interface_id,
                  interface_revision,
                  {ControllerClaimDirection::kCommand,
                   ControllerClaimDirection::kState}}},
              {},
              {}}},
          {},
          {ControllerActivationSetDTO{activation_id, {controller_role}, {}}},
          {ControlRouteDTO{route_id,
                           interface_id,
                           interface_revision,
                           {controller_role},
                           {activation_id}}}},
      DesiredControlConfigurationDTO{
          domain_id,
          domain_revision,
          topology_id,
          topology_revision,
          activation_id,
          {DesiredControlBindingRouteDTO{binding_id, binding_revision,
                                         route_id}}},
      {ControllerLifecycleExpectedBindingDTO{binding_id,
                                             binding_revision,
                                             interface_id,
                                             interface_revision,
                                             {interface},
                                             {interface}}},
      {ControllerLifecycleExpectedControllerDTO{
          controller_role,
          ControllerImplementationId("implementation.native_execution"),
          ControllerImplementationRevision("implementation-revision"),
          "manyforge::roboplan_backend::RoboPlanExecutionBackend",
          ContentDigest("native-controller-configuration"),
          {interface},
          {interface},
          {},
          {}}},
      {ControllerLifecycleExpectedEndpointDTO{
          binding_id, binding_revision, route_id,
          EndpointRoleId("native_execution_command"),
          "inproc://native-execution/arm", true, true}}};
  auto sealed = verify_controller_lifecycle_expected_domain(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] RuntimeParticipantBinding MakePhysicalBinding() {
  RuntimeParticipantBindingDTO value{
      ProgramId("program.native"),
      ProgramRevision("program-revision"),
      ProgramBindingId("program-binding"),
      RetainedBundleId("bundle"),
      GraphRevision("graph"),
      ComponentRoleId("robot"),
      TargetRoleId("arm"),
      ControllableComponentId("robot.1"),
      ComponentTypeId("robot"),
      ComponentTypeRevision("robot-revision"),
      CommandTargetId("arm"),
      CapabilityId("execution.trajectory.execute"),
      CapabilityDefinitionRevision("capability-revision"),
      std::nullopt,
      {ProgramModelRoleBindingDTO{ModelRoleId("body"),
                                  ModelInstanceId("robot.model"),
                                  ModelRevision("model-revision")}},
      {RuntimeModelGroupBindingDTO{ModelGroupRoleId("arm_group"),
                                   ModelInstanceId("robot.model"),
                                   "arm",
                                   {"joint_1"}}},
      {ProgramFrameRoleBindingDTO{FrameRoleId("tcp"),
                                  ModelInstanceId("robot.model"),
                                  ModelRevision("model-revision"), "tool0"}},
      {RuntimeResourceBindingDTO{ResourceRoleId("arm_joints"),
                                 ResourceId("robot.joint.1"), "joint_1",
                                 "joint_1", CoordinateKind::kAngular}},
      {},
      RuntimeControlPathDTO{
          ControlDomainId("domain.arm"),
          ControlDomainRevision("domain-revision"),
          ControlBindingId("binding.arm"),
          ControlBindingRevision("binding-revision"),
          ControlInterfaceContractId("manyforge.native_arm_position"),
          ControlInterfaceRevision("interface-revision"),
          ControllerTopologyId("manyforge.native_arm_direct"),
          TopologyRevision("topology-revision"),
          ControlRouteId("arm_position"),
          ActivationSetId("arm_direct"),
          ActiveControlConfigurationId("active.native.arm"),
          ActiveControlConfigurationGeneration(1U),
          ProviderInstanceId("provider.native"),
          ProviderManifestId("provider-manifest.native"),
          ProviderManifestRevision("provider-manifest-revision"),
          RuntimeUnitId("runtime.native"),
          RuntimeEpoch("runtime-epoch.1"),
          StateAuthorityId("state.native"),
          StateAuthorityClass::kPredicted,
          StopEvidencePolicyId("stop.native"),
          StopEvidencePolicyRevision("stop-revision"),
          ContainmentGranularity::kResource,
          std::nullopt},
      std::nullopt,
      {},
      {RuntimeUnitEpochBindingDTO{RuntimeUnitId("runtime.native"),
                                  RuntimeEpoch("runtime-epoch.1")}}};
  auto sealed = verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

TEST(RetainedNativeControllerLifecyclePort,
     ObservesExactFreshConstructionOwnedDirectState) {
  FakeClock clock;
  clock.set(1'000);
  const auto expected = MakeExpectedDomain();
  auto created = RetainedNativeControllerLifecyclePort::Create(
      expected, {MakePhysicalBinding()}, clock, 500);
  ASSERT_TRUE(created.ok()) << created.error().message;

  const auto query = MakeControllerLifecycleObservationQuery(expected.value());
  const auto first = created.value()->Observe(query);
  ASSERT_TRUE(first.ok()) << first.error().message;
  EXPECT_EQ(first.value().source_sequence, 1U);
  EXPECT_EQ(first.value().observed_at_ns, 1'000);
  EXPECT_EQ(first.value().valid_until_ns, 1'500);
  ASSERT_TRUE(first.value().active_configuration.has_value());
  EXPECT_EQ(first.value().active_configuration->activation_set_id,
            ActivationSetId("arm_direct"));
  ASSERT_EQ(first.value().controllers.size(), 1U);
  EXPECT_EQ(first.value().controllers.front().activity,
            ControllerLifecycleActivityDTO::kActive);
  ASSERT_EQ(first.value().endpoints.size(), 1U);
  EXPECT_TRUE(first.value().endpoints.front().ready);

  const auto reconciliation =
      ReconcileControllerLifecycle(expected.value(), first.value(), 1'001);
  ASSERT_TRUE(reconciliation.ok()) << reconciliation.error().message;
  EXPECT_EQ(reconciliation.value().disposition,
            ControllerLifecycleReconciliationDispositionDTO::kReconciled);
  const auto plan = BuildControllerLifecycleTransitionPlan(
      expected.value(), first.value(), 1'001);
  ASSERT_TRUE(plan.ok()) << plan.error().message;
  EXPECT_TRUE(plan.value().steps.empty());

  clock.advance(10);
  const auto second = created.value()->Observe(query);
  ASSERT_TRUE(second.ok()) << second.error().message;
  EXPECT_EQ(second.value().source_sequence, 2U);
  EXPECT_EQ(second.value().observed_at_ns, 1'010);
}

TEST(RetainedNativeControllerLifecyclePort,
     RejectsAnyNonExactSourceQueryWithoutRestamping) {
  FakeClock clock;
  const auto expected = MakeExpectedDomain();
  auto created = RetainedNativeControllerLifecyclePort::Create(
      expected, {MakePhysicalBinding()}, clock, 500);
  ASSERT_TRUE(created.ok());
  auto query = MakeControllerLifecycleObservationQuery(expected.value());
  query.source_runtime_epoch = RuntimeEpoch("runtime-epoch.stale");

  const auto missing = created.value()->Observe(query);
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().details.at("reason"),
            "native_controller_query_mismatch");

  const auto exact = created.value()->Observe(
      MakeControllerLifecycleObservationQuery(expected.value()));
  ASSERT_TRUE(exact.ok());
  EXPECT_EQ(exact.value().source_sequence, 1U);
}

TEST(RetainedNativeControllerLifecyclePort,
     RejectsIncompleteOrMismatchedPhysicalBindingCoverage) {
  FakeClock clock;
  const auto expected = MakeExpectedDomain();
  auto missing =
      RetainedNativeControllerLifecyclePort::Create(expected, {}, clock, 500);
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().details.at("reason"),
            "native_controller_binding_coverage_incomplete");

  auto mismatched_value = MakePhysicalBinding().value();
  mismatched_value.control_path->control_route_id = ControlRouteId("other");
  auto mismatched_binding =
      verify_runtime_participant_binding(std::move(mismatched_value));
  ASSERT_TRUE(mismatched_binding.ok());
  auto mismatched = RetainedNativeControllerLifecyclePort::Create(
      expected, {std::move(mismatched_binding.value())}, clock, 500);
  ASSERT_FALSE(mismatched.ok());
  EXPECT_EQ(mismatched.error().details.at("reason"),
            "native_controller_binding_identity_mismatch");
}

TEST(RetainedNativeControllerLifecyclePort,
     NeverClaimsAControllerTransitionWasDispatched) {
  FakeClock clock;
  const auto expected = MakeExpectedDomain();
  auto created = RetainedNativeControllerLifecyclePort::Create(
      expected, {MakePhysicalBinding()}, clock, 500);
  ASSERT_TRUE(created.ok());
  ControllerLifecycleTransitionIntentDTO intent{
      MakeControllerLifecycleObservationQuery(expected.value()),
      1U,
      0U,
      {ControllerLifecycleTransitionKindDTO::kDeactivate,
       {ControllerRoleId("native_execution")}},
      100};
  ASSERT_TRUE(intent.locally_valid());

  const auto result =
      created.value()->ApplyTransitionStep(intent, std::stop_token{});
  EXPECT_EQ(result.dispatch_state,
            ControllerLifecycleTransitionDispatchStateDTO::kNotDispatched);
  EXPECT_FALSE(result.known_result.has_value());
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("reason"),
            "native_controller_topology_immutable");
  EXPECT_TRUE(result.locally_valid());
}

TEST(RetainedExternallyManagedLifecyclePorts,
     ExactExternalControllerStateReconcilesWithoutMutationAuthority) {
  FakeClock clock;
  clock.set(1'000);
  const auto expected = MakeExpectedDomain();
  RetainedExternallyManagedDomainSnapshot external{
      7U, ComponentLifecycleStateDTO::kActive, true, {}};
  auto created = RetainedExternallyManagedControllerLifecyclePort::Create(
      expected, {MakePhysicalBinding()},
      [&external](const ControlDomainId &domain_id, const TimePoint)
          -> Result<RetainedExternallyManagedDomainSnapshot> {
        if (domain_id != ControlDomainId("domain.arm")) {
          return Result<RetainedExternallyManagedDomainSnapshot>::Err(
              {"unexpected_domain", "unexpected domain", {}});
        }
        return Result<RetainedExternallyManagedDomainSnapshot>::Ok(external);
      },
      clock, 500);
  ASSERT_TRUE(created.ok()) << created.error().message;

  const auto query = MakeControllerLifecycleObservationQuery(expected.value());
  const auto observed = created.value()->Observe(query);
  ASSERT_TRUE(observed.ok()) << observed.error().message;
  ASSERT_TRUE(observed.value().active_configuration.has_value());
  ASSERT_EQ(observed.value().controllers.size(), 1U);
  EXPECT_EQ(observed.value().controllers.front().activity,
            ControllerLifecycleActivityDTO::kActive);
  ASSERT_EQ(observed.value().endpoints.size(), 1U);
  EXPECT_TRUE(observed.value().endpoints.front().ready);
  const auto reconciled =
      ReconcileControllerLifecycle(expected.value(), observed.value(), 1'001);
  ASSERT_TRUE(reconciled.ok()) << reconciled.error().message;
  EXPECT_EQ(reconciled.value().disposition,
            ControllerLifecycleReconciliationDispositionDTO::kReconciled);

  ControllerLifecycleTransitionIntentDTO transition{
      query,
      observed.value().source_sequence,
      0U,
      {ControllerLifecycleTransitionKindDTO::kDeactivate,
       {ControllerRoleId("native_execution")}},
      100};
  const auto result =
      created.value()->ApplyTransitionStep(transition, std::stop_token{});
  ASSERT_TRUE(result.locally_valid());
  EXPECT_EQ(result.dispatch_state,
            ControllerLifecycleTransitionDispatchStateDTO::kNotDispatched);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("reason"),
            "external_controller_observer_only");
}

TEST(RetainedExternallyManagedLifecyclePorts,
     ExternalGenerationChangePublishesUnreconciledControllerTruth) {
  FakeClock clock;
  clock.set(1'000);
  const auto expected = MakeExpectedDomain();
  RetainedExternallyManagedDomainSnapshot external{
      1U, ComponentLifecycleStateDTO::kActive, true, {}};
  auto created = RetainedExternallyManagedControllerLifecyclePort::Create(
      expected, {MakePhysicalBinding()},
      [&external](const ControlDomainId &, const TimePoint) {
        return Result<RetainedExternallyManagedDomainSnapshot>::Ok(external);
      },
      clock, 500);
  ASSERT_TRUE(created.ok());
  const auto query = MakeControllerLifecycleObservationQuery(expected.value());
  ASSERT_TRUE(created.value()->Observe(query).ok());

  external = {2U, ComponentLifecycleStateDTO::kDegraded, false, {}};
  clock.advance(10);
  const auto changed = created.value()->Observe(query);
  ASSERT_TRUE(changed.ok()) << changed.error().message;
  EXPECT_FALSE(changed.value().active_configuration.has_value());
  const auto reconciliation =
      ReconcileControllerLifecycle(expected.value(), changed.value(), 1'011);
  ASSERT_TRUE(reconciliation.ok()) << reconciliation.error().message;
  EXPECT_EQ(reconciliation.value().disposition,
            ControllerLifecycleReconciliationDispositionDTO::
                kExternalOrUnknownUnreconciled);

  external = {2U, ComponentLifecycleStateDTO::kFaulted, false, {}};
  clock.advance(10);
  const auto conflicting = created.value()->Observe(query);
  ASSERT_FALSE(conflicting.ok());
  EXPECT_EQ(conflicting.error().details.at("reason"),
            "external_controller_lifecycle_generation_invalid");
}

TEST(RetainedExternallyManagedLifecyclePorts,
     BrandLifecycleIsReadOnlyAndPreservesExternalGeneration) {
  FakeClock clock;
  clock.set(5'000);
  const ResolvedLifecycleProviderOccurrenceDTO occurrence{
      ControlDomainId("domain.arm"),
      ControlDomainRevision("domain-revision"),
      ProviderInstanceId("provider.external"),
      ProviderManifestId("provider-manifest.external"),
      ProviderManifestRevision("provider-manifest-revision"),
      RuntimeUnitId("runtime.native"),
      RuntimeEpoch("runtime-epoch.1"),
      ContentDigest("catalog-digest")};
  auto created = RetainedExternallyManagedBrandLifecyclePort::Create(
      occurrence,
      [](const ControlDomainId &, const TimePoint) {
        return Result<RetainedExternallyManagedDomainSnapshot>::Ok(
            {11U, ComponentLifecycleStateDTO::kActive, true, {}});
      },
      clock, 1'000);
  ASSERT_TRUE(created.ok()) << created.error().message;
  const BrandLifecycleObservationQueryDTO query{
      LifecycleTransactionId("transaction"),
      LifecycleTransactionGeneration(1U),
      ControllableComponentId("robot.1"),
      occurrence.control_domain_id,
      occurrence.control_domain_revision,
      occurrence.provider_instance_id,
      occurrence.provider_manifest_id,
      occurrence.provider_manifest_revision,
      occurrence.provider_runtime_unit_id,
      occurrence.provider_runtime_epoch};
  const auto observed = created.value()->Observe(query);
  ASSERT_TRUE(observed.ok()) << observed.error().message;
  EXPECT_EQ(observed.value().observation_generation, 11U);
  EXPECT_EQ(observed.value().observed_state,
            ComponentLifecycleStateDTO::kActive);

  const BrandLifecyclePrimitiveIntentDTO intent{
      query.lifecycle_transaction_id,
      query.lifecycle_transaction_generation,
      query.controllable_component_id,
      query.control_domain_id,
      query.control_domain_revision,
      query.provider_instance_id,
      query.provider_manifest_id,
      query.provider_manifest_revision,
      query.provider_runtime_unit_id,
      query.provider_runtime_epoch,
      LifecyclePrimitiveId("external.wait_for_ready"),
      LifecyclePrimitiveAttemptId("attempt.1"),
      observed.value().observation_generation,
      100};
  const auto result = created.value()->Dispatch(intent, std::stop_token{});
  ASSERT_TRUE(result.locally_valid());
  EXPECT_EQ(result.dispatch_state,
            LifecyclePrimitiveDispatchStateDTO::kNotDispatched);
  EXPECT_EQ(result.effect, LifecyclePrimitiveEffectDTO::kNone);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("reason"),
            "external_brand_lifecycle_observer_only");
}

TEST(RetainedExternallyManagedLifecyclePorts,
     ConcurrentBrandObserversNeverRejectEachOthersNewerSample) {
  FakeClock clock;
  clock.set(5'000);
  const ResolvedLifecycleProviderOccurrenceDTO occurrence{
      ControlDomainId("domain.arm"),
      ControlDomainRevision("domain-revision"),
      ProviderInstanceId("provider.external"),
      ProviderManifestId("provider-manifest.external"),
      ProviderManifestRevision("provider-manifest-revision"),
      RuntimeUnitId("runtime.native"),
      RuntimeEpoch("runtime-epoch.1"),
      ContentDigest("catalog-digest")};
  // Every sample is a newer external generation, as when the controller
  // binding changes between two observers' samples.
  std::atomic<std::uint64_t> generation{0U};
  auto created = RetainedExternallyManagedBrandLifecyclePort::Create(
      occurrence,
      [&generation](const ControlDomainId &, const TimePoint) {
        return Result<RetainedExternallyManagedDomainSnapshot>::Ok(
            {++generation, ComponentLifecycleStateDTO::kActive, true, {}});
      },
      clock, 1'000);
  ASSERT_TRUE(created.ok()) << created.error().message;
  const BrandLifecycleObservationQueryDTO query{
      LifecycleTransactionId("transaction"),
      LifecycleTransactionGeneration(1U),
      ControllableComponentId("robot.1"),
      occurrence.control_domain_id,
      occurrence.control_domain_revision,
      occurrence.provider_instance_id,
      occurrence.provider_manifest_id,
      occurrence.provider_manifest_revision,
      occurrence.provider_runtime_unit_id,
      occurrence.provider_runtime_epoch};

  std::atomic<int> rejected{0};
  std::vector<std::thread> observers;
  for (int thread = 0; thread < 4; ++thread) {
    observers.emplace_back([&] {
      for (int sample = 0; sample < 500; ++sample) {
        if (!created.value()->Observe(query).ok()) ++rejected;
      }
    });
  }
  for (auto &observer : observers) observer.join();
  EXPECT_EQ(rejected.load(), 0);
}

} // namespace
} // namespace manyforge::roboplan_backend
