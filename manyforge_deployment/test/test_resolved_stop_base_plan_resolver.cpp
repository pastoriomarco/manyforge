#include <gtest/gtest.h>
#include <manyforge_core/motion_interrupt/stop_profile_mapper.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <manyforge_deployment/resolved_stop_base_plan_resolver.hpp>
#include <manyforge_deployment/cell_applicability.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include "build_manifest_internal.hpp"
#include "content_identity_internal.hpp"
#include "gripper_only_retention_fixture.hpp"
#include "lite6_retained_consumer_fixture.hpp"
#include "verified_retained_bundle_internal.hpp"
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>

namespace manyforge::deployment {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

class ScopedTrustedRoot final {
public:
  ScopedTrustedRoot()
      : path_(std::filesystem::temp_directory_path() /
              ("manyforge-resolved-stop-plan-" +
               std::to_string(static_cast<long long>(::getpid())) + "-" +
               std::to_string(sequence_.fetch_add(1U)))) {
    std::error_code error;
    (void)std::filesystem::remove_all(path_, error);
    if (!std::filesystem::create_directory(path_, error) || error) {
      throw std::logic_error("test could not create its trusted root");
    }
  }

  ~ScopedTrustedRoot() {
    std::error_code error;
    (void)std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  inline static std::atomic<std::uint64_t> sequence_{0U};
  std::filesystem::path path_;
};

template <typename Range, typename Predicate>
[[nodiscard]] const typename Range::value_type &
require_one(const Range &values, Predicate predicate,
            std::string_view description) {
  const auto found = std::find_if(values.begin(), values.end(), predicate);
  if (found == values.end() ||
      std::find_if(std::next(found), values.end(), predicate) != values.end()) {
    throw std::logic_error("fixture has no sole " + std::string(description));
  }
  return *found;
}

struct RuntimeContext final {
  core::RuntimeParticipantBinding binding;
  core::ActiveControlConfigurationDTO active_configuration;
  core::DesiredControlConfigurationDTO desired_configuration;
  core::RuntimeUnitId source_runtime_unit_id;
  core::RuntimeEpoch source_runtime_epoch;
};

[[nodiscard]] RuntimeContext
make_runtime_context(const RetainedBundle &retained_bundle,
                     std::string_view capability_id = "execution.trajectory.execute",
                     const VerifiedRetainedModelMap *model_map = nullptr) {
  const auto &graph = retained_bundle.value().cell_graph;
  const auto &control_binding = require_one(
      graph.control_bindings,
      [&](const auto &candidate) {
        return candidate.capability_id == core::CapabilityId(std::string(capability_id));
      },
      "selected control binding");
  const auto &component = require_one(
      graph.controllable_components,
      [&](const auto &candidate) {
        return candidate.controllable_component_id ==
               control_binding.controllable_component_id;
      },
      "controllable component");
  const auto &domain = require_one(
      graph.control_domains,
      [&](const auto &candidate) {
        return candidate.control_domain_id == control_binding.control_domain_id;
      },
      "control domain");
  const auto &desired = require_one(
      retained_bundle.value().desired_control_configurations,
      [&](const auto &candidate) {
        return candidate.control_domain_id == domain.control_domain_id;
      },
      "desired control configuration");
  const auto &route = require_one(
      desired.binding_routes,
      [&](const auto &candidate) {
        return candidate.control_binding_id ==
               control_binding.control_binding_id;
      },
      "desired binding route");

  std::vector<core::ProgramModelRoleBindingDTO> models;
  for (const auto &row : component.model_role_bindings) {
    const auto &model = require_one(
        graph.model_instances,
        [&](const auto &candidate) {
          return candidate.model_instance_id == row.model_instance_id;
        },
        "model instance");
    models.push_back(
        {row.model_role_id, row.model_instance_id, model.model_revision});
  }
  if (models.empty()) {
    throw std::logic_error("fixture component has no model binding");
  }
  std::vector<core::RuntimeResourceBindingDTO> resources;
  for (std::size_t index = 0U; index < control_binding.resource_bindings.size();
       ++index) {
    const auto &row = control_binding.resource_bindings[index];
    if (model_map != nullptr) {
      const auto &map = require_one(model_map->control_bindings(),
                                   [&](const auto &candidate) {
        return candidate.control_binding_id == control_binding.control_binding_id;
      }, "retained control coordinate map");
      const auto &resource = require_one(map.resources, [&](const auto &candidate) {
        return candidate.resource_id == row.resource_id &&
               candidate.resource_role_id == row.resource_role_id;
      }, "retained resource coordinate");
      if (resource.coordinates.empty())
        throw std::logic_error("selected gripper has no retained model coordinate");
      const auto &coordinate = resource.coordinates.front();
      resources.push_back({row.resource_role_id, row.resource_id,
                           coordinate.source_joint_name, coordinate.compiled_joint_name,
                           coordinate.coordinate_kind == UrdfCoordinateKind::kLinear
                               ? core::CoordinateKind::kLinear
                               : core::CoordinateKind::kAngular});
    } else {
      const auto symbol = "joint_" + std::to_string(index + 1U);
      resources.push_back({row.resource_role_id, row.resource_id, symbol, symbol,
                           core::CoordinateKind::kAngular});
    }
  }
  std::sort(resources.begin(), resources.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.resource_id < rhs.resource_id;
            });

  const core::RuntimeEpoch runtime_epoch(
      "epoch." + control_binding.provider_runtime_unit_id.value());
  const core::RuntimeEpoch provider_epoch("provider.epoch.1");
  core::RuntimeParticipantBindingDTO value{
      core::ProgramId("resolved.stop.plan.program"),
      core::ProgramRevision(std::string(kZeroDigest)),
      core::ProgramBindingId("resolved.stop.plan.binding"),
      retained_bundle.retained_bundle_id(),
      graph.graph_revision,
      core::ComponentRoleId("robot"),
      core::TargetRoleId(control_binding.command_target_id.value()),
      component.controllable_component_id,
      component.component_type_id,
      component.component_type_revision,
      control_binding.command_target_id,
      control_binding.capability_id,
      control_binding.capability_definition_revision,
      std::nullopt,
      std::move(models),
      {},
      {core::ProgramFrameRoleBindingDTO{
          core::FrameRoleId("base"),
          component.model_role_bindings.front().model_instance_id,
          require_one(
              graph.model_instances,
              [&](const auto &candidate) {
                return candidate.model_instance_id ==
                       component.model_role_bindings.front().model_instance_id;
              },
              "frame model")
              .model_revision,
          "base_link"}},
      resources,
      {},
      core::RuntimeControlPathDTO{
          domain.control_domain_id,
          domain.control_domain_revision,
          control_binding.control_binding_id,
          control_binding.control_binding_revision,
          control_binding.control_interface_contract_id,
          control_binding.control_interface_revision,
          control_binding.controller_topology_id,
          control_binding.topology_revision,
          route.control_route_id,
          desired.activation_set_id,
          core::ActiveControlConfigurationId("active.resolved.stop.plan"),
          core::ActiveControlConfigurationGeneration(4U),
          control_binding.provider_instance_id,
          control_binding.provider_manifest_id,
          control_binding.provider_manifest_revision,
          control_binding.provider_runtime_unit_id,
          provider_epoch,
          control_binding.state_authority_id,
          control_binding.state_authority_class,
          control_binding.stop_evidence_policy_id,
          control_binding.stop_evidence_policy_revision,
          control_binding.containment_granularity,
          std::nullopt},
      std::nullopt,
      {},
      {{control_binding.provider_runtime_unit_id, runtime_epoch}},
      {}};
  auto sealed = core::verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }

  std::vector<core::ActiveControlConfigurationBindingRouteDTO> active_routes;
  for (const auto &row : desired.binding_routes) {
    active_routes.push_back({row.control_binding_id,
                             row.control_binding_revision,
                             row.control_route_id});
  }
  std::sort(active_routes.begin(), active_routes.end(),
            core::CanonicalActiveControlConfigurationBindingRouteLess);
  std::vector<core::KinematicResourceSourceProjectionDTO> projections;
  const auto publication_source = [&] {
    switch (control_binding.state_authority_class) {
    case core::StateAuthorityClass::kMeasured:
      return core::ObservedStateSourceKind::kJointState;
    case core::StateAuthorityClass::kCommanded:
      return core::ObservedStateSourceKind::kCommandedState;
    case core::StateAuthorityClass::kPredicted:
      return core::ObservedStateSourceKind::kPredictedState;
    }
    throw std::logic_error("fixture has an unknown state-authority class");
  }();
  for (const auto &resource : resources) {
    projections.push_back({
        resource.resource_id,
        {{control_binding.control_binding_id,
          control_binding.control_binding_revision}},
        control_binding.provider_instance_id,
        control_binding.provider_manifest_id,
        control_binding.provider_manifest_revision,
        control_binding.state_authority_id,
        control_binding.state_authority_class,
        publication_source,
    });
  }
  const core::RuntimeUnitId source_runtime_unit_id =
      control_binding.provider_runtime_unit_id;
  const core::RuntimeEpoch source_runtime_epoch = runtime_epoch;
  core::ActiveControlConfigurationDTO active{
      core::ActiveControlConfigurationId("active.resolved.stop.plan"),
      domain.control_domain_id,
      domain.control_domain_revision,
      std::move(active_routes),
      desired.activation_set_id,
      domain.controller().controller_topology_id,
      domain.controller().topology_revision,
      source_runtime_unit_id,
      source_runtime_epoch,
      7U,
      std::move(projections),
      core::ActiveControlConfigurationGeneration(4U),
      core::ActiveControlConfigurationStateDTO::kReconciled};
  if (!active.locally_valid()) {
    throw std::logic_error("test active configuration is invalid");
  }
  return {std::move(sealed.value()), std::move(active), desired,
          source_runtime_unit_id, source_runtime_epoch};
}

TEST(ResolvedStopBasePlanResolverTest,
     ExplicitMaxStopTimeSurvivesCompilationDurableReloadAndReadiness) {
  // Q51/item 24 (as `main`): the robot's own `max_stop_time_s` lives in its
  // controlled-Stop profile and is its Stop deadline; there is no cell-wide
  // timing table and no Apply-time refusal against the profile.
  ScopedTrustedRoot root;
  const auto retained = test::materialize_lite6_retained_consumer_fixture_from(
      root.path(),
      test::make_lite6_stop_qualified_ros_motion_retention_fixture(3.0));
  FilesystemVerifiedRetainedProductArtifactSource source(root.path());
  const auto runtime = make_runtime_context(*retained.bundle);
  const auto expected = load_verified_controller_lifecycle_expected_domain(
      *retained.bundle, source, runtime.desired_configuration,
      runtime.source_runtime_unit_id, runtime.source_runtime_epoch);
  const auto plan = resolve_verified_stop_base_plan(
      *retained.bundle, source, expected, runtime.active_configuration,
      runtime.binding);
  const auto &nominal = plan.escalation_stages.front();
  ASSERT_TRUE(nominal.default_contract_parameters.has_value());
  const auto max_stop_time =
      nominal.default_contract_parameters->find("max_stop_time_s");
  ASSERT_NE(max_stop_time, nominal.default_contract_parameters->end());
  EXPECT_EQ(std::get<double>(max_stop_time->second.value), 3.0);
  EXPECT_EQ(core::controlled_stop_deadline_from_contract_parameters(
                *nominal.default_contract_parameters),
            std::optional<core::Duration>(3'000'000'000));
  const auto readiness = seal_runtime_participant_stop_readiness(
      *retained.bundle, plan, core::ComponentLifecycleGeneration(11U), expected,
      runtime.binding);
  ASSERT_TRUE(readiness.ok()) << readiness.error().message;

  const auto different_time =
      test::materialize_lite6_retained_consumer_fixture_from(
          root.path(),
          test::make_lite6_stop_qualified_ros_motion_retention_fixture(2.5));
  EXPECT_NE(different_time.bundle->retained_bundle_id(),
            retained.bundle->retained_bundle_id());
  EXPECT_FALSE(seal_runtime_participant_stop_readiness(
                   *different_time.bundle, plan,
                   core::ComponentLifecycleGeneration(11U), expected,
                   runtime.binding)
                   .ok());
  // Shorter than the 1.0 s deceleration: not refused in advance.
  EXPECT_NO_THROW(
      test::make_lite6_stop_qualified_ros_motion_retention_fixture(0.5));
  // Absent: the deadline is the deceleration time plus 0.5 s.
  const auto defaulted = test::materialize_lite6_retained_consumer_fixture_from(
      root.path(),
      test::make_lite6_stop_qualified_ros_motion_retention_fixture());
  const auto default_runtime = make_runtime_context(*defaulted.bundle);
  const auto default_expected =
      load_verified_controller_lifecycle_expected_domain(
          *defaulted.bundle, source, default_runtime.desired_configuration,
          default_runtime.source_runtime_unit_id,
          default_runtime.source_runtime_epoch);
  const auto default_plan = resolve_verified_stop_base_plan(
      *defaulted.bundle, source, default_expected,
      default_runtime.active_configuration, default_runtime.binding);
  ASSERT_TRUE(default_plan.escalation_stages.front()
                  .default_contract_parameters.has_value());
  EXPECT_EQ(core::controlled_stop_deadline_from_contract_parameters(
                *default_plan.escalation_stages.front()
                     .default_contract_parameters),
            std::optional<core::Duration>(1'500'000'000));
}

TEST(ResolvedStopBasePlanResolverTest,
     ResolvesExactActiveProductAndSealsReadiness) {
  ScopedTrustedRoot root;
  const auto retained = test::
      materialize_lite6_stop_capable_ros_motion_retained_consumer_fixture(
          root.path());
  const FilesystemVerifiedRetainedProductArtifactSource source(root.path());
  const auto runtime = make_runtime_context(*retained.bundle);
  const auto expected = load_verified_controller_lifecycle_expected_domain(
      *retained.bundle, source, runtime.desired_configuration,
      runtime.source_runtime_unit_id, runtime.source_runtime_epoch);

  const auto plan = resolve_verified_stop_base_plan(
      *retained.bundle, source, expected, runtime.active_configuration,
      runtime.binding);
  ASSERT_TRUE(core::VerifyResolvedStopBasePlan(plan).ok());
  EXPECT_EQ(plan.retained_bundle_id, retained.bundle->retained_bundle_id());
  EXPECT_EQ(plan.active_control_configuration_id,
            runtime.active_configuration.active_control_configuration_id);
  EXPECT_EQ(plan.source_sequence, 7U);
  ASSERT_EQ(plan.escalation_stages.size(), 2U);
  EXPECT_EQ(plan.escalation_stages.front().stage_kind,
            core::StopEscalationStageKind::kNominal);
  EXPECT_TRUE(
      plan.escalation_stages.front()
          .command_qualification.controlled_substitution_contract.has_value());
  EXPECT_EQ(plan.escalation_stages.back().stage_kind,
            core::StopEscalationStageKind::kSoftwareHard);
  EXPECT_EQ(
      plan.escalation_stages.back().command_qualification.stop_action_class,
      core::StopActionClass::kSoftwareHard);

  auto readiness = seal_runtime_participant_stop_readiness(
      *retained.bundle, plan, core::ComponentLifecycleGeneration(11U), expected,
      runtime.binding);
  ASSERT_TRUE(readiness.ok()) << readiness.error().message;
  EXPECT_EQ(readiness.value().value().resolved_stop_base_plan, plan);
}

TEST(ResolvedStopBasePlanResolverTest,
     RejectsExternalConfigurationOrRuntimeAuthorityDrift) {
  ScopedTrustedRoot root;
  const auto retained = test::
      materialize_lite6_stop_capable_ros_motion_retained_consumer_fixture(
          root.path());
  const FilesystemVerifiedRetainedProductArtifactSource source(root.path());
  const auto runtime = make_runtime_context(*retained.bundle);
  const auto expected = load_verified_controller_lifecycle_expected_domain(
      *retained.bundle, source, runtime.desired_configuration,
      runtime.source_runtime_unit_id, runtime.source_runtime_epoch);

  auto unreconciled = runtime.active_configuration;
  unreconciled.state =
      core::ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled;
  EXPECT_THROW(
      static_cast<void>(resolve_verified_stop_base_plan(
          *retained.bundle, source, expected, unreconciled, runtime.binding)),
      RetainedBundleError);

  auto changed_authority = runtime.active_configuration;
  changed_authority.kinematic_resource_source_projections.front()
      .state_authority_id = core::StateAuthorityId("state.substituted");
  EXPECT_THROW(static_cast<void>(resolve_verified_stop_base_plan(
                   *retained.bundle, source, expected, changed_authority,
                   runtime.binding)),
               RetainedBundleError);
}

} // namespace
} // namespace manyforge::deployment

namespace manyforge::deployment {
namespace {

TEST(ResolvedStopBasePlanResolverTest, RosGripperLogicalEvidenceSurvivesDurableReloadWithoutRest) {
  for (const std::string_view kind : {"pneumatic", "position_commanded"}) {
    ScopedTrustedRoot root;
    const auto retained = test::materialize_lite6_retained_consumer_fixture_from(
        root.path(), test::make_lite_gripper_only_retention_fixture({}, kind));
    ASSERT_EQ(retained.bundle->value().compiled_products.symbol_role_maps.size(), 1U);
    FilesystemVerifiedRetainedModelMapSource maps(root.path());
    const auto map = maps.get_verified(RetainedModelMapQuery{
        retained.bundle->retained_bundle_id(),
        retained.bundle->value().compiled_products.symbol_role_maps.front().product_id});
    ASSERT_TRUE(map.has_value());
    const auto runtime = make_runtime_context(*retained.bundle, "tool.gripper.command", &*map);
    FilesystemVerifiedRetainedProductArtifactSource source(root.path());
    const auto expected = load_verified_controller_lifecycle_expected_domain(
        *retained.bundle, source, runtime.desired_configuration,
        runtime.source_runtime_unit_id, runtime.source_runtime_epoch);
    EXPECT_EQ(expected.value().physical_expectation,
              core::ControllerLifecyclePhysicalExpectation::kRequired);
    for (const auto &binding : expected.value().expected_bindings) {
      EXPECT_TRUE(binding.logical_owner_resources.empty());
      EXPECT_EQ(binding.gripper_actuation_kind,
                kind == "pneumatic"
                    ? std::optional(core::GripperActuationKind::kPneumatic)
                    : std::nullopt);
    }
    const auto plan = resolve_verified_stop_base_plan(
        *retained.bundle, source, expected, runtime.active_configuration, runtime.binding);
    ASSERT_EQ(plan.escalation_stages.size(), kind == "pneumatic" ? 1U : 2U);
    EXPECT_EQ(plan.gripper_actuation_kind,
              kind == "pneumatic"
                  ? std::optional(core::GripperActuationKind::kPneumatic)
                  : std::nullopt);
    for (const auto &stage : plan.escalation_stages) {
      ASSERT_EQ(stage.evidence_requirements.size(), 1U);
      EXPECT_EQ(stage.evidence_requirements.front().requirement.evidence_kind,
                core::StopEvidenceRequirementKind::kLogicalCancellationNoContinuingEffect);
      EXPECT_FALSE(stage.command_target.endpoint_role_id.has_value());
      EXPECT_FALSE(stage.evidence_requirements.front().resolved_source.runtime_target
                       .endpoint_role_id.has_value());
    }
    const auto readiness = seal_runtime_participant_stop_readiness(
        *retained.bundle, plan, core::ComponentLifecycleGeneration(11U), expected,
        runtime.binding);
    ASSERT_TRUE(readiness.ok()) << readiness.error().message;
  }
}

TEST(ResolvedStopBasePlanResolverTest, ModelBackedCommandedGripperWithoutRowRetainsStrictCompilerRefusal) {
  ScopedTrustedRoot root;
  // Remove the row only from authored compiler input. No verified retained
  // product is rewritten or constructed after this genuine compiler refusal.
  const auto without_row = +[](CanonicalValue::Object &graph) {
    test::map_lite_gripper_only_graph(graph);
    const auto units_field = std::find_if(graph.begin(), graph.end(),
        [](const auto &field) { return field.first == "runtime_units"; });
    if (units_field == graph.end()) throw std::logic_error("authored fixture has no runtime units");
    auto units = units_field->second.as_array();
    std::size_t cleared = 0U;
    for (auto &unit : units) {
      auto fields = unit.as_object();
      const auto configuration = std::find_if(fields.begin(), fields.end(),
          [](const auto &field) { return field.first == "configuration"; });
      if (configuration == fields.end()) throw std::logic_error("authored runtime has no configuration");
      auto values = configuration->second.as_object();
      const auto rows = std::find_if(values.begin(), values.end(),
          [](const auto &field) { return field.first == "gripper_target_configurations"; });
      if (rows != values.end()) {
        rows->second = CanonicalValue(CanonicalValue::Array{});
        ++cleared;
      }
      // M6 TOOLSPLIT: the runtime codec requires the gripper_feedback stream
      // exactly for the authored rows; drop it with the row so the compiler's
      // own refusal is what is tested.
      const auto streams = std::find_if(values.begin(), values.end(),
          [](const auto &field) { return field.first == "component_streams"; });
      if (streams != values.end()) {
        auto kept = streams->second.as_array();
        std::erase_if(kept, [](const CanonicalValue &stream) {
          const auto *id = stream.find_object_member("stream_id");
          return id != nullptr && id->as_string() == "gripper_feedback";
        });
        streams->second = CanonicalValue(std::move(kept));
      }
      configuration->second = CanonicalValue(std::move(values));
      unit = CanonicalValue(std::move(fields));
    }
    if (cleared != 1U) throw std::logic_error("authored fixture does not have exactly one gripper row set");
    units_field->second = CanonicalValue(std::move(units));
  };
  try {
    // The real Lite gripper as a model-backed position gripper with its
    // genuine commanded provider/hard floor, not an invented None owner.
    const auto fixture = test::make_lite_gripper_only_retention_fixture(
        {}, "position_commanded", 0.2, "commanded", false, without_row);
    (void)test::materialize_lite6_retained_consumer_fixture_from(root.path(), fixture);
    FAIL() << "model-backed ROS gripper without a matching authored row was accepted";
  } catch (const CellApplicabilityError &error) {
    EXPECT_STREQ(error.what(),
        "model-backed ROS gripper binding lacks one exact authored target configuration");
  }
}

TEST(ResolvedStopBasePlanResolverTest, NoOutputGripperSealsOnlyItsExactLocalOwnerWithoutPhysicalControllers) {
  for (const double dwell : {0.0, 0.25}) {
    ScopedTrustedRoot root;
    const auto retained = test::materialize_lite6_retained_consumer_fixture_from(
        root.path(), test::make_lite_gripper_only_retention_fixture({}, "pneumatic", dwell, "none"));
    ASSERT_EQ(retained.bundle->value().compiled_products.symbol_role_maps.size(), 1U);
    FilesystemVerifiedRetainedModelMapSource maps(root.path());
    const auto map = maps.get_verified(RetainedModelMapQuery{
        retained.bundle->retained_bundle_id(),
        retained.bundle->value().compiled_products.symbol_role_maps.front().product_id});
    ASSERT_TRUE(map.has_value());
    const auto runtime = make_runtime_context(*retained.bundle, "tool.gripper.command", &*map);
    FilesystemVerifiedRetainedProductArtifactSource source(root.path());
    const auto expected = load_verified_controller_lifecycle_expected_domain(
        *retained.bundle, source, runtime.desired_configuration,
        runtime.source_runtime_unit_id, runtime.source_runtime_epoch);
    const auto &value = expected.value();
    EXPECT_EQ(value.physical_expectation,
              core::ControllerLifecyclePhysicalExpectation::kNotApplicablePneumaticNone);
    EXPECT_EQ(value.source_runtime_unit_id, runtime.source_runtime_unit_id);
    EXPECT_EQ(value.source_runtime_epoch, runtime.source_runtime_epoch);
    EXPECT_EQ(value.desired_configuration, runtime.desired_configuration);
    EXPECT_TRUE(value.expected_controllers.empty());
    EXPECT_TRUE(value.controller_topology.controllers.empty());
    EXPECT_TRUE(value.controller_topology.chain_edges.empty());
    ASSERT_FALSE(value.expected_bindings.empty());
    ASSERT_FALSE(value.expected_endpoints.empty()); // actual local owner routes
    for (const auto &binding : value.expected_bindings) {
      EXPECT_TRUE(binding.claimed_command_interfaces.empty());
      EXPECT_TRUE(binding.claimed_state_interfaces.empty());
      EXPECT_EQ(binding.gripper_actuation_kind,
                std::optional(core::GripperActuationKind::kPneumatic));
      const auto &retained_binding = require_one(
          retained.bundle->value().cell_graph.control_bindings,
          [&](const auto &candidate) {
            return candidate.control_binding_id == binding.control_binding_id &&
                   candidate.control_binding_revision == binding.control_binding_revision;
          }, "exact no-output resource binding");
      std::set<core::ResourceId> resources;
      for (const auto &resource : retained_binding.resource_bindings)
        resources.insert(resource.resource_id);
      ASSERT_FALSE(resources.empty());
      EXPECT_EQ(binding.logical_owner_resources,
                std::vector<core::ResourceId>(resources.begin(), resources.end()));
    }
    auto missing_resources = value;
    missing_resources.expected_bindings.front().logical_owner_resources.clear();
    EXPECT_FALSE(core::verify_controller_lifecycle_expected_domain(std::move(missing_resources)).ok());
    auto duplicate_resources = value;
    duplicate_resources.expected_bindings.front().logical_owner_resources.push_back(
        duplicate_resources.expected_bindings.front().logical_owner_resources.front());
    EXPECT_FALSE(core::verify_controller_lifecycle_expected_domain(std::move(duplicate_resources)).ok());
    for (const auto &activation : value.controller_topology.activation_sets) {
      EXPECT_TRUE(activation.active_controller_role_ids.empty());
      EXPECT_TRUE(activation.active_chain_edges.empty());
    }
    for (const auto &route : value.controller_topology.legal_routes) {
      EXPECT_TRUE(route.ordered_controller_role_ids.empty());
      EXPECT_FALSE(route.compatible_activation_set_ids.empty());
    }
    const auto config = ros2_control_execution_runtime_contract::require_component_configuration(
        *retained.bundle, runtime.source_runtime_unit_id,
        runtime.binding.value().controllable_component_id);
    ASSERT_EQ(config.gripper_target_configurations.size(), 1U);
    EXPECT_EQ(config.gripper_target_configurations.front().output_mode, core::GripperOutputMode::kNone);
    EXPECT_DOUBLE_EQ(config.gripper_target_configurations.front().command_duration_s, dwell);
    const auto projection = ros2_control_execution_runtime_contract::component_configuration_projection(
        *retained.bundle, runtime.source_runtime_unit_id,
        runtime.binding.value().controllable_component_id);
    const auto *projected = projection.find_object_member("gripper_target_configurations");
    ASSERT_NE(projected, nullptr);
    ASSERT_EQ(projected->as_array().size(), 1U);
    ASSERT_NE(projected->as_array().front().find_object_member("output_mode"), nullptr);
    EXPECT_EQ(projected->as_array().front().find_object_member("output_mode")->as_string(), "none");
    auto foreign = runtime.desired_configuration;
    foreign.control_domain_revision = core::ControlDomainRevision(std::string(kZeroDigest));
    EXPECT_THROW((void)load_verified_controller_lifecycle_expected_domain(
        *retained.bundle, source, foreign, runtime.source_runtime_unit_id, runtime.source_runtime_epoch),
        RetainedBundleError);
    EXPECT_THROW((void)load_verified_controller_lifecycle_expected_domain(
        *retained.bundle, source, runtime.desired_configuration,
        core::RuntimeUnitId("foreign.runtime"), runtime.source_runtime_epoch), RetainedBundleError);
    EXPECT_THROW((void)load_verified_controller_lifecycle_expected_domain(
        *retained.bundle, source, runtime.desired_configuration,
        runtime.source_runtime_unit_id, core::RuntimeEpoch("")), RetainedBundleError);
    auto foreign_resources = runtime.binding.value();
    ASSERT_FALSE(foreign_resources.resources.empty());
    foreign_resources.resources.front().resource_id = core::ResourceId("foreign.logical.resource");
    const auto foreign_participant = core::verify_runtime_participant_binding(std::move(foreign_resources));
    ASSERT_TRUE(foreign_participant.ok());
    EXPECT_THROW((void)resolve_verified_stop_base_plan(
        *retained.bundle, source, expected, runtime.active_configuration,
        foreign_participant.value()), RetainedBundleError);
    const auto plan = resolve_verified_stop_base_plan(
        *retained.bundle, source, expected, runtime.active_configuration, runtime.binding);
    EXPECT_EQ(plan.gripper_actuation_kind,
              std::optional(core::GripperActuationKind::kPneumatic));
    ASSERT_EQ(plan.escalation_stages.size(), 1U);
    EXPECT_EQ(plan.escalation_stages.front().stage_kind, core::StopEscalationStageKind::kNominal);
    const auto readiness = seal_runtime_participant_stop_readiness(
        *retained.bundle, plan, core::ComponentLifecycleGeneration(11U), expected,
        runtime.binding);
    ASSERT_TRUE(readiness.ok()) << readiness.error().message;
  }
}

} // namespace
} // namespace manyforge::deployment
