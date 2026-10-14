#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <thread>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/controller_lifecycle_evaluator.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_roboplan/retained_externally_managed_lifecycle_ports.hpp>
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
     EqualReconstructedDiagnosticKeepsItsExternalGeneration) {
  FakeClock clock;
  clock.set(1'000);
  const auto expected = MakeExpectedDomain();
  const auto payload = [](const char *text) {
    return std::make_shared<const content_identity::CanonicalValue>(
        content_identity::CanonicalValue::Object{
            {"evidence", content_identity::CanonicalValue(std::string(text))}});
  };
  RetainedExternallyManagedDomainSnapshot external{
      1U, ComponentLifecycleStateDTO::kActive, true, {}, std::nullopt,
      payload("same")};
  auto created = RetainedExternallyManagedControllerLifecyclePort::Create(
      expected, {MakePhysicalBinding()},
      [&external](const ControlDomainId &, const TimePoint) {
        return Result<RetainedExternallyManagedDomainSnapshot>::Ok(external);
      },
      clock, 500);
  ASSERT_TRUE(created.ok());
  const auto query = MakeControllerLifecycleObservationQuery(expected.value());
  ASSERT_TRUE(created.value()->Observe(query).ok());
  const auto original = external.diagnostic_snapshot;
  external.diagnostic_snapshot = payload("same");
  ASSERT_NE(external.diagnostic_snapshot.get(), original.get());
  clock.advance(10);
  const auto reconstructed = created.value()->Observe(query);
  ASSERT_TRUE(reconstructed.ok()) << reconstructed.error().message;
  EXPECT_TRUE(reconstructed.value().active_configuration.has_value());
  external.diagnostic_snapshot = payload("changed");
  clock.advance(10);
  const auto changed = created.value()->Observe(query);
  ASSERT_FALSE(changed.ok());
  EXPECT_EQ(changed.error().details.at("reason"),
            "external_controller_lifecycle_generation_invalid");
}

TEST(RetainedExternallyManagedLifecyclePorts,
     BoundedIncidentDiagnosticSurvivesAndOversizedDiagnosticCannotHideFault) {
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
  RetainedExternallyManagedDomainSnapshot external{
      11U,
      ComponentLifecycleStateDTO::kFaulted,
      false,
      {},
      ErrorDTO{"controller_launch_exited",
               "component=robot.1 signal=11 Apply=apply.7",
               {}}};
  auto created = RetainedExternallyManagedBrandLifecyclePort::Create(
      occurrence,
      [&](const ControlDomainId &, const TimePoint) {
        return Result<RetainedExternallyManagedDomainSnapshot>::Ok(external);
      },
      clock, 1'000);
  ASSERT_TRUE(created.ok());
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
  ASSERT_TRUE(observed.ok());
  ASSERT_TRUE(observed.value().diagnostic);
  EXPECT_EQ(observed.value().diagnostic, external.diagnostic);
  EXPECT_EQ(observed.value().observed_state,
            ComponentLifecycleStateDTO::kFaulted);
  // Auxiliary evidence failure does not convert the same primary fault into
  // an unavailable observation or suppress containment.
  ++external.source_generation;
  external.diagnostic->message =
      std::string(kMaxLifecycleErrorMessageBytes + 1U, 'x');
  const auto oversized = created.value()->Observe(query);
  ASSERT_TRUE(oversized.ok()) << oversized.error().message;
  EXPECT_FALSE(oversized.value().diagnostic);
  EXPECT_EQ(oversized.value().observed_state,
            ComponentLifecycleStateDTO::kFaulted);
  EXPECT_EQ(oversized.value().observation_generation,
            external.source_generation);
  for (const auto &malformed :
       {std::string("invalid\xFF", 8U), std::string("non-NFC e\xCC\x81"),
        std::string("truncated\xC3", 10U)}) {
    ++external.source_generation;
    external.diagnostic->message = malformed;
    const auto invalid = created.value()->Observe(query);
    ASSERT_TRUE(invalid.ok()) << invalid.error().message;
    EXPECT_FALSE(invalid.value().diagnostic);
    EXPECT_EQ(invalid.value().observed_state,
              ComponentLifecycleStateDTO::kFaulted);
  }
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
  const RetainedExternallyManagedDomainSnapshot external{
      11U, ComponentLifecycleStateDTO::kActive, true,
      {{LifecycleFactId("external.ready"),
        {LifecycleFactValueKindDTO::kBoolean, true, std::nullopt, std::nullopt}}}};
  std::size_t source_reads = 0U;
  std::optional<ErrorDTO> source_error;
  auto created = RetainedExternallyManagedBrandLifecyclePort::Create(
      occurrence,
      [&](const ControlDomainId &domain, const TimePoint now) {
        ++source_reads;
        EXPECT_EQ(domain, occurrence.control_domain_id);
        EXPECT_EQ(now, clock.now());
        if (source_error) {
          return Result<RetainedExternallyManagedDomainSnapshot>::Err(*source_error);
        }
        return Result<RetainedExternallyManagedDomainSnapshot>::Ok(external);
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

  EXPECT_EQ(observed.value().facts, external.facts);
  for (std::uint64_t read = 1U; read <= 3U; ++read) {
    const auto repeated = created.value()->Observe(query);
    ASSERT_TRUE(repeated.ok()) << repeated.error().message;
    EXPECT_EQ(repeated.value().source_sequence, observed.value().source_sequence + read);
    EXPECT_EQ(repeated.value().observation_generation, observed.value().observation_generation);
    EXPECT_EQ(repeated.value().observed_state, observed.value().observed_state);
    EXPECT_EQ(repeated.value().facts, observed.value().facts);
  }
  EXPECT_EQ(source_reads, 4U);

  // A source-issued failure must pass through with its original diagnostic.
  source_error = ErrorDTO{"source_unavailable", "selected external source unavailable",
                          {{"provider_instance_id", occurrence.provider_instance_id.value()}}};
  const auto unavailable = created.value()->Observe(query);
  ASSERT_FALSE(unavailable.ok());
  EXPECT_EQ(unavailable.error().code, source_error->code);
  EXPECT_EQ(unavailable.error().message, source_error->message);
  EXPECT_EQ(unavailable.error().details, source_error->details);
  EXPECT_EQ(source_reads, 5U);
  source_error.reset();

  auto foreign_domain = query;
  foreign_domain.control_domain_id = ControlDomainId("foreign.domain");
  const auto foreign_domain_result = created.value()->Observe(foreign_domain);
  ASSERT_FALSE(foreign_domain_result.ok());
  EXPECT_EQ(foreign_domain_result.error().details.at("reason"),
            "external_brand_lifecycle_query_invalid");
  EXPECT_EQ(source_reads, 5U);

  auto foreign_epoch = query;
  foreign_epoch.provider_runtime_epoch = RuntimeEpoch("foreign.epoch");
  const auto foreign_epoch_result = created.value()->Observe(foreign_epoch);
  ASSERT_FALSE(foreign_epoch_result.ok());
  EXPECT_EQ(foreign_epoch_result.error().details.at("reason"),
            "external_brand_lifecycle_query_invalid");
  EXPECT_EQ(source_reads, 5U);

  clock.set(-1);
  const auto invalid_clock = created.value()->Observe(query);
  ASSERT_FALSE(invalid_clock.ok());
  EXPECT_EQ(invalid_clock.error().details.at("reason"),
            "external_brand_lifecycle_query_invalid");
  EXPECT_EQ(source_reads, 5U);
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
