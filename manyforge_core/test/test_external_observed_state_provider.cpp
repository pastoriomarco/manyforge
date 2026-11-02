#include <gtest/gtest.h>

#include <atomic>
#include <manyforge_core/external_observed_state_provider.hpp>
#include <stdexcept>
#include <thread>
#include <utility>

namespace manyforge::core {
namespace {

[[nodiscard]] ObservedStateQueryContextDTO query_context(
    const std::int64_t freshness_budget_ns = 200) {
  return ObservedStateQueryContextDTO{ControllableComponentId("robot-a"),
                                      CommandTargetId("arm"),
                                      CapabilityId("state.observed.current"),
                                      ControlDomainId("domain-a"),
                                      ControlDomainRevision("domain-rev-a"),
                                      ControlBindingId("binding-a"),
                                      ControlBindingRevision("binding-rev-a"),
                                      ControllerTopologyId("topology-a"),
                                      TopologyRevision("topology-rev-a"),
                                      ControlRouteId("route-a"),
                                      {ResourceId("resource-1"), ResourceId("resource-2")},
                                      {"joint_1", "joint_2"},
                                      {CoordinateKind::kAngular, CoordinateKind::kLinear},
                                      ControlInterfaceContractId("control-contract-a"),
                                      ControlInterfaceRevision("control-contract-rev-a"),
                                      ActiveControlConfigurationId("active-a"),
                                      ActiveControlConfigurationGeneration(7U),
                                      ProviderInstanceId("state-provider-a"),
                                      ProviderManifestId("state-manifest-a"),
                                      ProviderManifestRevision("state-manifest-rev-a"),
                                      RuntimeEpoch("runtime-epoch-a"),
                                      StateAuthorityId("authority-a"),
                                      StateAuthorityClass::kMeasured,
                                      freshness_budget_ns};
}

[[nodiscard]] ExternalObservedStateSourceContractDTO contract(
    const std::int64_t freshness_budget_ns = 200) {
  return ExternalObservedStateSourceContractDTO{
      CellId("cell-a"),
      RetainedBundleId("bundle-a"),
      GraphRevision("graph-a"),
      ProjectionStageGeneration(5U),
      RuntimeUnitId("local-projection-runtime-a"),
      RuntimeEpoch("local-projection-epoch-a"),
      RuntimeUnitId("peer-runtime-a"),
      {ModelInstanceId("arm-model-a"), ModelInstanceId("gripper-model-a")},
      query_context(freshness_budget_ns),
      "peer-state-a",
      ObservedStateSourceKind::kControllerState};
}

[[nodiscard]] ObservedStatePublicationDTO publication(
    const std::uint64_t sequence = 1U, const double angular_position = 0.25,
    const std::int64_t remote_received_timestamp_ns = 100,
    const std::int64_t freshness_budget_ns = 200) {
  const auto context = query_context(freshness_budget_ns);
  ObservedStateSnapshotDTO snapshot{context.controllable_component_id,
                                    context.command_target_id,
                                    context.control_domain_id,
                                    context.control_domain_revision,
                                    context.control_binding_id,
                                    context.control_binding_revision,
                                    context.provider_instance_id,
                                    context.provider_manifest_id,
                                    context.provider_manifest_revision,
                                    context.controller_topology_id,
                                    context.topology_revision,
                                    context.control_route_id,
                                    context.active_control_configuration_id,
                                    context.active_control_configuration_generation,
                                    context.resources,
                                    sequence,
                                    context.joint_names,
                                    {AngularPositionDTO{angular_position}, LinearPositionDTO{0.1}},
                                    std::nullopt,
                                    "peer-state-a",
                                    context.runtime_epoch,
                                    ObservedStateSourceKind::kControllerState,
                                    80,
                                    remote_received_timestamp_ns,
                                    context.state_authority_id,
                                    context.state_authority_class,
                                    3U,
                                    context.freshness_budget_ns,
                                    true};
  return ObservedStatePublicationDTO{snapshot.controllable_component_id,
                                     snapshot.command_target_id,
                                     snapshot.control_domain_id,
                                     snapshot.control_domain_revision,
                                     snapshot.control_binding_id,
                                     snapshot.control_binding_revision,
                                     snapshot.controller_topology_id,
                                     snapshot.topology_revision,
                                     snapshot.control_route_id,
                                     snapshot.active_control_configuration_id,
                                     snapshot.active_control_configuration_generation,
                                     snapshot.provider_instance_id,
                                     snapshot.provider_manifest_id,
                                     snapshot.provider_manifest_revision,
                                     snapshot.state_authority_id,
                                     snapshot.state_authority_class,
                                     snapshot.source_epoch,
                                     snapshot.authority_generation,
                                     snapshot.sequence,
                                     std::move(snapshot),
                                     remote_received_timestamp_ns};
}

[[nodiscard]] ExternalObservedStatePublicationDTO sample(
    const ExternalObservedStateProvider& provider, ObservedStatePublicationDTO value,
    const std::int64_t receiver_received_timestamp_ns) {
  return ExternalObservedStatePublicationDTO{provider.contract_digest(), std::move(value),
                                             receiver_received_timestamp_ns};
}

TEST(ExternalObservedStateProviderTest,
     ContractIdentityIsDeterministicAndRejectsNoncanonicalModels) {
  const auto source = contract();
  const auto first = external_observed_state_source_contract_digest(source);
  const auto second = external_observed_state_source_contract_digest(source);
  ASSERT_TRUE(first.ok()) << first.error().message;
  ASSERT_TRUE(second.ok()) << second.error().message;
  EXPECT_EQ(first.value(), second.value());
  EXPECT_TRUE(IsExactLowercaseSha256Digest(first.value().value()));

  auto reordered = source;
  std::swap(reordered.model_instance_ids[0], reordered.model_instance_ids[1]);
  EXPECT_FALSE(validate_external_observed_state_source_contract(reordered).ok());

  auto duplicate = source;
  duplicate.model_instance_ids[1] = duplicate.model_instance_ids[0];
  EXPECT_FALSE(validate_external_observed_state_source_contract(duplicate).ok());
}

TEST(ExternalObservedStateProviderTest,
     ConstructionRejectsIncompleteOrAuthorityIncompatibleContracts) {
  auto incomplete = contract();
  incomplete.receiver_runtime_epoch = RuntimeEpoch("");
  EXPECT_FALSE(validate_external_observed_state_source_contract(incomplete).ok());
  EXPECT_THROW((void)ExternalObservedStateProvider(std::move(incomplete)), std::invalid_argument);

  auto mismatched_authority = contract();
  mismatched_authority.publication_source = ObservedStateSourceKind::kPredictedState;
  EXPECT_FALSE(validate_external_observed_state_source_contract(mismatched_authority).ok());

  auto nonexpiring_measured = contract(0);
  EXPECT_FALSE(validate_external_observed_state_source_contract(nonexpiring_measured).ok());

  auto duplicate_resource = contract();
  duplicate_resource.query_context.resources[1] = duplicate_resource.query_context.resources[0];
  EXPECT_FALSE(validate_external_observed_state_source_contract(duplicate_resource).ok());
}

TEST(ExternalObservedStateProviderTest, AcceptsExactSampleAndUsesReceiverLocalFreshnessTime) {
  ExternalObservedStateProvider provider(contract());

  const auto ingested = provider.ingest(sample(provider, publication(), 900));

  ASSERT_TRUE(ingested.ok()) << ingested.error().message;
  EXPECT_EQ(ingested.value(), ExternalObservedStateIngestDisposition::kAccepted);
  const auto current = provider.current_state(query_context(), 1000);
  ASSERT_TRUE(current.ok()) << current.error().message;
  EXPECT_EQ(current.value().received_timestamp_ns, 900);
  EXPECT_EQ(current.value().source_timestamp_ns, 80);
  const auto envelope =
      provider.publication(ControlBindingId("binding-a"), ControlBindingRevision("binding-rev-a"));
  ASSERT_TRUE(envelope.ok()) << envelope.error().message;
  EXPECT_EQ(envelope.value().received_timestamp_ns, 900);
  EXPECT_EQ(envelope.value().snapshot.received_timestamp_ns, 900);
}

TEST(ExternalObservedStateProviderTest,
     RejectsForeignContractAndPublicationInstanceBeforeExposure) {
  ExternalObservedStateProvider provider(contract());
  auto other_contract = contract();
  other_contract.receiver_runtime_epoch = RuntimeEpoch("local-projection-epoch-b");
  const auto other_digest = external_observed_state_source_contract_digest(other_contract);
  ASSERT_TRUE(other_digest.ok()) << other_digest.error().message;
  auto foreign_contract = sample(provider, publication(), 900);
  foreign_contract.source_contract_digest = other_digest.value();
  EXPECT_FALSE(provider.ingest(std::move(foreign_contract)).ok());

  other_contract = contract();
  other_contract.projection_stage_generation = ProjectionStageGeneration(6U);
  const auto other_stage_digest = external_observed_state_source_contract_digest(other_contract);
  ASSERT_TRUE(other_stage_digest.ok()) << other_stage_digest.error().message;
  auto foreign_stage = sample(provider, publication(), 900);
  foreign_stage.source_contract_digest = other_stage_digest.value();
  EXPECT_FALSE(provider.ingest(std::move(foreign_stage)).ok());

  auto foreign_component = publication();
  foreign_component.controllable_component_id = ControllableComponentId("robot-b");
  foreign_component.snapshot.controllable_component_id = ControllableComponentId("robot-b");
  EXPECT_FALSE(provider.ingest(sample(provider, std::move(foreign_component), 900)).ok());

  auto foreign_epoch = publication();
  foreign_epoch.runtime_epoch = RuntimeEpoch("runtime-epoch-b");
  foreign_epoch.snapshot.source_epoch = RuntimeEpoch("runtime-epoch-b");
  EXPECT_FALSE(provider.ingest(sample(provider, std::move(foreign_epoch), 900)).ok());

  auto foreign_source = publication();
  foreign_source.snapshot.source_id = "peer-state-b";
  EXPECT_FALSE(provider.ingest(sample(provider, std::move(foreign_source), 900)).ok());

  auto inconsistent_envelope = publication();
  inconsistent_envelope.snapshot.control_binding_revision = ControlBindingRevision("binding-rev-b");
  EXPECT_FALSE(provider.ingest(sample(provider, std::move(inconsistent_envelope), 900)).ok());

  EXPECT_FALSE(provider.ingest(sample(provider, publication(), -1)).ok());
  EXPECT_FALSE(
      provider.publication(ControlBindingId("binding-a"), ControlBindingRevision("binding-rev-a"))
          .ok());
}

TEST(ExternalObservedStateProviderTest,
     DuplicateDoesNotRefreshAndReplayOrSameSequenceConflictRejects) {
  ExternalObservedStateProvider provider(contract());
  ASSERT_TRUE(provider.ingest(sample(provider, publication(2U), 900)).ok());

  const auto duplicate = provider.ingest(sample(provider, publication(2U), 1050));
  ASSERT_TRUE(duplicate.ok()) << duplicate.error().message;
  EXPECT_EQ(duplicate.value(), ExternalObservedStateIngestDisposition::kDuplicate);
  EXPECT_FALSE(provider.current_state(query_context(), 1101).ok());
  const auto retained_for_observation =
      provider.publication(ControlBindingId("binding-a"), ControlBindingRevision("binding-rev-a"));
  ASSERT_TRUE(retained_for_observation.ok()) << retained_for_observation.error().message;
  EXPECT_EQ(retained_for_observation.value().publication_sequence, 2U);
  EXPECT_EQ(retained_for_observation.value().received_timestamp_ns, 900);

  EXPECT_FALSE(provider.ingest(sample(provider, publication(1U), 1102)).ok());
  EXPECT_FALSE(provider.ingest(sample(provider, publication(2U, 0.5), 1102)).ok());

  const auto advanced = provider.ingest(sample(provider, publication(3U, 0.5), 1102));
  ASSERT_TRUE(advanced.ok()) << advanced.error().message;
  EXPECT_EQ(advanced.value(), ExternalObservedStateIngestDisposition::kAccepted);
}

TEST(ExternalObservedStateProviderTest, ExactQueryAndBindingAreRequiredAfterPublication) {
  ExternalObservedStateProvider provider(contract());
  ASSERT_TRUE(provider.ingest(sample(provider, publication(), 900)).ok());

  auto wrong_query = query_context();
  wrong_query.runtime_epoch = RuntimeEpoch("runtime-epoch-b");
  EXPECT_FALSE(provider.current_state(wrong_query, 1000).ok());
  EXPECT_FALSE(
      provider.publication(ControlBindingId("binding-b"), ControlBindingRevision("binding-rev-a"))
          .ok());
}

TEST(ExternalObservedStateProviderTest, ConcurrentReadsObserveOnlyCompleteImmutablePublications) {
  ExternalObservedStateProvider provider(contract(1000000));
  ASSERT_TRUE(provider.ingest(sample(provider, publication(1U, 0.25, 100, 1000000), 1000)).ok());
  std::atomic<bool> writer_done{false};
  std::atomic<std::uint64_t> failures{0U};

  std::thread writer([&] {
    for (std::uint64_t sequence = 2U; sequence <= 1001U; ++sequence) {
      auto next = publication(sequence, static_cast<double>(sequence) / 1000.0, 100, 1000000);
      const auto result = provider.ingest(
          sample(provider, std::move(next), 1000 + static_cast<std::int64_t>(sequence)));
      if (!result.ok()) {
        failures.fetch_add(1U);
      }
    }
    writer_done.store(true, std::memory_order_release);
  });

  std::thread reader([&] {
    do {
      const auto value = provider.publication(ControlBindingId("binding-a"),
                                              ControlBindingRevision("binding-rev-a"));
      if (!value.ok() || value.value().publication_sequence != value.value().snapshot.sequence ||
          value.value().received_timestamp_ns != value.value().snapshot.received_timestamp_ns ||
          !validate_observed_state_publication(value.value(), value.value().received_timestamp_ns)
               .ok()) {
        failures.fetch_add(1U);
      }
    } while (!writer_done.load(std::memory_order_acquire));
  });

  writer.join();
  reader.join();
  EXPECT_EQ(failures.load(), 0U);
  const auto final =
      provider.publication(ControlBindingId("binding-a"), ControlBindingRevision("binding-rev-a"));
  ASSERT_TRUE(final.ok()) << final.error().message;
  EXPECT_EQ(final.value().publication_sequence, 1001U);
}

}  // namespace
}  // namespace manyforge::core
