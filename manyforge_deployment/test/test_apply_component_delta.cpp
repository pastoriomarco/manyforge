#include <gtest/gtest.h>

#include <manyforge_deployment/apply_affected_scope.hpp>
#include <manyforge_deployment/apply_component_delta.hpp>
#include <manyforge_deployment/native_execution_runtime_contract.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

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

TEST(ApplyComponentDelta, UnchangedBundleRetainsEveryComponentRuntime) {
  const auto delta = derive_apply_component_delta(TwoArms(), TwoArms());
  ASSERT_TRUE(delta.valid());
  ASSERT_EQ(delta.components.size(), 2U);
  for (const auto &row : delta.components) {
    EXPECT_EQ(row.disposition, ApplyComponentDisposition::kRetain);
    EXPECT_EQ(row.predecessor_runtime_identity, row.candidate_runtime_identity);
    EXPECT_EQ(row.candidate_runtime_identity,
              derive_component_runtime_identity(TwoArms(),
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
  const auto first = derive_component_runtime_identity(TwoArms(), FirstArm());
  const auto second = derive_component_runtime_identity(TwoArms(), SecondArm());
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_NE(*first, *second);
  EXPECT_EQ(derive_component_runtime_identity(TwoArms(), FirstArm()), first);
  EXPECT_FALSE(derive_component_runtime_identity(
                   OneArm(), SecondArm())
                   .has_value());
}

TEST(ApplyComponentDelta, NativeConfigurationSelectionKeepsExactComponentSettings) {
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
      test::kLite6ArmTargetId, std::nullopt, std::nullopt, "", "lite6_grf_ros2",
      false);
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

TEST(ApplyComponentDelta, CellWorldChangeReplacesIsaacAndRetainsMockPeer) {
  const auto materialize = [](std::string_view world) {
    return Materialize(test::make_lite6_controllable_retention_fixture(
        false, false, 2U, false, true, kFixtureCellId, false, false, "resource",
        false, {}, {}, true, false, false, std::nullopt, false,
        test::kLite6ArmTargetId, std::nullopt, std::nullopt, "", world, true));
  };
  const auto first = materialize("lite6_grf_ros2");
  const auto next = materialize("test_scene_realsense_foundationpose_trocar");
  const auto delta = derive_apply_component_delta(*first, *next);
  EXPECT_EQ(Row(delta, FirstArm()).disposition,
            ApplyComponentDisposition::kReplace);
  EXPECT_EQ(Row(delta, SecondArm()).disposition,
            ApplyComponentDisposition::kRetain);
  const auto &unit = next->value().cell_graph.runtime_units.front();
  EXPECT_EQ(
      ros2_control_execution_runtime_contract::require_component_configuration(
          *next, unit.runtime_unit_id, FirstArm())
          .isaac_world,
      "test_scene_realsense_foundationpose_trocar");
  EXPECT_FALSE(
      ros2_control_execution_runtime_contract::require_component_configuration(
          *next, unit.runtime_unit_id, SecondArm())
          .isaac_world.has_value());
}

TEST(ApplyComponentDelta, AddedAndRemovedComponentsAreClassifiedAndFenced) {
  const auto grow = derive_apply_component_delta(OneArm(), TwoArms());
  ASSERT_TRUE(grow.valid());
  ASSERT_EQ(grow.components.size(), 2U);
  const auto added = Row(grow, SecondArm());
  EXPECT_EQ(added.disposition, ApplyComponentDisposition::kAdd);
  EXPECT_FALSE(added.predecessor_runtime_identity.has_value());
  EXPECT_EQ(added.candidate_runtime_identity,
            derive_component_runtime_identity(TwoArms(), SecondArm()));
  EXPECT_TRUE(added.predecessor_domains.empty());
  EXPECT_FALSE(added.candidate_domains.empty());
  // Adding an independent arm changes shared projections, not this arm's
  // controller configuration. Its actual producer identities must stay equal.
  const auto kept = Row(grow, FirstArm());
  EXPECT_EQ(kept.predecessor_runtime_identity,
            derive_component_runtime_identity(OneArm(), FirstArm()));
  EXPECT_EQ(kept.candidate_runtime_identity,
            derive_component_runtime_identity(TwoArms(), FirstArm()));
  EXPECT_EQ(kept.disposition, ApplyComponentDisposition::kRetain);
  EXPECT_EQ(kept.predecessor_runtime_identity, kept.candidate_runtime_identity);
  EXPECT_TRUE(kept.replacement_reasons.empty());
  EXPECT_TRUE(apply_component_delta_is_fenced(
      grow, derive_apply_configuration_fence_scope(OneArm(), TwoArms())));

  const auto shrink = derive_apply_component_delta(TwoArms(), OneArm());
  ASSERT_TRUE(shrink.valid());
  EXPECT_EQ(Row(shrink, FirstArm()).disposition,
            ApplyComponentDisposition::kRetain);
  const auto removed = Row(shrink, SecondArm());
  EXPECT_EQ(removed.disposition, ApplyComponentDisposition::kRemove);
  EXPECT_EQ(removed.predecessor_runtime_identity,
            derive_component_runtime_identity(TwoArms(), SecondArm()));
  EXPECT_FALSE(removed.candidate_runtime_identity.has_value());
  EXPECT_TRUE(removed.candidate_domains.empty());
  EXPECT_TRUE(apply_component_delta_is_fenced(
      shrink, derive_apply_configuration_fence_scope(TwoArms(), OneArm())));
}

TEST(ApplyComponentDelta, CodecAndDigestAreCanonicalAndExact) {
  const auto delta = derive_apply_component_delta(OneArm(), TwoArms());
  const auto encoded = EncodeApplyComponentDelta(delta);
  EXPECT_EQ(DecodeApplyComponentDelta(encoded), delta);
  EXPECT_EQ(EncodeApplyComponentDelta(DecodeApplyComponentDelta(encoded)),
            encoded);
  EXPECT_EQ(DigestApplyComponentDelta(delta), DigestApplyComponentDelta(delta));
  EXPECT_NE(DigestApplyComponentDelta(delta),
            DigestApplyComponentDelta(
                derive_apply_component_delta(TwoArms(), OneArm())));

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

} // namespace
} // namespace manyforge::deployment
