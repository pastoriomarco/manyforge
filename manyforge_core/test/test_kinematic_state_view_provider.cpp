#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <manyforge_core/external_observed_state_provider.hpp>
#include <manyforge_core/kinematic_state_view_provider.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <map>
#include <memory>
#include <utility>

namespace manyforge::core {
namespace {

using SourceKey = std::pair<ControlBindingId, ControlBindingRevision>;

class FakeObservedStateProvider final : public IObservedStateProvider {
 public:
  std::map<SourceKey, ObservedStatePublicationDTO> publications;
  mutable std::atomic<std::uint64_t> publication_reads{0U};

  [[nodiscard]] Result<ObservedStateSnapshotDTO> current_state(const ObservedStateQueryContextDTO&,
                                                               std::int64_t) const override {
    return Result<ObservedStateSnapshotDTO>::Err(ErrorCode::kInternalError);
  }

  [[nodiscard]] Result<ObservedStatePublicationDTO> publication(
      const ControlBindingId& binding_id,
      const ControlBindingRevision& binding_revision) const override {
    publication_reads.fetch_add(1U);
    const auto found = publications.find({binding_id, binding_revision});
    if (found == publications.end()) {
      return Result<ObservedStatePublicationDTO>::Err(ErrorCode::kMissingReference);
    }
    return Result<ObservedStatePublicationDTO>::Ok(found->second);
  }
};

[[nodiscard]] ObservedStatePublicationDTO publication(std::vector<ResourceId> resources,
                                                      std::vector<CoordinatePositionDTO> positions,
                                                      std::int64_t received_timestamp_ns = 900) {
  ObservedStateSnapshotDTO snapshot{ControllableComponentId("robot-a"),
                                    CommandTargetId("arm"),
                                    ControlDomainId("domain-a"),
                                    ControlDomainRevision("domain-rev-a"),
                                    ControlBindingId("binding-a"),
                                    ControlBindingRevision("binding-rev-a"),
                                    ProviderInstanceId("state-provider-a"),
                                    ProviderManifestId("state-manifest"),
                                    ProviderManifestRevision("state-manifest-rev"),
                                    ControllerTopologyId("topology-a"),
                                    TopologyRevision("topology-rev-a"),
                                    ControlRouteId("route-a"),
                                    ActiveControlConfigurationId("active-a"),
                                    ActiveControlConfigurationGeneration(7U),
                                    std::move(resources),
                                    11U,
                                    {"joint_1", "joint_2"},
                                    std::move(positions),
                                    std::nullopt,
                                    "local-controller",
                                    RuntimeEpoch("runtime-epoch-a"),
                                    ObservedStateSourceKind::kControllerState,
                                    880,
                                    received_timestamp_ns,
                                    StateAuthorityId("authority-a"),
                                    StateAuthorityClass::kMeasured,
                                    3U,
                                    200,
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
                                     received_timestamp_ns};
}

[[nodiscard]] KinematicResourceCoordinateDependencyDTO dependency(ResourceId resource_id,
                                                                  CoordinateKind kind,
                                                                  double multiplier = 1.0,
                                                                  double offset = 0.0,
                                                                  bool derived_mimic = false) {
  return KinematicResourceCoordinateDependencyDTO{ControlDomainId("domain-a"),
                                                  ControlDomainRevision("domain-rev-a"),
                                                  ControlBindingId("binding-a"),
                                                  ControlBindingRevision("binding-rev-a"),
                                                  std::move(resource_id),
                                                  kind,
                                                  multiplier,
                                                  offset,
                                                  derived_mimic};
}

[[nodiscard]] KinematicFramePathDTO path() {
  std::vector<KinematicFramePathSegmentDTO> segments;
  segments.push_back(KinematicDirectComponentAnchorPathSegmentDTO{
      ControllableComponentId("robot-a"), ModelInstanceId("model-a"), ModelRevision("model-rev-a"),
      "anchor_joint", "cell", "base_link", PlacementPoseDTO{}});
  segments.push_back(KinematicModelJointPathSegmentDTO{
      ControllableComponentId("robot-a"),
      ModelInstanceId("model-a"),
      ModelRevision("model-rev-a"),
      0U,
      "joint_1",
      "robot_a_joint_1",
      "base_link",
      "link_1",
      KinematicJointCoordinateKind::kAngular,
      {dependency(ResourceId("resource-1"), CoordinateKind::kAngular)}});
  segments.push_back(KinematicModelJointPathSegmentDTO{
      ControllableComponentId("robot-a"),
      ModelInstanceId("model-a"),
      ModelRevision("model-rev-a"),
      1U,
      "joint_2",
      "robot_a_joint_2",
      "link_1",
      "tool_link",
      KinematicJointCoordinateKind::kLinear,
      {dependency(ResourceId("resource-2"), CoordinateKind::kLinear, 2.0, 0.1)}});
  return KinematicFramePathDTO{ResolvedComponentModelFrameDTO{
                                   ControllableComponentId("robot-a"), ModelInstanceId("model-a"),
                                   FrameRoleId("tool"), "tool_link", ModelRevision("model-rev-a")},
                               std::move(segments)};
}

[[nodiscard]] KinematicResourceSourceProjectionDTO source_projection(std::string resource_id) {
  return KinematicResourceSourceProjectionDTO{
      ResourceId(std::move(resource_id)),
      {KinematicControlBindingSourceDTO{ControlBindingId("binding-a"),
                                        ControlBindingRevision("binding-rev-a")}},
      ProviderInstanceId("state-provider-a"),
      ProviderManifestId("state-manifest"),
      ProviderManifestRevision("state-manifest-rev"),
      StateAuthorityId("authority-a"),
      StateAuthorityClass::kMeasured,
      ObservedStateSourceKind::kControllerState};
}

[[nodiscard]] KinematicStatePathQueryDTO query() {
  return KinematicStatePathQueryDTO{
      RetainedBundleId("bundle-a"),
      GraphRevision("graph-a"),
      path(),
      {KinematicActiveControlConfigurationExpectationDTO{
          ControlDomainId("domain-a"), ControlDomainRevision("domain-rev-a"),
          ActiveControlConfigurationId("active-a"), ActiveControlConfigurationGeneration(7U)}},
      50,
      {source_projection("resource-1"), source_projection("resource-2")}};
}

[[nodiscard]] std::shared_ptr<FakeObservedStateProvider> valid_source() {
  auto source = std::make_shared<FakeObservedStateProvider>();
  source->publications.emplace(
      SourceKey{ControlBindingId("binding-a"), ControlBindingRevision("binding-rev-a")},
      publication({ResourceId("resource-1"), ResourceId("resource-2")},
                  {AngularPositionDTO{0.5}, LinearPositionDTO{0.2}}));
  return source;
}

[[nodiscard]] ExternalObservedStateSourceContractDTO external_contract() {
  return ExternalObservedStateSourceContractDTO{
      CellId("cell-a"),
      RetainedBundleId("bundle-a"),
      GraphRevision("graph-a"),
      ProjectionStageGeneration(5U),
      RuntimeUnitId("local-projection-runtime-a"),
      RuntimeEpoch("local-projection-epoch-a"),
      RuntimeUnitId("peer-runtime-a"),
      {ModelInstanceId("model-a")},
      ObservedStateQueryContextDTO{ControllableComponentId("robot-a"),
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
                                   ProviderManifestId("state-manifest"),
                                   ProviderManifestRevision("state-manifest-rev"),
                                   RuntimeEpoch("runtime-epoch-a"),
                                   StateAuthorityId("authority-a"),
                                   StateAuthorityClass::kMeasured,
                                   200},
      "local-controller",
      ObservedStateSourceKind::kControllerState};
}

TEST(ImmutableKinematicStateViewProviderTest,
     ProducesExactImmutableModelViewWithOneReadPerBinding) {
  const auto source = valid_source();
  const ImmutableKinematicStateViewProvider provider({KinematicObservedStateSourceBinding{
      ControlBindingId("binding-a"), ControlBindingRevision("binding-rev-a"), source}});

  const auto result = provider.current_kinematic_state_path(query(), 1000);

  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(source->publication_reads.load(), 1U);
  ASSERT_EQ(result.value().resource_states.size(), 2U);
  ASSERT_EQ(result.value().model_states.size(), 1U);
  ASSERT_EQ(result.value().model_states.front().positions.size(), 2U);
  EXPECT_DOUBLE_EQ(
      std::get<AngularPositionDTO>(result.value().model_states.front().positions[0]).value_rad,
      0.5);
  EXPECT_DOUBLE_EQ(
      std::get<LinearPositionDTO>(result.value().model_states.front().positions[1]).value_m, 0.5);
  EXPECT_TRUE(IsExactLowercaseSha256Digest(result.value().view_digest.value()));
  EXPECT_EQ(result.value().actual_receiver_time_skew_ns, 0);
}

TEST(ImmutableKinematicStateViewProviderTest,
     ExternalTransportAdapterFeedsTheUnchangedPathViewContract) {
  auto external = std::make_shared<ExternalObservedStateProvider>(external_contract());
  const auto ingested = external->ingest(ExternalObservedStatePublicationDTO{
      external->contract_digest(),
      publication({ResourceId("resource-1"), ResourceId("resource-2")},
                  {AngularPositionDTO{0.5}, LinearPositionDTO{0.2}}, 100),
      900});
  ASSERT_TRUE(ingested.ok()) << ingested.error().message;
  const ImmutableKinematicStateViewProvider provider({KinematicObservedStateSourceBinding{
      ControlBindingId("binding-a"), ControlBindingRevision("binding-rev-a"), external}});

  const auto result = provider.current_kinematic_state_path(query(), 1000);

  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_EQ(result.value().resource_states.size(), 2U);
  EXPECT_EQ(result.value().resource_states.front().snapshot_received_timestamp_ns, 900);
  EXPECT_EQ(result.value().resource_states.front().publication_received_timestamp_ns, 900);
  EXPECT_DOUBLE_EQ(
      std::get<AngularPositionDTO>(result.value().resource_states.front().position).value_rad, 0.5);
}

TEST(ImmutableKinematicStateViewProviderTest,
     PreservesCompilerCoordinateOrderWhenUrdfChainOrderDiffers) {
  const auto source = valid_source();
  const ImmutableKinematicStateViewProvider provider({KinematicObservedStateSourceBinding{
      ControlBindingId("binding-a"), ControlBindingRevision("binding-rev-a"), source}});
  auto selected = query();
  std::get<KinematicModelJointPathSegmentDTO>(selected.frame_path.segments[1])
      .composition_coordinate_index = 1U;
  std::get<KinematicModelJointPathSegmentDTO>(selected.frame_path.segments[2])
      .composition_coordinate_index = 0U;

  const auto result = provider.current_kinematic_state_path(selected, 1000);

  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_EQ(result.value().model_states.size(), 1U);
  const auto& model = result.value().model_states.front();
  ASSERT_EQ(model.coordinate_projection.size(), 2U);
  ASSERT_EQ(model.positions.size(), 2U);
  EXPECT_EQ(model.coordinate_projection[0].composition_coordinate_index, 0U);
  EXPECT_EQ(model.coordinate_projection[1].composition_coordinate_index, 1U);
  EXPECT_DOUBLE_EQ(std::get<LinearPositionDTO>(model.positions[0]).value_m, 0.5);
  EXPECT_DOUBLE_EQ(std::get<AngularPositionDTO>(model.positions[1]).value_rad, 0.5);
}

TEST(ImmutableKinematicStateViewProviderTest, FixedCellPathNeedsNoRuntimeSource) {
  const ImmutableKinematicStateViewProvider provider({});
  KinematicStatePathQueryDTO fixed{
      RetainedBundleId("bundle-a"),
      GraphRevision("graph-a"),
      KinematicFramePathDTO{ResolvedCellFrameDTO{CellId("cell-a"), "cell"}, {}},
      {},
      0,
      {}};

  const auto result = provider.current_kinematic_state_path(fixed, 0);

  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_TRUE(result.value().resource_states.empty());
  EXPECT_TRUE(result.value().model_states.empty());
  EXPECT_FALSE(result.value().earliest_received_timestamp_ns.has_value());
  EXPECT_TRUE(IsExactLowercaseSha256Digest(result.value().view_digest.value()));
}

TEST(ImmutableKinematicStateViewProviderTest, RejectsStaleOrForeignPublicationBeforeProjection) {
  auto stale_source = valid_source();
  stale_source->publications.begin()->second.snapshot.received_timestamp_ns = 700;
  stale_source->publications.begin()->second.received_timestamp_ns = 700;
  const ImmutableKinematicStateViewProvider stale_provider({KinematicObservedStateSourceBinding{
      ControlBindingId("binding-a"), ControlBindingRevision("binding-rev-a"), stale_source}});
  EXPECT_FALSE(stale_provider.current_kinematic_state_path(query(), 1000).ok());

  auto foreign_source = valid_source();
  foreign_source->publications.begin()->second.provider_instance_id =
      ProviderInstanceId("other-provider");
  foreign_source->publications.begin()->second.snapshot.provider_instance_id =
      ProviderInstanceId("other-provider");
  const ImmutableKinematicStateViewProvider foreign_provider({KinematicObservedStateSourceBinding{
      ControlBindingId("binding-a"), ControlBindingRevision("binding-rev-a"), foreign_source}});
  EXPECT_FALSE(foreign_provider.current_kinematic_state_path(query(), 1000).ok());
}

TEST(ImmutableKinematicStateViewProviderTest, RejectsMissingAndCallerAddedResourceCoverage) {
  const auto source = valid_source();
  const ImmutableKinematicStateViewProvider provider({KinematicObservedStateSourceBinding{
      ControlBindingId("binding-a"), ControlBindingRevision("binding-rev-a"), source}});
  auto missing = query();
  missing.source_projections.pop_back();
  EXPECT_FALSE(provider.current_kinematic_state_path(missing, 1000).ok());

  auto extra = query();
  auto extra_source = source_projection("resource-3");
  extra.source_projections.push_back(std::move(extra_source));
  EXPECT_FALSE(provider.current_kinematic_state_path(extra, 1000).ok());
}

TEST(ImmutableKinematicStateViewProviderTest,
     RejectsNonCanonicalSourcesAndDuplicateRegistryBindings) {
  const auto source = valid_source();
  EXPECT_THROW(
      static_cast<void>(ImmutableKinematicStateViewProvider(
          {KinematicObservedStateSourceBinding{ControlBindingId("binding-a"),
                                               ControlBindingRevision("binding-rev-a"), source},
           KinematicObservedStateSourceBinding{ControlBindingId("binding-a"),
                                               ControlBindingRevision("binding-rev-a"), source}})),
      std::invalid_argument);

  const ImmutableKinematicStateViewProvider provider({KinematicObservedStateSourceBinding{
      ControlBindingId("binding-a"), ControlBindingRevision("binding-rev-a"), source}});
  auto reordered = query();
  std::reverse(reordered.source_projections.begin(), reordered.source_projections.end());
  EXPECT_FALSE(provider.current_kinematic_state_path(reordered, 1000).ok());
}

TEST(ImmutableKinematicStateViewProviderTest,
     CollapsedBindingsRequireOneIdenticalAuthoritativeResourceValue) {
  const auto first = valid_source();
  auto second = std::make_shared<FakeObservedStateProvider>();
  auto second_publication = first->publications.begin()->second;
  second_publication.control_binding_id = ControlBindingId("binding-b");
  second_publication.control_binding_revision = ControlBindingRevision("binding-rev-b");
  second_publication.snapshot.control_binding_id = ControlBindingId("binding-b");
  second_publication.snapshot.control_binding_revision = ControlBindingRevision("binding-rev-b");
  second->publications.emplace(
      SourceKey{ControlBindingId("binding-b"), ControlBindingRevision("binding-rev-b")},
      second_publication);
  const ImmutableKinematicStateViewProvider provider(
      {KinematicObservedStateSourceBinding{ControlBindingId("binding-a"),
                                           ControlBindingRevision("binding-rev-a"), first},
       KinematicObservedStateSourceBinding{ControlBindingId("binding-b"),
                                           ControlBindingRevision("binding-rev-b"), second}});
  auto selected = query();
  auto second_dependency =
      std::get<KinematicModelJointPathSegmentDTO>(selected.frame_path.segments[1])
          .eligible_resource_dependencies.front();
  second_dependency.control_binding_id = ControlBindingId("binding-b");
  second_dependency.control_binding_revision = ControlBindingRevision("binding-rev-b");
  std::get<KinematicModelJointPathSegmentDTO>(selected.frame_path.segments[1])
      .eligible_resource_dependencies.push_back(second_dependency);
  selected.source_projections.front().contributing_control_bindings.push_back(
      KinematicControlBindingSourceDTO{ControlBindingId("binding-b"),
                                       ControlBindingRevision("binding-rev-b")});

  const auto accepted = provider.current_kinematic_state_path(selected, 1000);

  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(first->publication_reads.load(), 1U);
  EXPECT_EQ(second->publication_reads.load(), 1U);

  second->publications.begin()->second.snapshot.positions[0] = AngularPositionDTO{0.6};
  EXPECT_FALSE(provider.current_kinematic_state_path(selected, 1000).ok());
}

TEST(ImmutableKinematicStateViewProviderTest, EnforcesReceiverLocalSkewAcrossSelectedBindings) {
  const auto first = valid_source();
  auto second = std::make_shared<FakeObservedStateProvider>();
  auto second_publication = first->publications.begin()->second;
  second_publication.control_binding_id = ControlBindingId("binding-b");
  second_publication.control_binding_revision = ControlBindingRevision("binding-rev-b");
  second_publication.snapshot.control_binding_id = ControlBindingId("binding-b");
  second_publication.snapshot.control_binding_revision = ControlBindingRevision("binding-rev-b");
  second_publication.snapshot.received_timestamp_ns = 840;
  second_publication.received_timestamp_ns = 840;
  second->publications.emplace(
      SourceKey{ControlBindingId("binding-b"), ControlBindingRevision("binding-rev-b")},
      std::move(second_publication));
  const ImmutableKinematicStateViewProvider provider(
      {KinematicObservedStateSourceBinding{ControlBindingId("binding-a"),
                                           ControlBindingRevision("binding-rev-a"), first},
       KinematicObservedStateSourceBinding{ControlBindingId("binding-b"),
                                           ControlBindingRevision("binding-rev-b"), second}});
  auto selected = query();
  auto& second_joint = std::get<KinematicModelJointPathSegmentDTO>(selected.frame_path.segments[2]);
  second_joint.eligible_resource_dependencies.front().control_binding_id =
      ControlBindingId("binding-b");
  second_joint.eligible_resource_dependencies.front().control_binding_revision =
      ControlBindingRevision("binding-rev-b");
  selected.source_projections[1].contributing_control_bindings.front() =
      KinematicControlBindingSourceDTO{ControlBindingId("binding-b"),
                                       ControlBindingRevision("binding-rev-b")};

  EXPECT_FALSE(provider.current_kinematic_state_path(selected, 1000).ok());
  selected.maximum_receiver_time_skew_ns = 60;
  const auto accepted = provider.current_kinematic_state_path(selected, 1000);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(accepted.value().actual_receiver_time_skew_ns, 60);
}

}  // namespace
}  // namespace manyforge::core
