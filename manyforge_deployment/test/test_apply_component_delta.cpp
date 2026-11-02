#include <gtest/gtest.h>

#include <manyforge_deployment/apply_affected_scope.hpp>
#include <manyforge_deployment/apply_component_delta.hpp>
#include <manyforge_deployment/cell_kinematic_index.hpp>
#include <manyforge_deployment/component_spec.hpp>
#include <manyforge_deployment/deployment_applicability_codec.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "activation_deployment_compiler_internal.hpp"
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
/// The fixtures run no Real component: no site facts.
const SiteFacts kNoSite{};

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
    change.replacement_reasons = {std::string(kComponentSpecBindings)};
  }
  return change;
}

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
  const auto delta = derive_apply_component_delta(RosTwoArms(), RosTwoArms(), kNoSite);
  ASSERT_TRUE(delta.valid());
  ASSERT_EQ(delta.components.size(), 2U);
  for (const auto &row : delta.components) {
    EXPECT_EQ(row.disposition, ApplyComponentDisposition::kRetain);
    EXPECT_EQ(row.predecessor_runtime_identity, row.candidate_runtime_identity);
    EXPECT_EQ(row.candidate_runtime_identity,
              derive_component_runtime_identity(RosTwoArms(), kNoSite,
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
  const auto first = derive_component_runtime_identity(RosTwoArms(), kNoSite, FirstArm());
  const auto second = derive_component_runtime_identity(RosTwoArms(), kNoSite, SecondArm());
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_NE(*first, *second);
  EXPECT_EQ(derive_component_runtime_identity(RosTwoArms(), kNoSite, FirstArm()), first);
  EXPECT_FALSE(derive_component_runtime_identity(
                   RosOneArm(), kNoSite, SecondArm())
                   .has_value());
}

TEST(ApplyComponentDelta, RosConfigurationSelectionUsesExactBindingResources) {
  const auto fixture = test::make_lite6_controllable_retention_fixture(
      false, false, 1U, kFixtureCellId, false, false,
      "resource", false, {}, {});
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
      false, false, count, kFixtureCellId, false, false,
      "resource", false, {}, {}, false, false, first_position,
      test::kLite6ArmTargetId, std::nullopt, std::nullopt, first_prefix));
}

TEST(ApplyComponentDelta, RosSharedHostRetainsUnrelatedRobotAndOwnChangesReplace) {
  const auto one = RosArms(1U);
  const auto two = RosArms(2U);
  const auto changed_positions = RosArms(2U, 0.25);
  const auto changed_prefix = RosArms(2U, std::nullopt, "own/");
  EXPECT_EQ(Row(derive_apply_component_delta(*one, *two, kNoSite), FirstArm()).disposition,
            ApplyComponentDisposition::kRetain);
  EXPECT_EQ(Row(derive_apply_component_delta(*two, *one, kNoSite), FirstArm()).disposition,
            ApplyComponentDisposition::kRetain);
  for (const auto &changed : {changed_positions, changed_prefix}) {
    const auto delta = derive_apply_component_delta(*two, *changed, kNoSite);
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
      false, false, 2U, kFixtureCellId, false, false, "resource",
      false, {}, {}, false, false, std::nullopt,
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

TEST(ApplyComponentDelta, EmptyRosHostHasNoFakeComponents) {
  const auto catalog = test::lite6_controllable_fixture_detail::qualify_control_catalog(
      false, false, "resource", false, {});
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
      false, false, 0U, kFixtureCellId, false, false, "resource",
      false, {}, {}, false, false, std::nullopt,
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
      false, false, 1U, kFixtureCellId, false, false, "resource",
      false, {}, {});
  const auto valid_bundle = Materialize(valid_fixture);
  const auto &graph = valid_bundle->value().cell_graph;
  for (const std::string_view field : {"controller_profiles",
                                       "initial_joint_positions_rad",
                                       "motion_target_limits"}) {
    SCOPED_TRACE(field);
    EXPECT_THROW(
        static_cast<void>(test::make_lite6_controllable_retention_fixture(
            false, false, 1U, kFixtureCellId, false, false, "resource",
            false, {}, {}, false, false, std::nullopt,
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
  const auto grow = derive_apply_component_delta(RosOneArm(), RosTwoArms(), kNoSite);
  ASSERT_TRUE(grow.valid());
  ASSERT_EQ(grow.components.size(), 2U);
  const auto added = Row(grow, SecondArm());
  EXPECT_EQ(added.disposition, ApplyComponentDisposition::kAdd);
  EXPECT_FALSE(added.predecessor_runtime_identity.has_value());
  EXPECT_EQ(added.candidate_runtime_identity,
            derive_component_runtime_identity(RosTwoArms(), kNoSite, SecondArm()));
  EXPECT_TRUE(added.predecessor_domains.empty());
  EXPECT_FALSE(added.candidate_domains.empty());
  // Adding an independent arm changes shared projections, not this arm's
  // controller configuration. Its actual producer identities must stay equal.
  const auto kept = Row(grow, FirstArm());
  EXPECT_EQ(kept.predecessor_runtime_identity,
            derive_component_runtime_identity(RosOneArm(), kNoSite, FirstArm()));
  EXPECT_EQ(kept.candidate_runtime_identity,
            derive_component_runtime_identity(RosTwoArms(), kNoSite, FirstArm()));
  EXPECT_EQ(kept.disposition, ApplyComponentDisposition::kRetain);
  EXPECT_EQ(kept.predecessor_runtime_identity, kept.candidate_runtime_identity);
  EXPECT_TRUE(kept.replacement_reasons.empty());
  EXPECT_TRUE(apply_component_delta_is_fenced(
      grow, derive_apply_configuration_fence_scope(RosOneArm(), RosTwoArms())));

  const auto shrink = derive_apply_component_delta(RosTwoArms(), RosOneArm(), kNoSite);
  ASSERT_TRUE(shrink.valid());
  EXPECT_EQ(Row(shrink, FirstArm()).disposition,
            ApplyComponentDisposition::kRetain);
  const auto removed = Row(shrink, SecondArm());
  EXPECT_EQ(removed.disposition, ApplyComponentDisposition::kRemove);
  EXPECT_EQ(removed.predecessor_runtime_identity,
            derive_component_runtime_identity(RosTwoArms(), kNoSite, SecondArm()));
  EXPECT_FALSE(removed.candidate_runtime_identity.has_value());
  EXPECT_TRUE(removed.candidate_domains.empty());
  EXPECT_TRUE(apply_component_delta_is_fenced(
      shrink, derive_apply_configuration_fence_scope(RosTwoArms(), RosOneArm())));
}

TEST(ApplyComponentDelta, CodecAndDigestAreCanonicalAndExact) {
  const auto delta = derive_apply_component_delta(RosOneArm(), RosTwoArms(), kNoSite);
  const auto encoded = EncodeApplyComponentDelta(delta);
  EXPECT_EQ(DecodeApplyComponentDelta(encoded), delta);
  EXPECT_EQ(EncodeApplyComponentDelta(DecodeApplyComponentDelta(encoded)),
            encoded);
  EXPECT_EQ(DigestApplyComponentDelta(delta), DigestApplyComponentDelta(delta));
  EXPECT_NE(DigestApplyComponentDelta(delta),
            DigestApplyComponentDelta(
                derive_apply_component_delta(RosTwoArms(), RosOneArm(), kNoSite)));

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

// Build spec plan: the takeover budgets are CellContext. A policy-only Apply
// retains every component (each takes the new budgets on retain).
/// `root` (a retained bundle's normalized root, edited) sealed under its new
/// identity and reloaded through the verifier.
[[nodiscard]] std::shared_ptr<const RetainedBundle>
Resealed(CanonicalValue::Object root) {
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
}

TEST(ApplyComponentDelta,
     MovingTakeoverBudgetEditRetainsEveryComponent) {
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
    return Resealed(std::move(root));
  };
  const auto selected = with_budget(RosTwoArms(), 3.5);
  const auto changed = with_budget(*selected, 7.0);
  const auto planning = with_budget(*selected, 3.5, 1.25);
  const auto changed_planning = with_budget(*planning, 3.5, 2.0);
  for (const auto &delta :
       {derive_apply_component_delta(RosTwoArms(), *selected, kNoSite),
        derive_apply_component_delta(*selected, *changed, kNoSite),
        derive_apply_component_delta(*selected, RosTwoArms(), kNoSite),
        derive_apply_component_delta(*selected, *planning, kNoSite),
        derive_apply_component_delta(*planning, *changed_planning, kNoSite),
        derive_apply_component_delta(*planning, *selected, kNoSite)}) {
    ASSERT_TRUE(delta.valid());
    ASSERT_EQ(delta.components.size(), 2U);
    for (const auto &row : delta.components) {
      EXPECT_EQ(row.disposition, ApplyComponentDisposition::kRetain);
      EXPECT_TRUE(row.replacement_reasons.empty());
      EXPECT_EQ(row.predecessor_runtime_identity,
                row.candidate_runtime_identity);
    }
    EXPECT_TRUE(apply_component_delta_is_fenced(delta, {}));
  }
  EXPECT_NE(cell_context(*selected).value(), cell_context(*changed).value());
  EXPECT_NE(cell_context(*planning).value(), cell_context(*changed_planning).value());
}

// N17 R4: an arm's in-motion staleness budget (its Kernel participant's
// `controller_state_max_age_ns`) is its effective `measured_joints` budget,
// which a `policies.component_streams` row overrides. The host bakes it into
// the arm at construction, so an edit of the arm's own override must rebuild
// that arm, fenced, and leave every other component retained.
TEST(ApplyComponentDelta, OwnMeasuredJointsOverrideEditReachesThatArmsInMotionBudget) {
  const auto with_override = [](const RetainedBundle &source,
                                const core::ControllableComponentId &component,
                                const std::int64_t budget_ns) {
    auto root = source.normalized_root().as_object();
    root.erase(std::remove_if(root.begin(), root.end(),
                              [](const auto &row) { return row.first == "component_streams"; }),
               root.end());
    root.emplace_back(
        "component_streams",
        encode_component_stream_overrides({core::StreamParameterOverrideDTO{
            component,
            std::string(ros2_control_execution_runtime_contract::kMeasuredJointsStreamId),
            budget_ns, std::nullopt, std::nullopt}}));
    return Resealed(std::move(root));
  };
  const auto before = with_override(RosTwoArms(), FirstArm(), 300'000'000);
  const auto after = with_override(*before, FirstArm(), 150'000'000);
  ASSERT_EQ(ComponentStreamParameterOverrides(*after).front().budget_ns, 150'000'000);

  const auto delta = derive_apply_component_delta(*before, *after, kNoSite);
  ASSERT_TRUE(delta.valid());
  const auto first_arm = FirstArm();
  const auto &edited = Row(delta, first_arm);
  // Its own override rows are its spec `adapter` configuration.
  EXPECT_EQ(edited.disposition, ApplyComponentDisposition::kReplace);
  EXPECT_EQ(edited.replacement_reasons,
            (std::vector<std::string>{std::string(kComponentSpecAdapter)}));
  // The other arm's budget did not change: it is retained either way.
  EXPECT_EQ(Row(delta, SecondArm()).disposition, ApplyComponentDisposition::kRetain);
  EXPECT_TRUE(apply_component_delta_is_fenced(
      delta, derive_apply_configuration_fence_scope(*before, *after)));
  // N17 S4: the host builds the arm's budget from its spec's own override.
  const auto spec = component_spec(*after, kNoSite, first_arm);
  ASSERT_EQ(spec.stream_overrides.size(), 1U);
  EXPECT_EQ(spec.stream_overrides.front().budget_ns, 150'000'000);
  EXPECT_TRUE(component_spec(*after, kNoSite, SecondArm()).stream_overrides.empty());
}

// -- CONTRACTS §11: one Draft -> Apply path for every structural change ------

/// Two ROS arms on the shared host, as RosTwoArms(), with the first arm's lane
/// and graph optionally changed the way a Draft edit changes them.
[[nodiscard]] std::shared_ptr<const RetainedBundle> RosTwoArmsEdited(
    const bool first_arm_on_mock,
    void (*graph_edit)(CanonicalValue::Object &) = nullptr) {
  return Materialize(test::make_lite6_controllable_retention_fixture(
      false, false, 2U, kFixtureCellId, false, false, "resource", false,
      {}, {}, false, false, std::nullopt, test::kLite6ArmTargetId,
      std::nullopt, std::nullopt, {}, true, {}, "lite6_arm_provider", false,
      first_arm_on_mock, nullptr, graph_edit));
}

void MoveFirstArmBase(CanonicalValue::Object &graph) {
  for (auto &[name, value] : graph) {
    if (name != "controllable_components") continue;
    auto components = value.as_array();
    for (auto &component : components) {
      if (component.find_object_member("controllable_component_id")->as_string() !=
          FirstArm().value())
        continue;
      auto fields = component.as_object();
      for (auto &[field, pose] : fields) {
        if (field == "root_pose_in_cell")
          pose = test::lite6_controllable_fixture_detail::translated_pose(0.5);
      }
      component = CanonicalValue(std::move(fields));
    }
    value = CanonicalValue(std::move(components));
  }
}

[[nodiscard]] bool HasReason(const ApplyComponentChangeDTO &row,
                             const std::string_view reason) {
  return std::find(row.replacement_reasons.begin(), row.replacement_reasons.end(),
                   reason) != row.replacement_reasons.end();
}

// CONTRACTS §16: make the second arm a member of the first (its controller
// profile names the owner and a role), as the compiler writes a machine's
// members.
void SecondArmMemberOfFirstAs(CanonicalValue::Object &graph, const std::string &role);

void SecondArmMemberOfFirst(CanonicalValue::Object &graph) {
  SecondArmMemberOfFirstAs(graph, "tool_io");
}

void SecondArmMemberOfFirstAs(CanonicalValue::Object &graph, const std::string &role) {
  for (auto &[name, value] : graph) {
    if (name != "runtime_units") continue;
    auto units = value.as_array();
    for (auto &unit : units) {
      auto unit_fields = unit.as_object();
      for (auto &[field, configuration] : unit_fields) {
        if (field != "configuration") continue;
        auto rows = configuration.as_object();
        for (auto &[row_name, profiles] : rows) {
          if (row_name != "controller_profiles") continue;
          auto items = profiles.as_array();
          for (auto &item : items) {
            if (item.find_object_member("controllable_component_id")->as_string() !=
                SecondArm().value())
              continue;
            auto profile = item.as_object();
            profile.emplace_back("owner_component_id",
                                 CanonicalValue(FirstArm().value()));
            profile.emplace_back("member_role", CanonicalValue(role));
            item = CanonicalValue(std::move(profile));
          }
          profiles = CanonicalValue(std::move(items));
        }
        configuration = CanonicalValue(std::move(rows));
      }
      unit = CanonicalValue(std::move(unit_fields));
    }
    value = CanonicalValue(std::move(units));
  }
}

TEST(ApplyComponentDelta, ReplacingAnOwnerReplacesItsMembersWithTheOwnerReason) {
  const auto members = RosTwoArmsEdited(false, &SecondArmMemberOfFirst);
  const auto owner_on_mock = RosTwoArmsEdited(true, &SecondArmMemberOfFirst);
  const auto delta = derive_apply_component_delta(*members, *owner_on_mock, kNoSite);
  ASSERT_TRUE(delta.valid());
  const auto owner_id = FirstArm();
  const auto &owner = Row(delta, owner_id);
  EXPECT_EQ(owner.disposition, ApplyComponentDisposition::kReplace);
  EXPECT_FALSE(HasReason(owner, kComponentSpecOwner));
  const auto member_id = SecondArm();
  const auto &member = Row(delta, member_id);
  EXPECT_EQ(member.disposition, ApplyComponentDisposition::kReplace);
  EXPECT_EQ(member.replacement_reasons,
            (std::vector<std::string>{std::string(kComponentSpecOwner)}));
  // Without the relation the peer is retained (as before).
  EXPECT_EQ(Row(derive_apply_component_delta(RosTwoArms(), *RosTwoArmsEdited(true), kNoSite),
                SecondArm())
                .disposition,
            ApplyComponentDisposition::kRetain);
  // An unchanged machine retains both.
  const auto same = derive_apply_component_delta(*members, *members, kNoSite);
  for (const auto &row : same.components)
    EXPECT_EQ(row.disposition, ApplyComponentDisposition::kRetain);
}

// CONTRACTS §11, §16: the second arm as the first's `arm` member.
void SecondArmIsTheArmOfFirst(CanonicalValue::Object &graph) {
  SecondArmMemberOfFirstAs(graph, "arm");
}

// The same machine with a custom TCP (a runtime-editable arm field) on the
// arm member's model occurrence.
void SecondArmIsTheArmOfFirstWithACustomTcp(CanonicalValue::Object &graph) {
  SecondArmIsTheArmOfFirst(graph);
  std::string model_instance;
  for (const auto &[name, value] : graph) {
    if (name != "controllable_components") continue;
    for (const auto &component : value.as_array())
      if (component.find_object_member("controllable_component_id")->as_string() ==
          SecondArm().value())
        model_instance = component.find_object_member("model_role_bindings")
                             ->as_array()
                             .front()
                             .find_object_member("model_instance_id")
                             ->as_string();
  }
  for (auto &[name, value] : graph) {
    if (name != "model_instances") continue;
    auto instances = value.as_array();
    for (auto &instance : instances) {
      if (instance.find_object_member("model_instance_id")->as_string() != model_instance)
        continue;
      auto fields = instance.as_object();
      fields.emplace_back(
          "custom_tcps",
          CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
              {"frame_role_id", CanonicalValue("pen")},
              {"parent_link_name", CanonicalValue("link_eef")},
              {"pose_in_parent_link",
               test::lite6_controllable_fixture_detail::translated_pose(0.12)}})}));
      instance = CanonicalValue(std::move(fields));
    }
    value = CanonicalValue(std::move(instances));
  }
}

// The owner's identity holds what its process is launched with from its arm:
// a custom TCP on the arm is a static frame of the launched description, so
// it replaces the machine (and, through it, the arm).
TEST(ApplyComponentDelta, AnArmCustomTcpReplacesItsMachine) {
  const auto machine = RosTwoArmsEdited(false, &SecondArmIsTheArmOfFirst);
  const auto edited = RosTwoArmsEdited(false, &SecondArmIsTheArmOfFirstWithACustomTcp);
  const auto delta = derive_apply_component_delta(*machine, *edited, kNoSite);
  ASSERT_TRUE(delta.valid());
  const auto owner_id = FirstArm();
  const auto &owner = Row(delta, owner_id);
  EXPECT_EQ(owner.disposition, ApplyComponentDisposition::kReplace);
  EXPECT_TRUE(HasReason(owner, kComponentSpecDescription));
  const auto arm_id = SecondArm();
  const auto &arm = Row(delta, arm_id);
  EXPECT_EQ(arm.disposition, ApplyComponentDisposition::kReplace);
  EXPECT_TRUE(HasReason(arm, kComponentSpecDescription));
  EXPECT_TRUE(HasReason(arm, kComponentSpecOwner));
}

// The same machine with the arm member's initial joint positions edited (a
// launch row of the arm only; the host joins every motion target's rows in
// cell order, the second arm's after the first's).
void SecondArmIsTheArmOfFirstWithEditedInitialPositions(CanonicalValue::Object &graph) {
  SecondArmIsTheArmOfFirst(graph);
  for (auto &[name, value] : graph) {
    if (name != "runtime_units") continue;
    auto units = value.as_array();
    for (auto &unit : units) {
      auto unit_fields = unit.as_object();
      for (auto &[field, configuration] : unit_fields) {
        if (field != "configuration") continue;
        auto rows = configuration.as_object();
        for (auto &[row_name, positions] : rows) {
          if (row_name != "initial_joint_positions_rad") continue;
          auto joints = positions.as_array();
          joints.at(6U) = CanonicalValue(0.3);
          positions = CanonicalValue(std::move(joints));
        }
        configuration = CanonicalValue(std::move(rows));
      }
      unit = CanonicalValue(std::move(unit_fields));
    }
    value = CanonicalValue(std::move(units));
  }
}

[[nodiscard]] std::shared_ptr<const RetainedBundle> RosThreeArmsEdited(
    void (*graph_edit)(CanonicalValue::Object &)) {
  return Materialize(test::make_lite6_controllable_retention_fixture(
      false, false, 3U, kFixtureCellId, false, false, "resource", false,
      {}, {}, false, false, std::nullopt, test::kLite6ArmTargetId,
      std::nullopt, std::nullopt, {}, true, {}, "lite6_arm_provider", false,
      false, nullptr, graph_edit));
}

// The host rebuilds a machine's owner with its arm member (the owner's
// process is the arm's launch), so an arm-only launch edit replaces the owner
// too (and the owner's members through it); other components are retained.
TEST(ApplyComponentDelta, AnArmLaunchEditReplacesItsMachine) {
  const auto machine = RosThreeArmsEdited(&SecondArmIsTheArmOfFirst);
  const auto edited = RosThreeArmsEdited(&SecondArmIsTheArmOfFirstWithEditedInitialPositions);
  const auto delta = derive_apply_component_delta(*machine, *edited, kNoSite);
  ASSERT_TRUE(delta.valid());
  ASSERT_EQ(delta.components.size(), 3U);
  const auto owner_id = FirstArm();
  const auto &owner = Row(delta, owner_id);
  EXPECT_EQ(owner.disposition, ApplyComponentDisposition::kReplace);
  EXPECT_EQ(owner.replacement_reasons,
            (std::vector<std::string>{std::string(kComponentSpecLaunch)}));
  const auto arm_id = SecondArm();
  const auto &arm = Row(delta, arm_id);
  EXPECT_EQ(arm.disposition, ApplyComponentDisposition::kReplace);
  EXPECT_TRUE(HasReason(arm, kComponentSpecLaunch));
  EXPECT_TRUE(HasReason(arm, kComponentSpecOwner));
  const core::ControllableComponentId other(
      test::lite6_controllable_fixture_detail::indexed_id(
          test::kLite6ControllableComponentId, 2U));
  EXPECT_EQ(Row(delta, other).disposition, ApplyComponentDisposition::kRetain);
  // The arm's identity cites the owner's: no cycle, and an unchanged machine
  // retains both.
  for (const auto &row : derive_apply_component_delta(*edited, *edited, kNoSite).components)
    EXPECT_EQ(row.disposition, ApplyComponentDisposition::kRetain);
}

TEST(ApplyComponentDelta, SettingsChangeReplacesOnlyThatComponentWithItsReason) {
  const auto changed = RosArms(2U, 0.25);
  const auto delta = derive_apply_component_delta(RosTwoArms(), *changed, kNoSite);
  ASSERT_TRUE(delta.valid());
  const auto first_arm = FirstArm();
  const auto &first = Row(delta, first_arm);
  EXPECT_EQ(first.disposition, ApplyComponentDisposition::kReplace);
  EXPECT_TRUE(HasReason(first, kComponentSpecLaunch));
  EXPECT_EQ(Row(delta, SecondArm()).disposition, ApplyComponentDisposition::kRetain);
  EXPECT_TRUE(apply_component_delta_is_fenced(
      delta, derive_apply_configuration_fence_scope(RosTwoArms(), *changed)));
}

// The lane is component data on the execution host (its controller profile
// row), so a lane change replaces exactly that component, never its peer.
TEST(ApplyComponentDelta, LaneChangeReplacesThatComponentWithTheRuntimeReason) {
  const auto mock_first = RosTwoArmsEdited(true);
  for (const auto &delta : {derive_apply_component_delta(RosTwoArms(), *mock_first, kNoSite),
                            derive_apply_component_delta(*mock_first, RosTwoArms(), kNoSite)}) {
    ASSERT_TRUE(delta.valid());
    const auto first_arm = FirstArm();
    const auto &first = Row(delta, first_arm);
    EXPECT_EQ(first.disposition, ApplyComponentDisposition::kReplace);
    EXPECT_TRUE(HasReason(first, kComponentSpecLaunch));
    EXPECT_EQ(first.predecessor_domains, first.candidate_domains);
    EXPECT_EQ(Row(delta, SecondArm()).disposition, ApplyComponentDisposition::kRetain);
    EXPECT_TRUE(delta.attachment_changed_components.empty());
  }
}

// A base pose is shared Scene data: it moves the planning model, not the
// controller, so the component keeps running and the Scene is reprojected.
TEST(ApplyComponentDelta, BasePoseChangeRetainsTheComponentAndReprojectsTheScene) {
  const auto moved = RosTwoArmsEdited(false, &MoveFirstArmBase);
  const auto delta = derive_apply_component_delta(RosTwoArms(), *moved, kNoSite);
  ASSERT_TRUE(delta.valid());
  EXPECT_TRUE(delta.shared_scene_projection_changed);
  for (const auto &row : delta.components) {
    EXPECT_EQ(row.disposition, ApplyComponentDisposition::kRetain)
        << row.controllable_component_id.value();
  }
  EXPECT_TRUE(delta.attachment_changed_components.empty());
}

// Close and New are an ordinary Apply of the empty cell: the empty Deployment
// compiles like any other, every predecessor component is removed, and every
// removal is inside the Apply's fence.
TEST(ApplyComponentDelta, CloseIsAnApplyOfTheEmptyCellThatRemovesEveryComponent) {
  const auto empty = Materialize(test::make_lite6_controllable_retention_fixture(
      false, false, 0U, kFixtureCellId, false, false, "resource",
      false, {}, {}, false, false, std::nullopt, test::kLite6ArmTargetId));
  ASSERT_TRUE(empty->value().cell_graph.controllable_components.empty());
  const auto close = derive_apply_component_delta(RosTwoArms(), *empty, kNoSite);
  ASSERT_TRUE(close.valid());
  ASSERT_EQ(close.components.size(), 2U);
  for (const auto &row : close.components) {
    EXPECT_EQ(row.disposition, ApplyComponentDisposition::kRemove)
        << row.controllable_component_id.value();
    EXPECT_EQ(row.predecessor_runtime_identity,
              derive_component_runtime_identity(RosTwoArms(), kNoSite,
                                                row.controllable_component_id));
    EXPECT_FALSE(row.candidate_runtime_identity.has_value());
  }
  EXPECT_TRUE(close.shared_scene_projection_changed);
  EXPECT_TRUE(apply_component_delta_is_fenced(
      close, derive_apply_configuration_fence_scope(RosTwoArms(), *empty)));
  // New after Close, and a Close of an empty cell, change no component.
  EXPECT_TRUE(derive_apply_component_delta(*empty, *empty, kNoSite).components.empty());
  // Load after Close adds every component of the file.
  for (const auto &row : derive_apply_component_delta(*empty, RosTwoArms(), kNoSite).components) {
    EXPECT_EQ(row.disposition, ApplyComponentDisposition::kAdd);
  }
}

} // namespace
} // namespace manyforge::deployment
