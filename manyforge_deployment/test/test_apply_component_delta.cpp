#include <gtest/gtest.h>

#include <manyforge_deployment/apply_affected_scope.hpp>
#include <manyforge_deployment/apply_component_delta.hpp>
#include <manyforge_deployment/cell_kinematic_index.hpp>
#include <manyforge_deployment/native_execution_runtime_contract.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "activation_deployment_compiler_internal.hpp"
#include "apply_component_delta_internal.hpp"
#include "content_identity_internal.hpp"
#include "lite6_controllable_retention_fixture.hpp"
#include "retained_bundle_internal.hpp"
#include "retention_store_fixtures.hpp"

namespace manyforge::deployment::detail {

/// Uninstalled test-only access to the producer-less retention context, as in
/// test_retained_bundle_materializer.cpp; it proves mechanics only.
class RetentionMaterializerTestAccess final {
public:
  [[nodiscard]] static BuildQualifiedRetentionContext make_context() {
    return BuildQualifiedRetentionContext(
        core::RetainedNodeEnvironmentDTO{
            "manyforge_apply_component_delta_test",
            "1.0.0",
            core::ContentDigest("sha256:"
                                "2222222222222222222222222222222222222222222222"
                                "222222222222222222"),
            core::ContentDigest("sha256:"
                                "3333333333333333333333333333333333333333333333"
                                "333333333333333333"),
        },
        core::ContentDigest(
            "sha256:"
            "4444444444444444444444444444444444444444444444444444444444444444"),
        core::ContentDigest("sha256:"
                            "55555555555555555555555555555555555555555555555555"
                            "55555555555555"));
  }
};

} // namespace manyforge::deployment::detail

namespace manyforge::deployment {
namespace {

namespace core = manyforge::core;

constexpr std::string_view kDigestOne =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kDigestTwo =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";
constexpr std::string_view kFixtureCellId = "apply_component_delta_cell";

[[nodiscard]] std::shared_ptr<const RetainedBundle>
Materialize(const test::Lite6ControllableRetentionFixture &fixture) {
  const test::Lite6ControllableCompilerInputArtifactSource input_source(
      fixture);
  test::InMemoryCatalogArtifactStoreFixture catalog_store;
  test::InMemoryRawArtifactStoreFixture raw_store;
  auto bundle = detail::RetainedBundleMaterializer::materialize(
      fixture.qualified,
      detail::RetentionMaterializerTestAccess::make_context(), input_source,
      fixture.structural_preparations(), catalog_store, raw_store);
  if (bundle == nullptr || !verify_retained_bundle_identity(*bundle)) {
    throw std::logic_error("test bundle did not materialize");
  }
  return bundle;
}

[[nodiscard]] const RetainedBundle &OneArm() {
  static const auto bundle =
      Materialize(test::make_lite6_controllable_retention_fixture(
          false, false, 1U, false, true, kFixtureCellId));
  return *bundle;
}

[[nodiscard]] const RetainedBundle &TwoArms() {
  static const auto bundle =
      Materialize(test::make_lite6_controllable_retention_fixture(
          false, false, 2U, false, true, kFixtureCellId));
  return *bundle;
}

[[nodiscard]] core::ControllableComponentId FirstArm() {
  return core::ControllableComponentId(
      std::string(test::kLite6ControllableComponentId));
}

[[nodiscard]] core::ControllableComponentId SecondArm() {
  return core::ControllableComponentId(
      test::lite6_controllable_fixture_detail::indexed_id(
          test::kLite6ControllableComponentId, 1U));
}

[[nodiscard]] const ApplyComponentChangeDTO &
Row(const ApplyComponentDeltaDTO &delta,
    const core::ControllableComponentId &id) {
  const auto found = std::find_if(
      delta.components.begin(), delta.components.end(),
      [&](const auto &row) { return row.controllable_component_id == id; });
  if (found == delta.components.end()) {
    throw std::logic_error("test delta lacks the component row");
  }
  return *found;
}

[[nodiscard]] ApplyComponentChangeDTO
Change(std::string id, const ApplyComponentDisposition disposition) {
  ApplyComponentChangeDTO change{
      core::ControllableComponentId(std::move(id)),
      ApplyComponentDisposition::kRetain,
      std::nullopt,
      std::nullopt,
      {},
      {},
      {}};
  change.disposition = disposition;
  if (disposition != ApplyComponentDisposition::kAdd) {
    change.predecessor_runtime_identity = core::ContentDigest(std::string(kDigestOne));
    change.predecessor_domains = {core::ControlDomainId("domain")};
  }
  if (disposition != ApplyComponentDisposition::kRemove) {
    change.candidate_runtime_identity = core::ContentDigest(std::string(
        disposition == ApplyComponentDisposition::kReplace ? kDigestTwo
                                                           : kDigestOne));
    change.candidate_domains = {core::ControlDomainId("domain")};
  }
  if (disposition == ApplyComponentDisposition::kReplace) {
    change.replacement_reasons = {std::string(kApplyComponentReasonBindings)};
  }
  return change;
}

// DX-753: keep the archived Native fixture entrypoint compiled without admission.
[[maybe_unused]] constexpr auto kHeldNativeOneArmFixture = &OneArm;

[[nodiscard]] std::shared_ptr<const RetainedBundle> RosArms(
    std::size_t count, std::optional<double> first_position,
    std::string_view first_prefix);

[[nodiscard]] const RetainedBundle &RosOneArm() {
  static const auto bundle = RosArms(1U, std::nullopt, {});
  return *bundle;
}

[[nodiscard]] const RetainedBundle &RosTwoArms() {
  static const auto bundle = RosArms(2U, std::nullopt, {});
  return *bundle;
}

TEST(ApplyComponentDelta, UnchangedBundleRetainsEveryComponentRuntime) {
  const auto delta = derive_apply_component_delta(RosTwoArms(), RosTwoArms());
  ASSERT_TRUE(delta.valid());
  ASSERT_EQ(delta.components.size(), 2U);
  for (const auto &row : delta.components) {
    EXPECT_EQ(row.disposition, ApplyComponentDisposition::kRetain);
    EXPECT_EQ(row.predecessor_runtime_identity, row.candidate_runtime_identity);
    EXPECT_EQ(row.candidate_runtime_identity,
              derive_component_runtime_identity(RosTwoArms(),
                                                row.controllable_component_id));
    EXPECT_FALSE(row.candidate_domains.empty());
    EXPECT_TRUE(row.replacement_reasons.empty());
  }
  EXPECT_FALSE(delta.shared_scene_projection_changed);
  EXPECT_TRUE(delta.attachment_changed_components.empty());
  // A Program/Scene-only Apply needs no component inside its fence.
  EXPECT_TRUE(apply_component_delta_is_fenced(delta, {}));
}

TEST(ApplyComponentDelta, RuntimeIdentityIsPerComponentAndExact) {
  const auto first = derive_component_runtime_identity(RosTwoArms(), FirstArm());
  const auto second = derive_component_runtime_identity(RosTwoArms(), SecondArm());
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_NE(*first, *second);
  EXPECT_EQ(derive_component_runtime_identity(RosTwoArms(), FirstArm()), first);
  EXPECT_FALSE(derive_component_runtime_identity(
                   RosOneArm(), SecondArm())
                   .has_value());
}

TEST(ApplyComponentDelta, NativeConfigurationSelectionKeepsExactComponentSettings) {
  GTEST_SKIP() << "held: multi-unit discussion";
  const auto &graph = TwoArms().value().cell_graph;
  for (const auto &component : graph.controllable_components) {
    ASSERT_EQ(component.runtime_unit_ids.size(), 1U);
    const auto &unit = component.runtime_unit_ids.front();
    const auto selected = native_execution_runtime_contract::require_component_configuration(
        TwoArms(), unit, component.controllable_component_id);
    ASSERT_EQ(selected.initial_joint_positions_rad.size(), 6U);
    ASSERT_EQ(selected.motion_target_limits.size(), 1U);
    EXPECT_EQ(selected.motion_target_limits.front().controllable_component_id,
              component.controllable_component_id);
    for (const auto &row : selected.direct_control_joint_limits) {
      EXPECT_EQ(row.controllable_component_id, component.controllable_component_id);
    }
    const auto projection = native_execution_runtime_contract::component_configuration_projection(
        TwoArms(), unit, component.controllable_component_id);
    EXPECT_EQ(projection.find_object_member("initial_joint_positions_rad")->as_array().size(),
              selected.initial_joint_positions_rad.size());
    EXPECT_THROW(native_execution_runtime_contract::require_component_configuration(
                     TwoArms(), unit, core::ControllableComponentId("missing")),
                 RetainedBundleError);
  }
}

TEST(ApplyComponentDelta, RosConfigurationSelectionUsesExactBindingResources) {
  const auto fixture = test::make_lite6_controllable_retention_fixture(
      false, false, 1U, false, true, kFixtureCellId, false, false,
      "resource", false, {}, {}, true);
  const auto bundle = Materialize(fixture);
  const auto &component = bundle->value().cell_graph.controllable_components.front();
  const auto unit = component.runtime_unit_ids.front();
  const auto selected = ros2_control_execution_runtime_contract::require_component_configuration(
      *bundle, unit, component.controllable_component_id);
  ASSERT_EQ(selected.controller_profiles.size(), 1U);
  EXPECT_TRUE(selected.controller_profiles.front().frame_prefix.empty());
  EXPECT_EQ(selected.initial_joint_positions_rad.size(), 6U);
  ASSERT_EQ(selected.motion_target_limits.size(), 1U);
  EXPECT_EQ(selected.motion_target_limits.front().controllable_component_id,
            component.controllable_component_id);
  const auto projection = ros2_control_execution_runtime_contract::component_configuration_projection(
      *bundle, unit, component.controllable_component_id);
  EXPECT_EQ(projection.find_object_member("controller_profiles")->as_array().size(), 1U);
  EXPECT_THROW(ros2_control_execution_runtime_contract::require_component_configuration(
                   *bundle, unit, core::ControllableComponentId("missing")), RetainedBundleError);
}

[[nodiscard]] std::shared_ptr<const RetainedBundle> RosArms(
    const std::size_t count, std::optional<double> first_position = std::nullopt,
    const std::string_view first_prefix = {}) {
  return Materialize(test::make_lite6_controllable_retention_fixture(
      false, false, count, false, true, kFixtureCellId, false, false,
      "resource", false, {}, {}, true, false, false, first_position, false,
      test::kLite6ArmTargetId, std::nullopt, std::nullopt, first_prefix));
}

TEST(ApplyComponentDelta, RosSharedHostRetainsUnrelatedRobotAndOwnChangesReplace) {
  const auto one = RosArms(1U);
  const auto two = RosArms(2U);
  const auto changed_positions = RosArms(2U, 0.25);
  const auto changed_prefix = RosArms(2U, std::nullopt, "own/");
  EXPECT_EQ(Row(derive_apply_component_delta(*one, *two), FirstArm()).disposition,
            ApplyComponentDisposition::kRetain);
  EXPECT_EQ(Row(derive_apply_component_delta(*two, *one), FirstArm()).disposition,
            ApplyComponentDisposition::kRetain);
  for (const auto &changed : {changed_positions, changed_prefix}) {
    const auto delta = derive_apply_component_delta(*two, *changed);
    EXPECT_EQ(Row(delta, FirstArm()).disposition, ApplyComponentDisposition::kReplace);
    EXPECT_EQ(Row(delta, SecondArm()).disposition, ApplyComponentDisposition::kRetain);
  }
  const auto &unit = two->value().cell_graph.runtime_units.front();
  const auto selected = ros2_control_execution_runtime_contract::require_component_configuration(
      *changed_positions, unit.runtime_unit_id, FirstArm());
  EXPECT_EQ(selected.initial_joint_positions_rad.front(), 0.25);
  const auto peer = ros2_control_execution_runtime_contract::require_component_configuration(
      *changed_positions, unit.runtime_unit_id, SecondArm());
  EXPECT_EQ(peer.initial_joint_positions_rad.front(), 0.0);
  EXPECT_EQ(peer.controller_profiles.front().frame_prefix, SecondArm().value() + "/");
}

TEST(ApplyComponentDelta,
     RetainedActivationRefusesExclusiveIsaacTopicCollision) {
  const auto fixture = test::make_lite6_controllable_retention_fixture(
      false, false, 2U, false, true, kFixtureCellId, false, false, "resource",
      false, {}, {}, true, false, false, std::nullopt, false,
      test::kLite6ArmTargetId, std::nullopt, std::nullopt, "", false);
  const auto bundle = Materialize(fixture);
  const auto &unit = bundle->value().cell_graph.runtime_units.front();
  try {
    static_cast<void>(
        ros2_control_execution_runtime_contract::require_configuration(
            *bundle, unit.runtime_unit_id));
    FAIL() << "colliding topics must refuse before activation";
  } catch (const RetainedBundleError &error) {
    const std::string reason(error.what());
    EXPECT_NE(reason.find(FirstArm().value()), std::string::npos);
    EXPECT_NE(reason.find(SecondArm().value()), std::string::npos);
    EXPECT_NE(reason.find("/isaac_joint_commands"), std::string::npos);
  }
}

// S5: a robot model without a `flange` frame role is refused when the
// Deployment compiles (Apply), typed, under `active_tcp_and_flange` (the
// default); `active_tcp` needs no flange. The runtime host refused the same
// robot only at activation, with untyped text.
TEST(ApplyComponentDelta, SpeedCheckAtTheFlangeRefusesARobotWithoutFlangeAtCompile) {
  const std::vector<core::ModelFrameRoleBindingDTO> without_flange{
      {core::FrameRoleId("base"), "link_base"}, {core::FrameRoleId("tcp"), "link_eef"}};
  auto with_flange = without_flange;
  with_flange.push_back({core::FrameRoleId("flange"), "link6"});
  try {
    detail::require_speed_check_flange("third_party_robot", "active_tcp_and_flange",
                                       without_flange);
    FAIL() << "a robot without flange must be refused under active_tcp_and_flange";
  } catch (const std::invalid_argument &error) {
    const std::string what(error.what());
    EXPECT_NE(what.find("robot_flange_frame_missing: third_party_robot"), std::string::npos)
        << what;
  }
  EXPECT_NO_THROW(detail::require_speed_check_flange("third_party_robot", "active_tcp",
                                                     without_flange));
  EXPECT_NO_THROW(detail::require_speed_check_flange("lite6_robot", "active_tcp_and_flange",
                                                     with_flange));
}

TEST(ApplyComponentDelta, EmptyRosHostHasNoFakeComponents) {
  const auto catalog = test::lite6_controllable_fixture_detail::qualify_control_catalog(
      false, true, false, "resource", false, {}, true);
  const auto &profile = *catalog.ros2_control_execution_runtime_profile;
  detail::ActivationDeploymentIntent intent{};
  intent.deployment_artifact_id = "empty_ros_compiler_regression";
  intent.name = "Empty ROS compiler regression";
  intent.attachments = CanonicalValue(CanonicalValue::Array{});
  intent.policies = CanonicalValue(CanonicalValue::Object{});
  const auto authored = detail::compile_activation_deployment(
      intent, core::CellId(std::string(kFixtureCellId)), {}, {},
      {profile.reference()}, {profile});
  const auto *authored_graph = authored.find_object_member("graph");
  ASSERT_NE(authored_graph, nullptr);
  const auto *units = authored_graph->find_object_member("runtime_units");
  ASSERT_NE(units, nullptr);
  ASSERT_EQ(units->as_array().size(), 1U);
  const auto &host = units->as_array().front();
  EXPECT_EQ(host.find_object_member("runtime_profile_id")->as_string(),
            ros2_control_execution_runtime_contract::kRuntimeProfileId);
  EXPECT_EQ(canonical_json(*host.find_object_member("configuration")),
            canonical_json(
                ros2_control_execution_runtime_contract::empty_host_configuration()));
  const auto fixture = test::make_lite6_controllable_retention_fixture(
      false, false, 0U, false, true, kFixtureCellId, false, false, "resource",
      false, {}, {}, true, false, false, std::nullopt, false,
      test::kLite6ArmTargetId);
  ASSERT_TRUE(fixture.structural_preparations().empty());
  ASSERT_TRUE(fixture.qualified.compiler_ir().applicability().composition()
                  .model_assets().empty());
  const auto bundle = Materialize(fixture);
  const auto &graph = bundle->value().cell_graph;
  ASSERT_EQ(graph.runtime_units.size(), 1U);
  EXPECT_TRUE(graph.controllable_components.empty());
  EXPECT_TRUE(graph.model_instances.empty());
  EXPECT_TRUE(graph.control_domains.empty());
  EXPECT_TRUE(graph.control_bindings.empty());
  const auto configuration =
      ros2_control_execution_runtime_contract::require_configuration(
          *bundle, graph.runtime_units.front().runtime_unit_id);
  EXPECT_TRUE(configuration.controller_profiles.empty());
  EXPECT_TRUE(configuration.initial_joint_positions_rad.empty());
  EXPECT_TRUE(configuration.motion_target_limits.empty());
  EXPECT_EQ(configuration.planner_rng_seed, 12345U);
  EXPECT_EQ(configuration.execution_tick_hz, 100U);
  EXPECT_EQ(configuration.collision_monitor_period_ms, 50.0);
}

TEST(ApplyComponentDelta, NonemptyRosHostStillRefusesEmptyRequiredRows) {
  const auto valid_fixture = test::make_lite6_controllable_retention_fixture(
      false, false, 1U, false, true, kFixtureCellId, false, false, "resource",
      false, {}, {}, true);
  const auto valid_bundle = Materialize(valid_fixture);
  const auto &graph = valid_bundle->value().cell_graph;
  for (const std::string_view field : {"controller_profiles",
                                       "initial_joint_positions_rad",
                                       "motion_target_limits"}) {
    SCOPED_TRACE(field);
    EXPECT_THROW(
        static_cast<void>(test::make_lite6_controllable_retention_fixture(
            false, false, 1U, false, true, kFixtureCellId, false, false, "resource",
            false, {}, {}, true, false, false, std::nullopt, false,
            test::kLite6ArmTargetId, std::nullopt, std::nullopt, "", true, field)),
        CellGraphCodecError);
    auto unit = graph.runtime_units.front();
    unit.configuration.at(std::string(field)).value = core::JsonValueDTO::Array{};
    EXPECT_THROW(
        static_cast<void>(detail::require_ros2_control_execution_runtime_required_rows(
            graph, unit)),
        RetainedBundleError);
  }
}

TEST(ApplyComponentDelta, AddedAndRemovedComponentsAreClassifiedAndFenced) {
  const auto grow = derive_apply_component_delta(RosOneArm(), RosTwoArms());
  ASSERT_TRUE(grow.valid());
  ASSERT_EQ(grow.components.size(), 2U);
  const auto added = Row(grow, SecondArm());
  EXPECT_EQ(added.disposition, ApplyComponentDisposition::kAdd);
  EXPECT_FALSE(added.predecessor_runtime_identity.has_value());
  EXPECT_EQ(added.candidate_runtime_identity,
            derive_component_runtime_identity(RosTwoArms(), SecondArm()));
  EXPECT_TRUE(added.predecessor_domains.empty());
  EXPECT_FALSE(added.candidate_domains.empty());
  // Adding an independent arm changes shared projections, not this arm's
  // controller configuration. Its actual producer identities must stay equal.
  const auto kept = Row(grow, FirstArm());
  EXPECT_EQ(kept.predecessor_runtime_identity,
            derive_component_runtime_identity(RosOneArm(), FirstArm()));
  EXPECT_EQ(kept.candidate_runtime_identity,
            derive_component_runtime_identity(RosTwoArms(), FirstArm()));
  EXPECT_EQ(kept.disposition, ApplyComponentDisposition::kRetain);
  EXPECT_EQ(kept.predecessor_runtime_identity, kept.candidate_runtime_identity);
  EXPECT_TRUE(kept.replacement_reasons.empty());
  EXPECT_TRUE(apply_component_delta_is_fenced(
      grow, derive_apply_configuration_fence_scope(RosOneArm(), RosTwoArms())));

  const auto shrink = derive_apply_component_delta(RosTwoArms(), RosOneArm());
  ASSERT_TRUE(shrink.valid());
  EXPECT_EQ(Row(shrink, FirstArm()).disposition,
            ApplyComponentDisposition::kRetain);
  const auto removed = Row(shrink, SecondArm());
  EXPECT_EQ(removed.disposition, ApplyComponentDisposition::kRemove);
  EXPECT_EQ(removed.predecessor_runtime_identity,
            derive_component_runtime_identity(RosTwoArms(), SecondArm()));
  EXPECT_FALSE(removed.candidate_runtime_identity.has_value());
  EXPECT_TRUE(removed.candidate_domains.empty());
  EXPECT_TRUE(apply_component_delta_is_fenced(
      shrink, derive_apply_configuration_fence_scope(RosTwoArms(), RosOneArm())));
}

TEST(ApplyComponentDelta, CodecAndDigestAreCanonicalAndExact) {
  const auto delta = derive_apply_component_delta(RosOneArm(), RosTwoArms());
  const auto encoded = EncodeApplyComponentDelta(delta);
  EXPECT_EQ(DecodeApplyComponentDelta(encoded), delta);
  EXPECT_EQ(EncodeApplyComponentDelta(DecodeApplyComponentDelta(encoded)),
            encoded);
  EXPECT_EQ(DigestApplyComponentDelta(delta), DigestApplyComponentDelta(delta));
  EXPECT_NE(DigestApplyComponentDelta(delta),
            DigestApplyComponentDelta(
                derive_apply_component_delta(RosTwoArms(), RosOneArm())));

  const ApplyComponentDeltaDTO empty;
  EXPECT_EQ(DecodeApplyComponentDelta(EncodeApplyComponentDelta(empty)), empty);
  EXPECT_THROW(static_cast<void>(DecodeApplyComponentDelta(encoded + " ")),
               ApplyComponentDeltaError);
  EXPECT_THROW(static_cast<void>(DecodeApplyComponentDelta("{}")),
               ApplyComponentDeltaError);
  auto unknown = encoded;
  const auto position = unknown.find("\"add\"");
  ASSERT_NE(position, std::string::npos);
  unknown.replace(position, 5U, "\"new\"");
  EXPECT_THROW(static_cast<void>(DecodeApplyComponentDelta(unknown)),
               ApplyComponentDeltaError);
}

TEST(ApplyComponentDelta, RowsStateExactlyWhatTheirDispositionMeans) {
  for (const auto disposition :
       {ApplyComponentDisposition::kRetain, ApplyComponentDisposition::kAdd,
        ApplyComponentDisposition::kRemove,
        ApplyComponentDisposition::kReplace}) {
    EXPECT_TRUE(Change("arm", disposition).valid()) << to_string(disposition);
  }
  auto retained_with_new_identity =
      Change("arm", ApplyComponentDisposition::kRetain);
  retained_with_new_identity.candidate_runtime_identity =
      core::ContentDigest(std::string(kDigestTwo));
  EXPECT_FALSE(retained_with_new_identity.valid());
  auto unexplained_replace = Change("arm", ApplyComponentDisposition::kReplace);
  unexplained_replace.replacement_reasons.clear();
  EXPECT_FALSE(unexplained_replace.valid());
  auto unknown_reason = Change("arm", ApplyComponentDisposition::kReplace);
  unknown_reason.replacement_reasons = {"placement"};
  EXPECT_FALSE(unknown_reason.valid());
  auto added_with_predecessor = Change("arm", ApplyComponentDisposition::kAdd);
  added_with_predecessor.predecessor_runtime_identity =
      core::ContentDigest(std::string(kDigestOne));
  EXPECT_FALSE(added_with_predecessor.valid());

  ApplyComponentDeltaDTO unsorted{
      {Change("b", ApplyComponentDisposition::kRetain),
       Change("a", ApplyComponentDisposition::kRetain)},
      false,
      {}};
  EXPECT_FALSE(unsorted.valid());
}

[[nodiscard]] core::ControllableComponentDTO
GraphComponent(std::string id, const bool placed) {
  return {core::ControllableComponentId(std::move(id)),
          core::ComponentTypeId("lite6_component"),
          core::ComponentTypeRevision("lite6_component-r1"),
          {{core::ModelRoleId("body"), core::ModelInstanceId("model")}},
          {},
          placed ? std::optional<core::PlacementPoseDTO>(core::PlacementPoseDTO{})
                 : std::nullopt};
}

[[nodiscard]] core::ComponentAttachmentEdgeDTO Edge(std::string parent,
                                                    std::string child) {
  return {core::ComponentAttachmentId(parent + "->" + child),
          core::ComponentAttachmentRevision("attachment-r1"),
          {core::ControllableComponentId(std::move(parent)),
           core::AttachmentInterfaceId("flange")},
          {core::ControllableComponentId(std::move(child)),
           core::AttachmentInterfaceId("mount")},
          core::PlacementPoseDTO{}};
}

[[nodiscard]] core::CellGraphDTO Graph(
    std::vector<core::ControllableComponentDTO> components,
    std::vector<core::ComponentAttachmentEdgeDTO> attachments = {}) {
  return {std::string(core::kCellGraphApiVersion),
          core::GraphRevision("graph.1"),
          core::CellId("cell.1"),
          {},
          {},
          std::move(components),
          {},
          {},
          std::move(attachments)};
}

TEST(ApplyComponentDelta, RemovedRobotLeavesItsGripperPlacedNotDropped) {
  const auto predecessor = Graph(
      {GraphComponent("robot", true), GraphComponent("gripper", false)},
      {Edge("robot", "gripper")});

  // The robot is removed; the independent gripper stays at an authored pose.
  const auto candidate = Graph({GraphComponent("gripper", true)});
  EXPECT_EQ(detached_attachment_children(predecessor, candidate),
            (std::vector{core::ControllableComponentId("gripper")}));

  // Still attached (to the same or another parent): nothing detaches.
  EXPECT_TRUE(detached_attachment_children(predecessor, predecessor).empty());

  // Explicit removal of the gripper too is a removal, not a detachment.
  EXPECT_TRUE(detached_attachment_children(predecessor, Graph({})).empty());

  // A detached child without a verified pose refuses before any effect.
  const auto unplaced = Graph({GraphComponent("gripper", false)});
  EXPECT_THROW(static_cast<void>(detached_attachment_children(predecessor, unplaced)),
               ApplyComponentDeltaError);
}

TEST(ApplyComponentDelta, EveryRuntimeChangeOwningADomainMustBeFenced) {
  const ApplyComponentDeltaDTO delta{
      {Change("arm", ApplyComponentDisposition::kReplace),
       Change("tool", ApplyComponentDisposition::kRetain)},
      false,
      {}};
  core::ConfigurationFenceScopeDTO scope;
  EXPECT_FALSE(apply_component_delta_is_fenced(delta, scope));
  scope.controllable_component_ids = {core::ControllableComponentId("arm")};
  EXPECT_TRUE(apply_component_delta_is_fenced(delta, scope));

  // A component without command domains has no route readiness to fence.
  auto passive = Change("sensor", ApplyComponentDisposition::kAdd);
  passive.candidate_domains.clear();
  EXPECT_TRUE(apply_component_delta_is_fenced({{passive}, false, {}}, {}));
}

// M7 part A (D17), equivalence oracle: the merge-6b `Relations` of
// apply_component_delta.cpp, copied verbatim (only the result type is the
// now-shared one). Test-local; removed at the next milestone.
namespace m6b {

[[nodiscard]] const core::ControllableComponentDTO *
FindComponent(const core::RetainedBundleDTO &bundle,
              const core::ControllableComponentId &id) {
  const auto &components = bundle.cell_graph.controllable_components;
  const auto found = std::find_if(
      components.begin(), components.end(),
      [&](const auto &component) { return component.controllable_component_id == id; });
  return found == components.end() ? nullptr : &*found;
}

[[nodiscard]] detail::ApplyComponentRelations
Relations(const core::RetainedBundleDTO &bundle,
          const core::ControllableComponentDTO &component) {
  const auto &graph = bundle.cell_graph;
  detail::ApplyComponentRelations relations;
  for (const auto &role : component.model_role_bindings) {
    relations.model_instances.insert(role.model_instance_id);
  }
  // A component's launch description carries the static frames of the
  // components mounted on it (the mount, and a process-less tool's frames),
  // so their model occurrences are part of its runtime facts.
  for (const auto &attachment : graph.component_attachments) {
    if (attachment.parent.controllable_component_id != component.controllable_component_id)
      continue;
    if (const auto *child = FindComponent(bundle, attachment.child.controllable_component_id)) {
      for (const auto &role : child->model_role_bindings)
        relations.model_instances.insert(role.model_instance_id);
    }
  }
  relations.runtime_units.insert(component.runtime_unit_ids.begin(),
                                 component.runtime_unit_ids.end());
  for (const auto &binding : graph.control_bindings) {
    if (binding.controllable_component_id != component.controllable_component_id) {
      continue;
    }
    relations.bindings.insert(binding.control_binding_id);
    relations.domains.insert(binding.control_domain_id);
    relations.runtime_units.insert(binding.provider_runtime_unit_id);
  }
  for (const auto &domain : graph.control_domains) {
    if (relations.domains.contains(domain.control_domain_id)) {
      relations.runtime_units.insert(domain.runtime_unit_ids.begin(),
                                     domain.runtime_unit_ids.end());
    }
  }
  // A component depends on every runtime unit its units depend on.
  bool grew = true;
  while (grew) {
    grew = false;
    for (const auto &unit : graph.runtime_units) {
      if (!relations.runtime_units.contains(unit.runtime_unit_id)) continue;
      for (const auto &dependency : unit.dependency_runtime_unit_ids) {
        grew = relations.runtime_units.insert(dependency).second || grew;
      }
    }
  }
  for (const auto &provider : bundle.capability_provider_bindings) {
    const bool targets_component = std::visit(
        [&](const auto &target) {
          using Target = std::decay_t<decltype(target)>;
          if constexpr (std::is_same_v<Target,
                                       core::CellGlobalCapabilityProviderTargetDTO>) {
            return true;
          } else if constexpr (std::is_same_v<Target, core::ComponentTargetCapabilityProviderTargetDTO>) {
            return target.controllable_component_id == component.controllable_component_id;
          } else if constexpr (std::is_same_v<
                                   Target,
                                   core::TargetBindingCapabilityProviderTargetDTO>) {
            return relations.bindings.contains(target.control_binding_id);
          } else {
            static_assert(std::is_same_v<
                          Target, core::ControlDomainCapabilityProviderTargetDTO>);
            return relations.domains.contains(target.control_domain_id);
          }
        },
        provider.target);
    if (targets_component) {
      relations.providers.insert(provider.capability_provider_binding_id);
    }
  }
  return relations;
}

} // namespace m6b

[[nodiscard]] core::ControllableComponentDTO
MountableComponent(std::string id, std::optional<std::string> model) {
  std::vector<core::ModelRoleBindingDTO> roles;
  if (model) roles.push_back({core::ModelRoleId("body"), core::ModelInstanceId(*model)});
  return {core::ControllableComponentId(std::move(id)),
          core::ComponentTypeId("partial_apply_tool"),
          core::ComponentTypeRevision("partial_apply_tool-r1"),
          std::move(roles),
          {},
          std::nullopt};
}

[[nodiscard]] core::ComponentAttachmentEdgeDTO
EdgeOn(const core::ControllableComponentId &parent, std::string interface_id,
       std::string child) {
  return {core::ComponentAttachmentId(parent.value() + "." + interface_id + "->" + child),
          core::ComponentAttachmentRevision("attachment-r1"),
          {parent, core::AttachmentInterfaceId(std::move(interface_id))},
          {core::ControllableComponentId(std::move(child)),
           core::AttachmentInterfaceId("mount")},
          core::PlacementPoseDTO{}};
}

/// One Apply side: the two retained ROS arms plus synthetic tools, a
/// model-free tool and an unaffected station, mounted as `attachments` say.
/// Only the typed relations read it; it is never verified or sealed.
[[nodiscard]] core::RetainedBundleDTO
PartialApplySide(std::vector<core::ComponentAttachmentEdgeDTO> attachments) {
  auto bundle = RosTwoArms().value();
  auto &components = bundle.cell_graph.controllable_components;
  components.push_back(MountableComponent("zz_tool", "zz_tool_model"));
  components.push_back(MountableComponent("aa_camera", "aa_camera_model"));
  components.push_back(MountableComponent("bare_tool", std::nullopt));
  components.push_back(MountableComponent("station", "station_model"));
  bundle.cell_graph.component_attachments = std::move(attachments);
  return bundle;
}

// D17 (M7 part A): each Apply side derives its relations from an index of its
// own graph. A mount added, removed or changed, in both directions, and an
// unaffected component: both sides equal the merge-6b relations exactly.
TEST(ApplyComponentDelta, PartialApplyRelationsPerSideEqualMerge6b) {
  const auto first = FirstArm();
  const auto second = SecondArm();
  using Edges = std::vector<core::ComponentAttachmentEdgeDTO>;
  const Edges none;
  const Edges tool_on_first{EdgeOn(first, "flange", "zz_tool")};
  const Edges tool_on_second{EdgeOn(second, "flange", "zz_tool")};
  const Edges tool_on_first_other_interface{EdgeOn(first, "tool_flange", "zz_tool")};
  // Sibling edges in non-id order, and the same mounts in id order.
  const Edges siblings{EdgeOn(first, "flange", "zz_tool"),
                       EdgeOn(first, "flange", "aa_camera")};
  const Edges siblings_id_order{EdgeOn(first, "flange", "aa_camera"),
                                EdgeOn(first, "flange", "zz_tool")};
  const Edges chained{EdgeOn(first, "flange", "zz_tool"),
                      EdgeOn(core::ControllableComponentId("zz_tool"), "tool_flange",
                             "aa_camera")};
  const Edges model_free_tool{EdgeOn(first, "flange", "bare_tool")};
  const Edges arm_on_arm{EdgeOn(first, "flange", second.value())};
  const Edges missing_child{EdgeOn(first, "flange", "missing_tool")};

  struct Case final {
    const char *name;
    const Edges &predecessor;
    const Edges &candidate;
    bool first_arm_changes;
  };
  const std::vector<Case> cases{
      {"mount added", none, tool_on_first, true},
      {"mount removed", tool_on_first, none, true},
      {"mount moved to another arm", tool_on_first, tool_on_second, true},
      {"mount moved back", tool_on_second, tool_on_first, true},
      {"mount interface changed", tool_on_first, tool_on_first_other_interface, false},
      {"mount interface changed back", tool_on_first_other_interface, tool_on_first, false},
      {"second tool added", tool_on_first, siblings, true},
      {"second tool removed", siblings, tool_on_first, true},
      {"sibling edge order changed", siblings, siblings_id_order, false},
      {"chained tool added", tool_on_first, chained, false},
      {"chained tool removed", chained, tool_on_first, false},
      {"model-free tool added", none, model_free_tool, false},
      {"model-free tool removed", model_free_tool, none, false},
      {"arm mounted on arm", none, arm_on_arm, true},
      {"arm unmounted from arm", arm_on_arm, none, true},
      {"mount to a missing component row", tool_on_first, missing_child, true},
      {"mount from a missing component row", missing_child, tool_on_first, true},
  };
  for (const auto &test_case : cases) {
    SCOPED_TRACE(test_case.name);
    const auto predecessor = PartialApplySide(test_case.predecessor);
    const auto candidate = PartialApplySide(test_case.candidate);
    // One index per side, as derive_apply_component_delta builds them.
    const CellKinematicIndex predecessor_index(predecessor.cell_graph);
    const CellKinematicIndex candidate_index(candidate.cell_graph);
    for (const auto *side : {&predecessor, &candidate}) {
      const auto &index = side == &predecessor ? predecessor_index : candidate_index;
      for (const auto &component : side->cell_graph.controllable_components) {
        SCOPED_TRACE(component.controllable_component_id.value());
        EXPECT_EQ(detail::apply_component_relations(*side, index, component),
                  m6b::Relations(*side, component));
      }
    }
    const auto relations = [&](const core::RetainedBundleDTO &side,
                               const CellKinematicIndex &index,
                               const std::string &id) {
      const auto *component = m6b::FindComponent(side, core::ControllableComponentId(id));
      if (component == nullptr) throw std::logic_error("partial Apply side lacks " + id);
      return detail::apply_component_relations(side, index, *component);
    };
    // The unaffected component keeps its relations across the Apply.
    EXPECT_EQ(relations(predecessor, predecessor_index, "station"),
              relations(candidate, candidate_index, "station"));
    EXPECT_EQ(relations(predecessor, predecessor_index, first.value()) !=
                  relations(candidate, candidate_index, first.value()),
              test_case.first_arm_changes);
  }
}

TEST(ApplyComponentDelta,
     MovingTakeoverBudgetChangesEveryComponentPolicyIdentity) {
  const auto with_budget = [](const RetainedBundle &source, double budget_s,
                              std::optional<double> planning_s = std::nullopt) {
    auto root = source.normalized_root().as_object();
    root.erase(std::remove_if(
                   root.begin(), root.end(),
                   [](const auto &row) {
                     return row.first ==
                                "motion_takeover_preparation_budget_s" ||
                            row.first == "motion_takeover_planning_budget_s";
                   }),
               root.end());
    root.emplace_back("motion_takeover_preparation_budget_s",
                      CanonicalValue(budget_s));
    if (planning_s)
      root.emplace_back("motion_takeover_planning_budget_s",
                        CanonicalValue(*planning_s));
    auto projection = CanonicalValue(std::move(root));
    const auto identity = detail::compute_content_identity(
        projection,
        detail::ContentIdentitySpec{
            .identity_domain = "retained-bundle-id",
            .api_version = std::string(core::kRetainedBundleApiVersion),
            .own_field = "retained_bundle_id"});
    auto sealed = projection.as_object();
    for (auto &[name, value] : sealed) {
      if (name == "retained_bundle_id")
        value = CanonicalValue(identity);
    }
    return detail::RetainedBundleReloadAuthority::reload(
        canonical_json(CanonicalValue(std::move(sealed))));
  };
  const auto selected = with_budget(RosTwoArms(), 3.5);
  const auto changed = with_budget(*selected, 7.0);
  const auto planning = with_budget(*selected, 3.5, 1.25);
  const auto changed_planning = with_budget(*planning, 3.5, 2.0);
  for (const auto &delta :
       {derive_apply_component_delta(RosTwoArms(), *selected),
        derive_apply_component_delta(*selected, *changed),
        derive_apply_component_delta(*selected, RosTwoArms()),
        derive_apply_component_delta(*selected, *planning),
        derive_apply_component_delta(*planning, *changed_planning),
        derive_apply_component_delta(*planning, *selected)}) {
    ASSERT_TRUE(delta.valid());
    ASSERT_EQ(delta.components.size(), 2U);
    for (const auto &row : delta.components) {
      EXPECT_EQ(row.disposition, ApplyComponentDisposition::kReplace);
      EXPECT_EQ(row.replacement_reasons,
                (std::vector<std::string>{
                    std::string(kApplyComponentReasonRetainedPolicies)}));
      EXPECT_NE(row.predecessor_runtime_identity,
                row.candidate_runtime_identity);
    }
  }
  const auto unchanged = derive_apply_component_delta(*selected, *selected);
  for (const auto &row : unchanged.components) {
    EXPECT_EQ(row.disposition, ApplyComponentDisposition::kRetain);
  }
}

} // namespace
} // namespace manyforge::deployment
