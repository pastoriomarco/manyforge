#include <gtest/gtest.h>

#include <manyforge_core/configured_robot_measured_state.hpp>

#include <limits>
#include <stdexcept>

namespace manyforge::core {
namespace {

MeasuredStateSourceDescriptor descriptor(std::string component = "robot-a") {
  ObservedStateQueryContextDTO query{
      ControllableComponentId(component), CommandTargetId("arm"),
      CapabilityId("state.observed.current"), ControlDomainId("domain"),
      ControlDomainRevision("domain-revision"), ControlBindingId("binding"),
      ControlBindingRevision("binding-revision"), ControllerTopologyId("topology"),
      TopologyRevision("topology-revision"), ControlRouteId("route"),
      {ResourceId("joint"), ResourceId("slide")}, {"joint", "slide"},
      {CoordinateKind::kAngular, CoordinateKind::kLinear},
      ControlInterfaceContractId("interface"), ControlInterfaceRevision("interface-revision"),
      ActiveControlConfigurationId("configuration"), ActiveControlConfigurationGeneration(1U),
      ProviderInstanceId("provider"), ProviderManifestId("manifest"),
      ProviderManifestRevision("manifest-revision"), RuntimeEpoch("epoch"),
      StateAuthorityId("measured-feed"), StateAuthorityClass::kMeasured, 100};
  return {{query, {{ObservedRobotStateSourceKind::kJointState, "joint-state-feed",
                    ObservedRobotStateAuthorityProfile::kMeasured, 100, "measured-feed"}},
           3U, 256U}, {"physical_joint", "physical_slide"}};
}

std::shared_ptr<AddressedObservedStatePublication> published(
    const MeasuredStateSourceDescriptor& source) {
  auto reader = std::make_shared<AddressedObservedStatePublication>(source.publication);
  const std::vector<double> positions{0.2, 0.01};
  const std::vector<double> velocities;
  const auto written = reader->publish(ObservedStateSourceKind::kJointState,
                                      source.publication.query_context.joint_names,
                                      positions, velocities, 900, 1000);
  EXPECT_TRUE(written.published);
  EXPECT_TRUE(written.complete);
  return reader;
}

ConfiguredRobotMeasuredStateRequirement required(
    MeasuredStateSourceDescriptor source,
    std::shared_ptr<const IObservedStateProvider> reader) {
  return {source.publication.query_context.controllable_component_id,
          source.publication.query_context.resources, {{std::move(source), std::move(reader)}}};
}

class ReturnedSnapshot final : public IObservedStateProvider {
 public:
  explicit ReturnedSnapshot(ObservedStateSnapshotDTO value) : value_(std::move(value)) {}
  Result<ObservedStateSnapshotDTO> current_state(const ObservedStateQueryContextDTO&,
                                                std::int64_t) const override {
    return Result<ObservedStateSnapshotDTO>::Ok(value_);
  }
  Result<ObservedStatePublicationDTO> publication(const ControlBindingId&,
                                                 const ControlBindingRevision&) const override {
    return Result<ObservedStatePublicationDTO>::Err({"unused", "unused", {}});
  }
 private:
  const ObservedStateSnapshotDTO value_;
};

class ReturnedError final : public IObservedStateProvider {
 public:
  explicit ReturnedError(ErrorDTO error) : error_(std::move(error)) {}
  Result<ObservedStateSnapshotDTO> current_state(const ObservedStateQueryContextDTO&,
                                                std::int64_t) const override {
    return Result<ObservedStateSnapshotDTO>::Err(error_);
  }
  Result<ObservedStatePublicationDTO> publication(const ControlBindingId&,
                                                 const ControlBindingRevision&) const override {
    return Result<ObservedStatePublicationDTO>::Err({"unused", "unused", {}});
  }
 private:
  const ErrorDTO error_;
};

TEST(ConfiguredRobotMeasuredState, PreservesReaderFailureAndNamesRequiredSource) {
  const auto source = descriptor();
  const ErrorDTO original{"observed_state_unavailable", "original provider refusal",
                          {{"reason", "observed_state_not_published"}, {"provider_detail", "retained"}}};
  const ConfiguredRobotMeasuredStateInventory inventory{
      {required(source, std::make_shared<ReturnedError>(original))}};
  const auto result = inventory.Read(1050);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, original.code);
  EXPECT_EQ(result.error().message, original.message);
  for (const auto& [key, value] : original.details) {
    EXPECT_EQ(result.error().details.at(key), value);
  }
  EXPECT_EQ(result.error().details.at("required_component_id"), "robot-a");
  EXPECT_EQ(result.error().details.at("required_source_id"), "joint-state-feed");
  EXPECT_EQ(result.error().details.at("required_control_binding_id"), "binding");
  EXPECT_EQ(result.error().details.at("required_control_binding_revision"), "binding-revision");
  EXPECT_EQ(result.error().details.at("required_runtime_epoch"), "epoch");
  EXPECT_FALSE(result.error().details.contains("observed_age_ns"));
  EXPECT_FALSE(result.error().details.contains("observed_received_timestamp_ns"));
}

TEST(ConfiguredRobotMeasuredState, StaleReturnedSnapshotCarriesOnlyGenuineReceiptAge) {
  const auto source = descriptor();
  const auto sample = published(source)->current_state(source.publication.query_context, 1050).value();
  const ConfiguredRobotMeasuredStateInventory inventory{
      {required(source, std::make_shared<ReturnedSnapshot>(sample))}};
  const auto result = inventory.Read(1101);
  ASSERT_FALSE(result.ok());
  const auto original = validate_observed_state_snapshot(sample, 1101);
  ASSERT_FALSE(original.ok());
  EXPECT_EQ(result.error().code, original.error().code);
  for (const auto& [key, value] : original.error().details) {
    EXPECT_EQ(result.error().details.at(key), value);
  }
  EXPECT_EQ(result.error().details.at("required_source_id"), "joint-state-feed");
  EXPECT_EQ(result.error().details.at("observed_received_timestamp_ns"), "1000");
  EXPECT_EQ(result.error().details.at("observed_age_ns"), "101");
}

TEST(ConfiguredRobotMeasuredState, OwnsCompleteMembershipAndUsesOneReceiverTime) {
  auto a = descriptor();
  auto b = descriptor("robot-b");
  const ConfiguredRobotMeasuredStateInventory inventory{
      {required(a, published(a)), required(b, published(b))}};
  const auto state = inventory.Read(1050);
  ASSERT_TRUE(state.ok());
  EXPECT_EQ(state.value().evaluated_at_ns, 1050);
  ASSERT_EQ(state.value().robots.size(), 2U);
  EXPECT_EQ(state.value().robots[0].component_id, ControllableComponentId("robot-a"));
  EXPECT_EQ(state.value().robots[1].component_id, ControllableComponentId("robot-b"));
  EXPECT_EQ(coordinate_kind(state.value().robots[0].sources[0].positions[1]), CoordinateKind::kLinear);
  EXPECT_EQ(state.value().robots[0].sources[0].received_timestamp_ns, 1000);
}

TEST(ConfiguredRobotMeasuredState, UnavailableRobotCannotDisappearBesideFreshParticipant) {
  auto a = descriptor();
  auto b = descriptor("robot-b");
  const ConfiguredRobotMeasuredStateInventory inventory{
      {required(a, published(a)), required(b, nullptr)}};
  ASSERT_EQ(inventory.robots().size(), 2U);
  const auto state = inventory.Read(1050);
  ASSERT_FALSE(state.ok());
  EXPECT_EQ(state.error().details.at("component_id"), "robot-b");
  EXPECT_EQ(state.error().details.at("reason"), "measured_source_missing");
}

TEST(ConfiguredRobotMeasuredState, ReadsNeverRefreshAStaleFeedOrSeedReplacement) {
  auto source = descriptor();
  auto reader = published(source);
  const ConfiguredRobotMeasuredStateInventory inventory{{required(source, reader)}};
  ASSERT_TRUE(inventory.Read(1100).ok());
  ASSERT_FALSE(inventory.Read(1101).ok());
  ASSERT_FALSE(inventory.Read(1102).ok());
  EXPECT_EQ(reader->current_state(source.publication.query_context, 1050).value().received_timestamp_ns, 1000);
  auto successor = source;
  successor.publication.query_context.runtime_epoch = RuntimeEpoch("successor");
  auto empty_reader = std::make_shared<AddressedObservedStatePublication>(successor.publication);
  EXPECT_FALSE(ConfiguredRobotMeasuredStateInventory{{required(successor, empty_reader)}}.Read(1050).ok());
  EXPECT_TRUE(inventory.Read(1050).ok());
}

TEST(ConfiguredRobotMeasuredState, ChecksExactReturnedAuthorityBindingAndCoordinateUnits) {
  const auto source = descriptor();
  const auto original = published(source)->current_state(source.publication.query_context, 1050).value();
  for (int mutation = 0; mutation < 8; ++mutation) {
    auto wrong = original;
    switch (mutation) {
      case 0: wrong.source_epoch = RuntimeEpoch("foreign"); break;
      case 1: wrong.control_binding_revision = ControlBindingRevision("foreign"); break;
      case 2: wrong.state_authority_class = StateAuthorityClass::kCommanded; break;
      case 3: wrong.source_id = "foreign"; break;
      case 4: wrong.resources[1] = ResourceId("foreign"); break;
      case 5: wrong.positions[1] = AngularPositionDTO{0.01}; break;
      case 6: wrong.authority_generation++; break;
      case 7: wrong.provider_instance_id = ProviderInstanceId("foreign"); break;
    }
    const ConfiguredRobotMeasuredStateInventory inventory{
        {required(source, std::make_shared<ReturnedSnapshot>(wrong))}};
    const auto result = inventory.Read(1050);
    ASSERT_FALSE(result.ok()) << mutation;
    EXPECT_EQ(result.error().details.at("reason"), "measured_source_identity_mismatch") << mutation;
  }
}

TEST(ConfiguredRobotMeasuredState, ValidatesSamplesAndCompleteGeometryRatherThanOnlyBinding) {
  auto source = descriptor();
  const auto original = published(source)->current_state(source.publication.query_context, 1050).value();
  for (int mutation = 0; mutation < 3; ++mutation) {
    auto wrong = original;
    if (mutation == 0) wrong.complete = false;
    if (mutation == 1) wrong.positions[0] = AngularPositionDTO{std::numeric_limits<double>::quiet_NaN()};
    if (mutation == 2) wrong.received_timestamp_ns = -1;
    EXPECT_FALSE(ConfiguredRobotMeasuredStateInventory{
        {required(source, std::make_shared<ReturnedSnapshot>(wrong))}}.Read(1050).ok());
  }
  // A sample received after the read boundary is current
  // (received_sample_age.hpp): the Real `measured_state` no longer flickers.
  auto later = original;
  later.received_timestamp_ns = 1051;
  EXPECT_TRUE(ConfiguredRobotMeasuredStateInventory{
      {required(source, std::make_shared<ReturnedSnapshot>(later))}}.Read(1050).ok());
  auto partial = required(source, published(source));
  partial.required_resources.push_back(ResourceId("unobserved-gripper"));
  const ConfiguredRobotMeasuredStateInventory inventory{{std::move(partial)}};
  const auto result = inventory.Read(1050);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "measured_geometry_coordinates_missing");
}

TEST(ConfiguredRobotMeasuredState, ConstructionRejectsCommandedAndForeignGeometryDescriptors) {
  auto source = descriptor();
  auto commanded = source;
  commanded.publication.query_context.state_authority_class = StateAuthorityClass::kCommanded;
  EXPECT_FALSE(commanded.validate().ok());
  auto no_budget = source;
  no_budget.publication.query_context.freshness_budget_ns = 0;
  EXPECT_FALSE(no_budget.validate().ok());
  auto foreign = required(source, nullptr);
  foreign.component_id = ControllableComponentId("another-robot");
  EXPECT_THROW(ConfiguredRobotMeasuredStateInventory({foreign}), std::invalid_argument);
  auto overlap = required(source, nullptr);
  overlap.sources.push_back(overlap.sources[0]);
  EXPECT_THROW(ConfiguredRobotMeasuredStateInventory({overlap}), std::invalid_argument);
}

TEST(ConfiguredRobotMeasuredState, OwningInventoryKeepsReadersAliveAcrossExternalRelease) {
  const auto source = descriptor();
  auto reader = published(source);
  std::weak_ptr<const IObservedStateProvider> weak = reader;
  auto inventory = std::make_shared<const ConfiguredRobotMeasuredStateInventory>(
      std::vector<ConfiguredRobotMeasuredStateRequirement>{required(source, reader)});
  reader.reset();
  EXPECT_FALSE(weak.expired());
  EXPECT_TRUE(inventory->Read(1050).ok());
  inventory.reset();
  EXPECT_TRUE(weak.expired());
}

class CountedMeasuredReader final : public IObservedStateProvider {
 public:
  explicit CountedMeasuredReader(std::shared_ptr<const IObservedStateProvider> reader)
      : reader_(std::move(reader)) {}
  Result<ObservedStateSnapshotDTO> current_state(const ObservedStateQueryContextDTO& query,
                                                std::int64_t now_ns) const override {
    ++reads;
    return reader_->current_state(query, now_ns);
  }
  Result<ObservedStatePublicationDTO> publication(const ControlBindingId& binding,
                                                 const ControlBindingRevision& revision) const override {
    return reader_->publication(binding, revision);
  }
  mutable int reads{};
 private:
  std::shared_ptr<const IObservedStateProvider> reader_;
};

TEST(ConfiguredRobotMeasuredState, ExplicitOwningViewReadsOnlyItsConfiguredRequirement) {
  const auto a = descriptor();
  const auto b = descriptor("robot-b");
  auto a_reader = std::make_shared<CountedMeasuredReader>(published(a));
  auto b_reader = std::make_shared<CountedMeasuredReader>(
      std::make_shared<ReturnedError>(ErrorDTO{"missing", "robot-b unavailable", {}}));
  const ConfiguredRobotMeasuredStateInventory inventory{
      {required(a, a_reader), required(b, b_reader)}};
  const RobotMeasuredStateSourceView view{a.publication.query_context.controllable_component_id,
                                         {{a, a_reader}}};
  const auto scoped = inventory.ReadComponent(view, 1050);
  ASSERT_TRUE(scoped.ok());
  EXPECT_EQ(scoped.value().component_id, ControllableComponentId("robot-a"));
  EXPECT_EQ(a_reader->reads, 1);
  EXPECT_EQ(b_reader->reads, 0);
  ASSERT_EQ(inventory.robots().size(), 2U);
  EXPECT_FALSE(inventory.Read(1050).ok());
  EXPECT_EQ(b_reader->reads, 1);
}

TEST(ConfiguredRobotMeasuredState, SourceViewCannotSelectForeignOrChangedGeometryBeforeReads) {
  const auto source = descriptor();
  auto reader = std::make_shared<CountedMeasuredReader>(published(source));
  const ConfiguredRobotMeasuredStateInventory inventory{{required(source, reader)}};
  const RobotMeasuredStateSourceView view{ControllableComponentId("robot-a"), {{source, reader}}};
  EXPECT_FALSE(inventory.ReadComponent(view, -1).ok());
  for (int mutation = 0; mutation < 6; ++mutation) {
    auto malformed = view;
    auto& query = malformed.sources.front().descriptor.publication.query_context;
    switch (mutation) {
      case 0: malformed.component_id = ControllableComponentId("foreign"); break;
      case 1: query.controllable_component_id = ControllableComponentId("foreign"); break;
      case 2: query.resources.back() = ResourceId("foreign"); break;
      case 3: query.coordinate_kinds.back() = CoordinateKind::kAngular; break;
      case 4: query.joint_names.back() = "foreign"; break;
      case 5: malformed.sources.push_back(malformed.sources.front()); break;
    }
    EXPECT_FALSE(inventory.ReadComponent(malformed, 1050).ok()) << mutation;
  }
  EXPECT_EQ(reader->reads, 0);
  const auto missing = inventory.ReadComponent({ControllableComponentId("robot-a"), {}}, 1050);
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().details.at("reason"), "measured_geometry_coordinates_missing");
  EXPECT_EQ(reader->reads, 0);
}

TEST(ConfiguredRobotMeasuredState, ScopedAndAllCellReadsShareExactIdentityUnitsAndAgeFailures) {
  const auto source = descriptor();
  const auto original = published(source)->current_state(source.publication.query_context, 1050).value();
  for (int mutation = 0; mutation < 5; ++mutation) {
    auto invalid = original;
    if (mutation == 0) invalid.positions.back() = AngularPositionDTO{0.01};
    if (mutation == 1) invalid.state_authority_class = StateAuthorityClass::kCommanded;
    if (mutation == 2) invalid.provider_instance_id = ProviderInstanceId("foreign");
    if (mutation == 3) invalid.complete = false;
    auto reader = std::make_shared<ReturnedSnapshot>(invalid);
    const ConfiguredRobotMeasuredStateInventory inventory{{required(source, reader)}};
    const RobotMeasuredStateSourceView view{ControllableComponentId("robot-a"), {{source, reader}}};
    const auto now = mutation == 4 ? 1101 : 1050;
    const auto all = inventory.Read(now);
    const auto scoped = inventory.ReadComponent(view, now);
    ASSERT_FALSE(all.ok()) << mutation;
    ASSERT_FALSE(scoped.ok()) << mutation;
    EXPECT_EQ(scoped.error(), all.error()) << mutation;
  }
  auto reader = published(source);
  const ConfiguredRobotMeasuredStateInventory inventory{{required(source, reader)}};
  const RobotMeasuredStateSourceView view{ControllableComponentId("robot-a"), {{source, reader}}};
  ASSERT_TRUE(inventory.ReadComponent(view, 1100).ok());
  EXPECT_FALSE(inventory.ReadComponent(view, 1101).ok());
  EXPECT_EQ(reader->current_state(source.publication.query_context, 1050).value().received_timestamp_ns, 1000);
}

TEST(ConfiguredRobotMeasuredState, EmptySuccessorViewNeverFallsBackToPinnedStaticSource) {
  const auto old = descriptor();
  auto old_reader = published(old);
  const ConfiguredRobotMeasuredStateInventory inventory{{required(old, old_reader)}};
  auto successor = old;
  successor.publication.query_context.active_control_configuration_generation =
      ActiveControlConfigurationGeneration(2U);
  successor.publication.authority_generation = 4U;
  auto new_reader = std::make_shared<AddressedObservedStatePublication>(successor.publication);
  RobotMeasuredStateSourceView view{ControllableComponentId("robot-a"), {{successor, new_reader}}};
  EXPECT_FALSE(inventory.ReadComponent(view, 1050).ok());
  EXPECT_TRUE(inventory.Read(1050).ok());
  const std::vector<double> positions{0.3, 0.02};
  ASSERT_TRUE(new_reader->publish(ObservedStateSourceKind::kJointState,
      successor.publication.query_context.joint_names, positions, {}, 1040, 1050).published);
  const auto read = inventory.ReadComponent(view, 1060);
  ASSERT_TRUE(read.ok());
  ASSERT_EQ(read.value().sources.size(), 1U);
  EXPECT_EQ(read.value().sources.front().active_control_configuration_generation,
            ActiveControlConfigurationGeneration(2U));
  EXPECT_EQ(read.value().sources.front().received_timestamp_ns, 1050);
  EXPECT_EQ(old_reader->current_state(old.publication.query_context, 1050).value().received_timestamp_ns, 1000);
  view.sources.front().reader = old_reader;
  EXPECT_FALSE(inventory.ReadComponent(view, 1060).ok());
}

std::vector<RuntimeResourceBindingDTO> verified_coordinates() {
  return {{ResourceRoleId("joint-role"), ResourceId("joint"), "physical_joint", "joint",
           CoordinateKind::kAngular},
          {ResourceRoleId("slide-role"), ResourceId("slide"), "physical_slide", "slide",
           CoordinateKind::kLinear}};
}

TEST(ConfiguredRobotMeasuredState, ExplicitGeometryAdmitsRecoveredSourceWithoutOldSourceIdentity) {
  auto current = descriptor();
  current.publication.query_context.runtime_epoch = RuntimeEpoch("recovered-epoch");
  current.publication.query_context.active_control_configuration_generation =
      ActiveControlConfigurationGeneration(2U);
  current.publication.authority_generation = 4U;
  auto reader = std::make_shared<CountedMeasuredReader>(published(current));
  const ConfiguredRobotMeasuredStateInventory inventory{{
      {ControllableComponentId("robot-a"), current.publication.query_context.resources,
       {}, verified_coordinates()}}};
  EXPECT_FALSE(inventory.Read(1050).ok());
  EXPECT_FALSE(inventory.ReadComponent({ControllableComponentId("robot-a"), {}}, 1050).ok());
  EXPECT_FALSE(inventory.ReadComponent({ControllableComponentId("robot-a"), {{current, nullptr}}},
                                     1050).ok());
  EXPECT_EQ(reader->reads, 0);
  const auto captured = inventory.ReadComponent(
      {ControllableComponentId("robot-a"), {{current, reader}}}, 1050);
  ASSERT_TRUE(captured.ok());
  ASSERT_EQ(captured.value().sources.size(), 1U);
  EXPECT_EQ(captured.value().sources.front().source_epoch, RuntimeEpoch("recovered-epoch"));
  EXPECT_EQ(reader->reads, 1);
  // Source freshness and returned identity still use the original validator.
  EXPECT_FALSE(inventory.ReadComponent(
      {ControllableComponentId("robot-a"), {{current, reader}}}, 1101).ok());
}

TEST(ConfiguredRobotMeasuredState, ExplicitGeometryRejectsMalformedRowsAndDescriptorDisagreement) {
  const auto source = descriptor();
  auto base = required(source, nullptr);
  base.geometry_coordinates = verified_coordinates();
  EXPECT_NO_THROW(ConfiguredRobotMeasuredStateInventory({base}));
  auto model_local_names = base;
  model_local_names.sources.clear();
  model_local_names.geometry_coordinates.back().source_joint_name =
      model_local_names.geometry_coordinates.front().source_joint_name;
  // Source-local URDF names may repeat across distinct model bindings.
  EXPECT_NO_THROW(ConfiguredRobotMeasuredStateInventory({model_local_names}));
  model_local_names.geometry_coordinates.back().compiled_resource_name =
      model_local_names.geometry_coordinates.front().compiled_resource_name;
  // Cross-binding naming policy remains the verified producer's concern.
  EXPECT_NO_THROW(ConfiguredRobotMeasuredStateInventory({model_local_names}));
  for (int mutation = 0; mutation < 11; ++mutation) {
    auto invalid = base;
    auto& rows = invalid.geometry_coordinates;
    switch (mutation) {
      case 0: rows.pop_back(); break;
      case 1: rows.push_back(rows.front()); break;
      case 2: rows.back().resource_id = ResourceId("foreign"); break;
      case 3: rows.back().compiled_resource_name.clear(); break;
      case 4: rows.back().compiled_resource_name = rows.front().compiled_resource_name; break;
      case 5: rows.back().source_joint_name.reset(); break;
      case 6: rows.back().source_joint_name = ""; break;
      case 7: rows.back().source_joint_name = rows.front().source_joint_name; break;
      case 8: rows.back().coordinate_kind = static_cast<CoordinateKind>(255); break;
      case 9: rows.back().source_joint_name = "foreign_source"; break;
      case 10: rows.back().coordinate_kind = CoordinateKind::kAngular; break;
    }
    EXPECT_THROW(ConfiguredRobotMeasuredStateInventory({invalid}), std::invalid_argument) << mutation;
  }
}

TEST(ConfiguredRobotMeasuredState, ExplicitGeometryRejectsForeignViewBeforeReading) {
  const auto source = descriptor();
  auto reader = std::make_shared<CountedMeasuredReader>(published(source));
  const ConfiguredRobotMeasuredStateInventory inventory{{
      {ControllableComponentId("robot-a"), source.publication.query_context.resources,
       {}, verified_coordinates()}}};
  for (int mutation = 0; mutation < 5; ++mutation) {
    auto changed = source;
    auto& query = changed.publication.query_context;
    switch (mutation) {
      case 0: query.resources.back() = ResourceId("foreign"); break;
      case 1: query.joint_names.back() = "foreign"; break;
      case 2: query.coordinate_kinds.back() = CoordinateKind::kAngular; break;
      case 3: changed.source_joint_names.back() = "foreign_source"; break;
      case 4: query.controllable_component_id = ControllableComponentId("foreign"); break;
    }
    EXPECT_FALSE(inventory.ReadComponent(
        {ControllableComponentId("robot-a"), {{changed, reader}}}, 1050).ok()) << mutation;
  }
  EXPECT_EQ(reader->reads, 0);
}

}  // namespace
}  // namespace manyforge::core
